// Copyright 2020 The TCMalloc Authors
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

#include "tcmalloc/transfer_cache.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/internal/spinlock.h"
#include "absl/random/distributions.h"
#include "absl/random/random.h"
#include "absl/time/clock.h"
#include "absl/types/span.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/mock_central_freelist.h"
#include "tcmalloc/mock_transfer_cache.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/thread_manager.h"
#include "tcmalloc/transfer_cache_internals.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

using ::testing::AtLeast;
using ::testing::Return;

template <typename Env>
using TransferCacheTest = ::testing::Test;
TYPED_TEST_SUITE_P(TransferCacheTest);

TYPED_TEST_P(TransferCacheTest, IsolatedSmoke) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam e;
  EXPECT_CALL(e.central_freelist(), InsertRange)
      .Times(e.transfer_cache().IsFlexible() ? 0 : 1);
  EXPECT_CALL(e.central_freelist(), RemoveRange)
      .Times(e.transfer_cache().IsFlexible() ? 0 : 1);

  TransferCacheStats stats = e.transfer_cache().GetStats();
  EXPECT_EQ(stats.insert_hits, 0);
  EXPECT_EQ(stats.insert_misses, 0);
  EXPECT_EQ(stats.insert_non_batch_misses, 0);
  EXPECT_EQ(stats.remove_hits, 0);
  EXPECT_EQ(stats.remove_misses, 0);
  EXPECT_EQ(stats.remove_non_batch_misses, 0);
  EXPECT_EQ(stats.used, 0);

  int capacity = e.transfer_cache().CapacityNeeded(kSizeClass).capacity;
  EXPECT_EQ(stats.capacity, capacity);
  EXPECT_EQ(stats.max_capacity, e.transfer_cache().max_capacity());

  e.Insert(batch_size);
  stats = e.transfer_cache().GetStats();
  EXPECT_EQ(stats.insert_hits, 1);
  int used_expected = batch_size;
  EXPECT_EQ(stats.used, used_expected);
  EXPECT_EQ(stats.capacity, capacity);

  e.Insert(batch_size);
  stats = e.transfer_cache().GetStats();
  EXPECT_EQ(stats.insert_hits, 2);
  used_expected += batch_size;
  EXPECT_EQ(stats.used, used_expected);
  EXPECT_EQ(stats.capacity, capacity);

  e.Insert(batch_size - 1);
  stats = e.transfer_cache().GetStats();
  if (e.transfer_cache().IsFlexible()) {
    EXPECT_EQ(stats.insert_hits, 3);
    EXPECT_EQ(stats.insert_misses, 0);
    EXPECT_EQ(stats.insert_non_batch_misses, 0);
    used_expected += batch_size - 1;
    EXPECT_EQ(stats.used, used_expected);
  } else {
    EXPECT_EQ(stats.insert_hits, 2);
    EXPECT_EQ(stats.insert_misses, 1);
    EXPECT_EQ(stats.insert_non_batch_misses, 1);
    EXPECT_EQ(stats.used, used_expected);
  }
  EXPECT_EQ(stats.capacity, capacity);
  EXPECT_LE(capacity, e.transfer_cache().max_capacity());

  e.Remove(batch_size);
  stats = e.transfer_cache().GetStats();
  EXPECT_EQ(stats.remove_hits, 1);
  used_expected -= batch_size;
  EXPECT_EQ(stats.used, used_expected);
  EXPECT_EQ(stats.capacity, capacity);

  e.Remove(batch_size);
  stats = e.transfer_cache().GetStats();
  EXPECT_EQ(stats.remove_hits, 2);
  used_expected -= batch_size;
  EXPECT_EQ(stats.used, used_expected);
  EXPECT_EQ(stats.capacity, capacity);

  e.Remove(batch_size - 1);
  stats = e.transfer_cache().GetStats();
  if (e.transfer_cache().IsFlexible()) {
    EXPECT_EQ(stats.remove_hits, 3);
    EXPECT_EQ(stats.remove_misses, 0);
    EXPECT_EQ(stats.remove_non_batch_misses, 0);
    used_expected -= (batch_size - 1);
    EXPECT_EQ(stats.used, used_expected);
  } else {
    EXPECT_EQ(stats.remove_hits, 2);
    EXPECT_EQ(stats.remove_misses, 1);
    EXPECT_EQ(stats.remove_non_batch_misses, 1);
    EXPECT_EQ(stats.used, used_expected);
  }
  EXPECT_EQ(stats.capacity, capacity);
  EXPECT_EQ(stats.max_capacity, e.transfer_cache().max_capacity());
}

