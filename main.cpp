#include "demos/catalog.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class CommandKind {
  Help,
  ListCounters,
  ListDemos,
  ExplainCounter,
  ExplainDemo,
  RunCounter,
  RunDemo,
  Validate,
};

enum class TierFilter {
  Stable,
  Experimental,
  All,
};

struct CliOptions {
  CommandKind command = CommandKind::Help;
  std::string target;
  TierFilter tier_filter = TierFilter::All;
  std::optional<demo::Group> group_filter;
  std::optional<std::size_t> repeat_override;
  std::optional<std::size_t> warmup_override;
  demo::RunOptions run_options;
};

struct WorkloadRunSummary {
  const demo::WorkloadDefinition *workload = nullptr;
  PerfCounterSet measured_counters{};
  std::vector<PerfMeasurement> samples;
  std::uint64_t last_sink = 0;
  std::size_t attempts_used = 0;
  std::string error;
  bool valid = false;
};

struct CounterComparison {
  WorkloadRunSummary high;
  WorkloadRunSummary low;
  double high_mean = 0.0;
  double low_mean = 0.0;
  double ratio = 0.0;
  bool validation_passed = false;
};

std::string CounterLabel(PerfCounter counter) {
  for (const demo::CounterDefinition &definition : demo::Counters()) {
    if (definition.counter == counter) {
      return std::string(definition.name);
    }
  }
  if (counter.kind == perf::CounterKind::Named) {
    return counter.name != nullptr ? std::string(counter.name) : std::string("unnamed");
  }
  std::ostringstream oss;
  if (counter.name != nullptr && counter.name[0] != '\0') {
    oss << counter.name << '@';
  }
  oss << "raw_0x" << std::hex << std::nouppercase << counter.raw_config;
  return oss.str();
}

bool TierMatches(demo::Tier tier, TierFilter filter) {
  switch (filter) {
    case TierFilter::Stable:
      return tier == demo::Tier::Stable;
    case TierFilter::Experimental:
      return tier == demo::Tier::Experimental;
    case TierFilter::All:
    default:
      return true;
  }
}

std::optional<TierFilter> ParseTierFilter(std::string_view value) {
  if (value == "stable") {
    return TierFilter::Stable;
  }
  if (value == "experimental") {
    return TierFilter::Experimental;
  }
  if (value == "all") {
    return TierFilter::All;
  }
  return std::nullopt;
}

std::optional<demo::Group> ParseGroup(std::string_view value) {
  for (demo::Group group : {
           demo::Group::CoreExecution,
           demo::Group::MemoryCache,
           demo::Group::TlbPageWalk,
           demo::Group::BranchControl,
           demo::Group::InstructionSide,
           demo::Group::StoreOrdering,
           demo::Group::SimdUopMapping,
       }) {
    if (value == demo::ToString(group)) {
      return group;
    }
  }
  return std::nullopt;
}

bool GroupMatches(demo::Group group, const std::optional<demo::Group> &filter) {
  return !filter.has_value() || group == *filter;
}

void PrintUsage() {
  std::cout << "cpu_counter: perf.h production library + PMU demo lab\n\n";
  std::cout << "Usage:\n";
  std::cout << "  cpu_counter list counters [--tier stable|experimental|all] [--group <group>]\n";
  std::cout << "  cpu_counter list demos [--tier stable|experimental|all] [--group <group>]\n";
  std::cout << "  cpu_counter explain counter <name>\n";
  std::cout << "  cpu_counter explain demo <id>\n";
  std::cout << "  cpu_counter run counter <name> [--repeat N] [--warmup N] [--prefer-pcore|--prefer-ecore|--prefer-cpu N] [--require-stable-cpu] [--require-active-pmu]\n";
  std::cout << "  cpu_counter run demo <id> [--repeat N] [--warmup N] [--prefer-pcore|--prefer-ecore|--prefer-cpu N] [--require-stable-cpu] [--require-active-pmu]\n";
  std::cout << "  cpu_counter validate [--group <group>] [--prefer-pcore|--prefer-ecore|--prefer-cpu N] [--require-stable-cpu] [--require-active-pmu]\n\n";
  std::cout << "Groups:\n";
  for (demo::Group group : {
           demo::Group::CoreExecution,
           demo::Group::MemoryCache,
           demo::Group::TlbPageWalk,
           demo::Group::BranchControl,
           demo::Group::InstructionSide,
           demo::Group::StoreOrdering,
           demo::Group::SimdUopMapping,
       }) {
    std::cout << "  - " << demo::ToString(group) << '\n';
  }
}

