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
#include "tcmalloc/parameters.h"

#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/huge_page_aware_allocator.h"
#include "tcmalloc/lifetime_based_allocator.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/thread_cache.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// As decide_subrelease() is determined at runtime, we cannot require constant
// initialization for the atomic.  This avoids an initialization order fiasco.
static std::atomic<bool>* hpaa_subrelease_ptr() {
  static std::atomic<bool> v(decide_subrelease());
  return &v;
}

// As skip_subrelease_interval_ns() is determined at runtime, we cannot require
// constant initialization for the atomic.  This avoids an initialization order
// fiasco.
static std::atomic<int64_t>& skip_subrelease_interval_ns() {
  static std::atomic<int64_t> v(absl::ToInt64Nanoseconds(
#if defined(TCMALLOC_SMALL_BUT_SLOW)
      absl::ZeroDuration()
#else
      absl::Seconds(60)
#endif
          ));
  return v;
}

// As experiments are determined at runtime, we cannot require constant
// initialization for the atomic.  This avoids an initialization order fiasco.
static std::atomic<bool>& dynamic_slab_enabled() {
  static std::atomic<bool> v(
      IsExperimentActive(Experiment::TCMALLOC_DYNAMIC_SLABS));
  return v;
}

uint64_t Parameters::heap_size_hard_limit() {
  size_t amount;
  bool is_hard;
  std::tie(amount, is_hard) = Static::page_allocator().limit();
  if (!is_hard) {
    amount = 0;
  }
  return amount;
}

void Parameters::set_heap_size_hard_limit(uint64_t value) {
  TCMalloc_Internal_SetHeapSizeHardLimit(value);
}

bool Parameters::hpaa_subrelease() {
  return hpaa_subrelease_ptr()->load(std::memory_order_relaxed);
}

void Parameters::set_hpaa_subrelease(bool value) {
  TCMalloc_Internal_SetHPAASubrelease(value);
}

ABSL_CONST_INIT std::atomic<MallocExtension::BytesPerSecond>
    Parameters::background_release_rate_(MallocExtension::BytesPerSecond{
        0
    });
ABSL_CONST_INIT std::atomic<int64_t> Parameters::guarded_sampling_rate_(
    50 * kDefaultProfileSamplingRate);
ABSL_CONST_INIT std::atomic<bool> Parameters::shuffle_per_cpu_caches_enabled_(
    true);
ABSL_CONST_INIT std::atomic<int32_t> Parameters::max_per_cpu_cache_size_(
    kMaxCpuCacheSize);
ABSL_CONST_INIT std::atomic<bool> Parameters::prioritize_spans_enabled_(true);
ABSL_CONST_INIT std::atomic<bool> Parameters::partial_transfer_cache_enabled_(
    false);
ABSL_CONST_INIT std::atomic<bool> Parameters::madvise_cold_regions_nohugepage_(
    true);
ABSL_CONST_INIT std::atomic<int64_t> Parameters::max_total_thread_cache_bytes_(
    kDefaultOverallThreadCacheSize);
ABSL_CONST_INIT std::atomic<double>
    Parameters::peak_sampling_heap_growth_fraction_(1.1);
ABSL_CONST_INIT std::atomic<bool> Parameters::per_cpu_caches_enabled_(
#if defined(TCMALLOC_DEPRECATED_PERTHREAD)
    false
#else
    true
#endif
);
ABSL_CONST_INIT std::atomic<double>
    Parameters::per_cpu_caches_dynamic_slab_grow_threshold_(0.9);
ABSL_CONST_INIT std::atomic<double>
    Parameters::per_cpu_caches_dynamic_slab_shrink_threshold_(0.5);

ABSL_CONST_INIT std::atomic<int64_t> Parameters::profile_sampling_rate_(
    kDefaultProfileSamplingRate);
ABSL_CONST_INIT std::atomic<bool> Parameters::use_new_residency_api_(true);

absl::Duration Parameters::filler_skip_subrelease_interval() {
  return absl::Nanoseconds(
      skip_subrelease_interval_ns().load(std::memory_order_relaxed));
}

bool Parameters::pass_span_object_count_to_pageheap() {
  static bool v([]() {
    return IsExperimentActive(
               Experiment::
                   TEST_ONLY_TCMALLOC_PASS_SPAN_OBJECT_COUNT_TO_PAGEHEAP) ||
           IsExperimentActive(
               Experiment::TCMALLOC_PASS_SPAN_OBJECT_COUNT_TO_PAGEHEAP);
  }());
  return v;
}

