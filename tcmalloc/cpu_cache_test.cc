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

#include "tcmalloc/cpu_cache.h"

#include <sys/mman.h>

#include <new>
#include <string>
#include <thread>  // NOLINT(build/c++11)

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/dynamic_annotations.h"
#include "absl/random/bit_gen_ref.h"
#include "absl/random/random.h"
#include "absl/random/seed_sequences.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/util.h"
#include "tcmalloc/mock_transfer_cache.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/testing/testutil.h"
#include "tcmalloc/testing/thread_manager.h"
#include "tcmalloc/transfer_cache.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class TestStaticForwarder {
 public:
  TestStaticForwarder() : sharded_manager_(&owner_, &cpu_layout_) {
    numa_topology_.Init();
  }

  static void* Alloc(size_t size, std::align_val_t alignment) {
    void* ptr = ::operator new(size, alignment);
    if (static_cast<size_t>(alignment) >= getpagesize()) {
      // Emulate obtaining memory as if we got it from mmap (zero'd).
      memset(ptr, 0, size);
      madvise(ptr, size, MADV_DONTNEED);
    }
    return ptr;
  }

  static void Dealloc(void* ptr, size_t size, std::align_val_t alignment) {
    sized_aligned_delete(ptr, size, alignment);
  }

  size_t class_to_size(int size_class) const {
    return transfer_cache_.class_to_size(size_class);
  }

  static absl::Span<const size_t> cold_size_classes() { return {}; }

  static size_t max_per_cpu_cache_size() {
    // TODO(b/179516472):  Move this to CPUCache itself so it can be informed
    // when the parameter is changed at runtime.
    return Parameters::max_per_cpu_cache_size();
  }

  size_t num_objects_to_move(int size_class) const {
    return transfer_cache_.num_objects_to_move(size_class);
  }

  const NumaTopology<kNumaPartitions, kNumBaseClasses>& numa_topology() const {
    return numa_topology_;
  }

  using ShardedManager = ShardedTransferCacheManagerBase<
      FakeShardedTransferCacheEnvironment::Manager, FakeCpuLayout,
      MinimalFakeCentralFreeList>;

  ShardedManager& sharded_transfer_cache() { return sharded_manager_; }

  const ShardedManager& sharded_transfer_cache() const {
    return sharded_manager_;
  }

  TwoSizeClassManager<FakeCentralFreeList,
                      internal_transfer_cache::RingBufferTransferCache>&
  transfer_cache() {
    return transfer_cache_;
  }

 private:
  NumaTopology<kNumaPartitions, kNumBaseClasses> numa_topology_;
  ArenaBasedFakeTransferCacheManager owner_;
  FakeCpuLayout cpu_layout_;
  ShardedManager sharded_manager_;
  TwoSizeClassManager<FakeCentralFreeList,
                      internal_transfer_cache::RingBufferTransferCache>
      transfer_cache_;
};

using CPUCache = cpu_cache_internal::CPUCache<TestStaticForwarder>;
using MissCount = CPUCache::MissCount;

constexpr size_t kStressSlabs = 4;
void* OOMHandler(size_t) { return nullptr; }

