#include "demos/catalog.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <pthread/qos.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <random>
#include <sstream>
#include <thread>

namespace demo {
namespace {

volatile std::uint64_t g_sink = 0;
volatile std::sig_atomic_t g_interrupt_signal_count = 0;

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

using UnalignedU64 = std::uint64_t __attribute__((aligned(1)));

std::uint64_t ReadUnaligned64(const std::uint8_t *ptr) {
  return *reinterpret_cast<const volatile UnalignedU64 *>(ptr);
}

void WriteUnaligned64(std::uint8_t *ptr, std::uint64_t value) {
  *reinterpret_cast<volatile UnalignedU64 *>(ptr) = value;
}

void InterruptSignalHandler(int) {
  g_interrupt_signal_count =
      static_cast<std::sig_atomic_t>(g_interrupt_signal_count + static_cast<std::sig_atomic_t>(1));
}

#if defined(__aarch64__)
inline void NonTemporalLoadPairU64(const std::uint64_t *ptr, std::uint64_t &a, std::uint64_t &b) {
  asm volatile("ldnp %x0, %x1, [%2]" : "=&r"(a), "=&r"(b) : "r"(ptr) : "memory");
}

inline void NonTemporalStorePairU64(std::uint64_t a, std::uint64_t b, std::uint64_t *ptr) {
  asm volatile("stnp %x0, %x1, [%2]" : : "r"(a), "r"(b), "r"(ptr) : "memory");
}
#endif

#if !defined(__aarch64__)
std::uint64_t FallbackNonTemporalLoad(const std::uint64_t *ptr) {
#if defined(__has_builtin)
#if __has_builtin(__builtin_nontemporal_load)
  return __builtin_nontemporal_load(ptr);
#endif
#endif
  return *ptr;
}

void FallbackNonTemporalStore(std::uint64_t value, std::uint64_t *ptr) {
#if defined(__has_builtin)
#if __has_builtin(__builtin_nontemporal_store)
  __builtin_nontemporal_store(value, ptr);
  return;
#endif
#endif
  *ptr = value;
}
#endif

std::uint32_t EncodeMovzX0(std::uint16_t imm16) {
  return 0xD2800000u | (static_cast<std::uint32_t>(imm16) << 5);
}

#if defined(__aarch64__)
std::uint64_t ReduceU64x2(uint64x2_t value) {
  return vgetq_lane_u64(value, 0) + vgetq_lane_u64(value, 1);
}
#endif

[[gnu::noinline]] std::uint64_t AtomicCasLoop(std::atomic<std::uint64_t> &value,
                                              std::size_t iterations) {
  std::uint64_t observed = 0;
  for (std::size_t i = 0; i < iterations; ++i) {
    std::uint64_t expected = value.load(std::memory_order_relaxed);
    while (!value.compare_exchange_weak(expected, expected + 1, std::memory_order_acq_rel,
                                        std::memory_order_relaxed)) {
    }
    observed += expected;
  }
  return observed;
}

[[gnu::noinline]] void EmitBarrier() {
#if defined(__aarch64__)
  asm volatile("dmb ish" ::: "memory");
#else
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

constexpr std::size_t kFrontendRestartCalls = 1'048'576;

void PatchExecStub(std::uint8_t *stub_bytes, std::uint16_t imm16) {
  auto *stub = reinterpret_cast<std::uint32_t *>(stub_bytes);
  stub[0] = EncodeMovzX0(imm16);
  stub[1] = 0xD65F03C0u;
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

[[gnu::noinline]] std::uint64_t ScalarStreamRead(DemoEnvironment &state) {
  volatile const std::uint64_t *data = state.stream_read.data();
  std::uint64_t sum = 0;
  constexpr std::size_t kPasses = 8;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
#pragma clang loop vectorize(disable)
#pragma clang loop interleave(disable)
    for (std::size_t i = 0; i < state.stream_read.size(); ++i) {
      sum += data[i];
    }
  }
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] std::uint64_t SimdStreamRead(DemoEnvironment &state) {
#if defined(__aarch64__)
  const std::uint64_t *data = state.stream_read.data();
  uint64x2_t acc = vdupq_n_u64(0);
  constexpr std::size_t kPasses = 8;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
    for (std::size_t i = 0; i < state.stream_read.size(); i += 2) {
      acc = vaddq_u64(acc, vld1q_u64(data + i));
    }
  }
  const std::uint64_t sum = ReduceU64x2(acc);
  g_sink ^= sum;
  return sum;
#else
  return ScalarStreamRead(state);
#endif
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

[[gnu::noinline]] std::uint64_t ScalarStreamWrite(DemoEnvironment &state) {
  volatile std::uint64_t *data = state.stream_store.data();
  constexpr std::size_t kPasses = 8;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
#pragma clang loop vectorize(disable)
#pragma clang loop interleave(disable)
    for (std::size_t i = 0; i < state.stream_store.size(); ++i) {
      data[i] += static_cast<std::uint64_t>(i + pass + 1);
    }
  }
  g_sink ^= data[7];
  return data[7];
}

[[gnu::noinline]] std::uint64_t SimdStreamWrite(DemoEnvironment &state) {
#if defined(__aarch64__)
  std::uint64_t *data = state.stream_store.data();
  constexpr std::size_t kPasses = 8;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
    const uint64x2_t bias =
        vsetq_lane_u64(static_cast<std::uint64_t>(pass + 2), vdupq_n_u64(pass + 1), 1);
    for (std::size_t i = 0; i < state.stream_store.size(); i += 2) {
      const uint64x2_t value = vaddq_u64(vld1q_u64(data + i), bias);
      vst1q_u64(data + i, value);
    }
  }
  g_sink ^= data[7];
  return data[7];
#else
  return ScalarStreamWrite(state);
#endif
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

[[gnu::noinline]] std::uint64_t UncontendedAtomicCas(DemoEnvironment &) {
  std::atomic<std::uint64_t> value{0};
  constexpr std::size_t kIters = 4'000'000;
  const std::uint64_t observed = AtomicCasLoop(value, kIters);
  const std::uint64_t result = observed ^ value.load(std::memory_order_relaxed);
  g_sink ^= result;
  return result;
}

[[gnu::noinline]] std::uint64_t ContendedAtomicCas(DemoEnvironment &) {
  std::atomic<std::uint64_t> value{0};
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::atomic<bool> stop{false};
  constexpr std::size_t kHelpers = 3;
  constexpr std::size_t kIters = 2'000'000;

  auto helper = [&]() {
    ready.fetch_add(1, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) {
    }
    while (!stop.load(std::memory_order_acquire)) {
      std::uint64_t expected = value.load(std::memory_order_relaxed);
      while (!value.compare_exchange_weak(expected, expected + 1, std::memory_order_acq_rel,
                                          std::memory_order_relaxed)) {
      }
    }
  };

  std::array<std::thread, kHelpers> threads;
  for (std::thread &thread : threads) {
    thread = std::thread(helper);
  }
  while (ready.load(std::memory_order_acquire) != static_cast<int>(kHelpers)) {
  }
  start.store(true, std::memory_order_release);
  const std::uint64_t observed = AtomicCasLoop(value, kIters);
  stop.store(true, std::memory_order_release);
  for (std::thread &thread : threads) {
    thread.join();
  }

  const std::uint64_t result = observed ^ value.load(std::memory_order_relaxed);
  g_sink ^= result;
  return result;
}

[[gnu::noinline]] std::uint64_t BarrierLoop(DemoEnvironment &) {
  std::uint64_t a = 0x12345678ULL;
  std::uint64_t b = 0x9abcdef0ULL;
  constexpr std::size_t kIters = 20'000'000;
  for (std::size_t i = 0; i < kIters; ++i) {
    a += (b ^ static_cast<std::uint64_t>(i)) + 0x9e3779b97f4a7c15ULL;
    EmitBarrier();
    b ^= a + (a >> 7);
  }
  g_sink ^= (a + b);
  return g_sink;
}

[[gnu::noinline]] std::uint64_t InterruptStorm(DemoEnvironment &) {
  struct sigaction action {};
  struct sigaction old_action {};
  action.sa_handler = &InterruptSignalHandler;
  action.sa_flags = SA_RESTART;
  sigemptyset(&action.sa_mask);
  sigaction(SIGUSR1, &action, &old_action);

  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  pthread_sigmask(SIG_UNBLOCK, &set, nullptr);

  std::atomic<bool> stop{false};
  const pthread_t target = pthread_self();
  std::thread sender([&] {
    constexpr std::size_t kSignals = 100'000;
    for (std::size_t i = 0; i < kSignals && !stop.load(std::memory_order_acquire); ++i) {
      pthread_kill(target, SIGUSR1);
    }
  });

  std::uint64_t a = 0x12345678ULL;
  std::uint64_t b = 0x9abcdef0ULL;
  constexpr std::size_t kIters = 50'000'000;
  for (std::size_t i = 0; i < kIters; ++i) {
    a += (b ^ static_cast<std::uint64_t>(i)) + 0x9e3779b97f4a7c15ULL;
    b = (b << 9) | (b >> 55);
    b ^= a + 0x85ebca6bULL;
  }

  stop.store(true, std::memory_order_release);
  sender.join();
  sigaction(SIGUSR1, &old_action, nullptr);

  const std::uint64_t result = a ^ b ^ static_cast<std::uint64_t>(g_interrupt_signal_count);
  g_sink ^= result;
  return result;
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

[[gnu::noinline]] std::uint64_t NtStreamRead(DemoEnvironment &state) {
  const std::uint64_t *data = state.stream_read.data();
  std::uint64_t sum0 = 0;
  std::uint64_t sum1 = 0;
  constexpr std::size_t kPasses = 8;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
#pragma clang loop vectorize(disable)
#pragma clang loop interleave(disable)
    for (std::size_t i = 0; i + 1 < state.stream_read.size(); i += 2) {
#if defined(__aarch64__)
      std::uint64_t a = 0;
      std::uint64_t b = 0;
      NonTemporalLoadPairU64(data + i, a, b);
      sum0 += a;
      sum1 += b;
#else
      sum0 += FallbackNonTemporalLoad(data + i);
      sum1 += FallbackNonTemporalLoad(data + i + 1);
#endif
    }
  }
  const std::uint64_t sum = sum0 + sum1;
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] std::uint64_t NtStreamWrite(DemoEnvironment &state) {
  std::uint64_t *data = state.stream_store.data();
  std::uint64_t seed = 0x9e3779b97f4a7c15ULL;
  constexpr std::size_t kPasses = 8;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
#pragma clang loop vectorize(disable)
#pragma clang loop interleave(disable)
    for (std::size_t i = 0; i + 1 < state.stream_store.size(); i += 2) {
      seed += static_cast<std::uint64_t>(i + pass + 1);
      const std::uint64_t value0 = seed ^ (0x100000001b3ULL * (i + 1));
      const std::uint64_t value1 = (seed + 0x9e3779b97f4a7c15ULL) ^ (0xc2b2ae3d27d4eb4fULL * (i + 3));
#if defined(__aarch64__)
      NonTemporalStorePairU64(value0, value1, data + i);
#else
      FallbackNonTemporalStore(value0, data + i);
      FallbackNonTemporalStore(value1, data + i + 1);
#endif
    }
  }
  g_sink ^= data[7];
  return data[7];
}

[[gnu::noinline]] std::uint64_t FirstTouchFault(DemoEnvironment &state) {
  constexpr std::size_t kFaultBytes = 256 * 1024 * 1024;
  char path[] = "/tmp/cpu_counter_fault.XXXXXX";
  const int fd = ::mkstemp(path);
  if (fd < 0) {
    return 0;
  }
  ::unlink(path);
  if (::ftruncate(fd, static_cast<off_t>(kFaultBytes)) != 0) {
    ::close(fd);
    return 0;
  }
  void *mapping =
      ::mmap(nullptr, kFaultBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (mapping == MAP_FAILED) {
    return 0;
  }

  auto *bytes = static_cast<volatile std::uint8_t *>(mapping);
  std::uint64_t sum = 0;
  const std::size_t pages = kFaultBytes / state.page_size;
  for (std::size_t page = 0; page < pages; ++page) {
    const std::size_t offset = page * state.page_size;
    bytes[offset] = static_cast<std::uint8_t>(page);
    sum += bytes[offset];
  }

  ::msync(mapping, kFaultBytes, MS_SYNC);
  ::munmap(mapping, kFaultBytes);
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] std::uint64_t AlignedX64Load(DemoEnvironment &state) {
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(state.page_base);
  const std::size_t total_bytes = state.page_count * state.page_size;
  const std::size_t limit = total_bytes - 128;
  std::uint64_t sum = 0;
  constexpr std::size_t kPasses = 16;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
    for (std::size_t offset = 0; offset <= limit; offset += 64) {
      sum += ReadUnaligned64(bytes + offset);
    }
  }
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] std::uint64_t CrossX64Load(DemoEnvironment &state) {
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(state.page_base);
  const std::size_t total_bytes = state.page_count * state.page_size;
  const std::size_t limit = total_bytes - 128;
  std::uint64_t sum = 0;
  constexpr std::size_t kPasses = 16;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
    for (std::size_t offset = 0; offset <= limit; offset += 64) {
      sum += ReadUnaligned64(bytes + offset + 60);
    }
  }
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] std::uint64_t AlignedPageLoad(DemoEnvironment &state) {
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(state.page_base);
  std::uint64_t sum = 0;
  constexpr std::size_t kPasses = 256;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
    for (std::size_t page = 0; page < state.page_count; ++page) {
      sum += ReadUnaligned64(bytes + page * state.page_size);
    }
  }
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] std::uint64_t CrossPageLoad(DemoEnvironment &state) {
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(state.page_base);
  std::uint64_t sum = 0;
  constexpr std::size_t kPasses = 256;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
    for (std::size_t page = 0; page < state.page_count; ++page) {
      sum += ReadUnaligned64(bytes + page * state.page_size + state.page_size - 4);
    }
  }
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] std::uint64_t AlignedX64Store(DemoEnvironment &state) {
  auto *bytes = reinterpret_cast<std::uint8_t *>(state.page_base);
  const std::size_t total_bytes = state.page_count * state.page_size;
  const std::size_t limit = total_bytes - 128;
  std::uint64_t stamp = 0;
  constexpr std::size_t kPasses = 16;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
    for (std::size_t offset = 0; offset <= limit; offset += 64) {
      stamp += static_cast<std::uint64_t>(offset + pass + 1);
      WriteUnaligned64(bytes + offset, stamp);
    }
  }
  const std::uint64_t value = ReadUnaligned64(bytes + limit);
  g_sink ^= value;
  return value;
}

