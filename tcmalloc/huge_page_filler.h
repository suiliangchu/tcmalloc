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

#ifndef TCMALLOC_HUGE_PAGE_FILLER_H_
#define TCMALLOC_HUGE_PAGE_FILLER_H_

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <limits>

#include "absl/base/internal/cycleclock.h"
#include "tcmalloc/huge_allocator.h"
#include "tcmalloc/huge_cache.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stats.h"

namespace tcmalloc {

// PageTracker keeps track of the allocation status of every page in a HugePage.
// It allows allocation and deallocation of a contiguous run of pages.
//
// Its mutating methods are annotated as requiring the pageheap_lock, in order
// to support unlocking the page heap lock in a dynamic annotation-friendly way.
template <MemoryModifyFunction Unback>
class PageTracker : public TList<PageTracker<Unback>>::Elem {
 public:
  constexpr PageTracker(HugePage p, int64_t when)
      : location_(p),
        free_{},
        when_(when),
        released_(false),
        donated_(false),
        releasing_(0) {}

  // REQUIRES: there's a free range of at least n pages
  PageID Get(Length n) EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // REQUIRES: p was the result of a previous call to Get(n)
  void Put(PageID p, Length n) EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Are unused pages returned-to-system?
  bool released() const { return released_; }
  // Was this tracker donated from the tail of a multi-hugepage allocation?
  // Only up-to-date when the tracker is on a TrackerList in the Filler;
  // otherwise the value is meaningless.
  bool donated() const { return donated_; }
  // Set/reset the donated flag. The donated status is lost, for instance,
  // when further allocations are made on the tracker.
  void set_donated(bool status) { donated_ = status; }

  // These statistics help us measure the fragmentation of a hugepage and
  // the desirability of allocating from this hugepage.
  Length longest_free_range() const { return free_.longest_free(); }
  size_t nallocs() const { return free_.allocs(); }
  Length used_pages() const { return free_.used(); }
  Length free_pages() const;
  bool empty() const;
  bool full() const;

  // Returns the hugepage whose availability is being tracked.
  HugePage location() const { return location_; }

  // Return all unused pages to the system, mark future frees to do same.
  // Returns the count of pages unbacked.
  size_t ReleaseFree() EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Return this allocation to the system, if policy warrants it.
  //
  // Our policy is:  Once we break a hugepage by returning a fraction of it, we
  // return *anything* unused.  This simplifies tracking.
  void MaybeRelease(PageID p, Length n)
      EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    if (!released_) {
      return;
    }

    releasing_ += n;
    ReleasePagesWithoutLock(p, n);
    releasing_ -= n;
  }

  void AddSpanStats(SmallSpanStats *small, LargeSpanStats *large,
                    PageAgeHistograms *ages) const;

 private:
  HugePage location_;
  RangeTracker<kPagesPerHugePage> free_;
  // TODO(b/134691947): optimize computing this; it's on the fast path.
  int64_t when_;
  static_assert(kPagesPerHugePage <= std::numeric_limits<uint16_t>::max(),
                "nallocs must be able to support kPagesPerHugePage!");
  bool released_;
  bool donated_;
  // releasing_ needs to be a Length, since we may have up to
  // kPagesPerHugePage-1 parallel subreleases in-flight.  When they complete, we
  // need to have enough information to determine whether or not any remain
  // in-flight.  This is stored as a length, rather than a bitmap of pages.
  Length releasing_ GUARDED_BY(pageheap_lock);

  void ReleasePages(PageID p, Length n) {
    void *ptr = reinterpret_cast<void *>(p << kPageShift);
    size_t byte_len = n << kPageShift;
    Unback(ptr, byte_len);
  }

  void ReleasePagesWithoutLock(PageID p, Length n)
      EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    pageheap_lock.Unlock();

    void *ptr = reinterpret_cast<void *>(p << kPageShift);
    size_t byte_len = n << kPageShift;
    Unback(ptr, byte_len);

    pageheap_lock.Lock();
  }
};

// This tracks a set of unfilled hugepages, and fulfills allocations
// with a goal of filling some hugepages as tightly as possible and emptying
// out the remainder.
template <class TrackerType>
class HugePageFiller {
 public:
  HugePageFiller();

  typedef TrackerType Tracker;

