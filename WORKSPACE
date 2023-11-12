# Copyright 2019 The TCMalloc Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

workspace(name = "com_google_tcmalloc")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# Abseil
http_archive(
    name = "com_google_absl",  # 2021-05-20T02:59:16Z
    urls = ["https://github.com/abseil/abseil-cpp/archive/7971fb358ae376e016d2d4fc9327aad95659b25e.zip"],
    strip_prefix = "abseil-cpp-7971fb358ae376e016d2d4fc9327aad95659b25e",
    sha256 = "aeba534f7307e36fe084b452299e49b97420667a8d28102cf9a0daeed340b859",
)

# GoogleTest/GoogleMock framework. Used by most unit-tests.
http_archive(
    name = "com_google_googletest",  # 2021-05-19T20:10:13Z
    urls = ["https://github.com/google/googletest/archive/aa9b44a18678dfdf57089a5ac22c1edb69f35da5.zip"],
    strip_prefix = "googletest-aa9b44a18678dfdf57089a5ac22c1edb69f35da5",
    sha256 = "8cf4eaab3a13b27a95b7e74c58fb4c0788ad94d1f7ec65b20665c4caf1d245e8",
)

# Google benchmark.
http_archive(
    name = "com_github_google_benchmark",  # 2021-05-20T16:59:29Z
    urls = ["https://github.com/google/benchmark/archive/a4bcd937b298fdc2b0c7da9961fa202a5aecd56b.zip"],
    strip_prefix = "benchmark-a4bcd937b298fdc2b0c7da9961fa202a5aecd56b",
    sha256 = "a6abcbe9dec091cd570462bf712661d3944f73f964f82501456eaa5f89036b1a",
)

# C++ rules for Bazel.
http_archive(
    name = "rules_cc",  # 2021-05-14T14:51:14Z
    urls = ["https://github.com/bazelbuild/rules_cc/archive/68cb652a71e7e7e2858c50593e5a9e3b94e5b9a9.zip"],
    strip_prefix = "rules_cc-68cb652a71e7e7e2858c50593e5a9e3b94e5b9a9",
    sha256 = "1e19e9a3bc3d4ee91d7fcad00653485ee6c798efbbf9588d40b34cbfbded143d",
)

# Fuzzing
http_archive(
    name = "rules_fuzzing",
    sha256 = "a5734cb42b1b69395c57e0bbd32ade394d5c3d6afbfe782b24816a96da24660d",
    strip_prefix = "rules_fuzzing-0.1.1",
    urls = ["https://github.com/bazelbuild/rules_fuzzing/archive/v0.1.1.zip"],
)

load("@rules_fuzzing//fuzzing:repositories.bzl", "rules_fuzzing_dependencies")

rules_fuzzing_dependencies()

load("@rules_fuzzing//fuzzing:init.bzl", "rules_fuzzing_init")

rules_fuzzing_init()