[[gnu::noinline]] std::uint64_t CrossX64Store(DemoEnvironment &state) {
  auto *bytes = reinterpret_cast<std::uint8_t *>(state.page_base);
  const std::size_t total_bytes = state.page_count * state.page_size;
  const std::size_t limit = total_bytes - 128;
  std::uint64_t stamp = 0;
  constexpr std::size_t kPasses = 16;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
    for (std::size_t offset = 0; offset <= limit; offset += 64) {
      stamp += static_cast<std::uint64_t>(offset + pass + 1);
      WriteUnaligned64(bytes + offset + 60, stamp);
    }
  }
  const std::uint64_t value = ReadUnaligned64(bytes + limit + 60);
  g_sink ^= value;
  return value;
}

[[gnu::noinline]] std::uint64_t AlignedPageStore(DemoEnvironment &state) {
  auto *bytes = reinterpret_cast<std::uint8_t *>(state.page_base);
  std::uint64_t stamp = 0;
  constexpr std::size_t kPasses = 256;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
    for (std::size_t page = 0; page < state.page_count; ++page) {
      stamp += static_cast<std::uint64_t>(page + pass + 1);
      WriteUnaligned64(bytes + page * state.page_size, stamp);
    }
  }
  const std::uint64_t value =
      ReadUnaligned64(bytes + (state.page_count - 1) * state.page_size);
  g_sink ^= value;
  return value;
}