bool ParseNumber(std::string_view text, std::size_t &value) {
  if (text.empty()) {
    return false;
  }
  std::size_t parsed = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    parsed = parsed * 10 + static_cast<std::size_t>(c - '0');
  }
  value = parsed;
  return true;
}

bool ParseInt(std::string_view text, int &value) {
  if (text.empty()) {
    return false;
  }
  bool negative = false;
  std::size_t index = 0;
  if (text[0] == '-') {
    negative = true;
    index = 1;
  }
  if (index == text.size()) {
    return false;
  }
  int parsed = 0;
  for (; index < text.size(); ++index) {
    const char c = text[index];
    if (c < '0' || c > '9') {
      return false;
    }
    parsed = parsed * 10 + (c - '0');
  }
  value = negative ? -parsed : parsed;
  return true;
}

bool ParseFlags(int argc, char **argv, int start_index, CliOptions &options, std::string &error) {
  for (int i = start_index; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto require_value = [&](std::string_view flag) -> std::optional<std::string_view> {
      if (i + 1 >= argc) {
        error = std::string("missing value for ") + std::string(flag);
        return std::nullopt;
      }
      return std::string_view(argv[++i]);
    };

    if (arg == "--tier") {
      const auto value = require_value(arg);
      if (!value.has_value()) {
        return false;
      }
      const auto parsed = ParseTierFilter(*value);
      if (!parsed.has_value()) {
        error = std::string("unknown tier filter: ") + std::string(*value);
        return false;
      }
      options.tier_filter = *parsed;
      continue;
    }
    if (arg == "--group") {
      const auto value = require_value(arg);
      if (!value.has_value()) {
        return false;
      }
      const auto parsed = ParseGroup(*value);
      if (!parsed.has_value()) {
        error = std::string("unknown group: ") + std::string(*value);
        return false;
      }
      options.group_filter = *parsed;
      continue;
    }
    if (arg == "--repeat") {
      const auto value = require_value(arg);
      if (!value.has_value()) {
        return false;
      }
      std::size_t parsed = 0;
      if (!ParseNumber(*value, parsed)) {
        error = std::string("invalid --repeat value: ") + std::string(*value);
        return false;
      }
      options.repeat_override = parsed;
      continue;
    }
    if (arg == "--warmup") {
      const auto value = require_value(arg);
      if (!value.has_value()) {
        return false;
      }
      std::size_t parsed = 0;
      if (!ParseNumber(*value, parsed)) {
        error = std::string("invalid --warmup value: ") + std::string(*value);
        return false;
      }
      options.warmup_override = parsed;
      continue;
    }
    if (arg == "--prefer-pcore") {
      options.run_options.core_preference = demo::CorePreference::Performance;
      options.run_options.prefer_cpu.reset();
      continue;
    }
    if (arg == "--prefer-ecore") {
      options.run_options.core_preference = demo::CorePreference::Efficiency;
      options.run_options.prefer_cpu.reset();
      continue;
    }
    if (arg == "--prefer-cpu") {
      const auto value = require_value(arg);
      if (!value.has_value()) {
        return false;
      }
      int parsed = 0;
      if (!ParseInt(*value, parsed)) {
        error = std::string("invalid --prefer-cpu value: ") + std::string(*value);
        return false;
      }
      options.run_options.prefer_cpu = parsed;
      options.run_options.core_preference = demo::CorePreference::Any;
      continue;
    }
    if (arg == "--require-stable-cpu") {
      options.run_options.require_stable_cpu = true;
      continue;
    }
    if (arg == "--require-active-pmu") {
      options.run_options.require_active_pmu = true;
      continue;
    }

    error = std::string("unknown flag: ") + std::string(arg);
    return false;
  }
  return true;
}