  // Our API is simple, but note that it does not include an
  // unconditional allocation, only a "try"; we expect callers to
  // allocate new hugepages if needed.  This simplifies using it in a
  // few different contexts (and improves the testing story - no
  // dependencies.)
  bool TryGet(Length n, TrackerType **hugepage, PageID *p)
      EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Marks [p, p + n) as usable by new allocations into *pt; returns pt
  // if that hugepage is now empty (nullptr otherwise.)
  // REQUIRES: pt is owned by this object (has been Contribute()), and
  // {pt, p, n} was the result of a previous TryGet.
  TrackerType *Put(TrackerType *pt, PageID p, Length n)
      EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Contributes a tracker to the filler. If "donated," then the tracker is
  // marked as having come from the tail of a multi-hugepage allocation, which
  // causes it to be treated slightly differently.
  void Contribute(TrackerType *pt, bool donated);

  HugeLength size() const { return size_; }

  // Useful statistics
  Length pages_allocated() const { return allocated_; }
  Length used_pages() const { return allocated_; }
  Length unmapped_pages() const { return unmapped_; }
  Length free_pages() const;

  // Fraction of used pages that are on non-released hugepages and
  // thus could be backed by kernel hugepages. (Of course, we can't
  // guarantee that the kernel had available 2-mib regions of physical
  // memory--so this being 1 doesn't mean that everything actually
  // *is* hugepage-backed!)
  double hugepage_frac() const;

  // Find the emptiest possible hugepage and release its free memory
  // to the system.  Return the number of pages released.
  // Currently our implementation doesn't really use this (no need!)
  Length ReleasePages() EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  void AddSpanStats(SmallSpanStats *small, LargeSpanStats *large,
                    PageAgeHistograms *ages) const;

  BackingStats stats() const;
  void Print(TCMalloc_Printer *out, bool everything) const;
  void PrintInPbtxt(PbtxtRegion *hpaa, uint64_t filler_usage_used) const;

 private:
  typedef TList<TrackerType> TrackerList;

  // This class wraps an array of N TrackerLists and a Bitmap storing which
  // elements are non-empty.
  template <size_t N>
  class HintedTrackerLists {
   public:
    HintedTrackerLists() : nonempty_{} {
      for (int i = 0; i < N; ++i) {
        lists_[i].Init();
      }
    }
    // Removes a TrackerType from the first non-empty freelist with index at
    // least n and returns it. Returns nullptr if there is none.
    TrackerType *GetLeast(const size_t n) {
      ASSERT(n < N);
      size_t i = nonempty_.FindSet(n);
      if (i == N) {
        return nullptr;
      }
      ASSERT(!lists_[i].empty());
      TrackerType *pt = lists_[i].first();
      CHECK_CONDITION(pt != nullptr);
      if (lists_[i].remove(pt)) {
        nonempty_.ClearBit(i);
      }
      return pt;
    }
    void Add(TrackerType *pt, const size_t i) {
      ASSERT(i < N);
      ASSERT(pt != nullptr);
      lists_[i].prepend(pt);
      nonempty_.SetBit(i);
    }
    void Remove(TrackerType *pt, const size_t i) {
      ASSERT(i < N);
      ASSERT(pt != nullptr);
      if (lists_[i].remove(pt)) {
        nonempty_.ClearBit(i);
      }
    }
    TrackerList &operator[](const size_t n) {
      ASSERT(n < N);
      return lists_[n];
    }
    const TrackerList &operator[](const size_t n) const {
      ASSERT(n < N);
      return lists_[n];
    }
    // Runs a functor on all HugePages in the TrackerLists.
    // This method is const but the Functor gets passed a non-const pointer.
    // This quirk is inherited from TrackerList.
    template <typename Functor>
    void Iter(const Functor &func, size_t start) const {
      size_t i = nonempty_.FindSet(start);
      while (i < N) {
        auto &list = lists_[i];
        ASSERT(!list.empty());
        for (TrackerType *pt : list) {
          func(pt);
        }
        i++;
        if (i < N) i = nonempty_.FindSet(i);
      }
    }

   private:
    TrackerList lists_[N];
    Bitmap<N> nonempty_;
  };

  // We group hugepages first by longest-free (as a measure of fragmentation),
  // then into 8 chunks inside there by desirability of allocation.
  static constexpr size_t kChunks = 8;
  // Which chunk should this hugepage be in?
  // This returns the largest possible value kChunks-1 iff pt has a single
  // allocation.
  size_t IndexFor(TrackerType *pt);
  // Returns index for regular_alloc_.
  static size_t ListFor(size_t longest, size_t chunk);
  static constexpr size_t kNumLists = kPagesPerHugePage * kChunks;

