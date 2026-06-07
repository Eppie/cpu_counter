#include "demos/catalog.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
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
  RunDemos,
  Summary,
  Diff,
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
  std::string secondary_target;
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

struct DemoContrast {
  const demo::WorkloadDefinition *workload = nullptr;
  WorkloadRunSummary summary;
};

struct DerivedMetricRow {
  std::string label;
  std::string value;
  std::string basis;
};

void ApplyMeasuredRunDefaults(CliOptions &options) {
  options.run_options.require_active_pmu = true;
  options.run_options.max_attempts = std::max<std::size_t>(options.run_options.max_attempts, 128);
}

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
  std::cout << "  cpu_counter run demos [--tier stable|experimental|all] [--group <group>] [--repeat N] [--warmup N] [--prefer-pcore|--prefer-ecore|--prefer-cpu N] [--require-stable-cpu] [--require-active-pmu]\n";
  std::cout << "  cpu_counter summary <perf.jsonl>\n";
  std::cout << "  cpu_counter diff <baseline.jsonl> <candidate.jsonl>\n";
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
    if (argc < 3) {
      error = "run requires 'counter <name>', 'demo <id>', or 'demos'";
      return false;
    }
    const std::string_view subject = argv[2];
    if (subject == "counter") {
      if (argc < 4) {
        error = "run counter requires a counter name";
        return false;
      }
      options.target = argv[3];
      options.command = CommandKind::RunCounter;
      return ParseFlags(argc, argv, 4, options, error);
    } else if (subject == "demo") {
      if (argc < 4) {
        error = "run demo requires a demo id";
        return false;
      }
      options.target = argv[3];
      options.command = CommandKind::RunDemo;
      return ParseFlags(argc, argv, 4, options, error);
    } else if (subject == "demos") {
      options.command = CommandKind::RunDemos;
      return ParseFlags(argc, argv, 3, options, error);
    } else {
      error = "run requires 'counter <name>', 'demo <id>', or 'demos'";
      return false;
    }
  }

  if (arg1 == "validate") {
    options.command = CommandKind::Validate;
    return ParseFlags(argc, argv, 2, options, error);
  }

  if (arg1 == "summary") {
    if (argc < 3) {
      error = "summary requires a JSONL path";
      return false;
    }
    options.command = CommandKind::Summary;
    options.target = argv[2];
    return ParseFlags(argc, argv, 3, options, error);
  }

  if (arg1 == "diff") {
    if (argc < 4) {
      error = "diff requires baseline and candidate JSONL paths";
      return false;
    }
    options.command = CommandKind::Diff;
    options.target = argv[2];
    options.secondary_target = argv[3];
    return ParseFlags(argc, argv, 4, options, error);
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

std::optional<PerfCounter> LookupCounter(std::string_view cli_name) {
  if (const demo::CounterDefinition *definition = demo::FindCounter(cli_name)) {
    return definition->counter;
  }
  return std::nullopt;
}

std::optional<double> MeanCatalogCounterValue(const WorkloadRunSummary &summary,
                                              std::string_view cli_name) {
  const auto counter = LookupCounter(cli_name);
  if (!counter.has_value()) {
    return std::nullopt;
  }
  return MeanCounterValue(summary, *counter);
}

std::optional<double> SumCatalogCounterValues(const WorkloadRunSummary &summary,
                                              std::initializer_list<std::string_view> cli_names) {
  double total = 0.0;
  bool any = false;
  for (const std::string_view cli_name : cli_names) {
    const auto value = MeanCatalogCounterValue(summary, cli_name);
    if (!value.has_value()) {
      continue;
    }
    total += *value;
    any = true;
  }
  if (!any) {
    return std::nullopt;
  }
  return total;
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

std::string AddThousandsSeparators(std::string digits) {
  if (digits.empty()) {
    return digits;
  }
  bool negative = false;
  if (digits[0] == '-') {
    negative = true;
    digits.erase(digits.begin());
  }
  const std::size_t dot = digits.find('.');
  std::string integer = dot == std::string::npos ? digits : digits.substr(0, dot);
  const std::string fraction = dot == std::string::npos ? std::string() : digits.substr(dot);

  std::string grouped;
  grouped.reserve(integer.size() + integer.size() / 3);
  for (std::size_t i = 0; i < integer.size(); ++i) {
    if (i > 0 && ((integer.size() - i) % 3 == 0)) {
      grouped.push_back(',');
    }
    grouped.push_back(integer[i]);
  }
  if (negative) {
    grouped.insert(grouped.begin(), '-');
  }
  grouped += fraction;
  return grouped;
}

std::string FormatDouble(double value, int precision = 2) {
  if (std::isinf(value)) {
    return value < 0.0 ? "-inf" : "inf";
  }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(precision) << value;
  return AddThousandsSeparators(oss.str());
}

std::string FormatInteger(std::uint64_t value) {
  return AddThousandsSeparators(std::to_string(value));
}

std::string FormatWallNs(double ns) {
  std::ostringstream oss;
  oss << FormatDouble(ns, 0) << " ns";
  if (ns >= 1'000'000.0) {
    oss << " (" << FormatDouble(ns / 1'000'000.0, 3) << " ms)";
  } else if (ns >= 1'000.0) {
    oss << " (" << FormatDouble(ns / 1'000.0, 3) << " us)";
  }
  return oss.str();
}

struct JsonValue {
  enum class Type {
    Null,
    Bool,
    Number,
    String,
    Object,
    Array,
  };

  Type type = Type::Null;
  bool bool_value = false;
  std::string number_text;
  std::string string_value;
  std::vector<std::pair<std::string, JsonValue>> object_value;
  std::vector<JsonValue> array_value;
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view text) : text_(text) {}

  std::optional<JsonValue> Parse(std::string &error) {
    SkipWhitespace();
    auto value = ParseValue(error);
    if (!value.has_value()) {
      return std::nullopt;
    }
    SkipWhitespace();
    if (pos_ != text_.size()) {
      error = "trailing characters after JSON value";
      return std::nullopt;
    }
    return value;
  }

 private:
  std::string_view text_;
  std::size_t pos_ = 0;

  void SkipWhitespace() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
        break;
      }
      ++pos_;
    }
  }

  bool Consume(char expected) {
    SkipWhitespace();
    if (pos_ >= text_.size() || text_[pos_] != expected) {
      return false;
    }
    ++pos_;
    return true;
  }

  bool ConsumeLiteral(std::string_view literal) {
    SkipWhitespace();
    if (text_.substr(pos_, literal.size()) != literal) {
      return false;
    }
    pos_ += literal.size();
    return true;
  }

  std::optional<JsonValue> ParseValue(std::string &error) {
    SkipWhitespace();
    if (pos_ >= text_.size()) {
      error = "unexpected end of JSON";
      return std::nullopt;
    }

    const char c = text_[pos_];
    if (c == '"') {
      JsonValue value;
      value.type = JsonValue::Type::String;
      auto parsed = ParseString(error);
      if (!parsed.has_value()) {
        return std::nullopt;
      }
      value.string_value = std::move(*parsed);
      return value;
    }
    if (c == '{') {
      return ParseObject(error);
    }
    if (c == '[') {
      return ParseArray(error);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      return ParseNumber(error);
    }
    if (ConsumeLiteral("true")) {
      JsonValue value;
      value.type = JsonValue::Type::Bool;
      value.bool_value = true;
      return value;
    }
    if (ConsumeLiteral("false")) {
      JsonValue value;
      value.type = JsonValue::Type::Bool;
      value.bool_value = false;
      return value;
    }
    if (ConsumeLiteral("null")) {
      return JsonValue{};
    }

    error = "unexpected JSON token";
    return std::nullopt;
  }

  std::optional<std::string> ParseString(std::string &error) {
    if (!Consume('"')) {
      error = "expected JSON string";
      return std::nullopt;
    }
    std::string out;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') {
        return out;
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) {
        error = "unterminated JSON escape";
        return std::nullopt;
      }
      const char escaped = text_[pos_++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          out.push_back(escaped);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u':
          if (pos_ + 4 > text_.size()) {
            error = "truncated JSON unicode escape";
            return std::nullopt;
          }
          out.push_back('?');
          pos_ += 4;
          break;
        default:
          error = "unknown JSON escape";
          return std::nullopt;
      }
    }
    error = "unterminated JSON string";
    return std::nullopt;
  }

  std::optional<JsonValue> ParseNumber(std::string &error) {
    const std::size_t start = pos_;
    if (text_[pos_] == '-') {
      ++pos_;
    }
    if (pos_ >= text_.size()) {
      error = "invalid JSON number";
      return std::nullopt;
    }
    if (text_[pos_] == '0') {
      ++pos_;
    } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
    } else {
      error = "invalid JSON number";
      return std::nullopt;
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
        error = "invalid JSON number fraction";
        return std::nullopt;
      }
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
        ++pos_;
      }
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
        error = "invalid JSON number exponent";
        return std::nullopt;
      }
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
    }

    JsonValue value;
    value.type = JsonValue::Type::Number;
    value.number_text = std::string(text_.substr(start, pos_ - start));
    return value;
  }

  std::optional<JsonValue> ParseObject(std::string &error) {
    if (!Consume('{')) {
      error = "expected JSON object";
      return std::nullopt;
    }
    JsonValue value;
    value.type = JsonValue::Type::Object;
    SkipWhitespace();
    if (Consume('}')) {
      return value;
    }
    while (true) {
      auto key = ParseString(error);
      if (!key.has_value()) {
        return std::nullopt;
      }
      if (!Consume(':')) {
        error = "expected ':' after JSON object key";
        return std::nullopt;
      }
      auto child = ParseValue(error);
      if (!child.has_value()) {
        return std::nullopt;
      }
      value.object_value.push_back({std::move(*key), std::move(*child)});
      if (Consume('}')) {
        return value;
      }
      if (!Consume(',')) {
        error = "expected ',' in JSON object";
        return std::nullopt;
      }
    }
  }

  std::optional<JsonValue> ParseArray(std::string &error) {
    if (!Consume('[')) {
      error = "expected JSON array";
      return std::nullopt;
    }
    JsonValue value;
    value.type = JsonValue::Type::Array;
    SkipWhitespace();
    if (Consume(']')) {
      return value;
    }
    while (true) {
      auto child = ParseValue(error);
      if (!child.has_value()) {
        return std::nullopt;
      }
      value.array_value.push_back(std::move(*child));
      if (Consume(']')) {
        return value;
      }
      if (!Consume(',')) {
        error = "expected ',' in JSON array";
        return std::nullopt;
      }
    }
  }
};