bool ParseArgs(int argc, char **argv, CliOptions &options, std::string &error) {
  if (argc <= 1) {
    options.command = CommandKind::Help;
    return true;
  }

  const std::string_view arg1 = argv[1];
  if (arg1 == "list") {
    if (argc < 3) {
      error = "list requires 'counters' or 'demos'";
      return false;
    }
    const std::string_view subject = argv[2];
    if (subject == "counters") {
      options.command = CommandKind::ListCounters;
    } else if (subject == "demos") {
      options.command = CommandKind::ListDemos;
    } else {
      error = "list requires 'counters' or 'demos'";
      return false;
    }
    return ParseFlags(argc, argv, 3, options, error);
  }

  if (arg1 == "explain") {
    if (argc < 4) {
      error = "explain requires 'counter <name>' or 'demo <id>'";
      return false;
    }
    const std::string_view subject = argv[2];
    options.target = argv[3];
    if (subject == "counter") {
      options.command = CommandKind::ExplainCounter;
    } else if (subject == "demo") {
      options.command = CommandKind::ExplainDemo;
    } else {
      error = "explain requires 'counter <name>' or 'demo <id>'";
      return false;
    }
    return ParseFlags(argc, argv, 4, options, error);
  }

  if (arg1 == "run") {
    if (argc < 4) {
      error = "run requires 'counter <name>' or 'demo <id>'";
      return false;
    }
    const std::string_view subject = argv[2];
    options.target = argv[3];
    if (subject == "counter") {
      options.command = CommandKind::RunCounter;
    } else if (subject == "demo") {
      options.command = CommandKind::RunDemo;
    } else {
      error = "run requires 'counter <name>' or 'demo <id>'";
      return false;
    }
    return ParseFlags(argc, argv, 4, options, error);
  }

  if (arg1 == "validate") {
    options.command = CommandKind::Validate;
    return ParseFlags(argc, argv, 2, options, error);
  }

  if (arg1 == "help" || arg1 == "--help" || arg1 == "-h") {
    options.command = CommandKind::Help;
    return true;
  }

  error = std::string("unknown command: ") + std::string(arg1);
  return false;
}

std::optional<double> MeanCounterValue(const WorkloadRunSummary &summary, PerfCounter counter) {
  double total = 0.0;
  std::size_t count = 0;
  for (const PerfMeasurement &measurement : summary.samples) {
    const auto value = measurement.Get(counter);
    if (!value.has_value()) {
      continue;
    }
    total += static_cast<double>(*value);
    ++count;
  }
  if (count == 0) {
    return std::nullopt;
  }
  return total / static_cast<double>(count);
}

double MeanWallNs(const WorkloadRunSummary &summary) {
  if (summary.samples.empty()) {
    return 0.0;
  }
  double total = 0.0;
  for (const PerfMeasurement &measurement : summary.samples) {
    total += static_cast<double>(measurement.wall_ns);
  }
  return total / static_cast<double>(summary.samples.size());
}

std::string MismatchReason(const PerfMeasurement &measurement, const demo::RunOptions &options) {
  std::ostringstream oss;
  oss << "sample rejected";
  if (options.prefer_cpu.has_value()) {
    oss << ": wanted cpu " << *options.prefer_cpu << ", saw " << measurement.cpu_before << " -> "
        << measurement.cpu_after;
    return oss.str();
  }
  if (options.prefer_cpu_range.has_value()) {
    oss << ": wanted " << options.prefer_cpu_range->Description() << ", saw " << measurement.cpu_before
        << " -> " << measurement.cpu_after;
    return oss.str();
  }
  if (options.require_stable_cpu && measurement.cpu_before != measurement.cpu_after) {
    oss << ": cpu migrated " << measurement.cpu_before << " -> " << measurement.cpu_after;
    return oss.str();
  }
  if (options.require_active_pmu && !measurement.HasActiveConfigurableCounters()) {
    oss << ": configurable counters stayed inactive";
    return oss.str();
    }
  return oss.str();
}