TEST(CpuCacheTest, Metadata) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  const int num_cpus = absl::base_internal::NumCPUs();

  CPUCache cache;
  cache.Activate();

  PerCPUMetadataState r = cache.MetadataMemoryUsage();
  EXPECT_EQ(r.virtual_size,
            subtle::percpu::GetSlabsAllocSize(
                subtle::percpu::ToShiftType(CPUCache::kPerCpuShift), num_cpus));
  EXPECT_EQ(r.resident_size, 0);

  auto count_cores = [&]() {
    int populated_cores = 0;
    for (int i = 0; i < num_cpus; i++) {
      if (cache.HasPopulated(i)) {
        populated_cores++;
      }
    }
    return populated_cores;
  };

  EXPECT_EQ(0, count_cores());

  int allowed_cpu_id;
  const size_t kSizeClass = 2;
  const size_t num_to_move = cache.forwarder().num_objects_to_move(kSizeClass);
  const size_t virtual_cpu_id_offset = subtle::percpu::UsingFlatVirtualCpus()
                                           ? offsetof(kernel_rseq, vcpu_id)
                                           : offsetof(kernel_rseq, cpu_id);
  void* ptr;
  {
    // Restrict this thread to a single core while allocating and processing the
    // slow path.
    //
    // TODO(b/151313823):  Without this restriction, we may access--for reading
    // only--other slabs if we end up being migrated.  These may cause huge
    // pages to be faulted for those cores, leading to test flakiness.
    tcmalloc_internal::ScopedAffinityMask mask(
        tcmalloc_internal::AllowedCpus()[0]);
    allowed_cpu_id =
        subtle::percpu::GetCurrentVirtualCpuUnsafe(virtual_cpu_id_offset);

    ptr = cache.Allocate<OOMHandler>(kSizeClass);

    if (mask.Tampered() ||
        allowed_cpu_id !=
            subtle::percpu::GetCurrentVirtualCpuUnsafe(virtual_cpu_id_offset)) {
      return;
    }
  }
  EXPECT_NE(ptr, nullptr);
  EXPECT_EQ(1, count_cores());

  r = cache.MetadataMemoryUsage();
  EXPECT_EQ(r.virtual_size,
            subtle::percpu::GetSlabsAllocSize(
                subtle::percpu::ToShiftType(CPUCache::kPerCpuShift), num_cpus));

  // We expect to fault in a single core, but we may end up faulting an
  // entire hugepage worth of memory
  const size_t core_slab_size = r.virtual_size / num_cpus;
  const size_t upper_bound =
      ((core_slab_size + kHugePageSize - 1) & ~(kHugePageSize - 1));

  // A single core may be less than the full slab (core_slab_size), since we
  // do not touch every page within the slab.
  EXPECT_GT(r.resident_size, 0);
  EXPECT_LE(r.resident_size, upper_bound) << count_cores();

  // This test is much more sensitive to implementation details of the per-CPU
  // cache.  It may need to be updated from time to time.  These numbers were
  // calculated by MADV_NOHUGEPAGE'ing the memory used for the slab and
  // measuring the resident size.
  switch (CPUCache::kPerCpuShift) {
    case 12:
      EXPECT_GE(r.resident_size, 4096);
      break;
    case 18:
      EXPECT_GE(r.resident_size, 8192);
      break;
    default:
      ASSUME(false);
      break;
  };

  // Read stats from the CPU caches.  This should not impact resident_size.
  const size_t max_cpu_cache_size = Parameters::max_per_cpu_cache_size();
  size_t total_used_bytes = 0;
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    size_t used_bytes = cache.UsedBytes(cpu);
    total_used_bytes += used_bytes;

    if (cpu == allowed_cpu_id) {
      EXPECT_GT(used_bytes, 0);
      EXPECT_TRUE(cache.HasPopulated(cpu));
    } else {
      EXPECT_EQ(used_bytes, 0);
      EXPECT_FALSE(cache.HasPopulated(cpu));
    }

    EXPECT_LE(cache.Unallocated(cpu), max_cpu_cache_size);
    EXPECT_EQ(cache.Capacity(cpu), max_cpu_cache_size);
    EXPECT_EQ(cache.Allocated(cpu) + cache.Unallocated(cpu),
              cache.Capacity(cpu));
  }

  for (int size_class = 0; size_class < kNumClasses; ++size_class) {
    // This is sensitive to the current growth policies of CPUCache.  It may
    // require updating from time-to-time.
    EXPECT_EQ(cache.TotalObjectsOfClass(size_class),
              (size_class == kSizeClass ? num_to_move - 1 : 0))
        << size_class;
  }
  EXPECT_EQ(cache.TotalUsedBytes(), total_used_bytes);

  PerCPUMetadataState post_stats = cache.MetadataMemoryUsage();
  // Confirm stats are within expected bounds.
  EXPECT_GT(post_stats.resident_size, 0);
  EXPECT_LE(post_stats.resident_size, upper_bound) << count_cores();
  // Confirm stats are unchanged.
  EXPECT_EQ(r.resident_size, post_stats.resident_size);

  // Tear down.
  cache.Deallocate(ptr, kSizeClass);
  cache.Deactivate();
}

