#include <dlfcn.h>
#include <sys/sysctl.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
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

constexpr std::array<const char *, 4> kRequestedEvents = {
    "FIXED_CYCLES",
    "FIXED_INSTRUCTIONS",
    "INST_BRANCH",
    "BRANCH_MISPRED_NONSPEC",
};

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
  u32 (*kpc_get_config_count)(u32 classes) = nullptr;
  int (*kpc_get_config)(u32 classes, kpc_config_t *config) = nullptr;
  int (*kpc_set_config)(u32 classes, kpc_config_t *config) = nullptr;
  u32 (*kpc_get_counter_count)(u32 classes) = nullptr;
  int (*kpc_get_cpu_counters)(bool all_cpus, u32 classes, int *curcpu, u64 *buf) = nullptr;
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
           load_kperf("kpc_get_config_count", kpc_get_config_count) &&
           load_kperf("kpc_get_config", kpc_get_config) &&
           load_kperf("kpc_set_config", kpc_set_config) &&
           load_kperf("kpc_get_counter_count", kpc_get_counter_count) &&
           load_kperf("kpc_get_cpu_counters", kpc_get_cpu_counters) &&
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

struct CleanupState {
  Api &api;
  bool restore_thread_counting = false;
  bool restore_counting = false;
  bool release_force = false;

  ~CleanupState() {
    if (restore_thread_counting) {
      api.kpc_set_thread_counting(0);
    }
    if (restore_counting) {
      api.kpc_set_counting(0);
    }
    if (release_force) {
      api.kpc_force_all_ctrs_set(0);
    }
  }
};

[[gnu::noinline]] u64 RunWorkload() {
  volatile u64 accumulator = 0;
  for (u64 outer = 0; outer < 128; ++outer) {
    for (u64 i = 1; i <= 250000; ++i) {
      accumulator += ((i * 0x9e3779b97f4a7c15ULL) ^ (accumulator >> 7)) & 0xffff;
      if ((i & 63u) == 0) {
        accumulator ^= outer;
      }
    }
  }
  return accumulator;
}

void PrintCounterArray(const std::string &label, const std::array<u64, KPC_MAX_COUNTERS> &values,
                       usize count) {
  std::cout << label << '\n';
  for (usize i = 0; i < count; ++i) {
    std::cout << "  [" << i << "] " << values[i] << '\n';
  }
}