PerfCounterSet CounterMeasurementSet(const demo::CounterDefinition &definition) {
  return CYCLES | INSTRUCTIONS | definition.counter;
}

void PrintWorkloadRunSummary(const WorkloadRunSummary &summary) {
  std::cout << "- workload: " << summary.workload->id << '\n';
  std::cout << "  " << summary.workload->summary << '\n';
  std::cout << "  repeats=" << summary.samples.size() << " attempts=" << summary.attempts_used
            << " mean_wall_ns=" << std::fixed << std::setprecision(0) << MeanWallNs(summary)
            << '\n';

  for (std::uint8_t i = 0; i < summary.measured_counters.count; ++i) {
    const PerfCounter counter = summary.measured_counters.items[i];
    const auto mean = MeanCounterValue(summary, counter);
    if (!mean.has_value()) {
      continue;
    }
    std::cout << "  " << CounterLabel(counter) << '=' << std::fixed << std::setprecision(2)
              << *mean << '\n';
  }
}

bool ExecuteWorkload(const demo::WorkloadDefinition &workload, PerfCounterSet measured_counters,
                     demo::DemoEnvironment &environment, const CliOptions &options,
                     WorkloadRunSummary &summary) {
  summary = {};
  summary.workload = &workload;
  summary.measured_counters = measured_counters;

  if (measured_counters.overflow) {
    summary.error = "requested measurement set exceeds PERF_MAX_SCOPE_EVENTS";
    return false;
  }

  const std::size_t warmups = options.warmup_override.value_or(workload.default_warmups);
  const std::size_t repeats = options.repeat_override.value_or(workload.default_repeats);

  for (std::size_t i = 0; i < warmups; ++i) {
    summary.last_sink = workload.run(environment);
  }

  for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
    bool captured = false;
    PerfMeasurement last_measurement;
    for (std::size_t attempt = 0; attempt < options.run_options.max_attempts; ++attempt) {
      ++summary.attempts_used;
      std::uint64_t sink = 0;
      PerfMeasurement measurement = PerfMeasure(measured_counters, [&] { sink = workload.run(environment); });
      summary.last_sink = sink;
      last_measurement = measurement;

      if (!measurement.valid) {
        summary.error = measurement.error;
        return false;
      }
      if (!demo::SampleMatches(measurement, options.run_options)) {
        summary.error = MismatchReason(measurement, options.run_options);
        continue;
      }

      summary.samples.push_back(measurement);
      captured = true;
      break;
    }
    if (!captured) {
      if (summary.error.empty()) {
        summary.error = MismatchReason(last_measurement, options.run_options);
      }
      return false;
    }
  }

  summary.valid = true;
  return true;
}

