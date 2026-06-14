// Minimal perf.h usage example.
//
// Build (any of):
//   c++ -std=c++20 -O2 -I.. quickstart.cpp -o quickstart   # from examples/
//   cmake -B build && cmake --build build                  # builds perf_quickstart
//
// Run: requires macOS on Apple Silicon, and root to program the counters:
//   sudo ./quickstart
//
// Without privilege the program still runs; the measurement reports an error
// instead of crashing.

#include "perf.h"

#include <cstdint>
#include <cstdio>
#include <utility>

// A counter set -- the value built by `CYCLES | INSTRUCTIONS | ...` -- has type
// `PerfCounterSet` in the global namespace (it is `perf::CounterSet` inside the
// library's own namespace; the global re-exports are Perf-prefixed so the header
// does not drop short names like `Counter` into your global scope). You only
// need to name the type when you factor a measured region out behind a helper,
// as below; otherwise pass the bare `CYCLES | ...` expression straight into
// `PerfMeasure(...)`.
template <class F>
static void profile(const char* label, PerfCounterSet counters, F&& f) {
  PerfMeasurement m = PerfMeasure(counters, std::forward<F>(f));
  std::printf("%-12s wall=%.2f ms", label, m.wall_ns / 1e6);
  if (!m.valid) {
    std::printf("   [no PMU: %s  (programming counters usually needs sudo)]\n",
                m.error.c_str());
    return;
  }
  if (auto cycles = m.Get(CYCLES))
    std::printf("   cycles=%llu", static_cast<unsigned long long>(*cycles));
  if (auto inst = m.Get(INSTRUCTIONS))
    std::printf("   inst=%llu", static_cast<unsigned long long>(*inst));
  if (auto miss = m.Get(L1_LOAD_MISS))
    std::printf("   l1_load_miss=%llu", static_cast<unsigned long long>(*miss));
  std::printf("\n");
}

int main() {
  std::uint64_t sink = 0;
  std::printf("perf.h %s\n", PERF_VERSION);

  // Reuse one set across regions, or build a different one per call. A single
  // counter (e.g. CYCLES) and a curated bundle (e.g. CACHE_PROFILE) are both
  // valid PerfCounterSet values.
  const PerfCounterSet basic = CYCLES | INSTRUCTIONS | L1_LOAD_MISS;

  profile("multiply", basic, [&] {
    for (std::uint64_t i = 0; i < 5'000'000; ++i) sink += i * 2654435761u;
  });
  profile("xor-walk", CYCLES | INSTRUCTIONS, [&] {
    for (std::uint64_t i = 0; i < 5'000'000; ++i) sink ^= (sink >> 7) + i;
  });

  return static_cast<int>(sink & 0);  // keep `sink` live; always 0
}