  HintedTrackerLists<kNumLists> regular_alloc_;
  HintedTrackerLists<kPagesPerHugePage> donated_alloc_;
  // Partially released ones that we are trying to release.
  HugeLength n_released_;
  HintedTrackerLists<kNumLists> regular_alloc_released_;

  // Remove pt from the appropriate HintedTrackerList.
  void Remove(TrackerType *pt);
  // Put pt in the appropriate HintedTrackerList.
  void Place(TrackerType *pt);
  // Like Place(), but for use when donating from the tail of a multi-hugepage
  // allocation.
  void Donate(TrackerType *pt);

  HugeLength size_;

  Length allocated_;
  Length unmapped_;

  // How much have we eagerly unmapped (in already released hugepages), but
  // not reported to ReleasePages calls?
  Length unmapping_unaccounted_{0};
};

template <MemoryModifyFunction Unback>
inline PageID PageTracker<Unback>::Get(Length n) {
  size_t index = free_.FindAndMark(n);
  // If we are now using the entire capacity of the huge page and do not have a
  // partial release in-flight (releasing_ > 0), then it is no longer partially
  // released to the OS.
  if (releasing_ == 0) {
    released_ = released_ && !full();
  }
  return location_.first_page() + index;
}

template <MemoryModifyFunction Unback>
inline void PageTracker<Unback>::Put(PageID p, Length n) {
  size_t index = p - location_.first_page();
  const Length before = free_.total_free();
  free_.Unmark(index, n);

  when_ = static_cast<int64_t>(
      (static_cast<double>(before) * when_ +
       static_cast<double>(n) * absl::base_internal::CycleClock::Now()) /
      (before + n));
}

template <MemoryModifyFunction Unback>
inline size_t PageTracker<Unback>::ReleaseFree() {
  released_ = true;
  size_t count = 0;
  size_t index = 0;
  size_t n;
  while (free_.NextFreeRange(index, &index, &n)) {
    PageID p = location_.first_page() + index;
    ReleasePages(p, n);
    count += n;
    index += n;
  }
  when_ = absl::base_internal::CycleClock::Now();
  return count;
}

template <MemoryModifyFunction Unback>
inline void PageTracker<Unback>::AddSpanStats(SmallSpanStats *small,
                                              LargeSpanStats *large,
                                              PageAgeHistograms *ages) const {
  size_t index = 0, n;

  int64_t w = when_;
  while (free_.NextFreeRange(index, &index, &n)) {
    if (n < kMaxPages) {
      if (small != nullptr) {
        if (released()) {
          small->returned_length[n]++;
        } else {
          small->normal_length[n]++;
        }
      }
    } else {
      if (large != nullptr) {
        large->spans++;
        if (released()) {
          large->returned_pages += n;
        } else {
          large->normal_pages += n;
        }
      }
    }

    if (ages) {
      ages->RecordRange(n, released(), w);
    }
    index += n;
  }
}

template <MemoryModifyFunction Unback>
inline bool PageTracker<Unback>::empty() const {
  return free_.used() == 0;
}

template <MemoryModifyFunction Unback>
inline bool PageTracker<Unback>::full() const {
  return free_.used() == free_.size();
}

template <MemoryModifyFunction Unback>
inline Length PageTracker<Unback>::free_pages() const {
  return kPagesPerHugePage - used_pages();
}

template <class TrackerType>
inline HugePageFiller<TrackerType>::HugePageFiller()
    : n_released_(NHugePages(0)),
      size_(NHugePages(0)),
      allocated_(0),
      unmapped_(0) {}