bool CompareCounterShowcase(const demo::CounterDefinition &definition, demo::DemoEnvironment &environment,
                            const CliOptions &options, CounterComparison &comparison,
                            std::string &error) {
  const demo::WorkloadDefinition *high = demo::FindWorkload(definition.high_demo_id);
  const demo::WorkloadDefinition *low = demo::FindWorkload(definition.low_demo_id);
  if (high == nullptr || low == nullptr) {
    error = std::string("counter '") + std::string(definition.name) +
            "' is missing a registered showcase workload";
    return false;
  }

  const PerfCounterSet measurement_set = CounterMeasurementSet(definition);
  if (!ExecuteWorkload(*high, measurement_set, environment, options, comparison.high)) {
    error = std::string("high showcase failed for ") + std::string(definition.name) + ": " +
            comparison.high.error;
    return false;
  }
  if (!ExecuteWorkload(*low, measurement_set, environment, options, comparison.low)) {
    error = std::string("low showcase failed for ") + std::string(definition.name) + ": " +
            comparison.low.error;
    return false;
  }

  const auto high_mean = MeanCounterValue(comparison.high, definition.counter);
  const auto low_mean = MeanCounterValue(comparison.low, definition.counter);
  if (!high_mean.has_value() || !low_mean.has_value()) {
    error = std::string("failed to compute mean values for counter ") + std::string(definition.name);
    return false;
  }

  comparison.high_mean = *high_mean;
  comparison.low_mean = *low_mean;
  if (comparison.low_mean == 0.0) {
    comparison.ratio = comparison.high_mean > 0.0 ? std::numeric_limits<double>::infinity() : 1.0;
  } else {
    comparison.ratio = comparison.high_mean / comparison.low_mean;
  }
  const double delta = comparison.high_mean - comparison.low_mean;
  comparison.validation_passed =
      comparison.ratio >= definition.validation.min_ratio &&
      delta >= static_cast<double>(definition.validation.min_delta);
  return true;
}

bool PrepareRunEnvironment(CliOptions &options, demo::DemoEnvironment &environment,
                           demo::PerfLevelLayout &layout, std::string &error) {
  if (!environment.Initialize(error)) {
    return false;
  }
  layout = demo::ReadPerfLevelLayout();
  if (!demo::ResolveCorePreference(options.run_options, layout, error)) {
    return false;
  }
  if (!demo::ApplySchedulerPreference(options.run_options, error)) {
    return false;
  }
  return true;
}

void PrintRunSettings(const CliOptions &options, const demo::PerfLevelLayout &layout) {
  std::cout << "Run settings:\n";
  std::cout << "- core preference: " << demo::ToString(options.run_options.core_preference) << '\n';
  if (options.run_options.prefer_cpu.has_value()) {
    std::cout << "- preferred cpu: " << *options.run_options.prefer_cpu << '\n';
  }
  if (options.run_options.prefer_cpu_range.has_value()) {
    std::cout << "- preferred cpu range: " << options.run_options.prefer_cpu_range->Description()
              << '\n';
  }
  std::cout << "- require stable cpu: "
            << (options.run_options.require_stable_cpu ? "yes" : "no") << '\n';
  std::cout << "- require active pmu: "
            << (options.run_options.require_active_pmu ? "yes" : "no") << '\n';
  if (!layout.entries.empty()) {
    std::cout << "- perflevel layout:";
    for (const auto &entry : layout.entries) {
      std::cout << ' ' << entry.heuristic_range.Description();
    }
    std::cout << '\n';
  }
}

int ListCounters(const CliOptions &options) {
  std::cout << "Counters:\n";
  for (const demo::CounterDefinition &definition : demo::Counters()) {
    if (!TierMatches(definition.tier, options.tier_filter) ||
        !GroupMatches(definition.group, options.group_filter)) {
      continue;
    }
    std::cout << "- " << definition.name << " [" << demo::ToString(definition.tier) << ", "
              << demo::ToString(definition.group) << "] " << definition.title << '\n';
  }
  return 0;
}

int ListDemos(const CliOptions &options) {
  std::cout << "Demos:\n";
  for (const demo::WorkloadDefinition &workload : demo::Workloads()) {
    if (!TierMatches(workload.tier, options.tier_filter) ||
        !GroupMatches(workload.group, options.group_filter)) {
      continue;
    }
    std::cout << "- " << workload.id << " [" << demo::ToString(workload.tier) << ", "
              << demo::ToString(workload.group) << "] " << workload.title << '\n';
  }
  return 0;
}

