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
//
// Test for TCMalloc implementation of MallocExtension

#include "tcmalloc/malloc_extension.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/time/time.h"

namespace tcmalloc {
namespace {

TEST(MallocExtension, BackgroundReleaseRate) {

  // Mutate via MallocExtension.
  MallocExtension::SetBackgroundReleaseRate(
      MallocExtension::BytesPerSecond{100 << 20});

  EXPECT_EQ(static_cast<size_t>(MallocExtension::GetBackgroundReleaseRate()),
            100 << 20);

  // Disable release
  MallocExtension::SetBackgroundReleaseRate(MallocExtension::BytesPerSecond{0});

  EXPECT_EQ(static_cast<size_t>(MallocExtension::GetBackgroundReleaseRate()),
            0);
}

TEST(MallocExtension, SkipSubreleaseInterval) {

  // Mutate via MallocExtension.
  MallocExtension::SetSkipSubreleaseInterval(absl::Seconds(10));
  EXPECT_EQ(MallocExtension::GetSkipSubreleaseInterval(), absl::Seconds(10));

  // Disable skip subrelease
  MallocExtension::SetSkipSubreleaseInterval(absl::ZeroDuration());
  EXPECT_EQ(MallocExtension::GetSkipSubreleaseInterval(), absl::ZeroDuration());
}

TEST(MallocExtension, Properties) {
  // Verify that every property under GetProperties also works with
  // GetNumericProperty.
  const auto properties = MallocExtension::GetProperties();
  for (const auto& property : properties) {
    absl::optional<size_t> scalar =
        MallocExtension::GetNumericProperty(property.first);
    // The value of the property itself may have changed, so just check that it
    // is present.
    EXPECT_THAT(scalar, testing::Ne(absl::nullopt)) << property.first;
  }

  // Test that known GetNumericProperty keys exist under GetProperties.
  constexpr absl::string_view kKnownProperties[] = {
      // clang-format off
      // go/keep-sorted start
      "generic.bytes_in_use_by_app",
      "generic.current_allocated_bytes",
      "generic.heap_size",
      "generic.physical_memory_used",
      "generic.virtual_memory_used",
      "tcmalloc.central_cache_free",
      "tcmalloc.cpu_free",
      "tcmalloc.current_total_thread_cache_bytes",
      "tcmalloc.desired_usage_limit_bytes",
      "tcmalloc.external_fragmentation_bytes",
      "tcmalloc.hard_usage_limit_bytes",
      "tcmalloc.local_bytes",
      "tcmalloc.max_total_thread_cache_bytes",
      "tcmalloc.metadata_bytes",
      "tcmalloc.page_algorithm",
      "tcmalloc.page_heap_free",
      "tcmalloc.page_heap_unmapped",
      "tcmalloc.pageheap_free_bytes",
      "tcmalloc.pageheap_unmapped_bytes",
      "tcmalloc.per_cpu_caches_active",
      "tcmalloc.required_bytes",
      "tcmalloc.sampled_internal_fragmentation",
      "tcmalloc.sharded_transfer_cache_free",
      "tcmalloc.slack_bytes",
      "tcmalloc.thread_cache_count",
      "tcmalloc.thread_cache_free",
      "tcmalloc.transfer_cache_free",
      // go/keep-sorted end
      // clang-format on
  };

  for (const auto& known : kKnownProperties) {
    absl::optional<size_t> scalar = MallocExtension::GetNumericProperty(known);
    EXPECT_THAT(scalar, testing::Ne(absl::nullopt));
    EXPECT_THAT(properties,
                testing::Contains(testing::Key(testing::Eq(known))));
  }
}

}  // namespace
}  // namespace tcmalloc
