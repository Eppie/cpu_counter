#include "demos/catalog.h"

#include <libkern/OSCacheControl.h>
#include <pthread/qos.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <random>
#include <sstream>

namespace demo {
namespace {

volatile std::uint64_t g_sink = 0;

template <typename T>
std::optional<T> ReadSysctlIntegral(const char *name) {
  T value{};
  std::size_t size = sizeof(value);
  if (sysctlbyname(name, &value, &size, nullptr, 0) != 0 || size != sizeof(value)) {
    return std::nullopt;
  }
  return value;
}

std::string ReadSysctlString(const char *name) {
  std::size_t size = 0;
  if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) {
    return {};
  }
  std::string value(size, '\0');
  if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) {
    return {};
  }
  if (!value.empty() && value.back() == '\0') {
    value.pop_back();
  }
  return value;
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::uint64_t *AlignU64(std::vector<std::uint8_t> &storage, std::size_t alignment) {
  auto raw = reinterpret_cast<std::uintptr_t>(storage.data());
  raw = (raw + alignment - 1) & ~(alignment - 1);
  return reinterpret_cast<std::uint64_t *>(raw);
}

std::vector<std::uint32_t> BuildSequentialRing(std::size_t count) {
  std::vector<std::uint32_t> ring(count);
  for (std::size_t i = 0; i < count; ++i) {
    ring[i] = static_cast<std::uint32_t>((i + 1) % count);
  }
  return ring;
}

std::vector<std::uint32_t> BuildRandomRing(std::size_t count, std::uint64_t seed) {
  std::vector<std::uint32_t> order(count);
  std::iota(order.begin(), order.end(), 0u);
  std::mt19937_64 rng(seed);
  std::shuffle(order.begin(), order.end(), rng);

  std::vector<std::uint32_t> ring(count);
  for (std::size_t i = 0; i < count; ++i) {
    ring[order[i]] = order[(i + 1) % count];
  }
  return ring;
}

std::vector<std::size_t> BuildShuffledSequence(std::size_t count, std::uint64_t seed) {
  std::vector<std::size_t> order(count);
  std::iota(order.begin(), order.end(), 0);
  std::mt19937_64 rng(seed);
  std::shuffle(order.begin(), order.end(), rng);
  return order;
}

std::uint32_t EncodeMovzX0(std::uint16_t imm16) {
  return 0xD2800000u | (static_cast<std::uint32_t>(imm16) << 5);
}

}  // namespace

bool LogicalCpuRange::Contains(int cpu) const {
  return cpu >= first && cpu <= last;
}

std::string LogicalCpuRange::Description() const {
  std::ostringstream oss;
  oss << label << '[' << first << '-' << last << ']';
  return oss.str();
}

const char *ToString(Tier tier) {
  switch (tier) {
    case Tier::Stable:
      return "stable";
    case Tier::Experimental:
    default:
      return "experimental";
  }
}

const char *ToString(Group group) {
  switch (group) {
    case Group::CoreExecution:
      return "core-execution";
    case Group::MemoryCache:
      return "memory-cache";
    case Group::TlbPageWalk:
      return "tlb-page-walk";
    case Group::BranchControl:
      return "branch-control";
    case Group::InstructionSide:
      return "instruction-side";
    case Group::StoreOrdering:
      return "store-ordering";
    case Group::SimdUopMapping:
    default:
      return "simd-uop-mapping";
  }
}

const char *ToString(CorePreference preference) {
  switch (preference) {
    case CorePreference::Performance:
      return "performance";
    case CorePreference::Efficiency:
      return "efficiency";
    case CorePreference::Any:
    default:
      return "any";
  }
}

PerfLevelLayout ReadPerfLevelLayout() {
  PerfLevelLayout layout;
  const auto perf_levels = ReadSysctlIntegral<std::uint32_t>("hw.nperflevels").value_or(0);
  int next_cpu = 0;
  for (std::uint32_t index = 0; index < perf_levels; ++index) {
    const std::string prefix = "hw.perflevel" + std::to_string(index);
    const std::string name = ReadSysctlString((prefix + ".name").c_str());
    const int logical_cpus = static_cast<int>(
        ReadSysctlIntegral<std::uint32_t>((prefix + ".logicalcpu").c_str()).value_or(0));
    if (logical_cpus <= 0) {
      continue;
    }

    PerfLevelLayout::Entry entry;
    entry.index = static_cast<int>(index);
    entry.name = name.empty() ? ("perflevel" + std::to_string(index)) : name;
    entry.logical_cpus = logical_cpus;
    entry.heuristic_range = {
        next_cpu,
        next_cpu + logical_cpus - 1,
        entry.name,
    };
    layout.entries.push_back(entry);

    const std::string lowered_name = LowerAscii(entry.name);
    if (lowered_name == "performance") {
      layout.performance_range = entry.heuristic_range;
    } else if (lowered_name == "efficiency") {
      layout.efficiency_range = entry.heuristic_range;
    }
    next_cpu += logical_cpus;
  }
  return layout;
}