TYPED_TEST_P(TransferCacheTest, ReadStats) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam e;
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);
  EXPECT_CALL(e.central_freelist(), RemoveRange).Times(0);

  // Ensure there is at least one insert hit/remove hit, so we can assert a
  // non-tautology in t2.
  e.Insert(batch_size);
  e.Remove(batch_size);

  TransferCacheStats stats = e.transfer_cache().GetStats();
  ASSERT_EQ(stats.insert_hits, 1);
  ASSERT_EQ(stats.insert_misses, 0);
  ASSERT_EQ(stats.insert_non_batch_misses, 0);
  ASSERT_EQ(stats.remove_hits, 1);
  ASSERT_EQ(stats.remove_misses, 0);
  ASSERT_EQ(stats.remove_non_batch_misses, 0);

  std::atomic<bool> stop{false};

  std::thread t1([&]() {
    while (!stop.load(std::memory_order_acquire)) {
      e.Insert(batch_size);
      e.Remove(batch_size);
    }
  });

  std::thread t2([&]() {
    while (!stop.load(std::memory_order_acquire)) {
      TransferCacheStats stats = e.transfer_cache().GetStats();
      CHECK_CONDITION(stats.insert_hits >= 1);
      CHECK_CONDITION(stats.insert_misses == 0);
      CHECK_CONDITION(stats.insert_non_batch_misses == 0);
      CHECK_CONDITION(stats.remove_hits >= 1);
      CHECK_CONDITION(stats.remove_misses == 0);
      CHECK_CONDITION(stats.remove_non_batch_misses == 0);
    }
  });

  absl::SleepFor(absl::Seconds(1));
  stop.store(true, std::memory_order_release);

  t1.join();
  t2.join();
}

TYPED_TEST_P(TransferCacheTest, SingleItemSmoke) {
  const int batch_size = TypeParam::kBatchSize;
  if (batch_size == 1) {
    GTEST_SKIP() << "skipping trivial batch size";
  }
  TypeParam e;
  const int actions = e.transfer_cache().IsFlexible() ? 2 : 0;
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(2 - actions);
  EXPECT_CALL(e.central_freelist(), RemoveRange).Times(2 - actions);

  e.Insert(1);
  e.Insert(1);
  EXPECT_EQ(e.transfer_cache().GetStats().insert_hits, actions);
  e.Remove(1);
  e.Remove(1);
  EXPECT_EQ(e.transfer_cache().GetStats().remove_hits, actions);
}

TYPED_TEST_P(TransferCacheTest, FetchesFromFreelist) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam e;
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);
  EXPECT_CALL(e.central_freelist(), RemoveRange).Times(1);
  e.Remove(batch_size);
  EXPECT_EQ(e.transfer_cache().GetStats().remove_misses, 1);
}

TYPED_TEST_P(TransferCacheTest, PartialFetchFromFreelist) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam e;
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);
  EXPECT_CALL(e.central_freelist(), RemoveRange)
      .Times(2)
      .WillOnce([&](void** batch, int n) {
        int returned = static_cast<FakeCentralFreeList&>(e.central_freelist())
                           .RemoveRange(batch, std::min(batch_size / 2, n));
        // Overwrite the elements of batch that were not populated by
        // RemoveRange.
        memset(batch + returned, 0x3f, sizeof(*batch) * (n - returned));
        return returned;
      });
  e.Remove(batch_size);
  EXPECT_EQ(e.transfer_cache().GetStats().remove_misses, 2);
}

TYPED_TEST_P(TransferCacheTest, PushesToFreelist) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam e;

  EXPECT_CALL(e.central_freelist(), InsertRange).Times(1);

  while (e.transfer_cache().HasSpareCapacity(kSizeClass)) {
    e.Insert(batch_size);
  }
  size_t old_hits = e.transfer_cache().GetStats().insert_hits;
  e.Insert(batch_size);
  EXPECT_EQ(e.transfer_cache().GetStats().insert_hits, old_hits);
  EXPECT_EQ(e.transfer_cache().GetStats().insert_misses, 1);
}