template <class TrackerType>
inline bool HugePageFiller<TrackerType>::TryGet(Length n,
                                                TrackerType **hugepage,
                                                PageID *p) {
  // How do we choose which hugepage to allocate from (among those with
  // a free range of at least n?) Our goal is to be as space-efficient
  // as possible, which leads to two priorities:
  //
  // (1) avoid fragmentation; keep free ranges in a hugepage as long
  //     as possible. This maintains our ability to satisfy large
  //     requests without allocating new hugepages
  // (2) fill mostly-full hugepages more; let mostly-empty hugepages
  //     empty out.  This lets us recover totally empty hugepages (and
  //     return them to the OS.)
  //
  // In practice, avoiding fragmentation is by far more important:
  // space usage can explode if we don't jealously guard large free ranges.
  //
  // Our primary measure of fragmentation of a hugepage by a proxy measure: the
  // longest free range it contains. If this is short, any free space is
  // probably fairly fragmented.  It also allows us to instantly know if a
  // hugepage can support a given allocation.
  //
  // We quantize the number of allocations in a hugepage (chunked
  // logarithmically.) We favor allocating from hugepages with many allocations
  // already present, which helps with (2) above. Note that using the number of
  // allocations works substantially better than the number of allocated pages;
  // to first order allocations of any size are about as likely to be freed, and
  // so (by simple binomial probability distributions) we're more likely to
  // empty out a hugepage with 2 5-page allocations than one with 5 1-pages.
  //
  // The above suggests using the hugepage with the shortest longest empty
  // range, breaking ties in favor of fewest number of allocations. This works
  // well for most workloads but caused bad page heap fragmentation for some:
  // b/63301358 and b/138618726. The intuition for what went wrong is
  // that although the tail of large allocations is donated to the Filler (see
  // HugePageAwareAllocator::AllocRawHugepages) for use, we don't actually
  // want to use them until the regular Filler hugepages are used up. That
  // way, they can be reassembled as a single large hugepage range if the
  // large allocation is freed.
  // Some workloads can tickle this discrepancy a lot, because they have a lot
  // of large, medium-lifetime allocations. To fix this we treat hugepages
  // that are freshly donated as less preferable than hugepages that have been
  // already used for small allocations, regardless of their longest_free_range.
  //
  // Overall our allocation preference is:
  //  - We prefer allocating from used freelists rather than freshly donated
  //  - Among donated freelists we prefer smaller longest_free_range
  //  - Among used freelists we prefer smaller longest_free_range
  //    with ties broken by (quantized) alloc counts
  //
  // We group hugepages by longest_free_range and quantized alloc count and
  // store each group in a TrackerList. All freshly-donated groups are stored
  // in a "donated" array and the groups with (possibly prior) small allocs are
  // stored in a "regular" array. Each of these arrays is encapsulated in a
  // HintedTrackerLists object, which stores the array together with a bitmap to
  // quickly find non-empty lists. The lists are ordered to satisfy the
  // following two useful properties:
  //
  // - later (nonempty) freelists can always fulfill requests that
  //   earlier ones could.
  // - earlier freelists, by the above criteria, are preferred targets
  //   for allocation.
  //
  // So all we have to do is find the first nonempty freelist in the regular
  // HintedTrackerList that *could* support our allocation, and it will be our
  // best choice. If there is none we repeat with the donated HintedTrackerList.
  if (n >= kPagesPerHugePage) return false;
  TrackerType *pt;

  bool was_released = false;
  do {
    pt = regular_alloc_.GetLeast(ListFor(n, 0));
    if (pt) {
      ASSERT(!pt->donated());
      break;
    }
    pt = donated_alloc_.GetLeast(n);
    if (pt) {
      break;
    }
    pt = regular_alloc_released_.GetLeast(ListFor(n, 0));
    if (pt) {
      ASSERT(!pt->donated());
      was_released = true;
      --n_released_;
      break;
    }

    return false;
  } while (false);
  ASSERT(pt->longest_free_range() >= n);
  *hugepage = pt;
  *p = pt->Get(n);
  Place(pt);
  allocated_ += n;
  if (was_released) {
    ASSERT(unmapped_ >= n);
    unmapped_ -= n;
  }
  // We're being used for an allocation, so we are no longer considered
  // donated by this point.
  ASSERT(!pt->donated());
  return true;
}

// Marks [p, p + n) as usable by new allocations into *pt; returns pt
// if that hugepage is now empty (nullptr otherwise.)
// REQUIRES: pt is owned by this object (has been Contribute()), and
// {pt, p, n} was the result of a previous TryGet.
template <class TrackerType>
inline TrackerType *HugePageFiller<TrackerType>::Put(TrackerType *pt, PageID p,
                                                     Length n) {
  // Consider releasing [p, p+n).  We do this here:
  // * To unback the memory before we mark it as free.  When partially
  //   unbacking, we release the pageheap_lock.  Another thread could see the
  //   "free" memory and begin using it before we retake the lock.
  // * To maintain maintain the invariant that
  //     pt->released() => n_released_ > 0
  //   which is used for keeping our released hugepage stats in-sync with the
  //   cached n_released_ value:
  //     n_released_ = regular_alloc_released_.size()
  //   size() doesn't exist (it'd be O(n) while holding the pageheap_lock).
  //   We do this before removing pt from our lists, since another thread may
  //   encounter our post-Remove() update to n_released_ while encountering pt.
  pt->MaybeRelease(p, n);

  Remove(pt);

  pt->Put(p, n);

  allocated_ -= n;
  if (pt->released()) {
    unmapped_ += n;
    unmapping_unaccounted_ += n;
  }
  if (pt->longest_free_range() == kPagesPerHugePage) {
    --size_;
    if (pt->released()) {
      unmapped_ -= kPagesPerHugePage;
    }
    return pt;
  }
  Place(pt);
  return nullptr;
}