bool Parameters::per_cpu_caches_dynamic_slab_enabled() {
  return dynamic_slab_enabled().load(std::memory_order_relaxed);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

using tcmalloc::tcmalloc_internal::kLog;
using tcmalloc::tcmalloc_internal::Log;
using tcmalloc::tcmalloc_internal::Parameters;
using tcmalloc::tcmalloc_internal::Static;

extern "C" {

int64_t MallocExtension_Internal_GetProfileSamplingRate() {
  return Parameters::profile_sampling_rate();
}

void MallocExtension_Internal_SetProfileSamplingRate(int64_t value) {
  Parameters::set_profile_sampling_rate(value);
}

int64_t MallocExtension_Internal_GetGuardedSamplingRate() {
  return Parameters::guarded_sampling_rate();
}

void MallocExtension_Internal_SetGuardedSamplingRate(int64_t value) {
  Parameters::set_guarded_sampling_rate(value);
}

int64_t MallocExtension_Internal_GetMaxTotalThreadCacheBytes() {
  return Parameters::max_total_thread_cache_bytes();
}

void MallocExtension_Internal_SetMaxTotalThreadCacheBytes(int64_t value) {
  Parameters::set_max_total_thread_cache_bytes(value);
}

void MallocExtension_Internal_GetSkipSubreleaseInterval(absl::Duration* ret) {
  *ret = Parameters::filler_skip_subrelease_interval();
}

void MallocExtension_Internal_SetSkipSubreleaseInterval(absl::Duration value) {
  Parameters::set_filler_skip_subrelease_interval(value);
}

tcmalloc::MallocExtension::BytesPerSecond
MallocExtension_Internal_GetBackgroundReleaseRate() {
  return Parameters::background_release_rate();
}

void MallocExtension_Internal_SetBackgroundReleaseRate(
    tcmalloc::MallocExtension::BytesPerSecond rate) {
  Parameters::set_background_release_rate(rate);
}

void TCMalloc_Internal_SetBackgroundReleaseRate(size_t value) {
  Parameters::background_release_rate_.store(
      static_cast<tcmalloc::MallocExtension::BytesPerSecond>(value));
}

uint64_t TCMalloc_Internal_GetHeapSizeHardLimit() {
  return Parameters::heap_size_hard_limit();
}

bool TCMalloc_Internal_GetHPAASubrelease() {
  return Parameters::hpaa_subrelease();
}

bool TCMalloc_Internal_GetShufflePerCpuCachesEnabled() {
  return Parameters::shuffle_per_cpu_caches();
}

bool TCMalloc_Internal_GetPrioritizeSpansEnabled() {
  return Parameters::prioritize_spans();
}

bool TCMalloc_Internal_GetPartialTransferCacheEnabled() {
  return Parameters::partial_transfer_cache();
}

double TCMalloc_Internal_GetPeakSamplingHeapGrowthFraction() {
  return Parameters::peak_sampling_heap_growth_fraction();
}

bool TCMalloc_Internal_GetPerCpuCachesEnabled() {
  return Parameters::per_cpu_caches();
}

void TCMalloc_Internal_SetGuardedSamplingRate(int64_t v) {
  Parameters::guarded_sampling_rate_.store(v, std::memory_order_relaxed);
}

// update_lock guards changes via SetHeapSizeHardLimit.
ABSL_CONST_INIT static absl::base_internal::SpinLock update_lock(
    absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY);

void TCMalloc_Internal_SetHeapSizeHardLimit(uint64_t value) {
  // Ensure that page allocator is set up.
  Static::InitIfNecessary();

  absl::base_internal::SpinLockHolder l(&update_lock);

  size_t limit = std::numeric_limits<size_t>::max();
  bool active = false;
  if (value > 0) {
    limit = value;
    active = true;
  }

  bool currently_hard = Static::page_allocator().limit().second;
  if (active || currently_hard) {
    // Avoid resetting limit when current limit is soft.
    Static::page_allocator().set_limit(limit, active /* is_hard */);
    Log(kLog, __FILE__, __LINE__, "[tcmalloc] set page heap hard limit to",
        limit, "bytes");
  }
}

void TCMalloc_Internal_SetHPAASubrelease(bool v) {
  tcmalloc::tcmalloc_internal::hpaa_subrelease_ptr()->store(
      v, std::memory_order_relaxed);
}

void TCMalloc_Internal_SetShufflePerCpuCachesEnabled(bool v) {
  Parameters::shuffle_per_cpu_caches_enabled_.store(v,
                                                    std::memory_order_relaxed);
}

void TCMalloc_Internal_SetPrioritizeSpansEnabled(bool v) {
  Parameters::prioritize_spans_enabled_.store(v, std::memory_order_relaxed);
}

void TCMalloc_Internal_SetPartialTransferCacheEnabled(bool v) {
  Parameters::partial_transfer_cache_enabled_.store(v,
                                                    std::memory_order_relaxed);
}

void TCMalloc_Internal_SetMaxPerCpuCacheSize(int32_t v) {
  Parameters::max_per_cpu_cache_size_.store(v, std::memory_order_relaxed);
}

void TCMalloc_Internal_SetMaxTotalThreadCacheBytes(int64_t v) {
  Parameters::max_total_thread_cache_bytes_.store(v, std::memory_order_relaxed);

  absl::base_internal::SpinLockHolder l(
      &tcmalloc::tcmalloc_internal::pageheap_lock);
  tcmalloc::tcmalloc_internal::ThreadCache::set_overall_thread_cache_size(v);
}

void TCMalloc_Internal_SetPeakSamplingHeapGrowthFraction(double v) {
  Parameters::peak_sampling_heap_growth_fraction_.store(
      v, std::memory_order_relaxed);
}

void TCMalloc_Internal_SetPerCpuCachesEnabled(bool v) {
  Parameters::per_cpu_caches_enabled_.store(v, std::memory_order_relaxed);
}

void TCMalloc_Internal_SetProfileSamplingRate(int64_t v) {
  Parameters::profile_sampling_rate_.store(v, std::memory_order_relaxed);
}

void TCMalloc_Internal_GetHugePageFillerSkipSubreleaseInterval(
    absl::Duration* v) {
  *v = Parameters::filler_skip_subrelease_interval();
}

void TCMalloc_Internal_SetHugePageFillerSkipSubreleaseInterval(
    absl::Duration v) {
  tcmalloc::tcmalloc_internal::skip_subrelease_interval_ns().store(
      absl::ToInt64Nanoseconds(v), std::memory_order_relaxed);
}

void TCMalloc_Internal_SetLifetimeAllocatorOptions(absl::string_view s) {
  absl::base_internal::SpinLockHolder l(
      &tcmalloc::tcmalloc_internal::pageheap_lock);
  tcmalloc::tcmalloc_internal::HugePageAwareAllocator* hpaa =
      Static::page_allocator().default_hpaa();
  if (hpaa != nullptr) {
    hpaa->lifetime_based_allocator().Enable(
        tcmalloc::tcmalloc_internal::LifetimePredictionOptions::FromFlag(s));
  }
}

bool TCMalloc_Internal_GetPerCpuCachesDynamicSlabEnabled() {
  return Parameters::per_cpu_caches_dynamic_slab_enabled();
}

void TCMalloc_Internal_SetPerCpuCachesDynamicSlabEnabled(bool v) {
  tcmalloc::tcmalloc_internal::dynamic_slab_enabled().store(
      v, std::memory_order_relaxed);
}

double TCMalloc_Internal_GetPerCpuCachesDynamicSlabGrowThreshold() {
  return Parameters::per_cpu_caches_dynamic_slab_grow_threshold();
}

void TCMalloc_Internal_SetPerCpuCachesDynamicSlabGrowThreshold(double v) {
  Parameters::per_cpu_caches_dynamic_slab_grow_threshold_.store(
      v, std::memory_order_relaxed);
}

double TCMalloc_Internal_GetPerCpuCachesDynamicSlabShrinkThreshold() {
  return Parameters::per_cpu_caches_dynamic_slab_shrink_threshold();
}

void TCMalloc_Internal_SetPerCpuCachesDynamicSlabShrinkThreshold(double v) {
  Parameters::per_cpu_caches_dynamic_slab_shrink_threshold_.store(
      v, std::memory_order_relaxed);
}

bool TCMalloc_Internal_GetMadviseColdRegionsNoHugepage() {
  return Parameters::madvise_cold_regions_nohugepage();
}

void TCMalloc_Internal_SetMadviseColdRegionsNoHugepage(bool v) {
  Parameters::madvise_cold_regions_nohugepage_.store(v,
                                                     std::memory_order_relaxed);
}

bool TCMalloc_Internal_GetUseNewResidencyApi() {
  return Parameters::use_new_residency_api();
}
void TCMalloc_Internal_SetUseNewResidencyApi(bool v) {
  Parameters::use_new_residency_api_.store(v, std::memory_order_relaxed);
}

}  // extern "C"