TYPED_TEST_P(TransferCacheTest, WrappingWorks) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam env;

  EXPECT_CALL(env.central_freelist(), InsertRange).Times(0);
  EXPECT_CALL(env.central_freelist(), RemoveRange).Times(0);

  while (env.transfer_cache().HasSpareCapacity(kSizeClass)) {
    env.Insert(batch_size);
  }
  for (int i = 0; i < 100; ++i) {
    env.Remove(batch_size);
    env.Insert(batch_size);
  }
  if (env.transfer_cache().IsFlexible()) {
    for (size_t size = 1; size < batch_size + 2; size++) {
      env.Remove(size);
      env.Insert(size);
    }
  }
}

TYPED_TEST_P(TransferCacheTest, Plunder) {
  TypeParam env;
  if (!std::is_same<typename TypeParam::TransferCache,
                    internal_transfer_cache::RingBufferTransferCache<
                        typename TypeParam::FreeList,
                        typename TypeParam::Manager>>::value) {
    GTEST_SKIP() << "Skipping ring-specific test.";
  }

  // Fill in some elements.
  env.Insert(TypeParam::kBatchSize);
  env.Insert(TypeParam::kBatchSize);
  ASSERT_EQ(env.transfer_cache().tc_length(), 2 * TypeParam::kBatchSize);
  // Previously the transfer cache was empty, so we're not plundering yet.
  env.transfer_cache().TryPlunder(kSizeClass);
  ASSERT_EQ(env.transfer_cache().tc_length(), 2 * TypeParam::kBatchSize);
  // But now all these elements will be plundered.
  env.transfer_cache().TryPlunder(kSizeClass);
  ASSERT_EQ(env.transfer_cache().tc_length(), 0);

  // Stock up again.
  env.Insert(TypeParam::kBatchSize);
  env.Insert(TypeParam::kBatchSize);
  ASSERT_EQ(env.transfer_cache().tc_length(), 2 * TypeParam::kBatchSize);
  // Plundering doesn't do anything as the cache was empty after the last
  // plunder call.
  env.transfer_cache().TryPlunder(kSizeClass);
  ASSERT_EQ(env.transfer_cache().tc_length(), 2 * TypeParam::kBatchSize);

  void* buf[TypeParam::kBatchSize];
  // -1 +1, this sets the low_water_mark (the lowest end-state after a
  // call to RemoveRange to 1 batch.
  (void)env.transfer_cache().RemoveRange(kSizeClass, buf,
                                         TypeParam::kBatchSize);
  ASSERT_EQ(env.transfer_cache().tc_length(), TypeParam::kBatchSize);
  env.transfer_cache().InsertRange(kSizeClass, {buf, TypeParam::kBatchSize});
  ASSERT_EQ(env.transfer_cache().tc_length(), 2 * TypeParam::kBatchSize);
  // We have one batch, and this is the same as the low water mark, so nothing
  // gets plundered.
  env.transfer_cache().TryPlunder(kSizeClass);
  ASSERT_EQ(env.transfer_cache().tc_length(), TypeParam::kBatchSize);
  // If we plunder immediately the low_water_mark is at the current size
  // gets plundered.
  env.transfer_cache().TryPlunder(kSizeClass);
  ASSERT_EQ(env.transfer_cache().tc_length(), 0);

  // Fill it up completely.
  while (env.transfer_cache().HasSpareCapacity(kSizeClass)) {
    env.Insert(TypeParam::kBatchSize);
  }
  // low water mark should still be zero, so no plundering.
  env.transfer_cache().TryPlunder(kSizeClass);
  ASSERT_EQ(env.transfer_cache().tc_length(),
            env.transfer_cache().GetStats().capacity);

  // Inserting a one-element batch means we return one batch to the freelist and
  // then insert only one element after. I.e. the cache size should shrink.
  env.Insert(1);
  ASSERT_EQ(
      env.transfer_cache().tc_length(),
      env.transfer_cache().GetStats().capacity - TypeParam::kBatchSize + 1);
  // If we fill up the cache again, plundering should respect that.
  env.Insert(TypeParam::kBatchSize - 1);
  ASSERT_EQ(env.transfer_cache().tc_length(),
            env.transfer_cache().GetStats().capacity);
  env.transfer_cache().TryPlunder(kSizeClass);
  ASSERT_EQ(env.transfer_cache().tc_length(), TypeParam::kBatchSize - 1);
}

