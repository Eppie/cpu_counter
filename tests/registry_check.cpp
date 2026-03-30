#include "demos/catalog.h"

#include <iostream>
#include <string>
#include <unordered_set>

int main() {
  bool ok = true;

  std::unordered_set<std::string> demo_ids;
  for (const demo::WorkloadDefinition &workload : demo::Workloads()) {
    if (!demo_ids.insert(std::string(workload.id)).second) {
      std::cerr << "duplicate demo id: " << workload.id << '\n';
      ok = false;
    }

    std::size_t configurable = 0;
    for (std::uint8_t i = 0; i < workload.measurement_counters.count; ++i) {
      if (!workload.measurement_counters.items[i].fixed) {
        ++configurable;
      }
    }
    if (workload.measurement_counters.overflow || configurable > 5) {
      std::cerr << "workload measurement set is too large for curated demo use: "
                << workload.id << '\n';
      ok = false;
    }
  }

  std::unordered_set<std::string> counter_names;
  for (const demo::CounterDefinition &counter : demo::Counters()) {
    if (!counter_names.insert(std::string(counter.name)).second) {
      std::cerr << "duplicate counter name: " << counter.name << '\n';
      ok = false;
    }
    if (demo::FindCounter(counter.name) == nullptr) {
      std::cerr << "counter lookup failed: " << counter.name << '\n';
      ok = false;
    }
    if (counter.tier == demo::Tier::Stable) {
      if (demo::FindWorkload(counter.high_demo_id) == nullptr) {
        std::cerr << "stable counter missing high showcase: " << counter.name << '\n';
        ok = false;
      }
      if (demo::FindWorkload(counter.low_demo_id) == nullptr) {
        std::cerr << "stable counter missing low showcase: " << counter.name << '\n';
        ok = false;
      }
    }
  }

  return ok ? 0 : 1;
}