TEST(CpuCacheTest, CacheMissStats) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  const int num_cpus = absl::base_internal::NumCPUs();

  CPUCache cache;
  cache.Activate();

  //  The number of underflows and overflows must be zero for all the caches.
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    CPUCache::CpuCacheMissStats total_misses =
        cache.GetTotalCacheMissStats(cpu);
    CPUCache::CpuCacheMissStats shuffle_misses =
        cache.GetIntervalCacheMissStats(cpu, MissCount::kShuffle);
    EXPECT_EQ(total_misses.underflows, 0);
    EXPECT_EQ(total_misses.overflows, 0);
    EXPECT_EQ(shuffle_misses.underflows, 0);
    EXPECT_EQ(shuffle_misses.overflows, 0);
  }

  int allowed_cpu_id;
  const size_t kSizeClass = 2;
  const size_t virtual_cpu_id_offset = subtle::percpu::UsingFlatVirtualCpus()
                                           ? offsetof(kernel_rseq, vcpu_id)
                                           : offsetof(kernel_rseq, cpu_id);
  void* ptr;
  {
    // Restrict this thread to a single core while allocating and processing the
    // slow path.
    //
    // TODO(b/151313823):  Without this restriction, we may access--for reading
    // only--other slabs if we end up being migrated.  These may cause huge
    // pages to be faulted for those cores, leading to test flakiness.
    tcmalloc_internal::ScopedAffinityMask mask(
        tcmalloc_internal::AllowedCpus()[0]);
    allowed_cpu_id =
        subtle::percpu::GetCurrentVirtualCpuUnsafe(virtual_cpu_id_offset);

    ptr = cache.Allocate<OOMHandler>(kSizeClass);

    if (mask.Tampered() ||
        allowed_cpu_id !=
            subtle::percpu::GetCurrentVirtualCpuUnsafe(virtual_cpu_id_offset)) {
      return;
    }
  }

  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    CPUCache::CpuCacheMissStats total_misses =
        cache.GetTotalCacheMissStats(cpu);
    CPUCache::CpuCacheMissStats shuffle_misses =
        cache.GetIntervalCacheMissStats(cpu, MissCount::kShuffle);
    if (cpu == allowed_cpu_id) {
      EXPECT_EQ(total_misses.underflows, 1);
      EXPECT_EQ(shuffle_misses.underflows, 1);
    } else {
      EXPECT_EQ(total_misses.underflows, 0);
      EXPECT_EQ(shuffle_misses.underflows, 0);
    }
    EXPECT_EQ(total_misses.overflows, 0);
    EXPECT_EQ(shuffle_misses.overflows, 0);
  }

  // Tear down.
  cache.Deallocate(ptr, kSizeClass);
  cache.Deactivate();
}

static void ShuffleThread(CPUCache& cache, const std::atomic<bool>& stop) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  // Wake up every 10ms to shuffle the caches so that we can allow misses to
  // accumulate during that interval
  while (!stop.load(std::memory_order_acquire)) {
    cache.ShuffleCpuCaches();
    absl::SleepFor(absl::Milliseconds(10));
  }
}

static void StressThread(CPUCache& cache, size_t thread_id,
                         const std::atomic<bool>& stop) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  std::vector<std::pair<size_t, void*>> blocks;
  absl::BitGen rnd;
  while (!stop.load(std::memory_order_acquire)) {
    const int what = absl::Uniform<int32_t>(rnd, 0, 2);
    if (what) {
      // Allocate an object for a class
      size_t size_class = absl::Uniform<int32_t>(rnd, 1, 3);
      void* ptr = cache.Allocate<OOMHandler>(size_class);
      blocks.emplace_back(std::make_pair(size_class, ptr));
    } else {
      // Deallocate an object for a class
      if (!blocks.empty()) {
        cache.Deallocate(blocks.back().second, blocks.back().first);
        blocks.pop_back();
      }
    }
  }

  // Cleaup. Deallocate rest of the allocated memory.
  for (int i = 0; i < blocks.size(); i++) {
    cache.Deallocate(blocks[i].second, blocks[i].first);
  }
}