template <class TrackerType>
inline void HugePageFiller<TrackerType>::Contribute(TrackerType *pt,
                                                    bool donated) {
  allocated_ += pt->used_pages();
  if (donated) {
    Donate(pt);
  } else {
    Place(pt);
  }
  ++size_;
}

// Find the emptiest possible hugepage and release its free memory
// to the system.  Return the number of pages released.
// Currently our implementation doesn't really use this (no need!)
template <class TrackerType>
inline Length HugePageFiller<TrackerType>::ReleasePages() {
  // We also do eager release, once we've called this at least once:
  // claim credit for anything that gets done.
  if (unmapping_unaccounted_ > 0) {
    Length n = unmapping_unaccounted_;
    unmapping_unaccounted_ = 0;
    return n;
  }
  TrackerType *best = nullptr;
  auto loop = [&](TrackerType *pt) {
    if (!best || best->used_pages() > pt->used_pages()) {
      best = pt;
    }
  };

  // We can skip the first kChunks lists as they are known to be 100% full.
  // (Those lists are likely to be long.)
  //
  // We do not examine the regular_alloc_released_ lists, as deallocating on
  // an already released page causes it to fully return everything (see
  // PageTracker::Put).
  // TODO(b/138864853): Perhaps remove donated_alloc_ from here, it's not a
  // great candidate for partial release.
  regular_alloc_.Iter(loop, kChunks);
  donated_alloc_.Iter(loop, 0);

  if (best && !best->full()) {
    Remove(best);
    Length ret = best->ReleaseFree();
    unmapped_ += ret;
    Place(best);
    return ret;
  }
  return 0;
}

template <class TrackerType>
inline void HugePageFiller<TrackerType>::AddSpanStats(
    SmallSpanStats *small, LargeSpanStats *large,
    PageAgeHistograms *ages) const {
  auto loop = [&](const TrackerType *pt) {
    pt->AddSpanStats(small, large, ages);
  };
  // We can skip the first kChunks lists as they are known to be 100% full.
  regular_alloc_.Iter(loop, kChunks);
  donated_alloc_.Iter(loop, 0);

  regular_alloc_released_.Iter(loop, 0);
}

template <class TrackerType>
inline BackingStats HugePageFiller<TrackerType>::stats() const {
  BackingStats s;
  s.system_bytes = size_.in_bytes();
  s.free_bytes = free_pages() * kPageSize;
  s.unmapped_bytes = unmapped_pages() * kPageSize;
  return s;
}

namespace internal {
// Computes some histograms of fullness. Because nearly empty/full huge pages
// are much more interesting, we calculate 4 buckets at each of the beginning
// and end of size one, and then divide the overall space by 16 to have 16
// (mostly) even buckets in the middle.
class UsageInfo {
 public:
  enum Type { kRegular, kDonated, kReleased };

  UsageInfo() {
    size_t i;
    for (i = 0; i <= 4 && i < kPagesPerHugePage; ++i) {
      bucket_bounds_[buckets_size_] = i;
      buckets_size_++;
    }
    if (i < kPagesPerHugePage - 4) {
      // Because kPagesPerHugePage is a power of two, it must be at least 16
      // to get inside this "if" - either i=5 and kPagesPerHugePage=8 and
      // the test fails, or kPagesPerHugePage <= 4 and the test fails.
      ASSERT(kPagesPerHugePage >= 16);
      constexpr int step = kPagesPerHugePage / 16;
      // We want to move in "step"-sized increments, aligned every "step".
      // So first we have to round i up to the nearest step boundary. This
      // logic takes advantage of step being a power of two, so step-1 is
      // all ones in the low-order bits.
      i = ((i - 1) | (step - 1)) + 1;
      for (; i < kPagesPerHugePage - 4; i += step) {
        bucket_bounds_[buckets_size_] = i;
        buckets_size_++;
      }
      i = kPagesPerHugePage - 4;
    }
    for (; i < kPagesPerHugePage; ++i) {
      bucket_bounds_[buckets_size_] = i;
      buckets_size_++;
    }
    CHECK_CONDITION(buckets_size_ <= kBucketCapacity);
  }

