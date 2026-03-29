#include <arm_neon.h>
#include <dlfcn.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using usize = std::size_t;
using kpc_config_t = u64;

struct kpep_db;
struct kpep_config;
struct kpep_event;

constexpr u32 KPC_CLASS_FIXED = 0;
constexpr u32 KPC_CLASS_CONFIGURABLE = 1;

constexpr u32 KPC_CLASS_FIXED_MASK = 1u << KPC_CLASS_FIXED;
constexpr u32 KPC_CLASS_CONFIGURABLE_MASK = 1u << KPC_CLASS_CONFIGURABLE;

constexpr u32 KPC_PMU_ERROR = 0;
constexpr u32 KPC_PMU_INTEL_V3 = 1;
constexpr u32 KPC_PMU_ARM_APPLE = 2;
constexpr u32 KPC_PMU_INTEL_V2 = 3;
constexpr u32 KPC_PMU_ARM_V2 = 4;

constexpr usize KPC_MAX_COUNTERS = 32;

constexpr int KPEP_CONFIG_ERROR_NONE = 0;
constexpr int KPEP_CONFIG_ERROR_INVALID_ARGUMENT = 1;
constexpr int KPEP_CONFIG_ERROR_OUT_OF_MEMORY = 2;
constexpr int KPEP_CONFIG_ERROR_IO = 3;
constexpr int KPEP_CONFIG_ERROR_BUFFER_TOO_SMALL = 4;
constexpr int KPEP_CONFIG_ERROR_CUR_SYSTEM_UNKNOWN = 5;
constexpr int KPEP_CONFIG_ERROR_DB_PATH_INVALID = 6;
constexpr int KPEP_CONFIG_ERROR_DB_NOT_FOUND = 7;
constexpr int KPEP_CONFIG_ERROR_DB_ARCH_UNSUPPORTED = 8;
constexpr int KPEP_CONFIG_ERROR_DB_VERSION_UNSUPPORTED = 9;
constexpr int KPEP_CONFIG_ERROR_DB_CORRUPT = 10;
constexpr int KPEP_CONFIG_ERROR_EVENT_NOT_FOUND = 11;
constexpr int KPEP_CONFIG_ERROR_CONFLICTING_EVENTS = 12;
constexpr int KPEP_CONFIG_ERROR_COUNTERS_NOT_FORCED = 13;
constexpr int KPEP_CONFIG_ERROR_EVENT_UNAVAILABLE = 14;
constexpr int KPEP_CONFIG_ERROR_ERRNO = 15;

constexpr std::array<const char *, 2> kFixedEvents = {
    "FIXED_CYCLES",
    "FIXED_INSTRUCTIONS",
};

volatile u64 g_sink = 0;

const char *PmuVersionString(u32 version) {
  switch (version) {
    case KPC_PMU_INTEL_V3:
      return "Intel v3";
    case KPC_PMU_ARM_APPLE:
      return "ARM Apple";
    case KPC_PMU_INTEL_V2:
      return "Intel v2";
    case KPC_PMU_ARM_V2:
      return "ARM v2";
    case KPC_PMU_ERROR:
    default:
      return "error/unknown";
  }
}

const char *KpepErrorString(int code) {
  switch (code) {
    case KPEP_CONFIG_ERROR_NONE:
      return "none";
    case KPEP_CONFIG_ERROR_INVALID_ARGUMENT:
      return "invalid argument";
    case KPEP_CONFIG_ERROR_OUT_OF_MEMORY:
      return "out of memory";
    case KPEP_CONFIG_ERROR_IO:
      return "I/O";
    case KPEP_CONFIG_ERROR_BUFFER_TOO_SMALL:
      return "buffer too small";
    case KPEP_CONFIG_ERROR_CUR_SYSTEM_UNKNOWN:
      return "current system unknown";
    case KPEP_CONFIG_ERROR_DB_PATH_INVALID:
      return "database path invalid";
    case KPEP_CONFIG_ERROR_DB_NOT_FOUND:
      return "database not found";
    case KPEP_CONFIG_ERROR_DB_ARCH_UNSUPPORTED:
      return "database architecture unsupported";
    case KPEP_CONFIG_ERROR_DB_VERSION_UNSUPPORTED:
      return "database version unsupported";
    case KPEP_CONFIG_ERROR_DB_CORRUPT:
      return "database corrupt";
    case KPEP_CONFIG_ERROR_EVENT_NOT_FOUND:
      return "event not found";
    case KPEP_CONFIG_ERROR_CONFLICTING_EVENTS:
      return "conflicting events";
    case KPEP_CONFIG_ERROR_COUNTERS_NOT_FORCED:
      return "all counters must be forced";
    case KPEP_CONFIG_ERROR_EVENT_UNAVAILABLE:
      return "event unavailable";
    case KPEP_CONFIG_ERROR_ERRNO:
      return "check errno";
    default:
      return "unknown";
  }
}

std::string HexString(u32 value) {
  std::ostringstream oss;
  oss << "0x" << std::hex << std::nouppercase << value;
  return oss.str();
}

std::string ResolveKpepPlist(const std::string &cpu_string) {
  if (cpu_string.empty()) {
    return {};
  }

  const std::filesystem::path link =
      std::filesystem::path("/usr/share/kpep") / (cpu_string + ".plist");
  std::error_code ec;
  const std::filesystem::path target = std::filesystem::read_symlink(link, ec);
  if (ec) {
    return {};
  }
  return target.string();
}