TEST(CpuCacheTest, StealCpuCache) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  CPUCache cache;
  cache.Activate();

  std::vector<std::thread> threads;
  std::thread shuffle_thread;
  const int n_threads = absl::base_internal::NumCPUs();
  std::atomic<bool> stop(false);

  for (size_t t = 0; t < n_threads; ++t) {
    threads.push_back(
        std::thread(StressThread, std::ref(cache), t, std::ref(stop)));
  }
  shuffle_thread = std::thread(ShuffleThread, std::ref(cache), std::ref(stop));

  absl::SleepFor(absl::Seconds(5));
  stop = true;
  for (auto& t : threads) {
    t.join();
  }
  shuffle_thread.join();

  // Check that the total capacity is preserved after the shuffle.
  size_t capacity = 0;
  const int num_cpus = absl::base_internal::NumCPUs();
  const size_t kTotalCapacity = num_cpus * Parameters::max_per_cpu_cache_size();
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    EXPECT_EQ(cache.Allocated(cpu) + cache.Unallocated(cpu),
              cache.Capacity(cpu));
    capacity += cache.Capacity(cpu);
  }
  EXPECT_EQ(capacity, kTotalCapacity);

  cache.Deactivate();
}

// Runs a single allocate and deallocate operation to warm up the cache. Once a
// few objects are allocated in the cold cache, we can shuffle cpu caches to
// steal that capacity from the cold cache to the hot cache.
static void ColdCacheOperations(CPUCache& cache, int cpu_id,
                                size_t size_class) {
  // Temporarily fake being on the given CPU.
  ScopedFakeCpuId fake_cpu_id(cpu_id);

#if TCMALLOC_PERCPU_USE_RSEQ
  if (subtle::percpu::UsingFlatVirtualCpus()) {
    subtle::percpu::__rseq_abi.vcpu_id = cpu_id;
  }
#endif

  void* ptr = cache.Allocate<OOMHandler>(size_class);
  cache.Deallocate(ptr, size_class);
}

// Runs multiple allocate and deallocate operation on the cpu cache to collect
// misses. Once we collect enough misses on this cache, we can shuffle cpu
// caches to steal capacity from colder caches to the hot cache.
static void HotCacheOperations(CPUCache& cache, int cpu_id) {
  constexpr size_t kPtrs = 4096;
  std::vector<void*> ptrs;
  ptrs.resize(kPtrs);

  // Temporarily fake being on the given CPU.
  ScopedFakeCpuId fake_cpu_id(cpu_id);

#if TCMALLOC_PERCPU_USE_RSEQ
  if (subtle::percpu::UsingFlatVirtualCpus()) {
    subtle::percpu::__rseq_abi.vcpu_id = cpu_id;
  }
#endif

  // Allocate and deallocate objects to make sure we have enough misses on the
  // cache. This will make sure we have sufficient disparity in misses between
  // the hotter and colder cache, and that we may be able to steal bytes from
  // the colder cache.
  for (size_t size_class = 1; size_class <= 2; ++size_class) {
    for (auto& ptr : ptrs) {
      ptr = cache.Allocate<OOMHandler>(size_class);
    }
    for (void* ptr : ptrs) {
      cache.Deallocate(ptr, size_class);
    }
  }

  // We reclaim the cache to reset it so that we record underflows/overflows the
  // next time we allocate and deallocate objects. Without reclaim, the cache
  // would stay warmed up and it would take more time to drain the colder cache.
  cache.Reclaim(cpu_id);
}