int ExplainCounter(const CliOptions &options) {
  const demo::CounterDefinition *definition = demo::FindCounter(options.target);
  if (definition == nullptr) {
    std::cerr << "unknown counter: " << options.target << '\n';
    return 1;
  }

  const demo::WorkloadDefinition *high = demo::FindWorkload(definition->high_demo_id);
  const demo::WorkloadDefinition *low = demo::FindWorkload(definition->low_demo_id);

  std::cout << definition->title << " (" << definition->name << ")\n";
  std::cout << "- tier: " << demo::ToString(definition->tier) << '\n';
  std::cout << "- group: " << demo::ToString(definition->group) << '\n';
  std::cout << "- description: " << definition->description << '\n';
  if (!definition->caveats.empty()) {
    std::cout << "- caveats: " << definition->caveats << '\n';
  }
  if (high != nullptr) {
    std::cout << "- expected high demo: " << high->id << " — " << high->summary << '\n';
  }
  if (low != nullptr) {
    std::cout << "- expected low demo: " << low->id << " — " << low->summary << '\n';
  }
  return 0;
}

int ExplainDemo(const CliOptions &options) {
  const demo::WorkloadDefinition *workload = demo::FindWorkload(options.target);
  if (workload == nullptr) {
    std::cerr << "unknown demo: " << options.target << '\n';
    return 1;
  }

  std::cout << workload->title << " (" << workload->id << ")\n";
  std::cout << "- tier: " << demo::ToString(workload->tier) << '\n';
  std::cout << "- group: " << demo::ToString(workload->group) << '\n';
  std::cout << "- summary: " << workload->summary << '\n';
  std::cout << "- mechanism: " << workload->mechanism << '\n';
  std::cout << "- default warmups: " << workload->default_warmups << '\n';
  std::cout << "- default repeats: " << workload->default_repeats << '\n';
  std::cout << "- expected counters:\n";
  for (const auto &expectation : workload->expectations) {
    std::cout << "  - " << expectation.counter_name << " -> " << expectation.level << ": "
              << expectation.note << '\n';
  }
  return 0;
}

