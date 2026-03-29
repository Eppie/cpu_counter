#include "perf.h"

#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

namespace {

std::uint64_t MixerUpdate(std::vector<std::uint64_t> &buffer,
                          const std::vector<std::uint64_t> &coeffs) {
  PERF_SCOPE("mixer_update", CYCLES | INSTRUCTIONS | L1_MISS | DTLB_MISS);

  std::uint64_t acc = 0;
  for (std::size_t i = 0; i < buffer.size(); ++i) {
    buffer[i] = buffer[i] * coeffs[i % coeffs.size()] + static_cast<std::uint64_t>(i);
    acc ^= buffer[i];
  }
  return acc;
}

std::uint64_t RandomWalk(const std::vector<std::uint32_t> &ring) {
  PERF_SCOPE("random_walk", CYCLES | L1_LOAD_MISS | TLB_MISS | BRANCH_MISS);

  volatile const std::uint32_t *data = ring.data();
  std::uint32_t index = 0;
  for (std::size_t i = 0; i < 2'000'000; ++i) {
    index = data[index];
  }
  return index;
}

void HotLoop(std::vector<std::uint64_t> &values) {
  for (std::size_t iter = 0; iter < 200'000; ++iter) {
    PERF_SCOPE_SAMPLED("hot_loop", CYCLES | L1_MISS, 1024);
    values[iter % values.size()] += iter;
  }
}

}  // namespace

int main() {
  std::cout << "perf.h example; output defaults to perf.jsonl or $PERF_OUTPUT\n";

  std::string prime_error;
  const auto thread_counters =
      CYCLES | INSTRUCTIONS | L1_MISS | DTLB_MISS | L1_LOAD_MISS | TLB_MISS | BRANCH_MISS;
  if (!PerfPrimeThread(thread_counters, &prime_error)) {
    std::cout << "PerfPrimeThread failed: " << prime_error << '\n';
  }

  std::vector<std::uint64_t> mix_buffer(1 << 18, 1);
  std::vector<std::uint64_t> coeffs = {3, 5, 7, 11, 13, 17, 19, 23};
  std::vector<std::uint32_t> ring(1 << 20);
  std::vector<std::uint64_t> hot_values(4096, 0);

  std::mt19937 rng(12345);
  std::uniform_int_distribution<std::uint32_t> dist(0, static_cast<std::uint32_t>(ring.size() - 1));
  for (std::size_t i = 0; i < ring.size(); ++i) {
    ring[i] = dist(rng);
  }

  std::uint64_t mix_acc = 0;
  {
    PERF_SCOPE("frame", CYCLES | INSTRUCTIONS | L1_MISS);
    mix_acc ^= MixerUpdate(mix_buffer, coeffs);
    mix_acc ^= RandomWalk(ring);
    HotLoop(hot_values);
  }

  PerfPoint start(CYCLES | L1_MISS);
  for (std::size_t i = 0; i < hot_values.size(); ++i) {
    hot_values[i] ^= mix_acc + i;
  }
  PerfPoint end(CYCLES | L1_MISS);
  PerfPointDelta delta = end - start;
  std::cout << "manual PerfPoint delta: " << delta.ToJson() << '\n';

  std::cout << "done, sink=" << (mix_acc ^ hot_values[17]) << '\n';
  return 0;
}