TEST(CpuCacheTest, ColdHotCacheShuffleTest) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  CPUCache cache;
  cache.Activate();

  constexpr int hot_cpu_id = 0;
  constexpr int cold_cpu_id = 1;

  const size_t max_cpu_cache_size = Parameters::max_per_cpu_cache_size();

  // Empirical tests suggest that we should be able to steal all the steal-able
  // capacity from colder cache in < 100 tries. Keeping enough buffer here to
  // make sure we steal from colder cache, while at the same time avoid timeouts
  // if something goes bad.
  constexpr int kMaxStealTries = 1000;

  // We allocate and deallocate a single highest size_class object.
  // This makes sure that we have a single large object in the cache that faster
  // cache can steal.
  const size_t size_class = 2;

  for (int num_tries = 0;
       num_tries < kMaxStealTries &&
       cache.Capacity(cold_cpu_id) >
           CPUCache::kCacheCapacityThreshold * max_cpu_cache_size;
       ++num_tries) {
    ColdCacheOperations(cache, cold_cpu_id, size_class);
    HotCacheOperations(cache, hot_cpu_id);
    cache.ShuffleCpuCaches();

    // Check that the capacity is preserved.
    EXPECT_EQ(cache.Allocated(cold_cpu_id) + cache.Unallocated(cold_cpu_id),
              cache.Capacity(cold_cpu_id));
    EXPECT_EQ(cache.Allocated(hot_cpu_id) + cache.Unallocated(hot_cpu_id),
              cache.Capacity(hot_cpu_id));
  }

  size_t cold_cache_capacity = cache.Capacity(cold_cpu_id);
  size_t hot_cache_capacity = cache.Capacity(hot_cpu_id);

  // Check that we drained cold cache to the lower capacity limit.
  // We also keep some tolerance, up to the largest class size, below the lower
  // capacity threshold that we can drain cold cache to.
  EXPECT_GT(cold_cache_capacity,
            CPUCache::kCacheCapacityThreshold * max_cpu_cache_size -
                cache.forwarder().class_to_size(size_class));

  // Check that we have at least stolen some capacity.
  EXPECT_GT(hot_cache_capacity, max_cpu_cache_size);

  // Perform a few more shuffles to make sure that lower cache capacity limit
  // has been reached for the cold cache. A few more shuffles should not
  // change the capacity of either of the caches.
  for (int i = 0; i < 100; ++i) {
    ColdCacheOperations(cache, cold_cpu_id, size_class);
    HotCacheOperations(cache, hot_cpu_id);
    cache.ShuffleCpuCaches();

    // Check that the capacity is preserved.
    EXPECT_EQ(cache.Allocated(cold_cpu_id) + cache.Unallocated(cold_cpu_id),
              cache.Capacity(cold_cpu_id));
    EXPECT_EQ(cache.Allocated(hot_cpu_id) + cache.Unallocated(hot_cpu_id),
              cache.Capacity(hot_cpu_id));
  }

  // Check that the capacity of cold and hot caches is same as before.
  EXPECT_EQ(cache.Capacity(cold_cpu_id), cold_cache_capacity)
      << CPUCache::kCacheCapacityThreshold * max_cpu_cache_size;
  EXPECT_EQ(cache.Capacity(hot_cpu_id), hot_cache_capacity);

  // Make sure that the total capacity is preserved.
  EXPECT_EQ(cache.Capacity(cold_cpu_id) + cache.Capacity(hot_cpu_id),
            2 * max_cpu_cache_size);

  // Reclaim caches.
  cache.Deactivate();
}

