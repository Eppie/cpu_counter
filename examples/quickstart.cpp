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

int main() {
  std::uint64_t sink = 0;

  // Measure one region directly. The counter set may list any counters in any
  // order; the library handles slot allocation for you.
  PerfMeasurement m = PerfMeasure(CYCLES | INSTRUCTIONS | L1_LOAD_MISS, [&] {
    for (std::uint64_t i = 0; i < 5'000'000; ++i) {
      sink += i * 2654435761u;
    }
  });

  if (!m.valid) {
    std::fprintf(stderr, "measurement failed: %s\n", m.error.c_str());
    std::fprintf(stderr, "(programming counters usually needs sudo)\n");
    return 1;
  }

  std::printf("perf.h %s\n", PERF_VERSION);
  std::printf("wall_ns       = %llu\n", static_cast<unsigned long long>(m.wall_ns));
  if (auto cycles = m.Get(CYCLES))
    std::printf("cycles        = %llu\n", static_cast<unsigned long long>(*cycles));
  if (auto inst = m.Get(INSTRUCTIONS))
    std::printf("instructions  = %llu\n", static_cast<unsigned long long>(*inst));
  if (auto miss = m.Get(L1_LOAD_MISS))
    std::printf("l1_load_miss  = %llu\n", static_cast<unsigned long long>(*miss));

  return static_cast<int>(sink & 0);  // keep `sink` live; always 0
}