const JsonValue *JsonFind(const JsonValue &object, std::string_view key) {
  if (object.type != JsonValue::Type::Object) {
    return nullptr;
  }
  for (const auto &entry : object.object_value) {
    if (entry.first == key) {
      return &entry.second;
    }
  }
  return nullptr;
}

std::optional<std::string> JsonString(const JsonValue &object, std::string_view key) {
  const JsonValue *value = JsonFind(object, key);
  if (value == nullptr || value->type != JsonValue::Type::String) {
    return std::nullopt;
  }
  return value->string_value;
}

std::optional<double> JsonDouble(const JsonValue &object, std::string_view key) {
  const JsonValue *value = JsonFind(object, key);
  if (value == nullptr || value->type != JsonValue::Type::Number) {
    return std::nullopt;
  }
  try {
    return std::stod(value->number_text);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::uint64_t> JsonUInt(const JsonValue &object, std::string_view key) {
  const JsonValue *value = JsonFind(object, key);
  if (value == nullptr || value->type != JsonValue::Type::Number) {
    return std::nullopt;
  }
  try {
    return static_cast<std::uint64_t>(std::stoull(value->number_text));
  } catch (...) {
    return std::nullopt;
  }
}

struct JsonCounterAggregate {
  std::string name;
  std::string id;
  double total = 0.0;
  double min = 0.0;
  double max = 0.0;
  double mean = 0.0;
  std::optional<double> p50;
  std::optional<double> p95;
  std::optional<double> p99;
};

struct JsonScopeAggregate {
  std::string label;
  std::uint64_t sample_every = 1;
  std::uint64_t sampled_count = 0;
  std::uint64_t estimated_count = 0;
  std::uint64_t dropped_count = 0;
  double wall_total_ns = 0.0;
  double wall_mean_ns = 0.0;
  std::optional<double> wall_p50_ns;
  std::optional<double> wall_p95_ns;
  std::optional<double> wall_p99_ns;
  std::vector<JsonCounterAggregate> counters;
};

bool ParseScopeAggregateLine(std::string_view line, std::size_t line_number,
                             JsonScopeAggregate &record, std::string &error) {
  JsonParser parser(line);
  auto parsed = parser.Parse(error);
  if (!parsed.has_value()) {
    error = "line " + std::to_string(line_number) + ": " + error;
    return false;
  }
  if (parsed->type != JsonValue::Type::Object) {
    error = "line " + std::to_string(line_number) + ": expected JSON object";
    return false;
  }
  if (const auto type = JsonString(*parsed, "type");
      type.has_value() && *type != "scope_aggregate") {
    return true;
  }

  const auto label = JsonString(*parsed, "label");
  if (!label.has_value()) {
    error = "line " + std::to_string(line_number) + ": missing label";
    return false;
  }
  record = {};
  record.label = *label;
  record.sample_every = JsonUInt(*parsed, "sample_every").value_or(1);
  record.sampled_count = JsonUInt(*parsed, "sampled_count").value_or(0);
  record.estimated_count = JsonUInt(*parsed, "estimated_count")
                                .value_or(record.sampled_count * record.sample_every);
  record.dropped_count = JsonUInt(*parsed, "dropped_count").value_or(0);

  if (const JsonValue *wall = JsonFind(*parsed, "wall_ns")) {
    record.wall_total_ns = JsonDouble(*wall, "total").value_or(0.0);
    record.wall_mean_ns = JsonDouble(*wall, "mean").value_or(0.0);
    record.wall_p50_ns = JsonDouble(*wall, "p50");
    record.wall_p95_ns = JsonDouble(*wall, "p95");
    record.wall_p99_ns = JsonDouble(*wall, "p99");
  }

  const JsonValue *counters = JsonFind(*parsed, "counters");
  if (counters == nullptr || counters->type != JsonValue::Type::Array) {
    return true;
  }
  for (const JsonValue &counter_value : counters->array_value) {
    if (counter_value.type != JsonValue::Type::Object) {
      continue;
    }
    JsonCounterAggregate counter;
    counter.name = JsonString(counter_value, "name").value_or("");
    counter.id = JsonString(counter_value, "id").value_or(counter.name);
    counter.total = JsonDouble(counter_value, "total").value_or(0.0);
    counter.min = JsonDouble(counter_value, "min").value_or(0.0);
    counter.max = JsonDouble(counter_value, "max").value_or(0.0);
    counter.mean = JsonDouble(counter_value, "mean").value_or(
        record.sampled_count != 0 ? counter.total / static_cast<double>(record.sampled_count)
                                  : 0.0);
    counter.p50 = JsonDouble(counter_value, "p50");
    counter.p95 = JsonDouble(counter_value, "p95");
    counter.p99 = JsonDouble(counter_value, "p99");
    record.counters.push_back(std::move(counter));
  }
  return true;
}

bool LoadScopeAggregates(const std::string &path, std::vector<JsonScopeAggregate> &records,
                         std::string &error) {
  std::ifstream file(path);
  if (!file) {
    error = "failed to open " + path;
    return false;
  }

  records.clear();
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(file, line)) {
    ++line_number;
    if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
      continue;
    }
    JsonScopeAggregate record;
    if (!ParseScopeAggregateLine(line, line_number, record, error)) {
      return false;
    }
    if (!record.label.empty()) {
      records.push_back(std::move(record));
    }
  }
  return true;
}

bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.substr(text.size() - suffix.size(), suffix.size()) == suffix;
}

bool CounterMatches(const JsonCounterAggregate &counter, std::string_view key) {
  if (counter.name == key || counter.id == key) {
    return true;
  }
  const std::string suffix = ":" + std::string(key);
  return EndsWith(counter.id, suffix);
}

const JsonCounterAggregate *FindCounterAggregate(
    const JsonScopeAggregate &record, std::initializer_list<std::string_view> keys) {
  for (const std::string_view key : keys) {
    for (const JsonCounterAggregate &counter : record.counters) {
      if (CounterMatches(counter, key)) {
        return &counter;
      }
    }
  }
  return nullptr;
}

double SumCounterMeans(const JsonScopeAggregate &record,
                       std::initializer_list<std::string_view> keys) {
  double total = 0.0;
  for (const std::string_view key : keys) {
    if (const JsonCounterAggregate *counter = FindCounterAggregate(record, {key})) {
      total += counter->mean;
    }
  }
  return total;
}

struct AnalysisRow {
  std::string key;
  std::string label;
  std::uint64_t calls = 0;
  std::uint64_t samples = 0;
  std::uint64_t dropped = 0;
  double wall_ns_per_call = 0.0;
  double cycles_per_call = 0.0;
  double estimated_cycles = 0.0;
  double ipc = 0.0;
  double l1_miss_per_kinst = 0.0;
  double tlb_per_kinst = 0.0;
  std::optional<double> cycle_p50;
  std::optional<double> cycle_p95;
  bool has_cycles = false;
  bool has_instructions = false;
  bool has_ipc = false;
  bool has_instruction_metrics = false;
};

std::string ScopeAggregateKey(const JsonScopeAggregate &record) {
  std::vector<std::string> counter_ids;
  counter_ids.reserve(record.counters.size());
  for (const JsonCounterAggregate &counter : record.counters) {
    counter_ids.push_back(counter.id.empty() ? counter.name : counter.id);
  }
  std::sort(counter_ids.begin(), counter_ids.end());
  std::ostringstream oss;
  oss << record.label << "::sample_every=" << record.sample_every << "::";
  for (std::size_t i = 0; i < counter_ids.size(); ++i) {
    if (i != 0) {
      oss << '|';
    }
    oss << counter_ids[i];
  }
  return oss.str();
}