// PickCoprimeBatchSize picks a batch size in [2, max_batch_size) that is
// coprime with 2^32.  We choose the largest possible batch size within that
// constraint to minimize the number of iterations of insert/remove required.
static size_t PickCoprimeBatchSize(size_t max_batch_size) {
  while (max_batch_size > 1) {
    if ((size_t{1} << 32) % max_batch_size != 0) {
      return max_batch_size;
    }
    max_batch_size--;
  }

  return max_batch_size;
}

TEST(RingBufferTest, b172283201) {
  // This test is designed to exercise the wraparound behavior for the
  // RingBufferTransferCache, which manages its indices in uint32_t's.  Because
  // it uses a non-standard batch size (kBatchSize) as part of
  // PickCoprimeBatchSize, it triggers a TransferCache miss to the
  // CentralFreeList, which is uninteresting for exercising b/172283201.

  // For performance reasons, limit to optimized builds.
#if !defined(NDEBUG)
  GTEST_SKIP() << "skipping long running test on debug build";
#elif defined(THREAD_SANITIZER)
  // This test is single threaded, so thread sanitizer will not be useful.
  GTEST_SKIP() << "skipping under thread sanitizer, which slows test execution";
#endif

  using EnvType = FakeTransferCacheEnvironment<
      internal_transfer_cache::RingBufferTransferCache<
          MockCentralFreeList, FakeTransferCacheManager>>;
  EnvType env;

  // We pick the largest value <= EnvType::kBatchSize to use as a batch size,
  // such that it is prime relative to 2^32.  This ensures that when we
  // encounter a wraparound, the last operation actually spans both ends of the
  // buffer.
  const size_t batch_size = PickCoprimeBatchSize(EnvType::kBatchSize);
  ASSERT_GT(batch_size, 0);
  ASSERT_NE((size_t{1} << 32) % batch_size, 0) << batch_size;
  // For ease of comparison, allocate a buffer of char's.  We will use these to
  // generate unique addresses.  Since we assert that we will never miss in the
  // TransferCache and go to the CentralFreeList, these do not need to be valid
  // objects for deallocation.
  std::vector<char> buffer(batch_size);
  std::vector<void*> pointers;
  pointers.reserve(batch_size);
  for (size_t i = 0; i < batch_size; i++) {
    pointers.push_back(&buffer[i]);
  }

  // To produce wraparound in the RingBufferTransferCache, we fill up the cache
  // completely and then keep inserting new elements. This makes the cache
  // return old elements to the freelist and eventually wrap around.
  EXPECT_CALL(env.central_freelist(), RemoveRange).Times(0);
  // We do return items to the freelist, don't try to actually free them.
  ON_CALL(env.central_freelist(), InsertRange).WillByDefault(testing::Return());

  // First fill up the cache to its capacity.
  while (env.transfer_cache().HasSpareCapacity(kSizeClass)) {
    env.transfer_cache().InsertRange(kSizeClass, absl::MakeSpan(pointers));
  }

  // The current size of the transfer cache is close to its capacity. Insert
  // enough batches to make sure we wrap around twice (1 batch size should wrap
  // around as we are full currently, then insert the same amount of items
  // again, then one more wrap around).
  const size_t kObjects = env.transfer_cache().tc_length() + 2 * batch_size;

  // From now on, calls to InsertRange() should result in a corresponding call
  // to the freelist whenever the cache is full. This doesn't happen on every
  // call, as we return up to num_to_move (i.e. kBatchSize) items to the free
  // list in one batch.
  EXPECT_CALL(env.central_freelist(),
              InsertRange(testing::SizeIs(EnvType::kBatchSize)))
      .Times(testing::AnyNumber());
  for (size_t i = 0; i < kObjects; i += batch_size) {
    env.transfer_cache().InsertRange(kSizeClass, absl::MakeSpan(pointers));
  }
  // Manually drain the items in the transfercache, otherwise the destructor
  // will try to free them.
  std::vector<void*> to_free(batch_size);
  size_t N = env.transfer_cache().tc_length();
  while (N > 0) {
    const size_t to_remove = std::min(N, batch_size);
    const size_t removed =
        env.transfer_cache().RemoveRange(kSizeClass, to_free.data(), to_remove);
    ASSERT_THAT(removed, testing::Le(to_remove));
    ASSERT_THAT(removed, testing::Gt(0));
    N -= removed;
  }
  ASSERT_EQ(env.transfer_cache().tc_length(), 0);
}