TEST(CpuCacheTest, ReclaimCpuCache) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  CPUCache cache;
  cache.Activate();

  //  The number of underflows and overflows must be zero for all the caches.
  const int num_cpus = absl::base_internal::NumCPUs();
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    SCOPED_TRACE(absl::StrFormat("Failed CPU: %d", cpu));
    // Check that reclaim miss metrics are reset.
    CPUCache::CpuCacheMissStats reclaim_misses =
        cache.GetAndUpdateIntervalCacheMissStats(cpu, MissCount::kReclaim);
    EXPECT_EQ(reclaim_misses.underflows, 0);
    EXPECT_EQ(reclaim_misses.overflows, 0);

    // None of the caches should have been reclaimed yet.
    EXPECT_EQ(cache.GetNumReclaims(cpu), 0);

    // Check that caches are empty.
    uint64_t used_bytes = cache.UsedBytes(cpu);
    EXPECT_EQ(used_bytes, 0);
  }

  const size_t kSizeClass = 2;

  // We chose a different size class here so that we can populate different size
  // class slots and change the number of bytes used by the busy cache later in
  // our test.
  const size_t kBusySizeClass = 1;
  ASSERT_NE(kSizeClass, kBusySizeClass);

  // Perform some operations to warm up caches and make sure they are populated.
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    SCOPED_TRACE(absl::StrFormat("Failed CPU: %d", cpu));
    ColdCacheOperations(cache, cpu, kSizeClass);
    EXPECT_TRUE(cache.HasPopulated(cpu));
  }

  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    SCOPED_TRACE(absl::StrFormat("Failed CPU: %d", cpu));
    CPUCache::CpuCacheMissStats misses_last_interval =
        cache.GetAndUpdateIntervalCacheMissStats(cpu, MissCount::kReclaim);
    CPUCache::CpuCacheMissStats total_misses =
        cache.GetTotalCacheMissStats(cpu);

    // Misses since the last reclaim (i.e. since we initialized the caches)
    // should match the total miss metrics.
    EXPECT_EQ(misses_last_interval.underflows, total_misses.underflows);
    EXPECT_EQ(misses_last_interval.overflows, total_misses.overflows);

    // Caches should have non-zero used bytes.
    EXPECT_GT(cache.UsedBytes(cpu), 0);
  }

  cache.TryReclaimingCaches();

  // Miss metrics since the last interval were non-zero and the change in used
  // bytes was non-zero, so none of the caches should get reclaimed.
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    SCOPED_TRACE(absl::StrFormat("Failed CPU: %d", cpu));
    // As no cache operations were performed since the last reclaim
    // operation, the reclaim misses captured during the last interval (i.e.
    // since the last reclaim) should be zero.
    CPUCache::CpuCacheMissStats reclaim_misses =
        cache.GetAndUpdateIntervalCacheMissStats(cpu, MissCount::kReclaim);
    EXPECT_EQ(reclaim_misses.underflows, 0);
    EXPECT_EQ(reclaim_misses.overflows, 0);

    // None of the caches should have been reclaimed as the caches were
    // accessed in the previous interval.
    EXPECT_EQ(cache.GetNumReclaims(cpu), 0);

    // Caches should not have been reclaimed; used bytes should be non-zero.
    EXPECT_GT(cache.UsedBytes(cpu), 0);
  }

  absl::BitGen rnd;
  const int busy_cpu =
      absl::Uniform<int32_t>(rnd, 0, absl::base_internal::NumCPUs());
  const size_t prev_used = cache.UsedBytes(busy_cpu);
  ColdCacheOperations(cache, busy_cpu, kBusySizeClass);
  EXPECT_GT(cache.UsedBytes(busy_cpu), prev_used);

  // Try reclaiming caches again.
  cache.TryReclaimingCaches();

  // All caches, except the busy cpu cache against which we performed some
  // operations in the previous interval, should have been reclaimed exactly
  // once.
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    SCOPED_TRACE(absl::StrFormat("Failed CPU: %d", cpu));
    if (cpu == busy_cpu) {
      EXPECT_GT(cache.UsedBytes(cpu), 0);
      EXPECT_EQ(cache.GetNumReclaims(cpu), 0);
    } else {
      EXPECT_EQ(cache.UsedBytes(cpu), 0);
      EXPECT_EQ(cache.GetNumReclaims(cpu), 1);
    }
  }

  // Try reclaiming caches again.
  cache.TryReclaimingCaches();

  // All caches, including the busy cache, should have been reclaimed this
  // time. Note that the caches that were reclaimed in the previous interval
  // should not be reclaimed again and the number of reclaims reported for them
  // should still be one.
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    SCOPED_TRACE(absl::StrFormat("Failed CPU: %d", cpu));
    EXPECT_EQ(cache.UsedBytes(cpu), 0);
    EXPECT_EQ(cache.GetNumReclaims(cpu), 1);
  }

  cache.Deactivate();
}