  template <class TrackerType>
  void Record(const TrackerType *pt, Type which) {
    const size_t free = kPagesPerHugePage - pt->used_pages();
    const size_t lf = pt->longest_free_range();
    const size_t nalloc = pt->nallocs();
    // This is a little annoying as our buckets *have* to differ;
    // nalloc is in [1,256], free_pages and longest_free are in [0, 255].
    free_page_histo_[which][BucketNum(free)]++;
    longest_free_histo_[which][BucketNum(lf)]++;
    nalloc_histo_[which][BucketNum(nalloc - 1)]++;
  }

  void Print(TCMalloc_Printer *out) {
    PrintHisto(out, free_page_histo_[kRegular],
               "# of regular hps with a<= # of free pages <b", 0);
    PrintHisto(out, free_page_histo_[kDonated],
               "# of donated hps with a<= # of free pages <b", 0);
    PrintHisto(out, free_page_histo_[kReleased],
               "# of released hps with a<= # of free pages <b", 0);
    // For donated huge pages, number of allocs=1 and longest free range =
    // number of free pages, so it isn't useful to show the next two.
    PrintHisto(out, longest_free_histo_[kRegular],
               "# of regular hps with a<= longest free range <b", 0);
    PrintHisto(out, longest_free_histo_[kReleased],
               "# of released hps with a<= longest free range <b", 0);
    PrintHisto(out, nalloc_histo_[kRegular],
               "# of regular hps with a<= # of allocations <b", 1);
    PrintHisto(out, nalloc_histo_[kReleased],
               "# of released hps with a<= # of allocations <b", 1);
  }

  void Print(PbtxtRegion *hpaa) {
    static constexpr absl::string_view kTrackerTypes[3] = {"REGULAR", "DONATED",
                                                           "RELEASED"};
    for (int i = 0; i < 3; ++i) {
      PbtxtRegion scoped = hpaa->CreateSubRegion("filler_tracker");
      scoped.PrintRaw("type", kTrackerTypes[i]);
      PrintHisto(&scoped, free_page_histo_[i], "free_pages_histogram", 0);
      PrintHisto(&scoped, longest_free_histo_[i],
                 "longest_free_range_histogram", 0);
      PrintHisto(&scoped, nalloc_histo_[i], "allocations_histogram", 1);
    }
  }

 private:
  // Maximum of 4 buckets at the start and end, and 16 in the middle.
  static constexpr size_t kBucketCapacity = 4 + 16 + 4;
  using Histo = size_t[kBucketCapacity];

  int BucketNum(int page) {
    auto it =
        std::upper_bound(bucket_bounds_, bucket_bounds_ + buckets_size_, page);
    CHECK_CONDITION(it != bucket_bounds_);
    return it - bucket_bounds_ - 1;
  }

  void PrintHisto(TCMalloc_Printer *out, Histo h, const char blurb[],
                  size_t offset) {
    out->printf("\nHugePageFiller: %s", blurb);
    for (size_t i = 0; i < buckets_size_; ++i) {
      if (i % 6 == 0) {
        out->printf("\nHugePageFiller:");
      }
      out->printf(" <%3zu<=%6zu", bucket_bounds_[i] + offset, h[i]);
    }
    out->printf("\n");
  }

  void PrintHisto(PbtxtRegion *hpaa, Histo h, const char key[], size_t offset) {
    for (size_t i = 0; i < buckets_size_; ++i) {
      auto hist = hpaa->CreateSubRegion(key);
      hist.PrintI64("lower_bound", bucket_bounds_[i] + offset);
      hist.PrintI64("upper_bound",
                    (i == buckets_size_ - 1 ? bucket_bounds_[i]
                                            : bucket_bounds_[i + 1] - 1) +
                        offset);
      hist.PrintI64("value", h[i]);
    }
  }

  // Arrays, because they are split per alloc type.
  Histo free_page_histo_[3]{};
  Histo longest_free_histo_[3]{};
  Histo nalloc_histo_[3]{};
  size_t bucket_bounds_[kBucketCapacity];
  int buckets_size_ = 0;
};
}  // namespace internal

