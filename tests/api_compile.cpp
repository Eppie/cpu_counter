#include "perf.h"

#include <cstdint>
#include <optional>

namespace {

void ScopeSmoke() {
  PERF_SCOPE("scope-smoke", CYCLES | INSTRUCTIONS);
}

void SampledScopeSmoke() {
  for (std::size_t i = 0; i < 8; ++i) {
    PERF_SCOPE_SAMPLED("sampled-smoke", CYCLES | L1_MISS, 4);
  }
}

void BundleSmoke() {
  PERF_SCOPE("cache-profile-smoke", CACHE_PROFILE);
  PERF_SCOPE("branch-profile-smoke", BRANCH_PROFILE);
  PERF_SCOPE("frontend-profile-smoke", FRONTEND_PROFILE);
}

}  // namespace

int main() {
  ScopeSmoke();
  SampledScopeSmoke();
  BundleSmoke();

  std::string error;
  if (!PerfPrimeThread(PerfCounterSet{}, &error)) {
    return 1;
  }

  PerfMeasurement measurement = PerfMeasure(PerfCounterSet{}, [] {});
  if (!measurement.valid) {
    return 1;
  }
  if (measurement.count != 0) {
    return 1;
  }
  if (measurement.Get(CYCLES).has_value()) {
    return 1;
  }

  PerfPoint start(PerfCounterSet{});
  PerfPoint end(PerfCounterSet{});
  PerfPointDelta delta = end - start;
  (void)delta;
  return 0;
}