bool ResolveCorePreference(RunOptions &options, const PerfLevelLayout &layout, std::string &error) {
  (void)error;
  if (options.prefer_cpu.has_value() && options.core_preference != CorePreference::Any) {
    options.prefer_cpu_range.reset();
    options.core_preference = CorePreference::Any;
  }

  switch (options.core_preference) {
    case CorePreference::Performance:
      if (layout.performance_range.has_value()) {
        options.prefer_cpu_range = layout.performance_range;
      } else {
        options.prefer_cpu_range.reset();
      }
      break;
    case CorePreference::Efficiency:
      if (layout.efficiency_range.has_value()) {
        options.prefer_cpu_range = layout.efficiency_range;
      } else {
        options.prefer_cpu_range.reset();
      }
      break;
    case CorePreference::Any:
      options.prefer_cpu_range.reset();
      break;
  }

  if ((options.prefer_cpu.has_value() || options.prefer_cpu_range.has_value() ||
       options.require_active_pmu) &&
      options.max_attempts == 0) {
    options.max_attempts = 25;
  }
  return true;
}

bool ApplySchedulerPreference(const RunOptions &options, std::string &error) {
  qos_class_t qos = QOS_CLASS_UNSPECIFIED;
  switch (options.core_preference) {
    case CorePreference::Performance:
      qos = QOS_CLASS_USER_INTERACTIVE;
      break;
    case CorePreference::Efficiency:
      qos = QOS_CLASS_UTILITY;
      break;
    case CorePreference::Any:
      return true;
  }

  const int ret = pthread_set_qos_class_self_np(qos, 0);
  if (ret != 0) {
    error = std::string("pthread_set_qos_class_self_np failed: ") + std::strerror(ret);
    return false;
  }
  return true;
}

bool SampleMatches(const PerfMeasurement &measurement, const RunOptions &options) {
  if (options.require_stable_cpu) {
    if (measurement.cpu_before < 0 || measurement.cpu_after < 0 ||
        measurement.cpu_before != measurement.cpu_after) {
      return false;
    }
  }
  if (options.prefer_cpu.has_value()) {
    if (measurement.cpu_before != *options.prefer_cpu || measurement.cpu_after != *options.prefer_cpu) {
      return false;
    }
  }
  if (options.prefer_cpu_range.has_value()) {
    if (!options.prefer_cpu_range->Contains(measurement.cpu_before) ||
        !options.prefer_cpu_range->Contains(measurement.cpu_after)) {
      return false;
    }
  }
  if (options.require_active_pmu && !measurement.HasActiveConfigurableCounters()) {
    return false;
  }
  return true;
}

DemoEnvironment::~DemoEnvironment() {
  if (exec_code != nullptr && exec_bytes != 0) {
    ::munmap(exec_code, exec_bytes);
  }
}