template <class TrackerType>
inline void HugePageFiller<TrackerType>::Print(TCMalloc_Printer *out,
                                               bool everything) const {
  out->printf("HugePageFiller: densely pack small requests into hugepages\n");

  HugeLength nrel = n_released_;
  HugeLength nfull = NHugePages(0);

  // note kChunks, not kNumLists here--we're iterating *full* lists.
  for (size_t chunk = 0; chunk < kChunks; ++chunk) {
    nfull += NHugePages(regular_alloc_[ListFor(/*longest=*/0, chunk)].length());
  }
  // A donated alloc full list is impossible because it would have never been
  // donated in the first place. (It's an even hugepage.)
  ASSERT(donated_alloc_[0].empty());
  // Evaluate a/b, avoiding division by zero
  const auto safe_div = [](double a, double b) { return b == 0 ? 0 : a / b; };
  const HugeLength n_nonfull = size() - nrel - nfull;
  out->printf(
      "HugePageFiller: %zu total, %zu full, %zu partial, %zu released, 0 "
      "quarantined\n",
      size().raw_num(), nfull.raw_num(), n_nonfull.raw_num(), nrel.raw_num());
  out->printf("HugePageFiller: %zu pages free in %zu hugepages, %.4f free\n",
              free_pages(), size().raw_num(),
              safe_div(free_pages(), size().in_pages()));

  out->printf("HugePageFiller: among non-fulls, %.4f free\n",
              safe_div(free_pages(), n_nonfull.in_pages()));

  out->printf(
      "HugePageFiller: %zu hugepages partially released, %.4f released\n",
      nrel.raw_num(), safe_div(unmapped_pages(), nrel.in_pages()));
  out->printf("HugePageFiller: %.4f of used pages hugepageable\n",
              hugepage_frac());
  if (!everything) return;

  // Compute some histograms of fullness.
  using ::tcmalloc::internal::UsageInfo;
  UsageInfo usage;
  regular_alloc_.Iter(
      [&](const TrackerType *pt) { usage.Record(pt, UsageInfo::kRegular); }, 0);
  donated_alloc_.Iter(
      [&](const TrackerType *pt) { usage.Record(pt, UsageInfo::kDonated); }, 0);
  regular_alloc_released_.Iter(
      [&](const TrackerType *pt) { usage.Record(pt, UsageInfo::kReleased); },
      0);

  out->printf("\n");
  out->printf("HugePageFiller: fullness histograms\n");
  usage.Print(out);
}

template <class TrackerType>
inline void HugePageFiller<TrackerType>::PrintInPbtxt(
    PbtxtRegion *hpaa, uint64_t filler_usage_used) const {
  HugeLength nrel = n_released_;
  HugeLength nfull = NHugePages(0);

  // note kChunks, not kNumLists here--we're iterating *full* lists.
  for (size_t chunk = 0; chunk < kChunks; ++chunk) {
    nfull += NHugePages(regular_alloc_[ListFor(/*longest=*/0, chunk)].length());
  }
  // A donated alloc full list is impossible because it would have never been
  // donated in the first place. (It's an even hugepage.)
  ASSERT(donated_alloc_[0].empty());
  // Evaluate a/b, avoiding division by zero
  const auto safe_div = [](double a, double b) { return b == 0 ? 0 : a / b; };
  const HugeLength n_nonfull = size() - nrel - nfull;
  hpaa->PrintI64("filler_full_huge_pages", nfull.raw_num());
  hpaa->PrintI64("filler_partial_huge_pages", n_nonfull.raw_num());
  hpaa->PrintI64("filler_released_huge_pages", nrel.raw_num());
  hpaa->PrintI64("filler_free_pages", free_pages());
  hpaa->PrintI64(
      "filler_unmapped_bytes",
      static_cast<uint64_t>(nrel.raw_num() *
                          safe_div(unmapped_pages(), nrel.in_pages())));
  hpaa->PrintI64("filler_hugepageable_used_bytes",
                 static_cast<uint64_t>(hugepage_frac() *
                                     static_cast<double>(filler_usage_used)));

  // Compute some histograms of fullness.
  using ::tcmalloc::internal::UsageInfo;
  UsageInfo usage;
  regular_alloc_.Iter(
      [&](const TrackerType *pt) { usage.Record(pt, UsageInfo::kRegular); }, 0);
  donated_alloc_.Iter(
      [&](const TrackerType *pt) { usage.Record(pt, UsageInfo::kDonated); }, 0);
  regular_alloc_released_.Iter(
      [&](const TrackerType *pt) { usage.Record(pt, UsageInfo::kReleased); },
      0);

  usage.Print(hpaa);
}