TEST(FlexibleTransferCacheTest, ToggleCacheFlexibility) {
  // In this test, we toggle flexibility of the legacy transfer cache to make
  // sure that we encounter expected number of misses and do not lose capacity
  // in the process.
  using EnvType = FakeFlexibleTransferCacheEnvironment<
      internal_transfer_cache::TransferCache<MockCentralFreeList,
                                             FakeTransferCacheManager>>;
  EnvType env;

  // Make sure that flexibility is enabled by default in this environment.
  ASSERT_EQ(env.transfer_cache().IsFlexible(), true);

  const int batch_size = EnvType::kBatchSize;
  while (env.transfer_cache().HasSpareCapacity(kSizeClass)) {
    env.Insert(batch_size);
  }

  EXPECT_CALL(env.central_freelist(), InsertRange).Times(batch_size / 2);
  EXPECT_CALL(env.central_freelist(), RemoveRange).Times(batch_size / 2);

  bool flexible = env.transfer_cache().IsFlexible();
  TransferCacheStats previous_stats = env.transfer_cache().GetStats();
  for (size_t size = 1; size <= batch_size; size++) {
    flexible = !flexible;
    env.transfer_cache_manager().SetPartialLegacyTransferCache(flexible);
    ASSERT_EQ(env.transfer_cache().IsFlexible(), flexible);

    env.Remove(size);
    env.Insert(size);
  }

  // Make sure we haven't lost capacity and the number of used bytes in the
  // cache is same as before.
  TransferCacheStats current_stats = env.transfer_cache().GetStats();
  EXPECT_EQ(current_stats.used, previous_stats.used);
  EXPECT_EQ(current_stats.capacity, previous_stats.capacity);
  EXPECT_EQ(current_stats.max_capacity, previous_stats.max_capacity);

  // We should have missed half the number of times when the transfer cache did
  // not allow partial updates.
  EXPECT_EQ(current_stats.insert_misses, batch_size / 2);
  EXPECT_EQ(current_stats.insert_non_batch_misses, batch_size / 2);

  // Enable flexibility.
  flexible = true;
  env.transfer_cache_manager().SetPartialLegacyTransferCache(flexible);

  // When flexibility is enabled, we should be able to drain the transfer cache
  // one object at a time.
  int used = current_stats.used;
  while (used > 0) {
    env.Remove(1);
    --used;
    EXPECT_EQ(env.transfer_cache().tc_length(), used);
  }
  EXPECT_EQ(env.transfer_cache().tc_length(), 0);
}

REGISTER_TYPED_TEST_SUITE_P(TransferCacheTest, IsolatedSmoke, ReadStats,
                            FetchesFromFreelist, PartialFetchFromFreelist,
                            PushesToFreelist, WrappingWorks, SingleItemSmoke,
                            Plunder);

template <typename Env>
using FuzzTest = ::testing::Test;
TYPED_TEST_SUITE_P(FuzzTest);

TYPED_TEST_P(FuzzTest, MultiThreadedUnbiased) {
  TypeParam env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(0.3) > absl::Now()) env.RandomlyPoke();
  threads.Stop();
}

TYPED_TEST_P(FuzzTest, MultiThreadedBiasedInsert) {
  const int batch_size = TypeParam::kBatchSize;

  TypeParam env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) env.Insert(batch_size);
  threads.Stop();
}

TYPED_TEST_P(FuzzTest, MultiThreadedBiasedRemove) {
  const int batch_size = TypeParam::kBatchSize;

  TypeParam env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) env.Remove(batch_size);
  threads.Stop();
}

TYPED_TEST_P(FuzzTest, MultiThreadedBiasedShrink) {
  TypeParam env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) env.Shrink();
  threads.Stop();
}

TYPED_TEST_P(FuzzTest, MultiThreadedBiasedGrow) {
  TypeParam env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) env.Grow();
  threads.Stop();
}

REGISTER_TYPED_TEST_SUITE_P(FuzzTest, MultiThreadedUnbiased,
                            MultiThreadedBiasedInsert,
                            MultiThreadedBiasedRemove, MultiThreadedBiasedGrow,
                            MultiThreadedBiasedShrink);

TEST(ShardedTransferCacheManagerTest, DefaultConstructorDisables) {
  ShardedTransferCacheManager manager(nullptr, nullptr);
  for (int size_class = 0; size_class < kNumClasses; ++size_class) {
    EXPECT_FALSE(manager.should_use(size_class));
  }
}