int RunCounter(CliOptions options) {
  const demo::CounterDefinition *definition = demo::FindCounter(options.target);
  if (definition == nullptr) {
    std::cerr << "unknown counter: " << options.target << '\n';
    return 1;
  }

  demo::DemoEnvironment environment;
  demo::PerfLevelLayout layout;
  std::string error;
  if (!PrepareRunEnvironment(options, environment, layout, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  PrintRunSettings(options, layout);
  std::cout << '\n';
  std::cout << definition->title << " (" << definition->name << ")\n";
  std::cout << definition->description << '\n';
  if (!definition->caveats.empty()) {
    std::cout << "Caveat: " << definition->caveats << '\n';
  }
  std::cout << std::flush;

  CounterComparison comparison;
  if (!CompareCounterShowcase(*definition, environment, options, comparison, error)) {
    std::cout << "\nRun failed: " << error << '\n';
    return 1;
  }

  std::cout << "\nHigh showcase:\n";
  PrintWorkloadRunSummary(comparison.high);
  std::cout << "\nLow showcase:\n";
  PrintWorkloadRunSummary(comparison.low);

  std::cout << "\nInterpretation:\n";
  std::cout << "- expected high demo: " << definition->high_demo_id << '\n';
  std::cout << "- expected low demo: " << definition->low_demo_id << '\n';
  std::cout << "- mean " << definition->name << ": high=" << std::fixed << std::setprecision(2)
            << comparison.high_mean << " low=" << comparison.low_mean << '\n';
  if (std::isinf(comparison.ratio)) {
    std::cout << "- ratio: inf (low showcase stayed at zero)\n";
  } else {
    std::cout << "- ratio: " << std::fixed << std::setprecision(2) << comparison.ratio << "x\n";
  }
  if (comparison.validation_passed) {
    std::cout << "- verdict: PASS. The high/low pair separates cleanly for this counter.\n";
  } else {
    std::cout << "- verdict: WEAK. The current machine or run settings did not separate the pair enough.\n";
  }
  return comparison.validation_passed ? 0 : 1;
}

int RunDemo(CliOptions options) {
  const demo::WorkloadDefinition *workload = demo::FindWorkload(options.target);
  if (workload == nullptr) {
    std::cerr << "unknown demo: " << options.target << '\n';
    return 1;
  }

  demo::DemoEnvironment environment;
  demo::PerfLevelLayout layout;
  std::string error;
  if (!PrepareRunEnvironment(options, environment, layout, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  PrintRunSettings(options, layout);
  std::cout << '\n';
  std::cout << workload->title << " (" << workload->id << ")\n";
  std::cout << workload->summary << '\n';
  std::cout << workload->mechanism << '\n';
  std::cout << "\nExpected counter behavior:\n";
  for (const auto &expectation : workload->expectations) {
    std::cout << "- " << expectation.counter_name << " -> " << expectation.level << ": "
              << expectation.note << '\n';
  }
  std::cout << std::flush;

  WorkloadRunSummary summary;
  if (!ExecuteWorkload(*workload, workload->measurement_counters, environment, options, summary)) {
    std::cout << "\nRun failed: " << summary.error << '\n';
    return 1;
  }

  std::cout << "\nMeasured output:\n";
  PrintWorkloadRunSummary(summary);

  std::cout << "\nInterpretation:\n";
  std::cout << "- This demo is meant to explain a mechanism, not to score absolute performance.\n";
  std::cout << "- Compare the highlighted counters above against the expected high/low notes to understand what the code pattern is stressing.\n";
  return 0;
}

int Validate(CliOptions options) {
  options.tier_filter = TierFilter::Stable;
  options.run_options.require_active_pmu = true;
  options.run_options.require_stable_cpu = true;

  demo::DemoEnvironment environment;
  demo::PerfLevelLayout layout;
  std::string error;
  if (!PrepareRunEnvironment(options, environment, layout, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  PrintRunSettings(options, layout);
  std::cout << "\nValidating stable counters.\n";
  std::cout << std::flush;

  std::size_t failures = 0;
  for (const demo::CounterDefinition &definition : demo::Counters()) {
    if (definition.tier != demo::Tier::Stable || !GroupMatches(definition.group, options.group_filter)) {
      continue;
    }
    CounterComparison comparison;
    std::string comparison_error;
    if (!CompareCounterShowcase(definition, environment, options, comparison, comparison_error)) {
      ++failures;
      std::cout << "- " << definition.name << ": FAIL (" << comparison_error << ")\n";
      continue;
    }

    std::cout << "- " << definition.name << ": "
              << (comparison.validation_passed ? "PASS" : "FAIL") << " high="
              << std::fixed << std::setprecision(2) << comparison.high_mean << " low="
              << comparison.low_mean;
    if (std::isinf(comparison.ratio)) {
      std::cout << " ratio=inf";
    } else {
      std::cout << " ratio=" << comparison.ratio << "x";
    }
    std::cout << '\n';
    if (!comparison.validation_passed) {
      ++failures;
    }
  }

  if (failures != 0) {
    std::cout << "\nValidation failed: " << failures << " counter showcase(s) did not separate cleanly.\n";
    return 1;
  }

  std::cout << "\nValidation passed.\n";
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  CliOptions options;
  std::string error;
  if (!ParseArgs(argc, argv, options, error)) {
    std::cerr << error << '\n';
    PrintUsage();
    return 1;
  }

  switch (options.command) {
    case CommandKind::Help:
      PrintUsage();
      return 0;
    case CommandKind::ListCounters:
      return ListCounters(options);
    case CommandKind::ListDemos:
      return ListDemos(options);
    case CommandKind::ExplainCounter:
      return ExplainCounter(options);
    case CommandKind::ExplainDemo:
      return ExplainDemo(options);
    case CommandKind::RunCounter:
      return RunCounter(options);
    case CommandKind::RunDemo:
      return RunDemo(options);
    case CommandKind::Validate:
      return Validate(options);
  }

  return 1;
}