template <class TrackerType>
inline size_t HugePageFiller<TrackerType>::IndexFor(TrackerType *pt) {
  ASSERT(!pt->empty());
  // Prefer to allocate from hugepages with many allocations already present;
  // spaced logarithmically.
  const size_t na = pt->nallocs();
  // This equals 63 - ceil(log2(na))
  // (or 31 if size_t is 4 bytes, etc.)
  const size_t neg_ceil_log = __builtin_clzl(2 * na - 1);

  // We want the same spread as neg_ceil_log, but spread over [0,
  // kChunks) (clamped at the left edge) instead of [0, 64). So subtract off
  // the difference (computed by forcing na=1 to kChunks - 1.)
  const size_t kOffset = __builtin_clzl(1) - (kChunks - 1);
  const size_t i = std::max(neg_ceil_log, kOffset) - kOffset;
  ASSERT(i < kChunks);
  return i;
}

template <class TrackerType>
inline size_t HugePageFiller<TrackerType>::ListFor(const size_t longest,
                                                   const size_t chunk) {
  ASSERT(chunk < kChunks);
  ASSERT(longest < kPagesPerHugePage);
  return longest * kChunks + chunk;
}

template <class TrackerType>
inline void HugePageFiller<TrackerType>::Remove(TrackerType *pt) {
  size_t longest = pt->longest_free_range();
  ASSERT(longest < kPagesPerHugePage);

  if (pt->released()) {
    --n_released_;
  }

  if (pt->donated()) {
    donated_alloc_.Remove(pt, longest);
  } else {
    size_t chunk = IndexFor(pt);
    auto &list = pt->released() ? regular_alloc_released_ : regular_alloc_;
    size_t i = ListFor(longest, chunk);
    list.Remove(pt, i);
  }
}

template <class TrackerType>
inline void HugePageFiller<TrackerType>::Place(TrackerType *pt) {
  size_t chunk = IndexFor(pt);
  size_t longest = pt->longest_free_range();
  ASSERT(longest < kPagesPerHugePage);

  if (pt->released()) {
    ++n_released_;
  }
  // Once a donated alloc is used in any way, it degenerates into being a
  // regular alloc. This allows the algorithm to keep using it (we had to be
  // desperate to use it in the first place), and thus preserves the other
  // donated allocs.
  pt->set_donated(false);

  auto *list = pt->released() ? &regular_alloc_released_ : &regular_alloc_;
  size_t i = ListFor(longest, chunk);
  list->Add(pt, i);
}

template <class TrackerType>
inline void HugePageFiller<TrackerType>::Donate(TrackerType *pt) {
  size_t longest = pt->longest_free_range();
  ASSERT(longest < kPagesPerHugePage);

  // We should never be donating already-released trackers!
  ASSERT(!pt->released());
  pt->set_donated(true);

  donated_alloc_.Add(pt, longest);
}

template <class TrackerType>
inline double HugePageFiller<TrackerType>::hugepage_frac() const {
  // How many of our used pages are on non-huge pages? Since
  // everything on a released hugepage is either used or released,
  // just the difference:
  const Length nrel = n_released_.in_pages();
  const Length used = used_pages();
  const Length unmapped = unmapped_pages();
  const Length used_on_rel = nrel >= unmapped ? nrel - unmapped : 0;
  ASSERT(used >= used_on_rel);
  const Length used_on_huge = used - used_on_rel;

  const Length denom = used > 0 ? used : 1;
  const double ret = static_cast<double>(used_on_huge) / denom;
  ASSERT(ret >= 0);
  ASSERT(ret <= 1);
  // TODO(b/117611602):  Replace this with absl::clamp when that is
  // open-sourced.
  return (ret < 0) ? 0 : (ret > 1) ? 1 : ret;
}

// Helper for stat functions.
template <class TrackerType>
inline Length HugePageFiller<TrackerType>::free_pages() const {
  return size().in_pages() - used_pages() - unmapped_pages();
}

}  // namespace tcmalloc

#endif  // TCMALLOC_HUGE_PAGE_FILLER_H_
