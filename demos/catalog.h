#ifndef CPU_COUNTER_DEMOS_CATALOG_H_
#define CPU_COUNTER_DEMOS_CATALOG_H_

#include "perf.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace demo {

enum class Tier {
  Stable,
  Experimental,
};

enum class Group {
  CoreExecution,
  MemoryCache,
  TlbPageWalk,
  BranchControl,
  InstructionSide,
  StoreOrdering,
  SimdUopMapping,
};

enum class CorePreference {
  Any,
  Performance,
  Efficiency,
};

struct WorkloadExpectation {
  std::string_view counter_name;
  std::string_view level;
  std::string_view note;
};

struct ValidationRule {
  double min_ratio = 1.0;
  std::uint64_t min_delta = 0;
};

struct LogicalCpuRange {
  int first = 0;
  int last = -1;
  std::string label;

  [[nodiscard]] bool Contains(int cpu) const;
  [[nodiscard]] std::string Description() const;
};

struct PerfLevelLayout {
  struct Entry {
    int index = 0;
    std::string name;
    int logical_cpus = 0;
    LogicalCpuRange heuristic_range;
  };

  std::vector<Entry> entries;
  std::optional<LogicalCpuRange> performance_range;
  std::optional<LogicalCpuRange> efficiency_range;
};

struct RunOptions {
  std::optional<int> prefer_cpu;
  CorePreference core_preference = CorePreference::Performance;
  std::optional<LogicalCpuRange> prefer_cpu_range;
  std::size_t max_attempts = 25;
  bool require_stable_cpu = false;
  bool require_active_pmu = false;
};

struct DemoEnvironment {
  using ExecStub = std::uint64_t (*)(void);

  std::size_t page_size = 0;
  std::size_t cacheline_size = 0;
  std::string error_message;

  std::vector<std::uint32_t> hot_ring;
  std::vector<std::uint32_t> random_ring;
  std::vector<std::uint64_t> stream_read;
  std::vector<std::uint64_t> stream_store;
  std::vector<std::uint64_t> hot_store;
  std::vector<std::uint64_t> branch_source;

  std::vector<std::uint8_t> page_storage;
  std::uint64_t *page_base = nullptr;
  std::size_t page_count = 0;
  std::vector<std::size_t> page_order;

  std::uint8_t *exec_code = nullptr;
  std::size_t exec_bytes = 0;
  std::size_t exec_page_count = 0;
  std::vector<std::size_t> exec_page_order;

  DemoEnvironment() = default;
  DemoEnvironment(const DemoEnvironment &) = delete;
  DemoEnvironment &operator=(const DemoEnvironment &) = delete;
  ~DemoEnvironment();

  bool Initialize(std::string &error);
  [[nodiscard]] std::size_t PageWords() const;
  [[nodiscard]] ExecStub ExecStubAt(std::size_t page) const;
};

using WorkloadFn = std::uint64_t (*)(DemoEnvironment &state);

struct WorkloadDefinition {
  std::string_view id;
  std::string_view title;
  std::string_view summary;
  std::string_view mechanism;
  std::string_view configuration;
  std::string_view code_snippet;
  Group group = Group::CoreExecution;
  Tier tier = Tier::Stable;
  std::size_t default_repeats = 3;
  std::size_t default_warmups = 1;
  PerfCounterSet measurement_counters{};
  std::span<const WorkloadExpectation> expectations;
  WorkloadFn run = nullptr;
};

struct CounterDefinition {
  std::string_view name;
  std::string_view title;
  std::string_view description;
  std::string_view caveats;
  Group group = Group::CoreExecution;
  Tier tier = Tier::Stable;
  PerfCounter counter{};
  std::string_view high_demo_id;
  std::string_view low_demo_id;
  ValidationRule validation;
};

const char *ToString(Tier tier);
const char *ToString(Group group);
const char *ToString(CorePreference preference);

PerfLevelLayout ReadPerfLevelLayout();
bool ResolveCorePreference(RunOptions &options, const PerfLevelLayout &layout, std::string &error);
bool ApplySchedulerPreference(const RunOptions &options, std::string &error);
bool SampleMatches(const PerfMeasurement &measurement, const RunOptions &options);

std::span<const WorkloadDefinition> Workloads();
std::span<const CounterDefinition> Counters();
const WorkloadDefinition *FindWorkload(std::string_view id);
const CounterDefinition *FindCounter(std::string_view name);

namespace workloads {

std::uint64_t DenseIntegerAlu(DemoEnvironment &state);
std::uint64_t HotSequentialRead(DemoEnvironment &state);
std::uint64_t RandomPointerChase(DemoEnvironment &state);
std::uint64_t HotSequentialWrite(DemoEnvironment &state);
std::uint64_t RandomPageWrite(DemoEnvironment &state);
std::uint64_t PageStrideRead(DemoEnvironment &state);
std::uint64_t PredictableBranch(DemoEnvironment &state);
std::uint64_t UnpredictableBranch(DemoEnvironment &state);
std::uint64_t HotInstructionLoop(DemoEnvironment &state);
std::uint64_t RandomInstructionPages(DemoEnvironment &state);

}  // namespace workloads

}  // namespace demo

#endif  // CPU_COUNTER_DEMOS_CATALOG_H_