TEST(CpuCacheTest, SizeClassCapacityTest) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  CPUCache cache;
  cache.Activate();

  const int num_cpus = absl::base_internal::NumCPUs();
  constexpr size_t kSizeClass = 2;
  const size_t batch_size = cache.forwarder().num_objects_to_move(kSizeClass);

  // Perform some operations to warm up caches and make sure they are populated.
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    SCOPED_TRACE(absl::StrFormat("Failed CPU: %d", cpu));
    ColdCacheOperations(cache, cpu, kSizeClass);
    EXPECT_TRUE(cache.HasPopulated(cpu));
  }

  for (int size_class = 0; size_class < kNumClasses; ++size_class) {
    SCOPED_TRACE(absl::StrFormat("Failed size_class: %d", size_class));
    CPUCache::SizeClassCapacityStats capacity_stats =
        cache.GetSizeClassCapacityStats(size_class);
    if (size_class == kSizeClass) {
      // As all the caches are populated and each cache stores batch_size number
      // of kSizeClass objects, all the stats below should be equal to
      // batch_size.
      EXPECT_EQ(capacity_stats.min_capacity, batch_size);
      EXPECT_DOUBLE_EQ(capacity_stats.avg_capacity, batch_size);
      EXPECT_EQ(capacity_stats.max_capacity, batch_size);
    } else {
      // Capacity stats for other size classes should be zero.
      EXPECT_EQ(capacity_stats.min_capacity, 0);
      EXPECT_DOUBLE_EQ(capacity_stats.avg_capacity, 0);
      EXPECT_EQ(capacity_stats.max_capacity, 0);
    }
  }

  // Next, we reclaim per-cpu caches, one at a time, to drain all the kSizeClass
  // objects cached by them. As we progressively reclaim per-cpu caches, the
  // capacity for kSizeClass averaged over all CPUs should also drop linearly.
  // We reclaim all but one per-cpu caches (we reclaim last per-cpu cache
  // outside the loop so that we can check for max_capacity=0 separately).
  for (int cpu = 0; cpu < num_cpus - 1; ++cpu) {
    SCOPED_TRACE(absl::StrFormat("Failed CPU: %d", cpu));
    cache.Reclaim(cpu);

    CPUCache::SizeClassCapacityStats capacity_stats =
        cache.GetSizeClassCapacityStats(kSizeClass);
    // Reclaiming even one per-cpu cache should set min_capacity to zero.
    EXPECT_EQ(capacity_stats.min_capacity, 0);

    // (cpu+1) number of caches have been reclaimed. So, (num_cpus-cpu-1) number
    // of caches are currently populated, with each cache storing batch_size
    // number of kSizeClass objects.
    double expected_avg =
        static_cast<double>(batch_size * (num_cpus - cpu - 1)) / num_cpus;
    EXPECT_DOUBLE_EQ(capacity_stats.avg_capacity, expected_avg);

    // At least one per-cpu cache exists that caches batch_size number of
    // kSizeClass objects.
    EXPECT_EQ(capacity_stats.max_capacity, batch_size);
  }

  // We finally reclaim last per-cpu cache. All the reported capacity stats
  // should drop to zero as none of the caches hold any objects.
  cache.Reclaim(num_cpus - 1);
  CPUCache::SizeClassCapacityStats capacity_stats =
      cache.GetSizeClassCapacityStats(kSizeClass);
  EXPECT_EQ(capacity_stats.min_capacity, 0);
  EXPECT_DOUBLE_EQ(capacity_stats.avg_capacity, 0);
  EXPECT_EQ(capacity_stats.max_capacity, 0);

  cache.Deactivate();
}

class CpuCacheEnvironment {
 public:
  CpuCacheEnvironment() : num_cpus_(absl::base_internal::NumCPUs()) {}
  ~CpuCacheEnvironment() { cache_.Deactivate(); }

  void Activate() {
    cache_.Activate();
    ready_.store(true, std::memory_order_release);
  }

