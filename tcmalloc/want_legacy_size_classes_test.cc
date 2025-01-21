// Copyright 2022 The TCMalloc Authors
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

#include "gtest/gtest.h"
#include "absl/base/macros.h"
#include "tcmalloc/common.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class SizeClassesTest : public ::testing::Test {
 protected:
  SizeClassesTest() { m_.Init(); }

  SizeMap m_;
};

TEST_F(SizeClassesTest, SmallClasses) {
  if (__STDCPP_DEFAULT_NEW_ALIGNMENT__ > 8)
    GTEST_SKIP() << "Unexpected default new alignment.";

  const size_t kExpectedClasses[] = {0, 8, 16, 24, 32, 40, 48, 56, 64};
  const size_t kExpectedClassesSize = ABSL_ARRAYSIZE(kExpectedClasses);
  ASSERT_LE(kExpectedClassesSize, kNumClasses);
  for (int c = 0; c < kExpectedClassesSize; ++c) {
    EXPECT_EQ(m_.class_to_size(c), kExpectedClasses[c]) << c;
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