TEST(ShardedTransferCacheManagerTest, ShardsOnDemand) {
  FakeShardedTransferCacheEnvironment env;
  FakeShardedTransferCacheEnvironment::ShardedManager& manager =
      env.sharded_manager();

  EXPECT_FALSE(manager.shard_initialized(0));
  EXPECT_FALSE(manager.shard_initialized(1));

  size_t metadata = env.MetadataAllocated();
  // We already allocated some data for the sharded transfer cache.
  EXPECT_GT(metadata, 0);

  // Push something onto cpu 0/shard0.
  {
    void* ptr;
    env.central_freelist().AllocateBatch(&ptr, 1);
    env.SetCurrentCpu(0);
    manager.Push(kSizeClass, ptr);
    EXPECT_TRUE(manager.shard_initialized(0));
    EXPECT_EQ(manager.tc_length(0, kSizeClass), 1);
    EXPECT_FALSE(manager.shard_initialized(1));
    EXPECT_GT(env.MetadataAllocated(), metadata);
    metadata = env.MetadataAllocated();
  }

  // Popping again should work, but not deinitialize the shard.
  {
    void* ptr = manager.Pop(kSizeClass);
    ASSERT_NE(ptr, nullptr);
    env.central_freelist().FreeBatch({&ptr, 1});
    EXPECT_TRUE(manager.shard_initialized(0));
    EXPECT_EQ(manager.tc_length(0, kSizeClass), 0);
    EXPECT_FALSE(manager.shard_initialized(1));
    EXPECT_EQ(env.MetadataAllocated(), metadata);
  }

  // Push something onto cpu 1, also shard 0.
  {
    void* ptr;
    env.central_freelist().AllocateBatch(&ptr, 1);
    env.SetCurrentCpu(1);
    manager.Push(kSizeClass, ptr);
    EXPECT_TRUE(manager.shard_initialized(0));
    EXPECT_EQ(manager.tc_length(1, kSizeClass), 1);
    EXPECT_FALSE(manager.shard_initialized(1));
    // No new metadata allocated
    EXPECT_EQ(env.MetadataAllocated(), metadata);
  }

  // Push something onto cpu 2/shard 1.
  {
    void* ptr;
    env.central_freelist().AllocateBatch(&ptr, 1);
    env.SetCurrentCpu(2);
    manager.Push(kSizeClass, ptr);
    EXPECT_TRUE(manager.shard_initialized(0));
    EXPECT_TRUE(manager.shard_initialized(1));
    EXPECT_EQ(manager.tc_length(2, kSizeClass), 1);
    EXPECT_GT(env.MetadataAllocated(), metadata);
  }
}

namespace unit_tests {
using Env = FakeTransferCacheEnvironment<internal_transfer_cache::TransferCache<
    MockCentralFreeList, FakeTransferCacheManager>>;
INSTANTIATE_TYPED_TEST_SUITE_P(TransferCache, TransferCacheTest,
                               ::testing::Types<Env>);

using FlexibleTransferCacheEnv =
    FakeFlexibleTransferCacheEnvironment<internal_transfer_cache::TransferCache<
        MockCentralFreeList, FakeTransferCacheManager>>;
INSTANTIATE_TYPED_TEST_SUITE_P(FlexibleTransferCache, TransferCacheTest,
                               ::testing::Types<FlexibleTransferCacheEnv>);

using RingBufferEnv = FakeTransferCacheEnvironment<
    internal_transfer_cache::RingBufferTransferCache<MockCentralFreeList,
                                                     FakeTransferCacheManager>>;
INSTANTIATE_TYPED_TEST_SUITE_P(RingBuffer, TransferCacheTest,
                               ::testing::Types<RingBufferEnv>);

}  // namespace unit_tests