bool DemoEnvironment::Initialize(std::string &error) {
  page_size = static_cast<std::size_t>(::getpagesize());
  cacheline_size =
      static_cast<std::size_t>(ReadSysctlIntegral<std::uint32_t>("hw.cachelinesize").value_or(128));

  constexpr std::size_t kHotBytes = 32 * 1024;
  constexpr std::size_t kStreamBytes = 64 * 1024 * 1024;
  constexpr std::size_t kRandomRingBytes = 32 * 1024 * 1024;
  constexpr std::size_t kPageWorkingSetBytes = 64 * 1024 * 1024;
  constexpr std::size_t kExecWorkingSetBytes = 64 * 1024 * 1024;

  hot_ring = BuildSequentialRing(kHotBytes / sizeof(std::uint32_t));
  linear_ring = BuildSequentialRing(kRandomRingBytes / sizeof(std::uint32_t));
  random_ring = BuildRandomRing(kRandomRingBytes / sizeof(std::uint32_t), 0x5a17d3b9ULL);

  stream_read.resize(kStreamBytes / sizeof(std::uint64_t));
  stream_store.resize(kStreamBytes / sizeof(std::uint64_t));
  hot_store.resize(kHotBytes / sizeof(std::uint64_t));
  branch_source.resize(1 << 20);

  for (std::size_t i = 0; i < stream_read.size(); ++i) {
    stream_read[i] = static_cast<std::uint64_t>(i * 1315423911ULL);
    stream_store[i] = static_cast<std::uint64_t>(i ^ 0x9e3779b97f4a7c15ULL);
  }
  for (std::size_t i = 0; i < hot_store.size(); ++i) {
    hot_store[i] = static_cast<std::uint64_t>(i);
  }
  std::mt19937_64 branch_rng(0xdeadbeefULL);
  for (std::size_t i = 0; i < branch_source.size(); ++i) {
    branch_source[i] = branch_rng();
  }

  page_count = kPageWorkingSetBytes / page_size;
  page_order = BuildShuffledSequence(page_count, 0x1234fedcULL);
  page_storage.resize(page_count * page_size + page_size + 64);
  page_base = AlignU64(page_storage, page_size);
  const std::size_t page_words = page_size / sizeof(std::uint64_t);
  for (std::size_t page = 0; page <= page_count; ++page) {
    volatile std::uint64_t *word = page_base + page * page_words;
    *word = static_cast<std::uint64_t>(page * 17);
  }

  exec_page_count = kExecWorkingSetBytes / page_size;
  exec_page_order = BuildShuffledSequence(exec_page_count, 0xa11ce5eedULL);
  exec_bytes = exec_page_count * page_size;
  void *mapping =
      ::mmap(nullptr, exec_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (mapping == MAP_FAILED) {
    error = std::string("mmap executable working set failed: ") + std::strerror(errno);
    exec_code = nullptr;
    exec_bytes = 0;
    return false;
  }

  exec_code = static_cast<std::uint8_t *>(mapping);
  for (std::size_t page = 0; page < exec_page_count; ++page) {
    auto *stub = reinterpret_cast<std::uint32_t *>(exec_code + page * page_size);
    stub[0] = EncodeMovzX0(static_cast<std::uint16_t>((page + 1) & 0xffffu));
    stub[1] = 0xD65F03C0u;
  }
  sys_icache_invalidate(exec_code, exec_bytes);
  if (::mprotect(exec_code, exec_bytes, PROT_READ | PROT_EXEC) != 0) {
    error = std::string("mprotect(PROT_EXEC) for instruction working set failed: ") +
            std::strerror(errno);
    ::munmap(exec_code, exec_bytes);
    exec_code = nullptr;
    exec_bytes = 0;
    return false;
  }

  return true;
}

std::size_t DemoEnvironment::PageWords() const {
  return page_size / sizeof(std::uint64_t);
}

DemoEnvironment::ExecStub DemoEnvironment::ExecStubAt(std::size_t page) const {
  return reinterpret_cast<ExecStub>(exec_code + page * page_size);
}

namespace workloads {

[[gnu::noinline]] std::uint64_t BranchTakenPath(std::uint64_t sum, std::uint64_t x) {
  asm volatile("" : "+r"(sum) : "r"(x) : "memory");
  sum += (x * 3ULL) ^ 0x9e3779b97f4a7c15ULL;
  sum ^= (sum << 7) + x + 17ULL;
  return sum;
}

[[gnu::noinline]] std::uint64_t BranchNotTakenPath(std::uint64_t sum, std::uint64_t x) {
  asm volatile("" : "+r"(sum) : "r"(x) : "memory");
  sum += x + 1ULL;
  sum ^= (sum >> 3) + 0x85ebca6bULL;
  return sum;
}

[[gnu::noinline]] std::uint64_t DenseIntegerAlu(DemoEnvironment &) {
  std::uint64_t a = 0x12345678ULL;
  std::uint64_t b = 0x9abcdef0ULL;
  std::uint64_t c = 0x55aa55aaULL;
  std::uint64_t d = 0xf0f0f0f0ULL;
  constexpr std::size_t kIters = 20'000'000;
  for (std::size_t i = 0; i < kIters; ++i) {
    a += (b ^ static_cast<std::uint64_t>(i)) + 0x9e3779b97f4a7c15ULL;
    b = (b << 7) | (b >> 57);
    b ^= a + c;
    c += (a & 0xffffULL) * 17ULL + d;
    d ^= c + (a >> 3);
  }
  g_sink ^= (a + b + c + d);
  return g_sink;
}

[[gnu::noinline]] std::uint64_t HotSequentialRead(DemoEnvironment &state) {
  volatile const std::uint32_t *ring = state.hot_ring.data();
  std::uint32_t index = 0;
  constexpr std::size_t kSteps = 32'000'000;
  for (std::size_t i = 0; i < kSteps; ++i) {
    index = ring[index];
  }
  g_sink += index;
  return index;
}

[[gnu::noinline]] std::uint64_t LinearPointerChase(DemoEnvironment &state) {
  volatile const std::uint32_t *ring = state.linear_ring.data();
  std::uint32_t index = 0;
  constexpr std::size_t kSteps = 2'000'000;
  for (std::size_t i = 0; i < kSteps; ++i) {
    index = ring[index];
  }
  g_sink += index;
  return index;
}

[[gnu::noinline]] std::uint64_t RandomPointerChase(DemoEnvironment &state) {
  volatile const std::uint32_t *ring = state.random_ring.data();
  std::uint32_t index = 0;
  constexpr std::size_t kSteps = 2'000'000;
  for (std::size_t i = 0; i < kSteps; ++i) {
    index = ring[index];
  }
  g_sink += index;
  return index;
}

[[gnu::noinline]] std::uint64_t HotSequentialWrite(DemoEnvironment &state) {
  std::uint64_t *data = state.hot_store.data();
  constexpr std::size_t kPasses = 8192;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
#pragma clang loop vectorize(disable)
#pragma clang loop interleave(disable)
    for (std::size_t i = 0; i < state.hot_store.size(); ++i) {
      data[i] += static_cast<std::uint64_t>(i + pass);
    }
  }
  g_sink ^= data[7];
  return data[7];
}

[[gnu::noinline]] std::uint64_t RandomPageWrite(DemoEnvironment &state) {
  volatile std::uint64_t *base = state.page_base;
  const std::size_t stride = state.PageWords();
  constexpr std::size_t kPasses = 256;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
    for (std::size_t page : state.page_order) {
      base[page * stride] += static_cast<std::uint64_t>(pass + 1);
    }
  }
  g_sink += base[state.page_order.front() * stride];
  return base[state.page_order.front() * stride];
}