[[gnu::noinline]] std::uint64_t CrossPageStore(DemoEnvironment &state) {
  auto *bytes = reinterpret_cast<std::uint8_t *>(state.page_base);
  std::uint64_t stamp = 0;
  constexpr std::size_t kPasses = 256;
  for (std::size_t pass = 0; pass < kPasses; ++pass) {
    for (std::size_t page = 0; page < state.page_count; ++page) {
      stamp += static_cast<std::uint64_t>(page + pass + 1);
      WriteUnaligned64(bytes + page * state.page_size + state.page_size - 4, stamp);
    }
  }
  const std::uint64_t value =
      ReadUnaligned64(bytes + (state.page_count - 1) * state.page_size + state.page_size - 4);
  g_sink ^= value;
  return value;
}

[[gnu::noinline]] std::uint64_t SimdVectorAlu(DemoEnvironment &) {
#if defined(__aarch64__)
  const std::uint64_t init_a[2] = {0x1234567812345678ULL, 0x89abcdef00112233ULL};
  const std::uint64_t init_b[2] = {0x55aa55aa55aa55aaULL, 0xf0f0f0f00f0f0f0fULL};
  const std::uint64_t init_c[2] = {0x0fedcba987654321ULL, 0x13579bdf2468ace0ULL};
  const std::uint64_t init_d[2] = {0x1111222233334444ULL, 0xaaaabbbbccccddddULL};
  uint64x2_t a = vld1q_u64(init_a);
  uint64x2_t b = vld1q_u64(init_b);
  uint64x2_t c = vld1q_u64(init_c);
  uint64x2_t d = vld1q_u64(init_d);
  constexpr std::size_t kIters = 20'000'000;
  for (std::size_t i = 0; i < kIters; ++i) {
    a = vaddq_u64(a, b);
    b = veorq_u64(vshlq_n_u64(b, 1), c);
    c = vaddq_u64(c, vorrq_u64(a, d));
    d = veorq_u64(d, vshrq_n_u64(a, 7));
  }
  const std::uint64_t sum =
      ReduceU64x2(a) ^ ReduceU64x2(b) ^ ReduceU64x2(c) ^ ReduceU64x2(d);
  g_sink ^= sum;
  return sum;
#else
  std::uint64_t a = 0x1234567812345678ULL;
  std::uint64_t b = 0x55aa55aa55aa55aaULL;
  std::uint64_t c = 0x0fedcba987654321ULL;
  std::uint64_t d = 0x1111222233334444ULL;
  constexpr std::size_t kIters = 20'000'000;
  for (std::size_t i = 0; i < kIters; ++i) {
    a += b;
    b ^= (b << 1) + c;
    c += (a | d);
    d ^= (a >> 7);
  }
  const std::uint64_t sum = a ^ b ^ c ^ d;
  g_sink ^= sum;
  return sum;
#endif
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

[[gnu::noinline]] std::uint64_t StoreOrderFriendly(DemoEnvironment &) {
  constexpr std::size_t kBaseWords = 1u << 15;
  constexpr std::size_t kOffsetBytes = 4104;
  constexpr std::size_t kMask = kBaseWords - 1;
  constexpr std::size_t kIters = 8'000'000;
  std::vector<std::uint8_t> storage(kBaseWords * sizeof(std::uint32_t) + kOffsetBytes + 16, 0);
  volatile std::uint32_t *stores = reinterpret_cast<volatile std::uint32_t *>(storage.data());
  auto *base = storage.data();
  std::size_t index = 0;
  std::uint64_t sum = 0;
  for (std::size_t i = 0; i < kIters; ++i) {
    index = (((sum >> 7) + index * 1315423911ULL + 17ULL) & kMask) & ~std::size_t{1};
    stores[index] = static_cast<std::uint32_t>(i + sum);
    stores[(index + 1) & kMask] = static_cast<std::uint32_t>(sum ^ (i + 1));
    stores[(index + 2) & kMask] = static_cast<std::uint32_t>(sum + i * 3 + 1);
    stores[(index + 3) & kMask] = static_cast<std::uint32_t>(sum ^ (i * 7 + 3));
    std::atomic_signal_fence(std::memory_order_seq_cst);
    sum += ReadUnaligned64(base + kOffsetBytes + index * sizeof(std::uint32_t));
  }
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] std::uint64_t StoreOrderAlias(DemoEnvironment &) {
  constexpr std::size_t kBaseWords = 1u << 15;
  constexpr std::size_t kOffsetBytes = 4096;
  constexpr std::size_t kMask = kBaseWords - 1;
  constexpr std::size_t kIters = 8'000'000;
  std::vector<std::uint8_t> storage(kBaseWords * sizeof(std::uint32_t) + kOffsetBytes + 16, 0);
  volatile std::uint32_t *stores = reinterpret_cast<volatile std::uint32_t *>(storage.data());
  auto *base = storage.data();
  std::size_t index = 0;
  std::uint64_t sum = 0;
  for (std::size_t i = 0; i < kIters; ++i) {
    index = (((sum >> 7) + index * 1315423911ULL + 17ULL) & kMask) & ~std::size_t{1};
    stores[index] = static_cast<std::uint32_t>(i + sum);
    stores[(index + 1) & kMask] = static_cast<std::uint32_t>(sum ^ (i + 1));
    stores[(index + 2) & kMask] = static_cast<std::uint32_t>(sum + i * 3 + 1);
    stores[(index + 3) & kMask] = static_cast<std::uint32_t>(sum ^ (i * 7 + 3));
    std::atomic_signal_fence(std::memory_order_seq_cst);
    sum += ReadUnaligned64(base + kOffsetBytes + index * sizeof(std::uint32_t));
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

[[gnu::noinline]] std::uint64_t FrontendHotRestart(DemoEnvironment &state) {
  const auto fn = state.ExecStubAt(0);
  std::uint64_t sum = 0;
  for (std::size_t i = 0; i < kFrontendRestartCalls; ++i) {
    sum += fn();
  }
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] std::uint64_t FrontendRandomRestart(DemoEnvironment &state) {
  std::uint64_t sum = 0;
  const std::size_t page_count = state.exec_page_order.size();
  if (page_count == 0) {
    return 0;
  }
  for (std::size_t i = 0; i < kFrontendRestartCalls; ++i) {
    const std::size_t page = state.exec_page_order[i % page_count];
    sum += state.ExecStubAt(page)();
  }
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] std::uint64_t FrontendSelfModifyingRestart(DemoEnvironment &state) {
  const std::size_t bytes = state.page_size * 2;
  void *mapping =
      ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (mapping == MAP_FAILED) {
    return 0;
  }

  auto *code = static_cast<std::uint8_t *>(mapping);
  PatchExecStub(code + 0 * state.page_size, 1);
  PatchExecStub(code + 1 * state.page_size, 2);
  sys_icache_invalidate(code, bytes);
  if (::mprotect(code, bytes, PROT_READ | PROT_EXEC) != 0) {
    ::munmap(code, bytes);
    return 0;
  }

  std::uint64_t sum = 0;
  constexpr std::size_t kCallsPerBlock = 32;
  constexpr std::size_t kBlocks = kFrontendRestartCalls / kCallsPerBlock;
  for (std::size_t block = 0; block < kBlocks; ++block) {
    const std::size_t target_page = block & 1u;
    auto *target = code + target_page * state.page_size;
    if (::mprotect(code, bytes, PROT_READ | PROT_WRITE) != 0) {
      ::munmap(code, bytes);
      return sum;
    }
    PatchExecStub(target, static_cast<std::uint16_t>((block + 1) & 0xffffu));
    sys_icache_invalidate(target, 2 * sizeof(std::uint32_t));
    if (::mprotect(code, bytes, PROT_READ | PROT_EXEC) != 0) {
      ::munmap(code, bytes);
      return sum;
    }

    const auto fn = reinterpret_cast<DemoEnvironment::ExecStub>(target);
    for (std::size_t i = 0; i < kCallsPerBlock; ++i) {
      sum += fn();
    }
  }

  ::munmap(code, bytes);
  g_sink ^= sum;
  return sum;
}

}  // namespace workloads
}  // namespace demo