int main() {
  std::cout.setf(std::ios::unitbuf);
  std::cout << "cpu brand: " << ReadSysctlString("machdep.cpu.brand_string") << '\n';

  if (const auto cpu_family = ReadSysctlIntegral<u32>("hw.cpufamily")) {
    std::cout << "hw.cpufamily: " << HexString(*cpu_family) << '\n';
  }

  Api api;
  std::string load_error;
  if (!api.Load(load_error)) {
    std::cerr << "failed to load private kperf APIs: " << load_error << '\n';
    return 1;
  }

  std::array<char, 128> cpu_string{};
  if (api.kpc_cpu_string(cpu_string.data(), cpu_string.size()) >= 0) {
    std::cout << "kpc cpu string: " << cpu_string.data() << '\n';
    if (const std::string plist = ResolveKpepPlist(cpu_string.data()); !plist.empty()) {
      std::cout << "kpep plist: /usr/share/kpep/" << cpu_string.data() << ".plist -> " << plist
                << '\n';
    }
  }

  const u32 pmu_version = api.kpc_pmu_version();
  std::cout << "pmu version: " << pmu_version << " (" << PmuVersionString(pmu_version)
            << ")\n";

  std::cout << "current counting mask: " << HexString(api.kpc_get_counting()) << '\n';
  std::cout << "current thread counting mask: "
            << HexString(api.kpc_get_thread_counting()) << '\n';

  int force_all_counters = 0;
  if (api.kpc_force_all_ctrs_get(&force_all_counters) == 0) {
    std::cout << "force_all_ctrs: " << force_all_counters << '\n';
  }

  kpep_db *db = nullptr;
  int ret = api.kpep_db_create(nullptr, &db);
  if (ret != 0 || db == nullptr) {
    std::cerr << "kpep_db_create failed: " << ret << " (" << KpepErrorString(ret) << ")\n";
    return 1;
  }

  kpep_config *config = nullptr;
  ret = api.kpep_config_create(db, &config);
  if (ret != 0 || config == nullptr) {
    std::cerr << "kpep_config_create failed: " << ret << " (" << KpepErrorString(ret)
              << ")\n";
    api.kpep_db_free(db);
    return 1;
  }

  ret = api.kpep_config_force_counters(config);
  if (ret != 0) {
    std::cerr << "kpep_config_force_counters failed: " << ret << " ("
              << KpepErrorString(ret) << ")\n";
    api.kpep_config_free(config);
    api.kpep_db_free(db);
    return 1;
  }

  std::vector<kpep_event *> events;
  events.reserve(kRequestedEvents.size());
  for (const char *event_name : kRequestedEvents) {
    kpep_event *event = nullptr;
    ret = api.kpep_db_event(db, event_name, &event);
    if (ret != 0 || event == nullptr) {
      std::cerr << "kpep_db_event failed for " << event_name << ": " << ret << " ("
                << KpepErrorString(ret) << ")\n";
      api.kpep_config_free(config);
      api.kpep_db_free(db);
      return 1;
    }
    events.push_back(event);
  }

  for (kpep_event *event : events) {
    ret = api.kpep_config_add_event(config, &event, 0, nullptr);
    if (ret != 0) {
      std::cerr << "kpep_config_add_event failed: " << ret << " (" << KpepErrorString(ret)
                << ")\n";
      api.kpep_config_free(config);
      api.kpep_db_free(db);
      return 1;
    }
  }

  u32 classes = 0;
  usize reg_count = 0;
  std::array<kpc_config_t, KPC_MAX_COUNTERS> regs{};
  std::array<usize, KPC_MAX_COUNTERS> counter_map{};
  std::array<u64, KPC_MAX_COUNTERS> before{};
  std::array<u64, KPC_MAX_COUNTERS> after{};
  std::array<u64, KPC_MAX_COUNTERS> cpu_before{};
  std::array<u64, KPC_MAX_COUNTERS> cpu_after{};

  ret = api.kpep_config_kpc_classes(config, &classes);
  if (ret != 0) {
    std::cerr << "kpep_config_kpc_classes failed: " << ret << " (" << KpepErrorString(ret)
              << ")\n";
    api.kpep_config_free(config);
    api.kpep_db_free(db);
    return 1;
  }

  ret = api.kpep_config_kpc_count(config, &reg_count);
  if (ret != 0) {
    std::cerr << "kpep_config_kpc_count failed: " << ret << " (" << KpepErrorString(ret)
              << ")\n";
    api.kpep_config_free(config);
    api.kpep_db_free(db);
    return 1;
  }

  ret = api.kpep_config_kpc_map(config, counter_map.data(),
                                counter_map.size() * sizeof(counter_map[0]));
  if (ret != 0) {
    std::cerr << "kpep_config_kpc_map failed: " << ret << " (" << KpepErrorString(ret)
              << ")\n";
    api.kpep_config_free(config);
    api.kpep_db_free(db);
    return 1;
  }

  ret = api.kpep_config_kpc(config, regs.data(), regs.size() * sizeof(regs[0]));
  if (ret != 0) {
    std::cerr << "kpep_config_kpc failed: " << ret << " (" << KpepErrorString(ret)
              << ")\n";
    api.kpep_config_free(config);
    api.kpep_db_free(db);
    return 1;
  }

  std::cout << "requested class mask: " << HexString(classes) << '\n';
  std::cout << "register count: " << reg_count << '\n';
  const u32 fixed_count = api.kpc_get_counter_count(KPC_CLASS_FIXED_MASK);
  const u32 configurable_count = api.kpc_get_counter_count(KPC_CLASS_CONFIGURABLE_MASK);
  const u32 active_count = api.kpc_get_counter_count(classes);
  std::cout << "fixed counter count: " << fixed_count << '\n';
  std::cout << "configurable counter count: " << configurable_count << '\n';
  std::cout << "active counter count: " << active_count << '\n';
  std::cout << "event map:\n";
  for (usize i = 0; i < events.size(); ++i) {
    const char *name = nullptr;
    api.kpep_event_name(events[i], &name);
    std::cout << "  event[" << i << "] " << (name != nullptr ? name : "(null)")
              << " -> slot " << counter_map[i] << '\n';
  }

  CleanupState cleanup{api};

  ret = api.kpc_force_all_ctrs_set(1);
  if (ret != 0) {
    std::cerr << "kpc_force_all_ctrs_set failed: " << ret
              << " (likely run this binary with sudo)\n";
    api.kpep_config_free(config);
    api.kpep_db_free(db);
    return 1;
  }
  cleanup.release_force = true;

  if ((classes & KPC_CLASS_CONFIGURABLE_MASK) != 0 && reg_count != 0) {
    ret = api.kpc_set_config(classes, regs.data());
    if (ret != 0) {
      std::cerr << "kpc_set_config failed: " << ret
                << " (private API reachable, but kernel rejected the config)\n";
      api.kpep_config_free(config);
      api.kpep_db_free(db);
      return 1;
    }
  }

  ret = api.kpc_set_counting(classes);
  if (ret != 0) {
    std::cerr << "kpc_set_counting failed: " << ret
              << " (likely run this binary with sudo)\n";
    api.kpep_config_free(config);
    api.kpep_db_free(db);
    return 1;
  }
  cleanup.restore_counting = true;

  ret = api.kpc_set_thread_counting(classes);
  if (ret != 0) {
    std::cerr << "kpc_set_thread_counting failed: " << ret
              << " (likely run this binary with sudo)\n";
    api.kpep_config_free(config);
    api.kpep_db_free(db);
    return 1;
  }
  cleanup.restore_thread_counting = true;

  ret = api.kpc_get_thread_counters(0, static_cast<u32>(before.size()), before.data());
  if (ret != 0) {
    std::cerr << "kpc_get_thread_counters(before) failed: " << ret << '\n';
    api.kpep_config_free(config);
    api.kpep_db_free(db);
    return 1;
  }

  int curcpu = -1;
  ret = api.kpc_get_cpu_counters(false, classes, &curcpu, cpu_before.data());
  if (ret < 0) {
    std::cerr << "kpc_get_cpu_counters(before) failed: " << ret << '\n';
    api.kpep_config_free(config);
    api.kpep_db_free(db);
    return 1;
  }

  const u64 accumulator = RunWorkload();

  ret = api.kpc_get_thread_counters(0, static_cast<u32>(after.size()), after.data());
  if (ret != 0) {
    std::cerr << "kpc_get_thread_counters(after) failed: " << ret << '\n';
    api.kpep_config_free(config);
    api.kpep_db_free(db);
    return 1;
  }

  ret = api.kpc_get_cpu_counters(false, classes, &curcpu, cpu_after.data());
  if (ret < 0) {
    std::cerr << "kpc_get_cpu_counters(after) failed: " << ret << '\n';
    api.kpep_config_free(config);
    api.kpep_db_free(db);
    return 1;
  }

  std::cout << "loop complete, accumulator=" << accumulator << '\n';
  std::cout << "current cpu: " << curcpu << '\n';
  PrintCounterArray("thread counters before", before, active_count);
  PrintCounterArray("thread counters after", after, active_count);
  PrintCounterArray("cpu counters before", cpu_before, active_count);
  PrintCounterArray("cpu counters after", cpu_after, active_count);
  std::cout << "counter deltas:\n";

  std::optional<u64> cycles;
  std::optional<u64> instructions;

  for (usize i = 0; i < events.size(); ++i) {
    const usize slot = counter_map[i];
    const u64 thread_delta = after[slot] - before[slot];
    const u64 cpu_delta = cpu_after[slot] - cpu_before[slot];

    const char *name = nullptr;
    const char *alias = nullptr;
    api.kpep_event_name(events[i], &name);
    api.kpep_event_alias(events[i], &alias);

    std::cout << "  [" << slot << "] " << (name != nullptr ? name : "(null)");
    if (alias != nullptr && std::strlen(alias) != 0) {
      std::cout << " alias=" << alias;
    }
    std::cout << " thread_delta=" << thread_delta << " cpu_delta=" << cpu_delta << '\n';

    if (name != nullptr && std::strcmp(name, "FIXED_CYCLES") == 0) {
      cycles = thread_delta;
    }
    if (name != nullptr && std::strcmp(name, "FIXED_INSTRUCTIONS") == 0) {
      instructions = thread_delta;
    }
  }

  if (cycles.has_value() && instructions.has_value() && *cycles != 0) {
    const double ipc = static_cast<double>(*instructions) / static_cast<double>(*cycles);
    std::cout << std::fixed << std::setprecision(3) << "ipc=" << ipc << '\n';
  }

  api.kpep_config_free(config);
  api.kpep_db_free(db);
  return 0;
}