  void RandomlyPoke(absl::BitGenRef rng) {
    // We run a random operation based on our random number generated.
    const int coin = absl::Uniform(rng, 0, 18);
    const bool ready = ready_.load(std::memory_order_acquire);

    // Pick a random CPU and size class.  We will likely need one or both.
    const int cpu = absl::Uniform(rng, 0, num_cpus_);
    const int size_class = absl::Uniform(rng, 1, 3);

    if (!ready || coin < 1) {
      benchmark::DoNotOptimize(cache_.CacheLimit());
      return;
    }

    // Methods beyond this point require the CPUCache to be activated.

    switch (coin) {
      case 1: {
        // Allocate, Deallocate
        void* ptr = cache_.Allocate<OOMHandler>(size_class);
        EXPECT_NE(ptr, nullptr);
        // Touch *ptr to allow sanitizers to see an access (and a potential
        // race, if synchronization is insufficient).
        *static_cast<char*>(ptr) = 1;
        benchmark::DoNotOptimize(*static_cast<char*>(ptr));

        cache_.Deallocate(ptr, size_class);
        break;
      }
      case 2:
        benchmark::DoNotOptimize(cache_.TotalUsedBytes());
        break;
      case 3:
        benchmark::DoNotOptimize(cache_.UsedBytes(cpu));
        break;
      case 4:
        benchmark::DoNotOptimize(cache_.Allocated(cpu));
        break;
      case 5:
        benchmark::DoNotOptimize(cache_.HasPopulated(cpu));
        break;
      case 6: {
        auto metadata = cache_.MetadataMemoryUsage();
        EXPECT_GE(metadata.virtual_size, metadata.resident_size);
        EXPECT_GT(metadata.virtual_size, 0);
        break;
      }
      case 7:
        benchmark::DoNotOptimize(cache_.TotalObjectsOfClass(size_class));
        break;
      case 8:
        benchmark::DoNotOptimize(cache_.Unallocated(cpu));
        break;
      case 9:
        benchmark::DoNotOptimize(cache_.Capacity(cpu));
        break;
      case 10:
        cache_.ShuffleCpuCaches();
        break;
      case 11:
        cache_.TryReclaimingCaches();
        break;
      case 12:
        cache_.Reclaim(cpu);
        break;
      case 13:
        benchmark::DoNotOptimize(cache_.GetNumReclaims(cpu));
        break;
      case 14: {
        const auto total_misses = cache_.GetTotalCacheMissStats(cpu);
        const auto reclaim_misses =
            cache_.GetAndUpdateIntervalCacheMissStats(cpu, MissCount::kReclaim);
        const auto shuffle_misses =
            cache_.GetIntervalCacheMissStats(cpu, MissCount::kShuffle);

        benchmark::DoNotOptimize(total_misses);
        benchmark::DoNotOptimize(reclaim_misses);
        benchmark::DoNotOptimize(shuffle_misses);
        break;
      }
      case 15: {
        const auto stats = cache_.GetSizeClassCapacityStats(size_class);
        EXPECT_GE(stats.max_capacity, stats.avg_capacity);
        EXPECT_GE(stats.avg_capacity, stats.min_capacity);
        break;
      }
      case 16: {
        std::string out;
        out.resize(128 << 10);
        ANNOTATE_MEMORY_IS_UNINITIALIZED(out.data(), out.size());
        Printer p(out.data(), out.size());
        PbtxtRegion r(&p, kTop);

        cache_.PrintInPbtxt(&r);

        benchmark::DoNotOptimize(out.data());
        break;
      }
      case 17: {
        std::string out;
        out.resize(128 << 10);
        ANNOTATE_MEMORY_IS_UNINITIALIZED(out.data(), out.size());
        Printer p(out.data(), out.size());

        cache_.Print(&p);

        benchmark::DoNotOptimize(out.data());
        break;
      }
      default:
        GTEST_FAIL() << "Unexpected value " << coin;
        break;
    }
  }

  CPUCache& cache() { return cache_; }

  int num_cpus() const { return num_cpus_; }

 private:
  const int num_cpus_;
  CPUCache cache_;
  std::atomic<bool> ready_{false};
};

TEST(CpuCacheTest, Fuzz) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  const int kThreads = 10;
  struct ABSL_CACHELINE_ALIGNED ThreadState {
    absl::BitGen rng;
  };
  std::vector<ThreadState> thread_state(kThreads);

  CpuCacheEnvironment env;
  ThreadManager threads;
  threads.Start(10, [&](int thread_id) {
    // Ensure this thread has registered itself with the kernel to use
    // restartable sequences.
    CHECK_CONDITION(subtle::percpu::IsFast());
    env.RandomlyPoke(thread_state[thread_id].rng);
  });

  absl::SleepFor(absl::Seconds(0.1));
  env.Activate();
  absl::SleepFor(absl::Seconds(0.3));

  threads.Stop();

  // Inspect the CPUCache and validate invariants.

  // The number of caches * per-core limit should be equivalent to the bytes
  // managed by the cache.
  size_t capacity = 0;
  size_t allocated = 0;
  size_t unallocated = 0;
  for (int i = 0, n = env.num_cpus(); i < n; i++) {
    capacity += env.cache().Capacity(i);
    allocated += env.cache().Allocated(i);
    unallocated += env.cache().Unallocated(i);
  }

  EXPECT_EQ(allocated + unallocated, capacity);
  EXPECT_EQ(env.num_cpus() * env.cache().CacheLimit(), capacity);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