[[gnu::noinline]] std::uint64_t PageStrideRead(DemoEnvironment &state) {
  volatile const std::uint64_t *base = state.page_base;
  std::uint64_t sum = 0;
  const std::size_t stride = state.PageWords();
  constexpr std::size_t kPasses = 256;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
    for (std::size_t page : state.page_order) {
      sum += base[page * stride];
    }
  }
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] std::uint64_t PredictableBranch(DemoEnvironment &state) {
  std::uint64_t sum = 0;
  constexpr std::size_t kPasses = 16;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
#pragma clang loop vectorize(disable)
#pragma clang loop interleave(disable)
    for (std::size_t i = 0; i < state.branch_source.size(); ++i) {
      if ((i & 255ULL) != 0) {
        sum = BranchTakenPath(sum, i + pass);
      } else {
        sum = BranchNotTakenPath(sum, pass + 1);
      }
    }
  }
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] std::uint64_t UnpredictableBranch(DemoEnvironment &state) {
  const auto *data = state.branch_source.data();
  std::uint64_t sum = 0;
  constexpr std::size_t kPasses = 16;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
#pragma clang loop vectorize(disable)
#pragma clang loop interleave(disable)
    for (std::size_t i = 0; i < state.branch_source.size(); ++i) {
      const std::uint64_t bits = data[i] ^ static_cast<std::uint64_t>(pass);
      if (bits & 1ULL) {
        sum = BranchTakenPath(sum, bits + i);
      } else {
        sum = BranchNotTakenPath(sum, bits + pass + 1ULL);
      }
    }
  }
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] std::uint64_t HotInstructionLoop(DemoEnvironment &state) {
  const auto fn = state.ExecStubAt(0);
  std::uint64_t sum = 0;
  constexpr std::size_t kCalls = 32'000'000;
  for (std::size_t i = 0; i < kCalls; ++i) {
    sum += fn();
  }
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] std::uint64_t RandomInstructionPages(DemoEnvironment &state) {
  std::uint64_t sum = 0;
  constexpr std::size_t kPasses = 128;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
    for (std::size_t page : state.exec_page_order) {
      sum += state.ExecStubAt(page)();
    }
  }
  g_sink += sum;
  return sum;
}

}  // namespace workloads
}  // namespace demo