AnalysisRow BuildAnalysisRow(const JsonScopeAggregate &record) {
  AnalysisRow row;
  row.key = ScopeAggregateKey(record);
  row.label = record.label;
  row.calls = record.estimated_count != 0 ? record.estimated_count : record.sampled_count;
  row.samples = record.sampled_count;
  row.dropped = record.dropped_count;
  row.wall_ns_per_call = record.wall_mean_ns;

  const JsonCounterAggregate *cycles = FindCounterAggregate(record, {"FIXED_CYCLES"});
  const JsonCounterAggregate *instructions = FindCounterAggregate(record, {"FIXED_INSTRUCTIONS"});
  row.has_cycles = cycles != nullptr;
  row.has_instructions = instructions != nullptr;
  if (cycles != nullptr) {
    row.cycles_per_call = cycles->mean;
    row.estimated_cycles = cycles->mean * static_cast<double>(std::max<std::uint64_t>(1, row.calls));
    row.cycle_p50 = cycles->p50;
    row.cycle_p95 = cycles->p95;
  }
  if (cycles != nullptr && instructions != nullptr && cycles->mean > 0.0) {
    row.ipc = instructions->mean / cycles->mean;
    row.has_ipc = true;
  }
  if (instructions != nullptr && instructions->mean > 0.0) {
    const double l1_misses = SumCounterMeans(
        record, {"L1D_CACHE_MISS_LD", "L1D_CACHE_MISS_ST", "L1D_CACHE_MISS_LD_NONSPEC",
                 "L1D_CACHE_MISS_ST_NONSPEC"});
    const double tlb_pressure = SumCounterMeans(
        record, {"MMU_TABLE_WALK_DATA", "MMU_TABLE_WALK_INSTRUCTION", "L2_TLB_MISS_DATA",
                 "L2_TLB_MISS_INSTRUCTION", "L1D_TLB_MISS", "L1I_TLB_MISS_DEMAND"});
    row.l1_miss_per_kinst = (l1_misses * 1000.0) / instructions->mean;
    row.tlb_per_kinst = (tlb_pressure * 1000.0) / instructions->mean;
    row.has_instruction_metrics = true;
  }
  return row;
}

std::vector<AnalysisRow> BuildAnalysisRows(const std::vector<JsonScopeAggregate> &records) {
  std::vector<AnalysisRow> rows;
  rows.reserve(records.size());
  for (const JsonScopeAggregate &record : records) {
    rows.push_back(BuildAnalysisRow(record));
  }
  std::sort(rows.begin(), rows.end(), [](const AnalysisRow &lhs, const AnalysisRow &rhs) {
    const double lhs_hotness = lhs.has_cycles ? lhs.estimated_cycles : lhs.wall_ns_per_call * lhs.calls;
    const double rhs_hotness = rhs.has_cycles ? rhs.estimated_cycles : rhs.wall_ns_per_call * rhs.calls;
    return lhs_hotness > rhs_hotness;
  });
  return rows;
}

std::string FormatOptionalDouble(std::optional<double> value, int precision = 2) {
  if (!value.has_value()) {
    return "-";
  }
  return FormatDouble(*value, precision);
}

std::string FormatMaybeDouble(double value, bool present, int precision = 2) {
  return present ? FormatDouble(value, precision) : std::string("-");
}

std::string FormatSignedPercent(double candidate, double baseline) {
  if (baseline == 0.0) {
    return candidate == 0.0 ? std::string("+0.00%") : std::string("inf");
  }
  const double pct = ((candidate - baseline) * 100.0) / baseline;
  return std::string(pct >= 0.0 ? "+" : "") + FormatDouble(pct, 2) + "%";
}

double Ratio(double numerator, double denominator) {
  if (denominator == 0.0) {
    return numerator > 0.0 ? std::numeric_limits<double>::infinity() : 1.0;
  }
  return numerator / denominator;
}

std::string FormatRatio(double numerator, double denominator, int precision = 2) {
  return FormatDouble(Ratio(numerator, denominator), precision) + "x";
}

std::string_view TrimBlock(std::string_view block) {
  while (!block.empty() && (block.front() == '\n' || block.front() == '\r')) {
    block.remove_prefix(1);
  }
  while (!block.empty() && (block.back() == '\n' || block.back() == '\r')) {
    block.remove_suffix(1);
  }
  return block;
}