namespace fuzz_tests {
// Use the FakeCentralFreeList instead of the MockCentralFreeList for fuzz tests
// as it avoids the overheads of mocks and allows more iterations of the fuzzing
// itself.
using Env = FakeTransferCacheEnvironment<internal_transfer_cache::TransferCache<
    MockCentralFreeList, FakeTransferCacheManager>>;
INSTANTIATE_TYPED_TEST_SUITE_P(TransferCache, FuzzTest, ::testing::Types<Env>);

using FlexibleTransferCacheEnv =
    FakeFlexibleTransferCacheEnvironment<internal_transfer_cache::TransferCache<
        MockCentralFreeList, FakeTransferCacheManager>>;

INSTANTIATE_TYPED_TEST_SUITE_P(FlexibleTransferCache, TransferCacheTest,
                               ::testing::Types<FlexibleTransferCacheEnv>);

using RingBufferEnv = FakeTransferCacheEnvironment<
    internal_transfer_cache::RingBufferTransferCache<MockCentralFreeList,
                                                     FakeTransferCacheManager>>;
INSTANTIATE_TYPED_TEST_SUITE_P(RingBuffer, FuzzTest,
                               ::testing::Types<RingBufferEnv>);
}  // namespace fuzz_tests

namespace resize_tests {
struct MockTransferCacheManager {
  static constexpr size_t kNumClasses = 6;
  static constexpr size_t kMaxSizeClassesToResize = 2;

  MOCK_METHOD(bool, ShrinkCache, (int size_class));
  MOCK_METHOD(bool, CanIncreaseCapacity, (int size_class));
  MOCK_METHOD(bool, IncreaseCacheCapacity, (int size_class));
  MOCK_METHOD(size_t, FetchCommitIntervalMisses, (int size_class));
};

TEST(RealTransferCacheTest, ResizeOccurs) {
  testing::StrictMock<MockTransferCacheManager> m;
  {
    testing::InSequence seq;
    EXPECT_CALL(m, FetchCommitIntervalMisses)
        .Times(6)
        .WillRepeatedly([](int size_class) { return size_class + 1; });
    EXPECT_CALL(m, CanIncreaseCapacity(5)).WillOnce(Return(true));
    EXPECT_CALL(m, ShrinkCache(0)).WillOnce(Return(true));
    EXPECT_CALL(m, IncreaseCacheCapacity(5)).WillOnce(Return(true));

    EXPECT_CALL(m, CanIncreaseCapacity(4)).WillOnce(Return(false));
    EXPECT_CALL(m, CanIncreaseCapacity(3)).WillOnce(Return(true));
    EXPECT_CALL(m, ShrinkCache(1)).WillOnce(Return(false));
    EXPECT_CALL(m, ShrinkCache(2)).WillOnce(Return(true));
    EXPECT_CALL(m, IncreaseCacheCapacity(3)).WillOnce(Return(true));
  }
  internal_transfer_cache::TryResizingCaches(m);
  testing::Mock::VerifyAndClear(&m);
}

template <typename Env>
using RealTransferCacheTest = ::testing::Test;
TYPED_TEST_SUITE_P(RealTransferCacheTest);

TYPED_TEST_P(RealTransferCacheTest, StressResize) {
  TypeParam env;

  // Count capacity (measured in batches) currently allowed in the cache.
  auto count_batches = [&env]() {
    int batch_count = 0;
    for (int size_class = 0; size_class < kNumClasses; ++size_class) {
      const size_t batch_size =
          env.transfer_cache_manager().num_objects_to_move(size_class);
      const int capacity =
          env.transfer_cache_manager().GetStats(size_class).capacity;
      batch_count += batch_size > 0 ? capacity / batch_size : 0;
    }
    return batch_count;
  };

  const int total_capacity = count_batches();

  ThreadManager threads;
  threads.Start(5, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) {
    env.transfer_cache_manager().TryResizingCaches();
    absl::SleepFor(absl::Milliseconds(10));
  }
  threads.Stop();

  EXPECT_EQ(count_batches(), total_capacity);
}

REGISTER_TYPED_TEST_SUITE_P(RealTransferCacheTest, StressResize);

using TransferCacheRealEnv = MultiSizeClassTransferCacheEnvironment<
    internal_transfer_cache::TransferCache<CentralFreeList,
                                           FakeMultiClassTransferCacheManager>>;
INSTANTIATE_TYPED_TEST_SUITE_P(TransferCache, RealTransferCacheTest,
                               ::testing::Types<TransferCacheRealEnv>);

using RingTransferCacheRealEnv = MultiSizeClassTransferCacheEnvironment<
    internal_transfer_cache::RingBufferTransferCache<
        CentralFreeList, FakeMultiClassRingBufferManager>>;
INSTANTIATE_TYPED_TEST_SUITE_P(RingBuffer, RealTransferCacheTest,
                               ::testing::Types<RingTransferCacheRealEnv>);

}  // namespace resize_tests

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
