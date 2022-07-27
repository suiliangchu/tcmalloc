// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_PAGE_HEAP_H_
#define TCMALLOC_PAGE_HEAP_H_

#include <stdint.h>

#include "absl/base/thread_annotations.h"
#include "tcmalloc/common.h"
#include "tcmalloc/page_allocator_interface.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stats.h"

namespace tcmalloc {

// -------------------------------------------------------------------------
// Page-level allocator
//  * Eager coalescing
//
// Heap for page-level allocation.  We allow allocating and freeing a
// contiguous runs of pages (called a "span").
// -------------------------------------------------------------------------

class PageHeap : public PageAllocatorInterface {
 public:
  explicit PageHeap(bool tagged);
  // for testing
  PageHeap(PageMap* map, bool tagged);

  // Allocate a run of "n" pages.  Returns zero if out of memory.
  // Caller should not pass "n == 0" -- instead, n should have
  // been rounded up already.
  // The returned memory is backed.
  Span* New(Length n) LOCKS_EXCLUDED(pageheap_lock) override;

  // As New, but the returned span is aligned to a <align>-page boundary.
  // <align> must be a power of two.
  Span* NewAligned(Length n, Length align)
      LOCKS_EXCLUDED(pageheap_lock) override;

  // Delete the span "[p, p+n-1]".
  // REQUIRES: span was returned by earlier call to New() and
  //           has not yet been deleted.
  void Delete(Span* span) EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override;

  inline BackingStats stats() const
      EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override {
    return stats_;
  }

  void GetSmallSpanStats(SmallSpanStats* result)
      EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override;

  void GetLargeSpanStats(LargeSpanStats* result)
      EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override;

  // Try to release at least num_pages for reuse by the OS.  Returns
  // the actual number of pages released, which may be less than
  // num_pages if there weren't enough pages to release. The result
  // may also be larger than num_pages since page_heap might decide to
  // release one large range instead of fragmenting it into two
  // smaller released and unreleased ranges.
  Length ReleaseAtLeastNPages(Length num_pages)
      EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override;

  // Prints stats about the page heap to *out.
  void Print(TCMalloc_Printer* out) LOCKS_EXCLUDED(pageheap_lock) override;

  void PrintInPbtxt(PbtxtRegion* region) LOCKS_EXCLUDED(pageheap_lock) override;

 private:
  // Never delay scavenging for more than the following number of
  // deallocated pages.  With 8K pages, this comes to 8GiB of
  // deallocation.
  static const int kMaxReleaseDelay = 1 << 20;

  // If there is nothing to release, wait for so many pages before
  // scavenging again.  With 8K pages, this comes to 2GiB of memory.
  static const int kDefaultReleaseDelay = 1 << 18;

  // We segregate spans of a given size into two circular linked
  // lists: one for normal spans, and one for spans whose memory
  // has been returned to the system.
  struct SpanListPair {
    SpanList normal;
    SpanList returned;
  };

  // List of free spans of length >= kMaxPages
  SpanListPair large_ GUARDED_BY(pageheap_lock);

  // Array mapping from span length to a doubly linked list of free spans
  SpanListPair free_[kMaxPages] GUARDED_BY(pageheap_lock);

  // Statistics on system, free, and unmapped bytes
  BackingStats stats_ GUARDED_BY(pageheap_lock);

  Span* SearchFreeAndLargeLists(Length n, bool* from_returned)
      EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  bool GrowHeap(Length n) EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // REQUIRES: span->length >= n
  // REQUIRES: span->location != IN_USE
  // Remove span from its free list, and move any leftover part of
  // span into appropriate free lists.  Also update "span" to have
  // length exactly "n" and mark it as non-free so it can be returned
  // to the client.  After all that, decrease free_pages_ by n and
  // return span.
  Span* Carve(Span* span, Length n) EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Allocate a large span of length == n.  If successful, returns a
  // span of exactly the specified length.  Else, returns NULL.
  Span* AllocLarge(Length n, bool* from_returned)
      EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Coalesce span with neighboring spans if possible, prepend to
  // appropriate free list, and adjust stats.
  void MergeIntoFreeList(Span* span) EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Prepends span to appropriate free list, and adjusts stats.  You'll probably
  // want to adjust span->freelist_added_time before/after calling this
  // function.
  void PrependToFreeList(Span* span) EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Removes span from its free list, and adjust stats.
  void RemoveFromFreeList(Span* span) EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Incrementally release some memory to the system.
  // IncrementalScavenge(n) is called whenever n pages are freed.
  void IncrementalScavenge(Length n) EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Release the last span on the normal portion of this list.
  // Return the length of that span.
  Length ReleaseLastNormalSpan(SpanListPair* slist)
      EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Prints stats for the given list of Spans.
  //  - span_type: a short, human-readable string describing the spans
  //    (typically "live" or "unmapped").
  //  - span_size: the number of pages in each of the given spans.  For large
  //    spans (which are of various sizes), pass kMaxPages.
  //  - span_size_prefix: We print this string immediately to the left of the
  //    span size.  This is useful for large spans (e.g. you might pass ">=").
  void DumpSubSpanStats(TCMalloc_Printer* out, const SpanList* spans,
                        const char* span_type, int span_size,
                        uint64_t cycle_now, double cycle_clock_freq,
                        const char* span_size_prefix);
  // Prints summary stats for all live ("normal") or unmapped ("returned")
  // pages.
  void DumpSpanTotalStats(TCMalloc_Printer* out, uint64_t cycle_now,
                          double cycle_clock_freq, bool live_spans);

  // Do invariant testing.
  bool Check() EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Number of pages to deallocate before doing more scavenging
  int64_t scavenge_counter_ GUARDED_BY(pageheap_lock);

  // Index of last free list where we released memory to the OS.
  int release_index_ GUARDED_BY(pageheap_lock);

  Span* AllocateSpan(Length n, bool* from_returned)
      EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  void RecordSpan(Span* span) EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
};

}  // namespace tcmalloc

#endif  // TCMALLOC_PAGE_HEAP_H_