void PrintIndentedBlock(std::string_view block, std::string_view indent = "  ") {
  block = TrimBlock(block);
  std::size_t start = 0;
  while (start <= block.size()) {
    const std::size_t end = block.find('\n', start);
    const std::string_view line =
        end == std::string_view::npos ? block.substr(start) : block.substr(start, end - start);
    std::cout << indent << line << '\n';
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
}

const demo::WorkloadExpectation *FindExpectation(const demo::WorkloadDefinition &workload,
                                                 PerfCounter counter) {
  const std::string label = CounterLabel(counter);
  for (const auto &expectation : workload.expectations) {
    if (expectation.counter_name == label) {
      return &expectation;
    }
  }
  return nullptr;
}

std::string ExpectationDisplay(const demo::WorkloadExpectation &expectation) {
  switch (expectation.kind) {
    case demo::ExpectationKind::ApproximateValue: {
      std::string label = "≈" + FormatDouble(expectation.expected_value, 0);
      if (expectation.tolerance_fraction > 0.0) {
        label += " ±" + FormatDouble(expectation.tolerance_fraction * 100.0, 1) + "%";
      }
      return label;
    }
    case demo::ExpectationKind::NearZero:
      return "<= " + FormatDouble(expectation.expected_value, 0);
    case demo::ExpectationKind::Relative:
    default:
      return std::string(expectation.level);
  }
}

std::optional<std::string> ExpectationObservedSummary(const demo::WorkloadExpectation &expectation,
                                                      double measured) {
  switch (expectation.kind) {
    case demo::ExpectationKind::ApproximateValue: {
      if (expectation.expected_value <= 0.0) {
        return std::nullopt;
      }
      const double delta = measured - expectation.expected_value;
      const double error_pct = (delta * 100.0) / expectation.expected_value;
      std::ostringstream oss;
      oss << "measured " << FormatDouble(measured) << "; expected "
          << ExpectationDisplay(expectation) << "; error="
          << (error_pct >= 0.0 ? "+" : "") << FormatDouble(error_pct, 3) << '%';
      return oss.str();
    }
    case demo::ExpectationKind::NearZero: {
      std::ostringstream oss;
      oss << "measured " << FormatDouble(measured) << "; expected "
          << ExpectationDisplay(expectation);
      return oss.str();
    }
    case demo::ExpectationKind::Relative:
    default:
      return std::nullopt;
  }
}

std::optional<std::string> ExpectationWarningReason(const demo::WorkloadExpectation &expectation,
                                                    PerfCounter counter, double measured,
                                                    const DemoContrast *contrast,
                                                    std::optional<double> contrast_value) {
  switch (expectation.kind) {
    case demo::ExpectationKind::ApproximateValue: {
      if (expectation.expected_value <= 0.0) {
        return std::nullopt;
      }
      const double tolerance = std::abs(expectation.expected_value) * expectation.tolerance_fraction;
      if (std::abs(measured - expectation.expected_value) <= tolerance) {
        return std::nullopt;
      }
      std::ostringstream oss;
      oss << "measured " << FormatDouble(measured) << " outside expected band "
          << ExpectationDisplay(expectation);
      return oss.str();
    }
    case demo::ExpectationKind::NearZero:
      if (measured <= expectation.expected_value) {
        return std::nullopt;
      }
      return "measured " + FormatDouble(measured) + " above expected near-zero bound " +
             ExpectationDisplay(expectation);
    case demo::ExpectationKind::Relative:
    default:
      break;
  }

  if (contrast == nullptr || contrast->workload == nullptr) {
    if (expectation.level == "high" && measured == 0.0) {
      return std::string("expected-high counter stayed at zero");
    }
    return std::nullopt;
  }
  if (!contrast_value.has_value()) {
    return std::nullopt;
  }
  const demo::WorkloadExpectation *contrast_expectation =
      FindExpectation(*contrast->workload, counter);
  if (contrast_expectation == nullptr || contrast_expectation->level == expectation.level) {
    return std::nullopt;
  }
  if (expectation.level == "high" && measured <= *contrast_value) {
    return std::string("expected-high counter did not exceed the contrast workload");
  }
  if (expectation.level == "low" && measured >= *contrast_value) {
    return std::string("expected-low counter did not stay below the contrast workload");
  }
  return std::nullopt;
}

std::optional<double> MeanIpc(const WorkloadRunSummary &summary) {
  const auto cycles = MeanCounterValue(summary, CYCLES);
  const auto instructions = MeanCounterValue(summary, INSTRUCTIONS);
  if (!cycles.has_value() || !instructions.has_value() || *cycles <= 0.0) {
    return std::nullopt;
  }
  return *instructions / *cycles;
}

std::optional<double> MeanCpi(const WorkloadRunSummary &summary) {
  const auto cycles = MeanCounterValue(summary, CYCLES);
  const auto instructions = MeanCounterValue(summary, INSTRUCTIONS);
  if (!cycles.has_value() || !instructions.has_value() || *instructions <= 0.0) {
    return std::nullopt;
  }
  return *cycles / *instructions;
}

std::vector<DerivedMetricRow> ComputeDerivedMetrics(const WorkloadRunSummary &summary) {
  std::vector<DerivedMetricRow> metrics;

  const auto cycles = MeanCounterValue(summary, CYCLES);
  const auto instructions = MeanCounterValue(summary, INSTRUCTIONS);
  const auto branches = MeanCounterValue(summary, BRANCHES);
  const auto branch_miss = MeanCounterValue(summary, BRANCH_MISS);
  const auto cond_branches = MeanCatalogCounterValue(summary, "inst-branch-cond");
  const auto cond_branch_miss = MeanCatalogCounterValue(summary, "branch-cond-miss");
  const auto taken_branches = MeanCatalogCounterValue(summary, "inst-branch-taken");

  const auto l1_load_miss = MeanCounterValue(summary, L1_LOAD_MISS);
  const auto l1_store_miss = MeanCounterValue(summary, L1_STORE_MISS);
  const auto dtlb_miss = MeanCounterValue(summary, DTLB_MISS);
  const auto dtlb_miss_nonspec = MeanCatalogCounterValue(summary, "dtlb-miss-nonspec");
  const auto dtlb_access = MeanCatalogCounterValue(summary, "dtlb-access");
  const auto dtlb_fill = MeanCatalogCounterValue(summary, "dtlb-fill");
  const auto l2_dtlb_miss = MeanCounterValue(summary, L2_TLB_MISS);
  const auto data_page_walk = MeanCatalogCounterValue(summary, "mmu-table-walk-data");

  const auto itlb_miss = MeanCounterValue(summary, ITLB_MISS);
  const auto l1i_cache_miss = MeanCatalogCounterValue(summary, "l1i-cache-miss");
  const auto l1i_tlb_fill = MeanCatalogCounterValue(summary, "l1i-tlb-fill");
  const auto l2_itlb_miss = MeanCatalogCounterValue(summary, "l2-tlb-miss-instruction");
  const auto instruction_page_walk = MeanCatalogCounterValue(summary, "mmu-table-walk-instruction");

  const auto inst_int_ld = MeanCatalogCounterValue(summary, "inst-int-ld");
  const auto inst_simd_ld = MeanCatalogCounterValue(summary, "inst-simd-ld");
  const auto inst_int_st = MeanCatalogCounterValue(summary, "inst-int-st");
  const auto inst_simd_st = MeanCatalogCounterValue(summary, "inst-simd-st");
  const auto inst_ldst = MeanCatalogCounterValue(summary, "inst-ldst");
  const auto load_ops = SumCatalogCounterValues(summary, {"inst-int-ld", "inst-simd-ld"});
  const auto store_ops = SumCatalogCounterValues(summary, {"inst-int-st", "inst-simd-st"});

  const auto ldst_x64 = MeanCatalogCounterValue(summary, "ldst-x64-uop");
  const auto ldst_xpg = MeanCatalogCounterValue(summary, "ldst-xpg-uop");
  const auto ld_nt_uop = MeanCatalogCounterValue(summary, "ld-nt-uop");
  const auto st_nt_uop = MeanCatalogCounterValue(summary, "st-nt-uop");

  const auto atomic_succ = MeanCatalogCounterValue(summary, "atomic-succ");
  const auto atomic_fail = MeanCatalogCounterValue(summary, "atomic-fail");
  const auto inst_barrier = MeanCatalogCounterValue(summary, "inst-barrier");
  const auto interrupt_pending = MeanCatalogCounterValue(summary, "interrupt-pending");

  const auto core_active = MeanCatalogCounterValue(summary, "core-active-cycle");
  const auto retire_uop = MeanCatalogCounterValue(summary, "retire-uop");
  const auto map_uop = MeanCatalogCounterValue(summary, "map-uop");
  const auto map_int_uop = MeanCatalogCounterValue(summary, "map-int-uop");
  const auto map_ldst_uop = MeanCatalogCounterValue(summary, "map-ldst-uop");
  const auto map_simd_uop = MeanCatalogCounterValue(summary, "map-simd-uop");

  const auto fetch_restart = MeanCatalogCounterValue(summary, "fetch-restart");
  const auto flush_restart_other = MeanCatalogCounterValue(summary, "flush-restart-other");

  const auto add_value = [&](std::string_view label, double value, int precision,
                             std::string_view basis) {
    metrics.push_back({std::string(label), FormatDouble(value, precision), std::string(basis)});
  };
  const auto add_ratio = [&](std::string_view label, const std::optional<double> &numerator,
                             const std::optional<double> &denominator, int precision,
                             std::string_view basis) {
    if (!numerator.has_value() || !denominator.has_value() || *denominator <= 0.0) {
      return;
    }
    add_value(label, *numerator / *denominator, precision, basis);
  };
  const auto add_per_k = [&](std::string_view label, const std::optional<double> &numerator,
                             const std::optional<double> &denominator, int precision,
                             std::string_view basis) {
    if (!numerator.has_value() || !denominator.has_value() || *denominator <= 0.0) {
      return;
    }
    add_value(label, (*numerator * 1000.0) / *denominator, precision, basis);
  };
  const auto add_per_m = [&](std::string_view label, const std::optional<double> &numerator,
                             const std::optional<double> &denominator, int precision,
                             std::string_view basis) {
    if (!numerator.has_value() || !denominator.has_value() || *denominator <= 0.0) {
      return;
    }
    add_value(label, (*numerator * 1'000'000.0) / *denominator, precision, basis);
  };
  const auto add_percent = [&](std::string_view label, const std::optional<double> &numerator,
                               const std::optional<double> &denominator, int precision,
                               std::string_view basis) {
    if (!numerator.has_value() || !denominator.has_value() || *denominator <= 0.0) {
      return;
    }
    metrics.push_back(
        {std::string(label), FormatDouble((*numerator * 100.0) / *denominator, precision) + "%",
         std::string(basis)});
  };

  add_ratio("IPC", instructions, cycles, 3, "instructions / cycle");
  add_ratio("CPI", cycles, instructions, 3, "cycles / instruction");
  add_per_k("branches / kinst", branches, instructions, 3, "per 1k instructions");
  add_per_k("branch miss / kbranch", branch_miss, branches, 3, "per 1k retired branches");
  add_per_k("cond branch miss / kcond", cond_branch_miss, cond_branches, 3,
            "per 1k conditional branches");
  add_per_k("taken branch / kbranch", taken_branches, branches, 3, "per 1k retired branches");

  if (load_ops.has_value()) {
    add_per_k("L1 load miss / kload", l1_load_miss, load_ops, 3, "per 1k load instructions");
  } else {
    add_per_k("L1 load miss / kinst", l1_load_miss, instructions, 3, "per 1k instructions");
  }
  if (store_ops.has_value()) {
    add_per_k("L1 store miss / kstore", l1_store_miss, store_ops, 3, "per 1k store instructions");
  } else {
    add_per_k("L1 store miss / kinst", l1_store_miss, instructions, 3, "per 1k instructions");
  }

  add_per_k("int load / kinst", inst_int_ld, instructions, 3, "per 1k instructions");
  add_per_k("SIMD load / kinst", inst_simd_ld, instructions, 3, "per 1k instructions");
  add_per_k("int store / kinst", inst_int_st, instructions, 3, "per 1k instructions");
  add_per_k("SIMD store / kinst", inst_simd_st, instructions, 3, "per 1k instructions");
  add_per_k("LD/ST inst / kinst", inst_ldst, instructions, 3, "per 1k instructions");

  if (inst_int_ld.has_value()) {
    add_per_k("NT load uop / kscalar-load", ld_nt_uop, inst_int_ld, 3,
              "per 1k scalar load instructions");
  } else {
    add_per_k("NT load uop / kinst", ld_nt_uop, instructions, 3, "per 1k instructions");
  }
  if (inst_int_st.has_value()) {
    add_per_k("NT store uop / kscalar-store", st_nt_uop, inst_int_st, 3,
              "per 1k scalar store instructions");
  } else {
    add_per_k("NT store uop / kinst", st_nt_uop, instructions, 3, "per 1k instructions");
  }

  if (dtlb_access.has_value()) {
    add_per_k("DTLB miss / kaccess", dtlb_miss, dtlb_access, 3, "per 1k DTLB accesses");
    add_per_k("DTLB miss nonspec / kaccess", dtlb_miss_nonspec, dtlb_access, 3,
              "per 1k DTLB accesses");
    add_per_k("DTLB fill / kaccess", dtlb_fill, dtlb_access, 3, "per 1k DTLB accesses");
  } else {
    add_per_k("DTLB miss / kinst", dtlb_miss, instructions, 3, "per 1k instructions");
    add_per_k("DTLB miss nonspec / kinst", dtlb_miss_nonspec, instructions, 3,
              "per 1k instructions");
    add_per_k("DTLB fill / kinst", dtlb_fill, instructions, 3, "per 1k instructions");
  }
  add_per_k("L2 DTLB miss / kDTLB-miss", l2_dtlb_miss, dtlb_miss, 3,
            "per 1k first-level DTLB misses");
  add_per_k("data walk / kDTLB-miss", data_page_walk, dtlb_miss, 3,
            "per 1k first-level DTLB misses");

  add_per_k("ITLB miss / kinst", itlb_miss, instructions, 3, "per 1k instructions");
  add_per_k("L1I miss / kinst", l1i_cache_miss, instructions, 3, "per 1k instructions");
  add_per_k("L1I fill / kinst", l1i_tlb_fill, instructions, 3, "per 1k instructions");
  add_per_k("L2 ITLB miss / kITLB-miss", l2_itlb_miss, itlb_miss, 3,
            "per 1k first-level ITLB misses");
  add_per_k("instruction walk / kITLB-miss", instruction_page_walk, itlb_miss, 3,
            "per 1k first-level ITLB misses");

  if (inst_ldst.has_value()) {
    add_per_k("x64 split / kldst", ldst_x64, inst_ldst, 3, "per 1k load/store instructions");
    add_per_k("xpage split / kldst", ldst_xpg, inst_ldst, 3, "per 1k load/store instructions");
  } else {
    add_per_k("x64 split / kinst", ldst_x64, instructions, 3, "per 1k instructions");
    add_per_k("xpage split / kinst", ldst_xpg, instructions, 3, "per 1k instructions");
  }

  add_ratio("atomic fail / success", atomic_fail, atomic_succ, 3, "failed atomics / successful atomics");
  if (atomic_fail.has_value() && atomic_succ.has_value() &&
      (*atomic_fail + *atomic_succ) > 0.0) {
    metrics.push_back({"atomic fail rate",
                       FormatDouble((*atomic_fail * 100.0) / (*atomic_fail + *atomic_succ), 2) + "%",
                       "failed atomics / total atomics"});
  }

  add_per_k("barrier / kinst", inst_barrier, instructions, 3, "per 1k instructions");
  add_per_m("interrupt pending / Minst", interrupt_pending, instructions, 3,
            "per 1M instructions");

  add_percent("core active / cycle", core_active, cycles, 2, "active cycles as a share of total cycles");
  add_ratio("retire uop / inst", retire_uop, instructions, 3, "retired uops / instructions");
  add_ratio("map uop / inst", map_uop, instructions, 3, "mapped uops / instructions");
  add_percent("map int share", map_int_uop, map_uop, 2, "integer-class uops as a share of mapped uops");
  add_percent("map ldst share", map_ldst_uop, map_uop, 2, "load/store uops as a share of mapped uops");
  add_percent("map SIMD share", map_simd_uop, map_uop, 2, "SIMD uops as a share of mapped uops");

  add_per_k("fetch restart / kinst", fetch_restart, instructions, 3, "per 1k instructions");
  add_per_k("flush restart / kinst", flush_restart_other, instructions, 3,
            "per 1k instructions");

  return metrics;
}

void PrintDerivedMetrics(const std::vector<DerivedMetricRow> &metrics, std::string_view indent) {
  if (metrics.empty()) {
    return;
  }
  std::size_t label_width = std::string("metric").size();
  std::size_t value_width = std::string("value").size();
  std::size_t basis_width = std::string("basis").size();
  for (const DerivedMetricRow &metric : metrics) {
    label_width = std::max(label_width, metric.label.size());
    value_width = std::max(value_width, metric.value.size());
    basis_width = std::max(basis_width, metric.basis.size());
  }

  std::cout << indent << "derived metrics:\n";
  std::cout << indent << "  " << std::left << std::setw(static_cast<int>(label_width)) << "metric"
            << "  " << std::right << std::setw(static_cast<int>(value_width)) << "value"
            << "  " << std::left << std::setw(static_cast<int>(basis_width)) << "basis" << '\n';
  for (const DerivedMetricRow &metric : metrics) {
    std::cout << indent << "  " << std::left << std::setw(static_cast<int>(label_width))
              << metric.label << "  " << std::right << std::setw(static_cast<int>(value_width))
              << metric.value << "  " << std::left << metric.basis << '\n';
  }
}

void PrintExpectationWarnings(const demo::WorkloadDefinition &workload,
                              const WorkloadRunSummary &summary,
                              const DemoContrast *contrast = nullptr) {
  bool printed = false;
  for (const auto &expectation : workload.expectations) {
    const demo::CounterDefinition *definition = demo::FindCounter(expectation.counter_name);
    if (definition == nullptr) {
      continue;
    }
    const auto primary = MeanCounterValue(summary, definition->counter);
    if (!primary.has_value()) {
      continue;
    }

    const auto other = contrast != nullptr && contrast->workload != nullptr
                           ? MeanCounterValue(contrast->summary, definition->counter)
                           : std::nullopt;
    const auto reason =
        ExpectationWarningReason(expectation, definition->counter, *primary, contrast, other);
    if (!reason.has_value()) {
      continue;
    }
    if (!printed) {
      std::cout << "- warnings:\n";
      printed = true;
    }
    std::cout << "  - " << expectation.counter_name << ": " << *reason << ".\n";
  }
}

std::string MismatchReason(const PerfMeasurement &measurement, const demo::RunOptions &options) {
  std::vector<std::string> reasons;
  if (options.prefer_cpu.has_value() &&
      (measurement.cpu_before != *options.prefer_cpu || measurement.cpu_after != *options.prefer_cpu)) {
    std::ostringstream oss;
    oss << "wanted cpu " << *options.prefer_cpu << ", saw " << measurement.cpu_before << " -> "
        << measurement.cpu_after;
    reasons.push_back(oss.str());
  }
  if (options.prefer_cpu_range.has_value() &&
      (!options.prefer_cpu_range->Contains(measurement.cpu_before) ||
       !options.prefer_cpu_range->Contains(measurement.cpu_after))) {
    std::ostringstream oss;
    oss << "wanted " << options.prefer_cpu_range->Description() << ", saw "
        << measurement.cpu_before << " -> " << measurement.cpu_after;
    reasons.push_back(oss.str());
  }
  if (options.require_stable_cpu && measurement.cpu_before != measurement.cpu_after) {
    std::ostringstream oss;
    oss << "cpu migrated " << measurement.cpu_before << " -> " << measurement.cpu_after;
    reasons.push_back(oss.str());
  }
  if (options.require_active_pmu && !measurement.HasActiveConfigurableCounters()) {
    reasons.push_back("configurable counters stayed inactive");
  }
  if (reasons.empty()) {
    return "sample rejected";
  }
  std::ostringstream oss;
  oss << "sample rejected: ";
  for (std::size_t i = 0; i < reasons.size(); ++i) {
    if (i != 0) {
      oss << "; ";
    }
    oss << reasons[i];
  }
  return oss.str();
}

PerfCounterSet CounterMeasurementSet(const demo::CounterDefinition &definition) {
  if (definition.counter == BRANCH_MISS) {
    return CYCLES | INSTRUCTIONS | BRANCHES | BRANCH_MISS;
  }
  if (definition.counter == BRANCHES) {
    return CYCLES | INSTRUCTIONS | BRANCHES;
  }
  if (definition.counter == PerfCounter::Named("INST_BARRIER")) {
    return CYCLES | INSTRUCTIONS | PerfCounter::Named("INST_BARRIER") |
           PerfCounter::Named("INST_ALL");
  }
  if (definition.counter == PerfCounter::Named("INTERRUPT_PENDING")) {
    return CYCLES | INSTRUCTIONS | PerfCounter::Named("INTERRUPT_PENDING") |
           PerfCounter::Named("INST_ALL");
  }
  if (definition.counter == PerfCounter::Named("ST_MEMORY_ORDER_VIOLATION_NONSPEC")) {
    return CYCLES | INSTRUCTIONS | PerfCounter::Named("ST_MEMORY_ORDER_VIOLATION_NONSPEC") |
           PerfCounter::Named("INST_LDST");
  }
  if (definition.counter == PerfCounter::Named("MMU_VIRTUAL_MEMORY_FAULT_NONSPEC")) {
    return CYCLES | INSTRUCTIONS | PerfCounter::Named("MMU_VIRTUAL_MEMORY_FAULT_NONSPEC") |
           DTLB_MISS | PerfCounter::Named("MMU_TABLE_WALK_DATA");
  }
  if (definition.counter == PerfCounter::Named("LD_NT_UOP")) {
    return CYCLES | INSTRUCTIONS | PerfCounter::Named("LD_NT_UOP") |
           PerfCounter::Named("INST_INT_LD");
  }
  if (definition.counter == PerfCounter::Named("ST_NT_UOP")) {
    return CYCLES | INSTRUCTIONS | PerfCounter::Named("ST_NT_UOP") |
           PerfCounter::Named("INST_INT_ST");
  }
  return CYCLES | INSTRUCTIONS | definition.counter;
}

void PrintWorkloadRunSummary(const WorkloadRunSummary &summary) {
  std::cout << "- workload: " << summary.workload->id << '\n';
  std::cout << "  " << summary.workload->summary << '\n';
  std::cout << "  repeats=" << summary.samples.size()
            << " attempts=" << FormatInteger(summary.attempts_used)
            << " mean_wall=" << FormatWallNs(MeanWallNs(summary)) << '\n';

  std::size_t label_width = std::string("counter").size();
  std::size_t value_width = std::string("mean").size();
  std::size_t expected_width = std::string("expected").size();
  struct MeasuredRow {
    PerfCounter counter;
    double mean = 0.0;
    const demo::WorkloadExpectation *expectation = nullptr;
    std::string expectation_display;
  };
  std::vector<MeasuredRow> measured;
  for (std::uint8_t i = 0; i < summary.measured_counters.count; ++i) {
    const PerfCounter counter = summary.measured_counters.items[i];
    const auto mean = MeanCounterValue(summary, counter);
    if (!mean.has_value()) {
      continue;
    }
    const demo::WorkloadExpectation *expectation = FindExpectation(*summary.workload, counter);
    const std::string expectation_display =
        expectation != nullptr ? ExpectationDisplay(*expectation) : std::string("-");
    measured.push_back({counter, *mean, expectation, expectation_display});
    label_width = std::max(label_width, CounterLabel(counter).size());
    value_width = std::max(value_width, FormatDouble(*mean).size());
    expected_width = std::max(expected_width, expectation_display.size());
  }

  if (!measured.empty()) {
    std::cout << "  measured counters:\n";
    std::cout << "    " << std::left << std::setw(static_cast<int>(label_width)) << "counter"
              << "  " << std::right << std::setw(static_cast<int>(value_width)) << "mean"
              << "  " << std::left << std::setw(static_cast<int>(expected_width)) << "expected"
              << '\n';
    for (const MeasuredRow &row : measured) {
      const std::string label = CounterLabel(row.counter);
      std::cout << "    " << std::left << std::setw(static_cast<int>(label_width)) << label << "  "
                << std::right << std::setw(static_cast<int>(value_width))
                << FormatDouble(row.mean) << "  " << std::left
                << std::setw(static_cast<int>(expected_width)) << row.expectation_display << '\n';
    }

    bool printed_notes = false;
    for (const MeasuredRow &row : measured) {
      if (row.expectation == nullptr || row.expectation->note.empty()) {
        continue;
      }
      if (!printed_notes) {
        std::cout << "  expectation notes:\n";
        printed_notes = true;
      }
      std::cout << "    - " << CounterLabel(row.counter) << " (" << row.expectation_display
                << "): " << row.expectation->note << '\n';
    }
  }

  PrintDerivedMetrics(ComputeDerivedMetrics(summary), "  ");
}

void PrintDemoContrastSummary(const demo::WorkloadDefinition &primary,
                              const WorkloadRunSummary &primary_summary,
                              const demo::WorkloadDefinition &contrast,
                              const WorkloadRunSummary &contrast_summary) {
  std::cout << "\nComparison against " << contrast.title << " (" << contrast.id << "):\n";
  std::cout << "  " << contrast.summary << '\n';

  std::size_t metric_width = std::string("metric").size();
  std::size_t primary_width = primary.title.size();
  std::size_t contrast_width = contrast.title.size();
  std::size_t ratio_width = std::string("ratio").size();

  struct Row {
    std::string metric;
    std::string primary_value;
    std::string contrast_value;
    std::string ratio;
  };
  std::vector<Row> rows;

  rows.push_back({"wall", FormatWallNs(MeanWallNs(primary_summary)),
                  FormatWallNs(MeanWallNs(contrast_summary)),
                  FormatRatio(MeanWallNs(primary_summary), MeanWallNs(contrast_summary))});

  for (std::uint8_t i = 0; i < primary_summary.measured_counters.count; ++i) {
    const PerfCounter counter = primary_summary.measured_counters.items[i];
    const auto primary_mean = MeanCounterValue(primary_summary, counter);
    const auto contrast_mean = MeanCounterValue(contrast_summary, counter);
    if (!primary_mean.has_value() || !contrast_mean.has_value()) {
      continue;
    }
    Row row{
        CounterLabel(counter),
        FormatDouble(*primary_mean),
        FormatDouble(*contrast_mean),
        FormatRatio(*primary_mean, *contrast_mean),
    };
    metric_width = std::max(metric_width, row.metric.size());
    primary_width = std::max(primary_width, row.primary_value.size());
    contrast_width = std::max(contrast_width, row.contrast_value.size());
    ratio_width = std::max(ratio_width, row.ratio.size());
    rows.push_back(std::move(row));
  }

  const auto dashes = [](std::size_t count) { return std::string(count, '-'); };

  std::cout << "  " << std::left << std::setw(static_cast<int>(metric_width)) << "metric"
            << " | " << std::right << std::setw(static_cast<int>(primary_width)) << primary.title
            << " | " << std::right << std::setw(static_cast<int>(contrast_width))
            << contrast.title << " | " << std::right << std::setw(static_cast<int>(ratio_width))
            << "ratio" << '\n';
  std::cout << "  " << std::left << std::setw(static_cast<int>(metric_width))
            << dashes(metric_width) << " | " << std::right
            << std::setw(static_cast<int>(primary_width)) << dashes(primary_width) << " | "
            << std::right << std::setw(static_cast<int>(contrast_width))
            << dashes(contrast_width) << " | " << std::right
            << std::setw(static_cast<int>(ratio_width)) << dashes(ratio_width) << '\n';
  for (const Row &row : rows) {
    std::cout << "  " << std::left << std::setw(static_cast<int>(metric_width)) << row.metric
              << " | " << std::right << std::setw(static_cast<int>(primary_width))
              << row.primary_value << " | " << std::right
              << std::setw(static_cast<int>(contrast_width)) << row.contrast_value << " | "
              << std::right << std::setw(static_cast<int>(ratio_width)) << row.ratio << '\n';
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
  for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
    bool captured = false;
    PerfMeasurement last_measurement;
    std::string last_error;
    for (std::size_t attempt = 0; attempt < options.run_options.max_attempts; ++attempt) {
      ++summary.attempts_used;
      PerfMeasurement measurement;
      std::uint64_t sink = 0;
      std::string worker_error;
      std::thread worker([&] {
        if (!demo::ApplySchedulerPreference(options.run_options, worker_error)) {
          return;
        }
        for (std::size_t i = 0; i < warmups; ++i) {
          sink = workload.run(environment);
        }
        measurement = PerfMeasure(measured_counters, [&] { sink = workload.run(environment); });
      });
      worker.join();

      summary.last_sink = sink;
      if (!worker_error.empty()) {
        summary.error = worker_error;
        return false;
      }

      last_measurement = measurement;
      if (!measurement.valid) {
        summary.error = measurement.error;
        return false;
      }
      if (!demo::SampleMatches(measurement, options.run_options)) {
        last_error = MismatchReason(measurement, options.run_options);
        std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      summary.samples.push_back(measurement);
      captured = true;
      break;
    }
    if (!captured) {
      if (!last_error.empty()) {
        summary.error = last_error;
      } else if (summary.error.empty()) {
        summary.error = MismatchReason(last_measurement, options.run_options);
      }
      return false;
    }
  }

  summary.valid = true;
  return summary.valid;
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
  if (!workload->configuration.empty()) {
    std::cout << "- configuration: " << workload->configuration << '\n';
  }
  std::cout << "- default warmups: " << workload->default_warmups << '\n';
  std::cout << "- default repeats: " << workload->default_repeats << '\n';
  std::cout << "- expected counters:\n";
  for (const auto &expectation : workload->expectations) {
    std::cout << "  - " << expectation.counter_name << " -> " << ExpectationDisplay(expectation) << ": "
              << expectation.note << '\n';
  }
  if (!workload->code_snippet.empty()) {
    std::cout << "- representative code:\n";
    PrintIndentedBlock(workload->code_snippet, "    ");
  }
  if (!workload->contrast_demo_id.empty()) {
    if (const demo::WorkloadDefinition *contrast = demo::FindWorkload(workload->contrast_demo_id)) {
      std::cout << "- contrast demo: " << contrast->id << " — " << contrast->summary << '\n';
    }
    if (!workload->contrast_blurb.empty()) {
      std::cout << "- comparison goal: " << workload->contrast_blurb << '\n';
    }
  }
  return 0;
}

std::vector<const demo::WorkloadDefinition *> FilteredDemos(const CliOptions &options) {
  std::vector<const demo::WorkloadDefinition *> demos;
  for (const demo::WorkloadDefinition &workload : demo::Workloads()) {
    if (!TierMatches(workload.tier, options.tier_filter) ||
        !GroupMatches(workload.group, options.group_filter)) {
      continue;
    }
    demos.push_back(&workload);
  }
  return demos;
}

int RunDemoWorkload(const demo::WorkloadDefinition &workload, demo::DemoEnvironment &environment,
                    const CliOptions &options) {
  std::cout << workload.title << " (" << workload.id << ")\n";
  std::cout << workload.summary << '\n';
  std::cout << workload.mechanism << '\n';
  if (!workload.configuration.empty()) {
    std::cout << "\nWorkload details:\n";
    std::cout << "- configuration: " << workload.configuration << '\n';
  }
  if (!workload.code_snippet.empty()) {
    std::cout << "- representative code:\n";
    PrintIndentedBlock(workload.code_snippet, "    ");
  }
  if (!workload.contrast_demo_id.empty()) {
    if (const demo::WorkloadDefinition *contrast = demo::FindWorkload(workload.contrast_demo_id)) {
      std::cout << "- contrast demo: " << contrast->title << " (" << contrast->id << ")\n";
    }
    if (!workload.contrast_blurb.empty()) {
      std::cout << "- comparison goal: " << workload.contrast_blurb << '\n';
    }
  }
  std::cout << "\nExpected counter behavior:\n";
  for (const auto &expectation : workload.expectations) {
    std::cout << "- " << expectation.counter_name << " -> " << ExpectationDisplay(expectation)
              << ": "
              << expectation.note << '\n';
  }
  std::cout << std::flush;

  WorkloadRunSummary summary;
  if (!ExecuteWorkload(workload, workload.measurement_counters, environment, options, summary)) {
    std::cout << "\nRun failed: " << summary.error << '\n';
    return 1;
  }

  DemoContrast contrast;
  if (!workload.contrast_demo_id.empty()) {
    contrast.workload = demo::FindWorkload(workload.contrast_demo_id);
    if (contrast.workload == nullptr) {
      std::cout << "\nRun failed: unknown contrast demo " << workload.contrast_demo_id << '\n';
      return 1;
    }
    const PerfCounterSet contrast_set = workload.measurement_counters;
    if (contrast_set.overflow) {
      std::cout << "\nRun failed: contrast measurement set exceeds PERF_MAX_SCOPE_EVENTS\n";
      return 1;
    }
    if (!ExecuteWorkload(*contrast.workload, contrast_set, environment, options, contrast.summary)) {
      std::cout << "\nRun failed: contrast demo failed: " << contrast.summary.error << '\n';
      return 1;
    }
  }

  std::cout << "\nMeasured output:\n";
  PrintWorkloadRunSummary(summary);
  if (contrast.workload != nullptr) {
    std::cout << '\n';
    PrintWorkloadRunSummary(contrast.summary);
    PrintDemoContrastSummary(workload, summary, *contrast.workload, contrast.summary);
  }

  std::cout << "\nInterpretation:\n";
  const auto ipc = MeanIpc(summary);
  const auto cpi = MeanCpi(summary);
  if (contrast.workload != nullptr) {
    const auto contrast_ipc = MeanIpc(contrast.summary);
    std::cout << "- " << workload.title << " took "
              << FormatRatio(MeanWallNs(summary), MeanWallNs(contrast.summary))
              << " the wall time of " << contrast.workload->title << ".\n";
    if (const auto cycles = MeanCounterValue(summary, CYCLES);
        cycles.has_value()) {
      if (const auto contrast_cycles = MeanCounterValue(contrast.summary, CYCLES);
          contrast_cycles.has_value()) {
        std::cout << "- Cycle count changed by "
                  << FormatRatio(*cycles, *contrast_cycles)
                  << ", so the slowdown shows up directly in the core-cycle counter.\n";
      }
    }
    if (const auto misses = MeanCounterValue(summary, L1_LOAD_MISS);
        misses.has_value()) {
      if (const auto contrast_misses = MeanCounterValue(contrast.summary, L1_LOAD_MISS);
          contrast_misses.has_value()) {
        std::cout << "- L1 load misses changed by "
                  << FormatRatio(*misses, *contrast_misses)
                  << ", which is the clearest signal that randomizing the chain destroyed locality.\n";
      }
    }
    if (ipc.has_value() && contrast_ipc.has_value()) {
      if (*ipc < *contrast_ipc) {
        std::cout << "- IPC moved from " << FormatDouble(*contrast_ipc, 3) << " in "
                  << contrast.workload->id << " to " << FormatDouble(*ipc, 3) << " in "
                  << workload.id
                  << ", which means the primary workload retired less useful work per cycle.\n";
      } else if (*ipc > *contrast_ipc) {
        std::cout << "- IPC moved from " << FormatDouble(*contrast_ipc, 3) << " in "
                  << contrast.workload->id << " to " << FormatDouble(*ipc, 3) << " in "
                  << workload.id
                  << ", which means the primary workload retired more useful work per cycle.\n";
      } else {
        std::cout << "- IPC stayed effectively flat between " << contrast.workload->id << " and "
                  << workload.id << ".\n";
      }
    }
    if (!workload.contrast_blurb.empty()) {
      std::cout << "- " << workload.contrast_blurb << '\n';
    }
    PrintExpectationWarnings(workload, summary, &contrast);
  } else {
    if (ipc.has_value() && cpi.has_value()) {
      std::cout << "- IPC is " << FormatDouble(*ipc, 3) << " and CPI is "
                << FormatDouble(*cpi, 3) << ".\n";
    }
    for (const auto &expectation : workload.expectations) {
      const demo::CounterDefinition *definition = demo::FindCounter(expectation.counter_name);
      if (definition == nullptr) {
        continue;
      }
      const auto mean = MeanCounterValue(summary, definition->counter);
      if (!mean.has_value()) {
        continue;
      }
      if (const auto observed = ExpectationObservedSummary(expectation, *mean);
          observed.has_value()) {
        std::cout << "- " << expectation.counter_name << ": " << *observed << ". "
                  << expectation.note << '\n';
      } else {
        std::cout << "- " << expectation.counter_name << " measured "
                  << FormatDouble(*mean) << " and is expected to be "
                  << ExpectationDisplay(expectation) << " here because " << expectation.note
                  << '\n';
      }
    }
    PrintExpectationWarnings(workload, summary, nullptr);
  }
  return 0;
}

int Summary(const CliOptions &options) {
  std::vector<JsonScopeAggregate> records;
  std::string error;
  if (!LoadScopeAggregates(options.target, records, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  if (records.empty()) {
    std::cerr << "no scope aggregate records found in " << options.target << '\n';
    return 1;
  }

  const std::vector<AnalysisRow> rows = BuildAnalysisRows(records);
  std::size_t scope_width = std::string("scope").size();
  std::size_t calls_width = std::string("calls").size();
  std::size_t cycles_width = std::string("cyc/call").size();
  std::size_t ipc_width = std::string("IPC").size();
  std::size_t l1_width = std::string("L1miss/Kins").size();
  std::size_t tlb_width = std::string("TLB/Kins").size();
  std::size_t p95_width = std::string("p95cyc").size();
  std::size_t dropped_width = std::string("dropped").size();

  struct PrintableRow {
    std::string scope;
    std::string calls;
    std::string cycles;
    std::string ipc;
    std::string l1;
    std::string tlb;
    std::string p95;
    std::string dropped;
  };
  std::vector<PrintableRow> printable;
  printable.reserve(rows.size());
  for (const AnalysisRow &row : rows) {
    PrintableRow out{
        row.label,
        FormatInteger(row.calls),
        FormatMaybeDouble(row.cycles_per_call, row.has_cycles, 0),
        FormatMaybeDouble(row.ipc, row.has_ipc, 3),
        FormatMaybeDouble(row.l1_miss_per_kinst, row.has_instruction_metrics, 3),
        FormatMaybeDouble(row.tlb_per_kinst, row.has_instruction_metrics, 3),
        FormatOptionalDouble(row.cycle_p95, 0),
        FormatInteger(row.dropped),
    };
    scope_width = std::max(scope_width, out.scope.size());
    calls_width = std::max(calls_width, out.calls.size());
    cycles_width = std::max(cycles_width, out.cycles.size());
    ipc_width = std::max(ipc_width, out.ipc.size());
    l1_width = std::max(l1_width, out.l1.size());
    tlb_width = std::max(tlb_width, out.tlb.size());
    p95_width = std::max(p95_width, out.p95.size());
    dropped_width = std::max(dropped_width, out.dropped.size());
    printable.push_back(std::move(out));
  }

  std::cout << "Summary: " << options.target << '\n';
  std::cout << std::left << std::setw(static_cast<int>(scope_width)) << "scope" << "  "
            << std::right << std::setw(static_cast<int>(calls_width)) << "calls" << "  "
            << std::right << std::setw(static_cast<int>(cycles_width)) << "cyc/call" << "  "
            << std::right << std::setw(static_cast<int>(ipc_width)) << "IPC" << "  "
            << std::right << std::setw(static_cast<int>(l1_width)) << "L1miss/Kins" << "  "
            << std::right << std::setw(static_cast<int>(tlb_width)) << "TLB/Kins" << "  "
            << std::right << std::setw(static_cast<int>(p95_width)) << "p95cyc" << "  "
            << std::right << std::setw(static_cast<int>(dropped_width)) << "dropped" << '\n';
  for (const PrintableRow &row : printable) {
    std::cout << std::left << std::setw(static_cast<int>(scope_width)) << row.scope << "  "
              << std::right << std::setw(static_cast<int>(calls_width)) << row.calls << "  "
              << std::right << std::setw(static_cast<int>(cycles_width)) << row.cycles << "  "
              << std::right << std::setw(static_cast<int>(ipc_width)) << row.ipc << "  "
              << std::right << std::setw(static_cast<int>(l1_width)) << row.l1 << "  "
              << std::right << std::setw(static_cast<int>(tlb_width)) << row.tlb << "  "
              << std::right << std::setw(static_cast<int>(p95_width)) << row.p95 << "  "
              << std::right << std::setw(static_cast<int>(dropped_width)) << row.dropped << '\n';
  }
  return 0;
}

std::string DiffVerdict(const AnalysisRow &baseline, const AnalysisRow &candidate) {
  if (!baseline.has_cycles || !candidate.has_cycles || baseline.cycles_per_call <= 0.0) {
    return "no-cycle-data";
  }
  const double pct =
      ((candidate.cycles_per_call - baseline.cycles_per_call) * 100.0) / baseline.cycles_per_call;
  if (std::abs(pct) < 2.0) {
    return "no-change";
  }
  const bool aggregate_improved = pct < 0.0;
  if (baseline.cycle_p50.has_value() && candidate.cycle_p50.has_value() &&
      baseline.cycle_p95.has_value() && candidate.cycle_p95.has_value()) {
    const bool median_same_direction =
        aggregate_improved ? *candidate.cycle_p50 < *baseline.cycle_p50
                           : *candidate.cycle_p50 > *baseline.cycle_p50;
    const bool tail_same_direction =
        aggregate_improved ? *candidate.cycle_p95 < *baseline.cycle_p95
                           : *candidate.cycle_p95 > *baseline.cycle_p95;
    if (median_same_direction && tail_same_direction) {
      return aggregate_improved ? "improved-dist" : "regressed-dist";
    }
  }
  return aggregate_improved ? "improved-agg" : "regressed-agg";
}

int Diff(const CliOptions &options) {
  std::vector<JsonScopeAggregate> baseline_records;
  std::vector<JsonScopeAggregate> candidate_records;
  std::string error;
  if (!LoadScopeAggregates(options.target, baseline_records, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  if (!LoadScopeAggregates(options.secondary_target, candidate_records, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  const std::vector<AnalysisRow> baseline_rows = BuildAnalysisRows(baseline_records);
  const std::vector<AnalysisRow> candidate_rows = BuildAnalysisRows(candidate_records);
  std::unordered_map<std::string, AnalysisRow> baseline_by_key;
  std::unordered_map<std::string, AnalysisRow> candidate_by_key;
  for (const AnalysisRow &row : baseline_rows) {
    baseline_by_key.emplace(row.key, row);
  }
  for (const AnalysisRow &row : candidate_rows) {
    candidate_by_key.emplace(row.key, row);
  }

  struct DiffRow {
    std::string scope;
    AnalysisRow baseline;
    AnalysisRow candidate;
    std::string verdict;
  };
  std::vector<DiffRow> rows;
  for (const auto &entry : baseline_by_key) {
    const auto candidate_it = candidate_by_key.find(entry.first);
    if (candidate_it == candidate_by_key.end()) {
      continue;
    }
    rows.push_back({entry.second.label, entry.second, candidate_it->second,
                    DiffVerdict(entry.second, candidate_it->second)});
  }
  std::sort(rows.begin(), rows.end(), [](const DiffRow &lhs, const DiffRow &rhs) {
    const double lhs_delta =
        lhs.baseline.cycles_per_call > 0.0
            ? std::abs(lhs.candidate.cycles_per_call - lhs.baseline.cycles_per_call) /
                  lhs.baseline.cycles_per_call
            : 0.0;
    const double rhs_delta =
        rhs.baseline.cycles_per_call > 0.0
            ? std::abs(rhs.candidate.cycles_per_call - rhs.baseline.cycles_per_call) /
                  rhs.baseline.cycles_per_call
            : 0.0;
    return lhs_delta > rhs_delta;
  });

  if (rows.empty()) {
    std::cerr << "no matching scope aggregates found between inputs\n";
    return 1;
  }

  std::size_t scope_width = std::string("scope").size();
  std::size_t base_width = std::string("base cyc").size();
  std::size_t cand_width = std::string("cand cyc").size();
  std::size_t delta_width = std::string("cyc delta").size();
  std::size_t ipc_width = std::string("IPC delta").size();
  std::size_t l1_width = std::string("L1/Ki delta").size();
  std::size_t verdict_width = std::string("verdict").size();

  struct PrintableDiffRow {
    std::string scope;
    std::string base_cycles;
    std::string candidate_cycles;
    std::string cycle_delta;
    std::string ipc_delta;
    std::string l1_delta;
    std::string verdict;
  };
  std::vector<PrintableDiffRow> printable;
  printable.reserve(rows.size());
  for (const DiffRow &row : rows) {
    PrintableDiffRow out{
        row.scope,
        FormatMaybeDouble(row.baseline.cycles_per_call, row.baseline.has_cycles, 0),
        FormatMaybeDouble(row.candidate.cycles_per_call, row.candidate.has_cycles, 0),
        row.baseline.has_cycles && row.candidate.has_cycles && row.baseline.cycles_per_call > 0.0
            ? FormatSignedPercent(row.candidate.cycles_per_call, row.baseline.cycles_per_call)
            : std::string("-"),
        row.baseline.has_ipc && row.candidate.has_ipc
            ? FormatSignedPercent(row.candidate.ipc, row.baseline.ipc)
            : std::string("-"),
        row.baseline.has_instruction_metrics && row.candidate.has_instruction_metrics &&
                row.baseline.l1_miss_per_kinst > 0.0
            ? FormatSignedPercent(row.candidate.l1_miss_per_kinst, row.baseline.l1_miss_per_kinst)
            : std::string("-"),
        row.verdict,
    };
    scope_width = std::max(scope_width, out.scope.size());
    base_width = std::max(base_width, out.base_cycles.size());
    cand_width = std::max(cand_width, out.candidate_cycles.size());
    delta_width = std::max(delta_width, out.cycle_delta.size());
    ipc_width = std::max(ipc_width, out.ipc_delta.size());
    l1_width = std::max(l1_width, out.l1_delta.size());
    verdict_width = std::max(verdict_width, out.verdict.size());
    printable.push_back(std::move(out));
  }

  std::cout << "Diff: " << options.target << " -> " << options.secondary_target << '\n';
  std::cout << "Note: verdicts use aggregate means and available p50/p95 direction; formal statistical tests need repeated run-level samples.\n";
  std::cout << std::left << std::setw(static_cast<int>(scope_width)) << "scope" << "  "
            << std::right << std::setw(static_cast<int>(base_width)) << "base cyc" << "  "
            << std::right << std::setw(static_cast<int>(cand_width)) << "cand cyc" << "  "
            << std::right << std::setw(static_cast<int>(delta_width)) << "cyc delta" << "  "
            << std::right << std::setw(static_cast<int>(ipc_width)) << "IPC delta" << "  "
            << std::right << std::setw(static_cast<int>(l1_width)) << "L1/Ki delta" << "  "
            << std::left << std::setw(static_cast<int>(verdict_width)) << "verdict" << '\n';
  for (const PrintableDiffRow &row : printable) {
    std::cout << std::left << std::setw(static_cast<int>(scope_width)) << row.scope << "  "
              << std::right << std::setw(static_cast<int>(base_width)) << row.base_cycles << "  "
              << std::right << std::setw(static_cast<int>(cand_width)) << row.candidate_cycles
              << "  " << std::right << std::setw(static_cast<int>(delta_width))
              << row.cycle_delta << "  " << std::right << std::setw(static_cast<int>(ipc_width))
              << row.ipc_delta << "  " << std::right << std::setw(static_cast<int>(l1_width))
              << row.l1_delta << "  " << std::left << row.verdict << '\n';
  }
  return 0;
}

int RunCounter(CliOptions options) {
  ApplyMeasuredRunDefaults(options);
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
  ApplyMeasuredRunDefaults(options);
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
  return RunDemoWorkload(*workload, environment, options);
}

int RunDemos(CliOptions options) {
  ApplyMeasuredRunDefaults(options);
  std::vector<const demo::WorkloadDefinition *> demos = FilteredDemos(options);
  if (demos.empty()) {
    std::cerr << "no demos matched the requested filters\n";
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
  std::cout << "\nRunning " << demos.size() << " demo"
            << (demos.size() == 1 ? "" : "s") << ".\n";
  std::cout << std::flush;

  std::size_t failures = 0;
  std::vector<std::string> failed_ids;
  for (std::size_t i = 0; i < demos.size(); ++i) {
    if (i != 0) {
      std::cout << "\n";
    }
    std::cout << "============================================================\n";
    std::cout << "[" << (i + 1) << "/" << demos.size() << "] " << demos[i]->title << " ("
              << demos[i]->id << ")\n";
    std::cout << "============================================================\n";
    const int rc = RunDemoWorkload(*demos[i], environment, options);
    if (rc != 0) {
      ++failures;
      failed_ids.push_back(std::string(demos[i]->id));
    }
  }

  std::cout << "\nSummary:\n";
  std::cout << "- demos run: " << demos.size() << '\n';
  std::cout << "- successes: " << (demos.size() - failures) << '\n';
  std::cout << "- failures: " << failures << '\n';
  if (!failed_ids.empty()) {
    std::cout << "- failed ids:";
    for (const std::string &id : failed_ids) {
      std::cout << ' ' << id;
    }
    std::cout << '\n';
  }

  return failures == 0 ? 0 : 1;
}

int Validate(CliOptions options) {
  options.tier_filter = TierFilter::Stable;
  options.run_options.require_active_pmu = true;
  options.run_options.require_stable_cpu = true;
  options.run_options.max_attempts = std::max<std::size_t>(options.run_options.max_attempts, 64);

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
    case CommandKind::RunDemos:
      return RunDemos(options);
    case CommandKind::Summary:
      return Summary(options);
    case CommandKind::Diff:
      return Diff(options);
    case CommandKind::Validate:
      return Validate(options);
  }

  return 1;
}