std::string ReadSysctlString(const char *name) {
  usize size = 0;
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

template <typename T>
std::optional<T> ReadSysctlIntegral(const char *name) {
  T value{};
  usize size = sizeof(value);
  if (sysctlbyname(name, &value, &size, nullptr, 0) != 0 || size != sizeof(value)) {
    return std::nullopt;
  }
  return value;
}

void *OpenLibrary(const std::vector<const char *> &paths, std::string &chosen_path,
                  std::string &error) {
  for (const char *path : paths) {
    dlerror();
    void *handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (handle != nullptr) {
      chosen_path = path;
      return handle;
    }
    if (const char *dl_error = dlerror(); dl_error != nullptr) {
      error = dl_error;
    }
  }
  return nullptr;
}

template <typename Fn>
bool LoadSymbol(void *handle, const char *name, Fn &target, std::string &error) {
  dlerror();
  void *symbol = dlsym(handle, name);
  if (const char *dl_error = dlerror(); dl_error != nullptr) {
    error = dl_error;
    return false;
  }
  target = reinterpret_cast<Fn>(symbol);
  return true;
}

struct Api {
  void *kperf_handle = nullptr;
  void *kperfdata_handle = nullptr;

  int (*kpc_cpu_string)(char *buf, usize buf_size) = nullptr;
  u32 (*kpc_pmu_version)(void) = nullptr;
  u32 (*kpc_get_counting)(void) = nullptr;
  int (*kpc_set_counting)(u32 classes) = nullptr;
  u32 (*kpc_get_thread_counting)(void) = nullptr;
  int (*kpc_set_thread_counting)(u32 classes) = nullptr;
  int (*kpc_set_config)(u32 classes, kpc_config_t *config) = nullptr;
  u32 (*kpc_get_counter_count)(u32 classes) = nullptr;
  int (*kpc_get_thread_counters)(u32 tid, u32 buf_count, u64 *buf) = nullptr;
  int (*kpc_force_all_ctrs_set)(int val) = nullptr;
  int (*kpc_force_all_ctrs_get)(int *val_out) = nullptr;

  int (*kpep_config_create)(kpep_db *db, kpep_config **cfg_ptr) = nullptr;
  void (*kpep_config_free)(kpep_config *cfg) = nullptr;
  int (*kpep_config_add_event)(kpep_config *cfg, kpep_event **ev_ptr, u32 flag,
                               u32 *err) = nullptr;
  int (*kpep_config_force_counters)(kpep_config *cfg) = nullptr;
  int (*kpep_config_kpc)(kpep_config *cfg, kpc_config_t *buf, usize buf_size) = nullptr;
  int (*kpep_config_kpc_count)(kpep_config *cfg, usize *count_ptr) = nullptr;
  int (*kpep_config_kpc_classes)(kpep_config *cfg, u32 *classes_ptr) = nullptr;
  int (*kpep_config_kpc_map)(kpep_config *cfg, usize *buf, usize buf_size) = nullptr;
  int (*kpep_db_create)(const char *name, kpep_db **db_ptr) = nullptr;
  void (*kpep_db_free)(kpep_db *db) = nullptr;
  int (*kpep_db_name)(kpep_db *db, const char **name) = nullptr;
  int (*kpep_db_event)(kpep_db *db, const char *name, kpep_event **ev_ptr) = nullptr;
  int (*kpep_event_name)(kpep_event *ev, const char **name_ptr) = nullptr;
  int (*kpep_event_alias)(kpep_event *ev, const char **alias_ptr) = nullptr;

  ~Api() {
    if (kperf_handle != nullptr) {
      dlclose(kperf_handle);
    }
    if (kperfdata_handle != nullptr) {
      dlclose(kperfdata_handle);
    }
  }

  bool Load(std::string &error) {
    std::string chosen_kperf;
    std::string chosen_kperfdata;

    kperf_handle = OpenLibrary(
        {
            "/System/Library/PrivateFrameworks/kperf.framework/Versions/A/kperf",
            "/System/Library/PrivateFrameworks/kperf.framework/kperf",
        },
        chosen_kperf, error);
    if (kperf_handle == nullptr) {
      error = "failed to load kperf.framework: " + error;
      return false;
    }

    kperfdata_handle = OpenLibrary(
        {
            "/System/Library/PrivateFrameworks/kperfdata.framework/Versions/A/kperfdata",
            "/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata",
        },
        chosen_kperfdata, error);
    if (kperfdata_handle == nullptr) {
      error = "failed to load kperfdata.framework: " + error;
      return false;
    }

    auto load_kperf = [&](const char *name, auto &target) -> bool {
      if (!LoadSymbol(kperf_handle, name, target, error)) {
        error = std::string("missing kperf symbol ") + name + ": " + error;
        return false;
      }
      return true;
    };

    auto load_kperfdata = [&](const char *name, auto &target) -> bool {
      if (!LoadSymbol(kperfdata_handle, name, target, error)) {
        error = std::string("missing kperfdata symbol ") + name + ": " + error;
        return false;
      }
      return true;
    };

    return load_kperf("kpc_cpu_string", kpc_cpu_string) &&
           load_kperf("kpc_pmu_version", kpc_pmu_version) &&
           load_kperf("kpc_get_counting", kpc_get_counting) &&
           load_kperf("kpc_set_counting", kpc_set_counting) &&
           load_kperf("kpc_get_thread_counting", kpc_get_thread_counting) &&
           load_kperf("kpc_set_thread_counting", kpc_set_thread_counting) &&
           load_kperf("kpc_set_config", kpc_set_config) &&
           load_kperf("kpc_get_counter_count", kpc_get_counter_count) &&
           load_kperf("kpc_get_thread_counters", kpc_get_thread_counters) &&
           load_kperf("kpc_force_all_ctrs_set", kpc_force_all_ctrs_set) &&
           load_kperf("kpc_force_all_ctrs_get", kpc_force_all_ctrs_get) &&
           load_kperfdata("kpep_config_create", kpep_config_create) &&
           load_kperfdata("kpep_config_free", kpep_config_free) &&
           load_kperfdata("kpep_config_add_event", kpep_config_add_event) &&
           load_kperfdata("kpep_config_force_counters", kpep_config_force_counters) &&
           load_kperfdata("kpep_config_kpc", kpep_config_kpc) &&
           load_kperfdata("kpep_config_kpc_count", kpep_config_kpc_count) &&
           load_kperfdata("kpep_config_kpc_classes", kpep_config_kpc_classes) &&
           load_kperfdata("kpep_config_kpc_map", kpep_config_kpc_map) &&
           load_kperfdata("kpep_db_create", kpep_db_create) &&
           load_kperfdata("kpep_db_free", kpep_db_free) &&
           load_kperfdata("kpep_db_name", kpep_db_name) &&
           load_kperfdata("kpep_db_event", kpep_db_event) &&
           load_kperfdata("kpep_event_name", kpep_event_name) &&
           load_kperfdata("kpep_event_alias", kpep_event_alias);
  }
};

u64 *AlignU64(std::vector<u8> &storage, usize alignment) {
  auto raw = reinterpret_cast<std::uintptr_t>(storage.data());
  raw = (raw + alignment - 1) & ~(alignment - 1);
  return reinterpret_cast<u64 *>(raw);
}

std::vector<u32> BuildSequentialRing(usize count) {
  std::vector<u32> ring(count);
  for (usize i = 0; i < count; ++i) {
    ring[i] = static_cast<u32>((i + 1) % count);
  }
  return ring;
}

std::vector<u32> BuildRandomRing(usize count, u64 seed) {
  std::vector<u32> order(count);
  std::iota(order.begin(), order.end(), 0u);
  std::mt19937_64 rng(seed);
  std::shuffle(order.begin(), order.end(), rng);

  std::vector<u32> ring(count);
  for (usize i = 0; i < count; ++i) {
    ring[order[i]] = order[(i + 1) % count];
  }
  return ring;
}

std::vector<usize> BuildShuffledSequence(usize count, u64 seed) {
  std::vector<usize> order(count);
  std::iota(order.begin(), order.end(), 0);
  std::mt19937_64 rng(seed);
  std::shuffle(order.begin(), order.end(), rng);
  return order;
}

struct MemoryBenchState {
  usize page_size = 0;
  std::vector<u32> hot_ring;
  std::vector<u32> random_ring;
  std::vector<u64> stream_read;
  std::vector<u64> hot_store;
  std::vector<u64> stream_store;

  std::vector<u8> page_storage;
  u64 *page_base = nullptr;
  usize page_count = 0;
  std::vector<usize> page_order;

  std::vector<u8> line_storage;
  u64 *line_base = nullptr;
  usize line_count = 0;

  bool Initialize() {
    page_size = static_cast<usize>(::getpagesize());

    constexpr usize kHotBytes = 32 * 1024;
    constexpr usize kStreamBytes = 64 * 1024 * 1024;
    constexpr usize kRandomRingBytes = 32 * 1024 * 1024;
    constexpr usize kPageWorkingSetBytes = 64 * 1024 * 1024;
    constexpr usize kLineWorkingSetBytes = 16 * 1024 * 1024;

    hot_ring = BuildSequentialRing(kHotBytes / sizeof(u32));
    random_ring = BuildRandomRing(kRandomRingBytes / sizeof(u32), 0x5a17d3b9ULL);

    stream_read.resize(kStreamBytes / sizeof(u64));
    stream_store.resize(kStreamBytes / sizeof(u64));
    hot_store.resize(kHotBytes / sizeof(u64));

    for (usize i = 0; i < stream_read.size(); ++i) {
      stream_read[i] = static_cast<u64>(i * 1315423911ULL);
      stream_store[i] = static_cast<u64>(i ^ 0x9e3779b97f4a7c15ULL);
    }
    for (usize i = 0; i < hot_store.size(); ++i) {
      hot_store[i] = static_cast<u64>(i);
    }

    page_count = kPageWorkingSetBytes / page_size;
    page_order = BuildShuffledSequence(page_count, 0x1234fedcULL);
    page_storage.resize(page_count * page_size + page_size + 64);
    page_base = AlignU64(page_storage, page_size);
    const usize page_words = page_size / sizeof(u64);
    for (usize page = 0; page <= page_count; ++page) {
      volatile u64 *word = page_base + page * page_words;
      *word = static_cast<u64>(page * 17);
      volatile u32 *cross = reinterpret_cast<volatile u32 *>(
          reinterpret_cast<u8 *>(page_base) + page * page_size + page_size - sizeof(u32));
      *cross = static_cast<u32>(page * 29);
    }

    line_count = kLineWorkingSetBytes / 64;
    line_storage.resize(line_count * 64 + 64);
    line_base = AlignU64(line_storage, 64);
    for (usize line = 0; line < line_count; ++line) {
      volatile u64 *slot = reinterpret_cast<volatile u64 *>(
          reinterpret_cast<u8 *>(line_base) + line * 64);
      *slot = static_cast<u64>(line * 7);
      volatile u32 *cross = reinterpret_cast<volatile u32 *>(
          reinterpret_cast<u8 *>(line_base) + line * 64 + 60);
      *cross = static_cast<u32>(line * 11);
    }

    return true;
  }

  usize PageWords() const { return page_size / sizeof(u64); }
};

[[gnu::noinline]] u64 HotSequentialRead(MemoryBenchState &state) {
  volatile const u32 *ring = state.hot_ring.data();
  u32 index = 0;
  constexpr usize kSteps = 32'000'000;
  for (usize i = 0; i < kSteps; ++i) {
    index = ring[index];
  }
  g_sink += index;
  return index;
}

[[gnu::noinline]] u64 StreamingReadScalar(MemoryBenchState &state) {
  const u64 *data = state.stream_read.data();
  u64 sum = 0;
  constexpr usize kPasses = 8;
  for (usize pass = 0; pass < kPasses; ++pass) {
#pragma clang loop vectorize(disable)
#pragma clang loop interleave(disable)
    for (usize i = 0; i < state.stream_read.size(); ++i) {
      sum += data[i];
    }
  }
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] u64 StreamingReadSimd(MemoryBenchState &state) {
  const u64 *data = state.stream_read.data();
  uint64x2_t acc = vdupq_n_u64(0);
  constexpr usize kPasses = 8;
  constexpr usize kLanes = 2;
  const usize limit = state.stream_read.size() - (state.stream_read.size() % kLanes);

  for (usize pass = 0; pass < kPasses; ++pass) {
    for (usize i = 0; i < limit; i += kLanes) {
      acc = vaddq_u64(acc, vld1q_u64(data + i));
    }
  }

  u64 sum = vgetq_lane_u64(acc, 0) + vgetq_lane_u64(acc, 1);
  for (usize i = limit; i < state.stream_read.size(); ++i) {
    sum += data[i];
  }

  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] u64 RandomPointerChase(MemoryBenchState &state) {
  volatile const u32 *ring = state.random_ring.data();
  u32 index = 0;
  constexpr usize kSteps = 2'000'000;
  for (usize i = 0; i < kSteps; ++i) {
    index = ring[index];
  }
  g_sink += index;
  return index;
}

[[gnu::noinline]] u64 PageStrideRead(MemoryBenchState &state) {
  volatile const u64 *base = state.page_base;
  u64 sum = 0;
  const usize stride = state.PageWords();
  constexpr usize kPasses = 256;
  for (usize pass = 0; pass < kPasses; ++pass) {
    for (usize page : state.page_order) {
      sum += base[page * stride];
    }
  }
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] u64 HotSequentialWrite(MemoryBenchState &state) {
  u64 *data = state.hot_store.data();
  constexpr usize kPasses = 8192;
  for (usize pass = 0; pass < kPasses; ++pass) {
#pragma clang loop vectorize(disable)
#pragma clang loop interleave(disable)
    for (usize i = 0; i < state.hot_store.size(); ++i) {
      data[i] += static_cast<u64>(i + pass);
    }
  }
  u64 sample = data[7];
  g_sink += sample;
  return sample;
}

[[gnu::noinline]] u64 StreamingWriteScalar(MemoryBenchState &state) {
  u64 *data = state.stream_store.data();
  constexpr usize kPasses = 4;
  for (usize pass = 0; pass < kPasses; ++pass) {
#pragma clang loop vectorize(disable)
#pragma clang loop interleave(disable)
    for (usize i = 0; i < state.stream_store.size(); ++i) {
      data[i] = static_cast<u64>(i + pass * 3);
    }
  }
  u64 sample = data[state.stream_store.size() / 2];
  g_sink ^= sample;
  return sample;
}

[[gnu::noinline]] u64 StreamingWriteSimd(MemoryBenchState &state) {
  u64 *data = state.stream_store.data();
  constexpr usize kPasses = 4;
  constexpr usize kLanes = 2;
  const usize limit = state.stream_store.size() - (state.stream_store.size() % kLanes);

  for (usize pass = 0; pass < kPasses; ++pass) {
    const u64 base = static_cast<u64>(pass) << 32;
    for (usize i = 0; i < limit; i += kLanes) {
      uint64x2_t values = vdupq_n_u64(0);
      values = vsetq_lane_u64(base + i, values, 0);
      values = vsetq_lane_u64(base + i + 1, values, 1);
      vst1q_u64(data + i, values);
    }
    for (usize i = limit; i < state.stream_store.size(); ++i) {
      data[i] = base + i;
    }
  }

  u64 sample = data[state.stream_store.size() / 2];
  g_sink ^= sample;
  return sample;
}

[[gnu::noinline]] u64 RandomPageWrite(MemoryBenchState &state) {
  volatile u64 *base = state.page_base;
  const usize stride = state.PageWords();
  constexpr usize kPasses = 256;
  for (usize pass = 0; pass < kPasses; ++pass) {
    for (usize page : state.page_order) {
      base[page * stride] += static_cast<u64>(pass + 1);
    }
  }
  u64 sample = base[state.page_order.front() * stride];
  g_sink += sample;
  return sample;
}

[[gnu::noinline]] u64 AlignedCacheLineLoad(MemoryBenchState &state) {
  u64 sum = 0;
  constexpr usize kPasses = 16;
  for (usize pass = 0; pass < kPasses; ++pass) {
    for (usize line = 0; line < state.line_count; ++line) {
      volatile const u64 *ptr =
          reinterpret_cast<volatile const u64 *>(reinterpret_cast<u8 *>(state.line_base) +
                                                 line * 64);
      sum += *ptr;
    }
  }
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] u64 CrossCacheLineLoad(MemoryBenchState &state) {
  u64 sum = 0;
  constexpr usize kPasses = 16;
  for (usize pass = 0; pass < kPasses; ++pass) {
    for (usize line = 0; line < state.line_count; ++line) {
      volatile const u64 *ptr = reinterpret_cast<volatile const u64 *>(
          reinterpret_cast<u8 *>(state.line_base) + line * 64 + 60);
      sum += *ptr;
    }
  }
  g_sink ^= sum;
  return sum;
}

[[gnu::noinline]] u64 AlignedPageLoad(MemoryBenchState &state) {
  u64 sum = 0;
  constexpr usize kPasses = 256;
  for (usize pass = 0; pass < kPasses; ++pass) {
    for (usize page : state.page_order) {
      volatile const u64 *ptr = reinterpret_cast<volatile const u64 *>(
          reinterpret_cast<u8 *>(state.page_base) + page * state.page_size);
      sum += *ptr;
    }
  }
  g_sink += sum;
  return sum;
}

[[gnu::noinline]] u64 CrossPageLoad(MemoryBenchState &state) {
  u64 sum = 0;
  constexpr usize kPasses = 256;
  for (usize pass = 0; pass < kPasses; ++pass) {
    for (usize page : state.page_order) {
      volatile const u64 *ptr = reinterpret_cast<volatile const u64 *>(
          reinterpret_cast<u8 *>(state.page_base) + page * state.page_size +
          state.page_size - sizeof(u32));
      sum += *ptr;
    }
  }
  g_sink += sum;
  return sum;
}

[[gnu::noinline]] u64 AlignedCacheLineStore(MemoryBenchState &state) {
  constexpr usize kPasses = 16;
  for (usize pass = 0; pass < kPasses; ++pass) {
    for (usize line = 0; line < state.line_count; ++line) {
      volatile u64 *ptr =
          reinterpret_cast<volatile u64 *>(reinterpret_cast<u8 *>(state.line_base) +
                                           line * 64);
      *ptr += static_cast<u64>(pass + 1);
    }
  }
  u64 sample = *reinterpret_cast<volatile u64 *>(state.line_base);
  g_sink ^= sample;
  return sample;
}

[[gnu::noinline]] u64 CrossCacheLineStore(MemoryBenchState &state) {
  constexpr usize kPasses = 16;
  for (usize pass = 0; pass < kPasses; ++pass) {
    for (usize line = 0; line < state.line_count; ++line) {
      volatile u64 *ptr = reinterpret_cast<volatile u64 *>(
          reinterpret_cast<u8 *>(state.line_base) + line * 64 + 60);
      *ptr += static_cast<u64>(pass + 1);
    }
  }
  u64 sample = *reinterpret_cast<volatile u64 *>(reinterpret_cast<u8 *>(state.line_base) + 60);
  g_sink ^= sample;
  return sample;
}

[[gnu::noinline]] u64 CrossPageStore(MemoryBenchState &state) {
  constexpr usize kPasses = 256;
  for (usize pass = 0; pass < kPasses; ++pass) {
    for (usize page : state.page_order) {
      volatile u64 *ptr = reinterpret_cast<volatile u64 *>(
          reinterpret_cast<u8 *>(state.page_base) + page * state.page_size +
          state.page_size - sizeof(u32));
      *ptr += static_cast<u64>(pass + 1);
    }
  }
  u64 sample = *reinterpret_cast<volatile u64 *>(
      reinterpret_cast<u8 *>(state.page_base) + state.page_size - sizeof(u32));
  g_sink += sample;
  return sample;
}

struct WorkloadDefinition {
  std::string_view name;
  std::string_view description;
  u64 (*run)(MemoryBenchState &state);
};

struct EventGroupDefinition {
  std::string_view name;
  std::string_view description;
  std::array<const char *, 5> configurable_events;
};

struct CounterProgram {
  std::string name;
  std::string description;
  std::vector<std::string> event_names;
  std::vector<std::string> event_aliases;
  std::vector<usize> hardware_slots;
  std::vector<usize> counter_slots;
  u32 classes = 0;
  u32 fixed_count = 0;
  u32 configurable_count = 0;
  usize reg_count = 0;
  u32 active_count = 0;
  std::array<kpc_config_t, KPC_MAX_COUNTERS> regs{};
};

struct SampleResult {
  std::string workload_name;
  std::string workload_description;
  std::vector<u64> deltas;
  u64 sink = 0;
  double elapsed_ms = 0.0;
};

class Profiler {
 public:
  ~Profiler() {
    if (!initialized_) {
      return;
    }
    api_.kpc_set_thread_counting(0);
    api_.kpc_set_counting(0);
    if (forced_all_counters_) {
      api_.kpc_force_all_ctrs_set(0);
    }
    if (db_ != nullptr) {
      api_.kpep_db_free(db_);
    }
  }

  bool Initialize(std::string &error) {
    if (!api_.Load(error)) {
      return false;
    }

    if (api_.kpep_db_create(nullptr, &db_) != 0 || db_ == nullptr) {
      error = "failed to open the local kpep database";
      return false;
    }

    const int ret = api_.kpc_force_all_ctrs_set(1);
    if (ret != 0) {
      error = "kpc_force_all_ctrs_set(1) failed; run this binary with sudo";
      return false;
    }
    forced_all_counters_ = true;
    initialized_ = true;
    return true;
  }

  Api &api() { return api_; }

  std::string DatabaseName() const {
    if (db_ == nullptr) {
      return {};
    }
    const char *name = nullptr;
    if (api_.kpep_db_name(db_, &name) != 0 || name == nullptr) {
      return {};
    }
    return name;
  }

  bool BuildProgram(const EventGroupDefinition &group, CounterProgram &program,
                    std::string &error) {
    if (!initialized_) {
      error = "profiler is not initialized";
      return false;
    }

    program = {};
    program.name = std::string(group.name);
    program.description = std::string(group.description);

    kpep_config *config = nullptr;
    int ret = api_.kpep_config_create(db_, &config);
    if (ret != 0 || config == nullptr) {
      error = "kpep_config_create failed: " + std::to_string(ret) + " (" +
              KpepErrorString(ret) + ")";
      return false;
    }

    struct ConfigCleanup {
      Api &api;
      kpep_config *config;
      ~ConfigCleanup() {
        if (config != nullptr) {
          api.kpep_config_free(config);
        }
      }
    } cleanup{api_, config};

    ret = api_.kpep_config_force_counters(config);
    if (ret != 0) {
      error = "kpep_config_force_counters failed: " + std::to_string(ret) + " (" +
              KpepErrorString(ret) + ")";
      return false;
    }

    std::vector<const char *> requested_names;
    requested_names.insert(requested_names.end(), kFixedEvents.begin(), kFixedEvents.end());
    requested_names.insert(requested_names.end(), group.configurable_events.begin(),
                           group.configurable_events.end());

    for (const char *event_name : requested_names) {
      kpep_event *event = nullptr;
      ret = api_.kpep_db_event(db_, event_name, &event);
      if (ret != 0 || event == nullptr) {
        error = std::string("kpep_db_event failed for ") + event_name + ": " +
                std::to_string(ret) + " (" + KpepErrorString(ret) + ")";
        return false;
      }

      ret = api_.kpep_config_add_event(config, &event, 0, nullptr);
      if (ret != 0) {
        error = std::string("kpep_config_add_event failed for ") + event_name + ": " +
                std::to_string(ret) + " (" + KpepErrorString(ret) + ")";
        return false;
      }

      const char *resolved_name = nullptr;
      const char *alias = nullptr;
      api_.kpep_event_name(event, &resolved_name);
      api_.kpep_event_alias(event, &alias);
      program.event_names.emplace_back(resolved_name != nullptr ? resolved_name : event_name);
      program.event_aliases.emplace_back(alias != nullptr ? alias : "");
    }

    ret = api_.kpep_config_kpc_classes(config, &program.classes);
    if (ret != 0) {
      error = "kpep_config_kpc_classes failed: " + std::to_string(ret) + " (" +
              KpepErrorString(ret) + ")";
      return false;
    }

    ret = api_.kpep_config_kpc_count(config, &program.reg_count);
    if (ret != 0) {
      error = "kpep_config_kpc_count failed: " + std::to_string(ret) + " (" +
              KpepErrorString(ret) + ")";
      return false;
    }

    program.hardware_slots.resize(program.event_names.size());
    ret = api_.kpep_config_kpc_map(config, program.hardware_slots.data(),
                                   program.hardware_slots.size() *
                                       sizeof(program.hardware_slots[0]));
    if (ret != 0) {
      error = "kpep_config_kpc_map failed: " + std::to_string(ret) + " (" +
              KpepErrorString(ret) + ")";
      return false;
    }

    ret = api_.kpep_config_kpc(config, program.regs.data(),
                               program.regs.size() * sizeof(program.regs[0]));
    if (ret != 0) {
      error = "kpep_config_kpc failed: " + std::to_string(ret) + " (" +
              KpepErrorString(ret) + ")";
      return false;
    }

    program.fixed_count = api_.kpc_get_counter_count(KPC_CLASS_FIXED_MASK);
    program.active_count = api_.kpc_get_counter_count(program.classes);
    if (program.active_count == 0 || program.active_count > KPC_MAX_COUNTERS) {
      error = "unexpected active counter count: " + std::to_string(program.active_count);
      return false;
    }
    program.configurable_count =
        program.active_count > program.fixed_count ? program.active_count - program.fixed_count
                                                   : 0;
    program.counter_slots = program.hardware_slots;
    for (usize slot : program.counter_slots) {
      if (slot >= program.active_count) {
        error = "event map slot " + std::to_string(slot) +
                " is outside the active counter range";
        return false;
      }
    }
    return true;
  }

  bool Measure(const CounterProgram &program, const WorkloadDefinition &workload,
               MemoryBenchState &state, SampleResult &result, std::string &error) {
    if (!initialized_) {
      error = "profiler is not initialized";
      return false;
    }

    api_.kpc_set_thread_counting(0);
    api_.kpc_set_counting(0);

    if ((program.classes & KPC_CLASS_CONFIGURABLE_MASK) != 0 && program.reg_count != 0) {
      const int ret = api_.kpc_set_config(program.classes,
                                          const_cast<kpc_config_t *>(program.regs.data()));
      if (ret != 0) {
        error = "kpc_set_config failed: " + std::to_string(ret);
        return false;
      }
    }

    int ret = api_.kpc_set_counting(program.classes);
    if (ret != 0) {
      error = "kpc_set_counting failed: " + std::to_string(ret);
      return false;
    }

    ret = api_.kpc_set_thread_counting(program.classes);
    if (ret != 0) {
      error = "kpc_set_thread_counting failed: " + std::to_string(ret);
      return false;
    }

    std::array<u64, KPC_MAX_COUNTERS> before{};
    std::array<u64, KPC_MAX_COUNTERS> after{};

    ret = api_.kpc_get_thread_counters(0, program.active_count, before.data());
    if (ret != 0) {
      error = "kpc_get_thread_counters(before) failed: " + std::to_string(ret);
      return false;
    }

    const auto started = std::chrono::steady_clock::now();
    const u64 sink = workload.run(state);
    const auto finished = std::chrono::steady_clock::now();

    ret = api_.kpc_get_thread_counters(0, program.active_count, after.data());
    if (ret != 0) {
      error = "kpc_get_thread_counters(after) failed: " + std::to_string(ret);
      return false;
    }

    api_.kpc_set_thread_counting(0);
    api_.kpc_set_counting(0);

    result = {};
    result.workload_name = std::string(workload.name);
    result.workload_description = std::string(workload.description);
    result.deltas.reserve(program.event_names.size());
    result.sink = sink;
    result.elapsed_ms =
        std::chrono::duration<double, std::milli>(finished - started).count();

    for (usize i = 0; i < program.event_names.size(); ++i) {
      const usize slot = program.counter_slots[i];
      result.deltas.push_back(after[slot] - before[slot]);
    }

    return true;
  }

 private:
  Api api_;
  kpep_db *db_ = nullptr;
  bool initialized_ = false;
  bool forced_all_counters_ = false;
};

std::optional<usize> FindEventIndex(const CounterProgram &program, std::string_view name) {
  for (usize i = 0; i < program.event_names.size(); ++i) {
    if (program.event_names[i] == name) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<u64> FindEventDelta(const CounterProgram &program, const SampleResult &result,
                                  std::string_view name) {
  const auto index = FindEventIndex(program, name);
  if (!index.has_value()) {
    return std::nullopt;
  }
  return result.deltas[*index];
}

double RatioPerKilo(u64 numerator, u64 denominator) {
  if (denominator == 0) {
    return 0.0;
  }
  return static_cast<double>(numerator) * 1000.0 / static_cast<double>(denominator);
}

void PrintProgramHeader(const CounterProgram &program) {
  std::cout << "\n== Group: " << program.name << " ==\n";
  std::cout << program.description << '\n';
  std::cout << "active counters: fixed=" << program.fixed_count
            << " configurable=" << program.configurable_count
            << " total=" << program.active_count << '\n';
  std::cout << "events:";
  for (usize i = 0; i < program.event_names.size(); ++i) {
    std::cout << "\n  [" << i << "] " << program.event_names[i];
    if (!program.event_aliases[i].empty()) {
      std::cout << " alias=" << program.event_aliases[i];
    }
    std::cout << " hw_slot=" << program.hardware_slots[i]
              << " counter_slot=" << program.counter_slots[i];
  }
  std::cout << '\n';
}

void PrintDerivedMetrics(const CounterProgram &program, const SampleResult &result) {
  auto append_ratio = [&](std::string_view label, u64 numerator,
                          std::optional<u64> denominator) {
    if (!denominator.has_value() || *denominator == 0) {
      return;
    }
    std::cout << ' ' << label << '=' << std::fixed << std::setprecision(3)
              << RatioPerKilo(numerator, *denominator);
  };

  const auto cycles = FindEventDelta(program, result, "FIXED_CYCLES");
  const auto instructions = FindEventDelta(program, result, "FIXED_INSTRUCTIONS");
  if (cycles.has_value() && instructions.has_value() && *cycles != 0 &&
      *instructions != 0) {
    std::cout << "  derived: ipc=" << std::fixed << std::setprecision(3)
              << static_cast<double>(*instructions) / static_cast<double>(*cycles)
              << " cpi=" << std::fixed << std::setprecision(3)
              << static_cast<double>(*cycles) / static_cast<double>(*instructions);
  } else {
    std::cout << "  derived:";
  }

  const auto ldst = FindEventDelta(program, result, "INST_LDST");
  if (const auto load_miss = FindEventDelta(program, result, "L1D_CACHE_MISS_LD")) {
    append_ratio("l1d_load_miss_per_kldst", *load_miss, ldst);
  }
  if (const auto store_miss = FindEventDelta(program, result, "L1D_CACHE_MISS_ST")) {
    append_ratio("l1d_store_miss_per_kldst", *store_miss, ldst);
  }
  if (const auto load_miss = FindEventDelta(program, result, "L1D_CACHE_MISS_LD")) {
    if (const auto store_miss = FindEventDelta(program, result, "L1D_CACHE_MISS_ST")) {
      append_ratio("l1d_total_miss_per_kldst", *load_miss + *store_miss, ldst);
    }
  }
  if (const auto dtlb_miss = FindEventDelta(program, result, "L1D_TLB_MISS")) {
    append_ratio("dtlb_miss_per_kldst", *dtlb_miss, ldst);
  }
  if (const auto l2_dtlb_miss = FindEventDelta(program, result, "L2_TLB_MISS_DATA")) {
    append_ratio("l2_dtlb_miss_per_kldst", *l2_dtlb_miss, ldst);
  }
  if (const auto walks = FindEventDelta(program, result, "MMU_TABLE_WALK_DATA")) {
    append_ratio("table_walk_per_kldst", *walks, ldst);
  }
  if (const auto writebacks = FindEventDelta(program, result, "L1D_CACHE_WRITEBACK")) {
    append_ratio("writeback_per_kldst", *writebacks, ldst);
  }
  if (const auto int_loads = FindEventDelta(program, result, "INST_INT_LD")) {
    append_ratio("int_load_per_kldst", *int_loads, ldst);
  }
  if (const auto simd_loads = FindEventDelta(program, result, "INST_SIMD_LD")) {
    append_ratio("simd_load_per_kldst", *simd_loads, ldst);
  }
  if (const auto int_stores = FindEventDelta(program, result, "INST_INT_ST")) {
    append_ratio("int_store_per_kldst", *int_stores, ldst);
  }
  if (const auto simd_stores = FindEventDelta(program, result, "INST_SIMD_ST")) {
    append_ratio("simd_store_per_kldst", *simd_stores, ldst);
  }
  if (const auto x64 = FindEventDelta(program, result, "LDST_X64_UOP")) {
    append_ratio("x64_cross_per_kldst", *x64, ldst);
  }
  if (const auto xpg = FindEventDelta(program, result, "LDST_XPG_UOP")) {
    append_ratio("xpg_cross_per_kldst", *xpg, ldst);
  }
  std::cout << '\n';
}

void PrintSkippedGroup(const EventGroupDefinition &group, const std::string &error) {
  std::cout << "\n== Group: " << group.name << " ==\n";
  std::cout << group.description << '\n';
  std::cout << "skipped: " << error << '\n';
}

void PrintSampleResult(const CounterProgram &program, const SampleResult &result) {
  std::cout << "- workload: " << result.workload_name << '\n';
  std::cout << "  " << result.workload_description << '\n';
  std::cout << "  elapsed_ms=" << std::fixed << std::setprecision(3) << result.elapsed_ms
            << " sink=" << result.sink << '\n';

  const auto instructions = FindEventDelta(program, result, "FIXED_INSTRUCTIONS");
  for (usize i = 0; i < program.event_names.size(); ++i) {
    std::cout << "  " << program.event_names[i] << '=' << result.deltas[i];
    if (!program.event_aliases[i].empty()) {
      std::cout << " (" << program.event_aliases[i] << ')';
    }
    if (instructions.has_value() && *instructions != 0 &&
        program.event_names[i] != "FIXED_INSTRUCTIONS") {
      std::cout << " per_kinst=" << std::fixed << std::setprecision(3)
                << RatioPerKilo(result.deltas[i], *instructions);
    }
    std::cout << '\n';
  }
  PrintDerivedMetrics(program, result);
}

int main() {
  std::cout.setf(std::ios::unitbuf);

  std::cout << "cpu brand: " << ReadSysctlString("machdep.cpu.brand_string") << '\n';
  if (const auto cpu_family = ReadSysctlIntegral<u32>("hw.cpufamily")) {
    std::cout << "hw.cpufamily: " << HexString(*cpu_family) << '\n';
  }
  if (const auto cache_line = ReadSysctlIntegral<u32>("hw.cachelinesize")) {
    std::cout << "hw.cachelinesize: " << *cache_line << '\n';
  }
  if (const auto page_size = ReadSysctlIntegral<u32>("hw.pagesize")) {
    std::cout << "hw.pagesize: " << *page_size << '\n';
  }

  Profiler profiler;
  std::string error;
  if (!profiler.Initialize(error)) {
    std::cerr << error << '\n';
    return 1;
  }

  std::array<char, 128> cpu_string{};
  if (profiler.api().kpc_cpu_string(cpu_string.data(), cpu_string.size()) >= 0) {
    std::cout << "kpc cpu string: " << cpu_string.data() << '\n';
    if (const std::string plist = ResolveKpepPlist(cpu_string.data()); !plist.empty()) {
      std::cout << "kpep plist: /usr/share/kpep/" << cpu_string.data() << ".plist -> " << plist
                << '\n';
    }
  }

  std::cout << "kpep database: " << profiler.DatabaseName() << '\n';
  const u32 pmu_version = profiler.api().kpc_pmu_version();
  std::cout << "pmu version: " << pmu_version << " (" << PmuVersionString(pmu_version)
            << ")\n";
  std::cout << "current counting mask: " << HexString(profiler.api().kpc_get_counting()) << '\n';
  std::cout << "current thread counting mask: "
            << HexString(profiler.api().kpc_get_thread_counting()) << '\n';
  int force_all_counters = 0;
  if (profiler.api().kpc_force_all_ctrs_get(&force_all_counters) == 0) {
    std::cout << "force_all_ctrs: " << force_all_counters << '\n';
  }

  MemoryBenchState state;
  if (!state.Initialize()) {
    std::cerr << "failed to initialize benchmark state\n";
    return 1;
  }

  const WorkloadDefinition hot_seq_read{
      "hot_seq_read",
      "32 KiB dependent load ring that stays resident in L1D and should show very low data-cache and DTLB miss pressure.",
      &HotSequentialRead,
  };
  const WorkloadDefinition stream_read{
      "stream_read_scalar",
      "64 MiB scalar sequential read stream with vectorization disabled so INST_INT_LD has a clean target.",
      &StreamingReadScalar,
  };
  const WorkloadDefinition simd_stream_read{
      "stream_read_simd",
      "64 MiB explicit NEON read stream so INST_SIMD_LD lights up without relying on auto-vectorization.",
      &StreamingReadSimd,
  };
  const WorkloadDefinition random_read{
      "random_pointer_chase",
      "32 MiB random dependent load chain that should drive high L1D miss rates and poor prefetch behavior.",
      &RandomPointerChase,
  };
  const WorkloadDefinition page_read{
      "page_stride_read",
      "One demand load per randomly ordered page across 64 MiB to emphasize DTLB misses and table walks.",
      &PageStrideRead,
  };
  const WorkloadDefinition hot_seq_write{
      "hot_seq_write",
      "32 KiB sequential write loop that should keep store misses low because the working set fits in L1D.",
      &HotSequentialWrite,
  };
  const WorkloadDefinition stream_write{
      "stream_write_scalar",
      "64 MiB scalar sequential write stream with vectorization disabled so INST_INT_ST has a clean target.",
      &StreamingWriteScalar,
  };
  const WorkloadDefinition simd_stream_write{
      "stream_write_simd",
      "64 MiB explicit NEON write stream so INST_SIMD_ST can be demonstrated directly.",
      &StreamingWriteSimd,
  };
  const WorkloadDefinition random_write{
      "random_page_write",
      "One store per randomly ordered page across 64 MiB to drive store misses plus DTLB pressure.",
      &RandomPageWrite,
  };
  const WorkloadDefinition aligned_line_load{
      "aligned_x64_load",
      "Load from the start of each 64-byte region as a baseline with almost no split-64B accesses.",
      &AlignedCacheLineLoad,
  };
  const WorkloadDefinition cross_line_load{
      "cross_x64_load",
      "Load 8 bytes starting at byte 60 of each 64-byte region so every access crosses a 64-byte boundary.",
      &CrossCacheLineLoad,
  };
  const WorkloadDefinition aligned_page_load{
      "aligned_page_load",
      "Load from the start of each page as a baseline with no cross-page accesses.",
      &AlignedPageLoad,
  };
  const WorkloadDefinition cross_page_load{
      "cross_page_load",
      "Load 8 bytes starting 4 bytes before the page boundary so every access crosses into the next page.",
      &CrossPageLoad,
  };
  const WorkloadDefinition aligned_line_store{
      "aligned_x64_store",
      "Store to the start of each 64-byte region as a baseline with almost no split-64B accesses.",
      &AlignedCacheLineStore,
  };
  const WorkloadDefinition cross_line_store{
      "cross_x64_store",
      "Store 8 bytes starting at byte 60 of each 64-byte region so every store crosses a 64-byte boundary.",
      &CrossCacheLineStore,
  };
  const WorkloadDefinition cross_page_store{
      "cross_page_store",
      "Store 8 bytes starting 4 bytes before the page boundary so every store crosses into the next page.",
      &CrossPageStore,
  };

  const EventGroupDefinition memory_core_group{
      "memory_core",
      "Primary memory-boundness counters. This pass keeps the event set small on purpose because only five configurable PMCs fit at once and some combinations interact badly.",
      {
          "INST_LDST",
          "L1D_CACHE_MISS_LD",
          "L1D_CACHE_MISS_ST",
          "L1D_TLB_MISS",
          "MMU_TABLE_WALK_DATA",
      },
  };
  const EventGroupDefinition translation_group{
      "translation_deep",
      "Translation-focused counters for page-local versus page-scattered access patterns, including second-level DTLB pressure.",
      {
          "INST_LDST",
          "L1D_TLB_FILL",
          "L1D_TLB_MISS",
          "L2_TLB_MISS_DATA",
          "MMU_TABLE_WALK_DATA",
      },
  };
  const EventGroupDefinition load_mix_group{
      "load_mix",
      "Compare scalar and explicit-NEON read kernels using instruction-class counters plus load misses.",
      {
          "INST_LDST",
          "INST_INT_LD",
          "INST_SIMD_LD",
          "LD_UNIT_UOP",
          "L1D_CACHE_MISS_LD",
      },
  };
  const EventGroupDefinition store_mix_group{
      "store_mix",
      "Compare scalar and explicit-NEON write kernels using instruction-class counters, store misses, and writebacks.",
      {
          "INST_LDST",
          "INST_INT_ST",
          "INST_SIMD_ST",
          "L1D_CACHE_MISS_ST",
          "L1D_CACHE_WRITEBACK",
      },
  };
  const EventGroupDefinition crossing_group{
      "ldst_crossings",
      "Deliberately compare aligned accesses against cache-line-crossing and page-crossing accesses.",
      {
          "INST_LDST",
          "LD_UNIT_UOP",
          "ST_UNIT_UOP",
          "LDST_X64_UOP",
          "LDST_XPG_UOP",
      },
  };

  const std::array<const WorkloadDefinition *, 7> memory_core_workloads = {
      &hot_seq_read,
      &stream_read,
      &random_read,
      &page_read,
      &hot_seq_write,
      &stream_write,
      &random_write,
  };
  const std::array<const WorkloadDefinition *, 4> translation_workloads = {
      &hot_seq_read,
      &page_read,
      &random_read,
      &random_write,
  };
  const std::array<const WorkloadDefinition *, 4> load_mix_workloads = {
      &hot_seq_read,
      &stream_read,
      &simd_stream_read,
      &random_read,
  };
  const std::array<const WorkloadDefinition *, 4> store_mix_workloads = {
      &hot_seq_write,
      &stream_write,
      &simd_stream_write,
      &random_write,
  };
  const std::array<const WorkloadDefinition *, 7> crossing_workloads = {
      &aligned_line_load,
      &cross_line_load,
      &aligned_page_load,
      &cross_page_load,
      &aligned_line_store,
      &cross_line_store,
      &cross_page_store,
  };

  struct SuiteRun {
    const EventGroupDefinition *group;
    std::span<const WorkloadDefinition *const> workloads;
  };

  const std::array<SuiteRun, 5> suites = {
      SuiteRun{&memory_core_group, memory_core_workloads},
      SuiteRun{&translation_group, translation_workloads},
      SuiteRun{&load_mix_group, load_mix_workloads},
      SuiteRun{&store_mix_group, store_mix_workloads},
      SuiteRun{&crossing_group, crossing_workloads},
  };

  std::cout << "\nStarting memory counter suite. The groups are intentionally split because Apple only gives us a handful of configurable PMCs per pass, and some event combinations clearly interact.\n";
  std::cout << "This revision also separates scalar and explicit-NEON streaming kernels so the load/store instruction-class counters are easier to interpret.\n";
  std::cout << "LDST_X64_UOP counts 64-byte split accesses; on this machine hw.cachelinesize is 128, so the X64 workloads are about 64-byte boundaries rather than full L1 cache lines.\n";

  usize skipped_groups = 0;
  for (const SuiteRun &suite : suites) {
    CounterProgram program;
    if (!profiler.BuildProgram(*suite.group, program, error)) {
      PrintSkippedGroup(*suite.group, error);
      ++skipped_groups;
      error.clear();
      continue;
    }

    PrintProgramHeader(program);
    for (const WorkloadDefinition *workload : suite.workloads) {
      SampleResult result;
      if (!profiler.Measure(program, *workload, state, result, error)) {
        std::cerr << error << '\n';
        return 1;
      }
      PrintSampleResult(program, result);
    }
  }

  if (skipped_groups != 0) {
    std::cout << "\nskipped groups=" << skipped_groups << '\n';
  }
  std::cout << "\nfinal sink=" << g_sink << '\n';
  return 0;
}
