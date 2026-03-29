#ifndef PERF_H_
#define PERF_H_

#include <dlfcn.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <source_location>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef PERF_DISABLE

namespace perf {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using usize = std::size_t;
using kpc_config_t = u64;

constexpr u32 KPC_CLASS_FIXED = 0;
constexpr u32 KPC_CLASS_CONFIGURABLE = 1;
constexpr u32 KPC_CLASS_FIXED_MASK = 1u << KPC_CLASS_FIXED;
constexpr u32 KPC_CLASS_CONFIGURABLE_MASK = 1u << KPC_CLASS_CONFIGURABLE;
constexpr usize KPC_MAX_COUNTERS = 32;
constexpr usize PERF_MAX_SCOPE_EVENTS = 10;
constexpr usize PERF_MAX_THRESHOLDS = 8;

constexpr int KPEP_CONFIG_ERROR_CONFLICTING_EVENTS = 12;

struct kpep_db;
struct kpep_config;
struct kpep_event;

enum class CounterKind : u8 {
  Named,
  RawConfig,
};

struct Counter {
  CounterKind kind = CounterKind::Named;
  const char *name = nullptr;
  u64 raw_config = 0;
  bool fixed = false;

  constexpr Counter() = default;
  constexpr Counter(CounterKind kind_in, const char *name_in, u64 raw_in, bool fixed_in)
      : kind(kind_in), name(name_in), raw_config(raw_in), fixed(fixed_in) {}

  static constexpr Counter Named(const char *name, bool fixed = false) {
    return Counter{CounterKind::Named, name, 0, fixed};
  }

  static constexpr Counter Raw(u64 raw_config, const char *label = nullptr) {
    return Counter{CounterKind::RawConfig, label, raw_config, false};
  }
};

constexpr bool operator==(const Counter &lhs, const Counter &rhs) {
  return lhs.kind == rhs.kind && lhs.name == rhs.name && lhs.raw_config == rhs.raw_config &&
         lhs.fixed == rhs.fixed;
}

struct CounterSet {
  std::array<Counter, PERF_MAX_SCOPE_EVENTS> items{};
  u8 count = 0;
  bool overflow = false;

  constexpr CounterSet() = default;
  constexpr CounterSet(Counter counter) { Add(counter); }

  constexpr bool Contains(Counter counter) const {
    for (u8 i = 0; i < count; ++i) {
      if (items[i] == counter) {
        return true;
      }
    }
    return false;
  }

  constexpr void Add(Counter counter) {
    if (Contains(counter)) {
      return;
    }
    if (count >= items.size()) {
      overflow = true;
      return;
    }
    items[count++] = counter;
  }
};

constexpr CounterSet operator|(Counter lhs, Counter rhs) {
  CounterSet set(lhs);
  set.Add(rhs);
  return set;
}

constexpr CounterSet operator|(CounterSet lhs, Counter rhs) {
  lhs.Add(rhs);
  return lhs;
}

constexpr CounterSet operator|(Counter lhs, CounterSet rhs) {
  rhs.Add(lhs);
  return rhs;
}

constexpr CounterSet operator|(CounterSet lhs, CounterSet rhs) {
  for (u8 i = 0; i < rhs.count; ++i) {
    lhs.Add(rhs.items[i]);
  }
  return lhs;
}

struct Threshold {
  Counter counter{};
  u64 max_value = 0;
};

inline constexpr Counter CYCLES = Counter::Named("FIXED_CYCLES", true);
inline constexpr Counter INSTRUCTIONS = Counter::Named("FIXED_INSTRUCTIONS", true);
inline constexpr Counter BRANCHES = Counter::Named("INST_BRANCH");
inline constexpr Counter BRANCH_MISS = Counter::Named("BRANCH_MISPRED_NONSPEC");
inline constexpr Counter L1_LOAD_MISS = Counter::Named("L1D_CACHE_MISS_LD");
inline constexpr Counter L1_STORE_MISS = Counter::Named("L1D_CACHE_MISS_ST");
inline constexpr Counter L1_MISS = L1_LOAD_MISS;
inline constexpr Counter DTLB_MISS = Counter::Named("L1D_TLB_MISS");
inline constexpr Counter ITLB_MISS = Counter::Named("L1I_TLB_MISS_DEMAND");
inline constexpr Counter TLB_MISS = DTLB_MISS;
inline constexpr Counter L2_TLB_MISS = Counter::Named("L2_TLB_MISS_DATA");
inline constexpr Counter L2_MISS = L2_TLB_MISS;

constexpr Counter RawEvent(u64 raw_config, const char *label = nullptr) {
  return Counter::Raw(raw_config, label);
}

constexpr Threshold MaxThreshold(Counter counter, u64 max_value) {
  return Threshold{counter, max_value};
}

namespace detail {

inline u64 NowTicks() {
  return mach_absolute_time();
}

inline std::string JsonEscape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char c : text) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

inline std::string CounterName(const Counter &counter) {
  if (counter.kind == CounterKind::Named) {
    return counter.name != nullptr ? std::string(counter.name) : std::string("unnamed");
  }
  std::ostringstream oss;
  if (counter.name != nullptr && counter.name[0] != '\0') {
    oss << counter.name << '@';
  }
  oss << "raw_0x" << std::hex << std::nouppercase << counter.raw_config;
  return oss.str();
}

inline std::string CounterId(const Counter &counter) {
  if (counter.kind == CounterKind::Named) {
    return std::string(counter.fixed ? "fixed:" : "named:") +
           (counter.name != nullptr ? counter.name : "");
  }
  std::ostringstream oss;
  oss << "raw:" << std::hex << std::nouppercase << counter.raw_config << ':';
  if (counter.name != nullptr) {
    oss << counter.name;
  }
  return oss.str();
}

inline std::string CanonicalCounterSetKey(const CounterSet &set) {
  std::vector<std::string> parts;
  parts.reserve(set.count);
  for (u8 i = 0; i < set.count; ++i) {
    parts.push_back(CounterId(set.items[i]));
  }
  std::sort(parts.begin(), parts.end());
  std::ostringstream oss;
  for (usize i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      oss << '|';
    }
    oss << parts[i];
  }
  return oss.str();
}

inline std::string ThresholdKey(std::initializer_list<Threshold> thresholds) {
  std::vector<std::string> parts;
  parts.reserve(thresholds.size());
  for (const Threshold &threshold : thresholds) {
    std::ostringstream oss;
    oss << CounterId(threshold.counter) << "<=" << threshold.max_value;
    parts.push_back(oss.str());
  }
  std::sort(parts.begin(), parts.end());
  std::ostringstream joined;
  for (usize i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      joined << '|';
    }
    joined << parts[i];
  }
  return joined.str();
}

inline std::string AggregateKey(std::string_view label, const CounterSet &set,
                                std::initializer_list<Threshold> thresholds) {
  std::ostringstream oss;
  oss << label << "::" << CanonicalCounterSetKey(set) << "::" << ThresholdKey(thresholds);
  return oss.str();
}

inline std::string AggregateKey(std::string_view label, const CounterSet &set,
                                const std::vector<Threshold> &thresholds) {
  std::vector<std::string> parts;
  parts.reserve(thresholds.size());
  for (const Threshold &threshold : thresholds) {
    std::ostringstream oss;
    oss << CounterId(threshold.counter) << "<=" << threshold.max_value;
    parts.push_back(oss.str());
  }
  std::sort(parts.begin(), parts.end());
  std::ostringstream joined;
  for (usize i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      joined << '|';
    }
    joined << parts[i];
  }

  std::ostringstream oss;
  oss << label << "::" << CanonicalCounterSetKey(set) << "::" << joined.str();
  return oss.str();
}

inline bool IsSubset(const CounterSet &subset, const CounterSet &superset) {
  for (u8 i = 0; i < subset.count; ++i) {
    if (!superset.Contains(subset.items[i])) {
      return false;
    }
  }
  return true;
}

inline u64 SourceLocationHash(const std::source_location &where) {
  constexpr u64 kOffset = 1469598103934665603ULL;
  constexpr u64 kPrime = 1099511628211ULL;
  u64 hash = kOffset;
  auto mix_byte = [&](u8 byte) {
    hash ^= byte;
    hash *= kPrime;
  };
  const char *file = where.file_name();
  while (file != nullptr && *file != '\0') {
    mix_byte(static_cast<u8>(*file++));
  }
  const char *function = where.function_name();
  while (function != nullptr && *function != '\0') {
    mix_byte(static_cast<u8>(*function++));
  }
  for (u32 value : {where.line(), where.column()}) {
    for (u8 shift = 0; shift != 32; shift += 8) {
      mix_byte(static_cast<u8>((value >> shift) & 0xffu));
    }
  }
  return hash;
}

struct Api {
  void *kperf_handle = nullptr;
  void *kperfdata_handle = nullptr;

  int (*kpc_set_counting)(u32 classes) = nullptr;
  int (*kpc_set_thread_counting)(u32 classes) = nullptr;
  int (*kpc_set_config)(u32 classes, kpc_config_t *config) = nullptr;
  u32 (*kpc_get_counter_count)(u32 classes) = nullptr;
  int (*kpc_get_thread_counters)(u32 tid, u32 buf_count, u64 *buf) = nullptr;
  int (*kpc_force_all_ctrs_set)(int val) = nullptr;

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
  int (*kpep_db_event)(kpep_db *db, const char *name, kpep_event **ev_ptr) = nullptr;

  ~Api() {
    if (kperf_handle != nullptr) {
      dlclose(kperf_handle);
    }
    if (kperfdata_handle != nullptr) {
      dlclose(kperfdata_handle);
    }
  }
};

template <typename Fn>
inline bool LoadSymbol(void *handle, const char *name, Fn &target, std::string &error) {
  dlerror();
  void *symbol = dlsym(handle, name);
  if (const char *dl_error = dlerror(); dl_error != nullptr) {
    error = dl_error;
    return false;
  }
  target = reinterpret_cast<Fn>(symbol);
  return true;
}

inline void *OpenLibrary(std::initializer_list<const char *> paths, std::string &error) {
  for (const char *path : paths) {
    dlerror();
    void *handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (handle != nullptr) {
      return handle;
    }
    if (const char *dl_error = dlerror(); dl_error != nullptr) {
      error = dl_error;
    }
  }
  return nullptr;
}

inline bool LoadApi(Api &api, std::string &error) {
  api.kperf_handle = OpenLibrary(
      {"/System/Library/PrivateFrameworks/kperf.framework/Versions/A/kperf",
       "/System/Library/PrivateFrameworks/kperf.framework/kperf"},
      error);
  if (api.kperf_handle == nullptr) {
    error = "failed to load kperf.framework: " + error;
    return false;
  }

  api.kperfdata_handle = OpenLibrary(
      {"/System/Library/PrivateFrameworks/kperfdata.framework/Versions/A/kperfdata",
       "/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata"},
      error);
  if (api.kperfdata_handle == nullptr) {
    error = "failed to load kperfdata.framework: " + error;
    return false;
  }

  return LoadSymbol(api.kperf_handle, "kpc_set_counting", api.kpc_set_counting, error) &&
         LoadSymbol(api.kperf_handle, "kpc_set_thread_counting", api.kpc_set_thread_counting,
                    error) &&
         LoadSymbol(api.kperf_handle, "kpc_set_config", api.kpc_set_config, error) &&
         LoadSymbol(api.kperf_handle, "kpc_get_counter_count", api.kpc_get_counter_count,
                    error) &&
         LoadSymbol(api.kperf_handle, "kpc_get_thread_counters", api.kpc_get_thread_counters,
                    error) &&
         LoadSymbol(api.kperf_handle, "kpc_force_all_ctrs_set", api.kpc_force_all_ctrs_set,
                    error) &&
         LoadSymbol(api.kperfdata_handle, "kpep_config_create", api.kpep_config_create,
                    error) &&
         LoadSymbol(api.kperfdata_handle, "kpep_config_free", api.kpep_config_free, error) &&
         LoadSymbol(api.kperfdata_handle, "kpep_config_add_event", api.kpep_config_add_event,
                    error) &&
         LoadSymbol(api.kperfdata_handle, "kpep_config_force_counters",
                    api.kpep_config_force_counters, error) &&
         LoadSymbol(api.kperfdata_handle, "kpep_config_kpc", api.kpep_config_kpc, error) &&
         LoadSymbol(api.kperfdata_handle, "kpep_config_kpc_count", api.kpep_config_kpc_count,
                    error) &&
         LoadSymbol(api.kperfdata_handle, "kpep_config_kpc_classes",
                    api.kpep_config_kpc_classes, error) &&
         LoadSymbol(api.kperfdata_handle, "kpep_config_kpc_map", api.kpep_config_kpc_map,
                    error) &&
         LoadSymbol(api.kperfdata_handle, "kpep_db_create", api.kpep_db_create, error) &&
         LoadSymbol(api.kperfdata_handle, "kpep_db_free", api.kpep_db_free, error) &&
         LoadSymbol(api.kperfdata_handle, "kpep_db_event", api.kpep_db_event, error);
}

struct Program {
  CounterSet set;
  std::string key;
  u32 classes = 0;
  u32 fixed_count = 0;
  u32 active_count = 0;
  std::array<kpc_config_t, KPC_MAX_COUNTERS> regs{};
  std::array<int, PERF_MAX_SCOPE_EVENTS> counter_slot_for_requested{};
  std::array<std::string, PERF_MAX_SCOPE_EVENTS> counter_name_for_requested{};
  bool valid = false;
  std::string error;

  Program() { counter_slot_for_requested.fill(-1); }
};

struct Aggregate {
  std::string label;
  CounterSet set;
  std::vector<std::string> counter_names;
  std::vector<Threshold> thresholds;
  u32 sample_every = 1;
  u64 sampled_count = 0;
  u64 dropped_count = 0;
  u64 total_wall_ticks = 0;
  u64 min_wall_ticks = std::numeric_limits<u64>::max();
  u64 max_wall_ticks = 0;
  std::array<u64, PERF_MAX_SCOPE_EVENTS> total_counters{};
  std::array<u64, PERF_MAX_SCOPE_EVENTS> min_counters{};
  std::array<u64, PERF_MAX_SCOPE_EVENTS> max_counters{};
  u64 threshold_violations = 0;
  std::string last_error;

  Aggregate() {
    min_counters.fill(std::numeric_limits<u64>::max());
    max_counters.fill(0);
    total_counters.fill(0);
  }
};

struct ScopeFrame {
  const char *label = "";
  CounterSet requested{};
  std::vector<Threshold> thresholds;
  std::array<u64, PERF_MAX_SCOPE_EVENTS> start_values{};
  u64 start_ticks = 0;
  u32 sample_every = 1;
  bool active = false;
  bool dropped = false;
  std::string error;
  Aggregate *aggregate = nullptr;
};

struct ThreadState {
  std::vector<ScopeFrame *> active_frames;
  Program *installed_program = nullptr;
  CounterSet installed_set{};
  std::unordered_map<u64, u64> sample_counters;
  std::unordered_map<std::string, Aggregate> aggregates;
};

class Backend {
 public:
  struct PointSnapshot {
    CounterSet set{};
    std::array<u64, PERF_MAX_SCOPE_EVENTS> values{};
    u8 count = 0;
    bool valid = false;
    std::string error;
  };

  static Backend &Instance() {
    static Backend backend;
    return backend;
  }

  Backend(const Backend &) = delete;
  Backend &operator=(const Backend &) = delete;

  bool ShouldSample(u32 sample_every, const std::source_location &where) {
    if (sample_every <= 1) {
      return true;
    }
    ThreadState &state = CurrentThreadState();
    const u64 site_hash = SourceLocationHash(where);
    u64 &counter = state.sample_counters[site_hash];
    ++counter;
    return (counter % sample_every) == 0;
  }

  bool PrimeThread(CounterSet requested, std::string &error) {
    if (requested.overflow) {
      error = "requested counter set exceeds PERF_MAX_SCOPE_EVENTS";
      return false;
    }
    if (requested.count == 0) {
      return true;
    }
    if (!EnsureInitialized(error)) {
      return false;
    }

    ThreadState &state = CurrentThreadState();
    if (state.installed_program != nullptr) {
      if (IsSubset(requested, state.installed_set)) {
        return true;
      }
      if (!state.active_frames.empty()) {
        error =
            "cannot widen the installed thread counter set while scopes are active on this thread";
        return false;
      }
      requested = state.installed_set | requested;
      if (requested.overflow) {
        error = "too many simultaneous configurable counters in installed thread set";
        return false;
      }
    }
    return InstallProgram(state, requested, error);
  }

  void Enter(ScopeFrame &frame) {
    frame.start_ticks = NowTicks();
    ThreadState &state = CurrentThreadState();
    frame.aggregate = &GetOrCreateAggregate(state, frame.label, frame.requested, frame.thresholds,
                                            frame.sample_every);

    if (frame.requested.overflow) {
      frame.error = "requested counter set exceeds PERF_MAX_SCOPE_EVENTS";
      frame.dropped = true;
      return;
    }

    if (frame.requested.count == 0) {
      frame.active = true;
      state.active_frames.push_back(&frame);
      return;
    }

    if (!EnsureInitialized(frame.error)) {
      frame.dropped = true;
      return;
    }
    if (!EnsureThreadProgram(state, frame.requested, frame.error)) {
      frame.dropped = true;
      return;
    }

    std::array<u64, KPC_MAX_COUNTERS> current{};
    if (!ReadThreadCounters(*state.installed_program, current, frame.error)) {
      frame.dropped = true;
      return;
    }
    for (u8 i = 0; i < frame.requested.count; ++i) {
      const int slot = LookupCounterSlot(*state.installed_program, frame.requested.items[i]);
      if (slot >= 0) {
        frame.start_values[i] = current[static_cast<usize>(slot)];
      }
    }

    frame.active = true;
    state.active_frames.push_back(&frame);
  }

  void Exit(ScopeFrame &frame) {
    const u64 end_ticks = NowTicks();
    if (frame.aggregate == nullptr) {
      return;
    }
    if (frame.dropped) {
      RecordDropped(*frame.aggregate, frame);
      return;
    }
    if (!frame.active) {
      return;
    }

    ThreadState &state = CurrentThreadState();
    if (!state.active_frames.empty() && state.active_frames.back() == &frame) {
      state.active_frames.pop_back();
    } else {
      auto it = std::find(state.active_frames.begin(), state.active_frames.end(), &frame);
      if (it != state.active_frames.end()) {
        state.active_frames.erase(it);
      }
    }

    std::array<u64, PERF_MAX_SCOPE_EVENTS> deltas{};
    deltas.fill(0);
    if (frame.requested.count != 0) {
      if (state.installed_program == nullptr) {
        frame.error = "no installed program on scope exit";
        RecordDropped(*frame.aggregate, frame);
        return;
      }
      std::array<u64, KPC_MAX_COUNTERS> current{};
      if (!ReadThreadCounters(*state.installed_program, current, frame.error)) {
        RecordDropped(*frame.aggregate, frame);
        return;
      }
      for (u8 i = 0; i < frame.requested.count; ++i) {
        const int slot = LookupCounterSlot(*state.installed_program, frame.requested.items[i]);
        if (slot >= 0) {
          deltas[i] = current[static_cast<usize>(slot)] - frame.start_values[i];
        }
      }
    }

    RecordComplete(*frame.aggregate, frame, end_ticks - frame.start_ticks, deltas);
  }

  PointSnapshot CapturePoint(const CounterSet &requested) {
    PointSnapshot snapshot;
    snapshot.set = requested;
    snapshot.count = requested.count;
    if (requested.overflow) {
      snapshot.error = "requested counter set exceeds PERF_MAX_SCOPE_EVENTS";
      return snapshot;
    }
    if (requested.count == 0) {
      snapshot.valid = true;
      return snapshot;
    }
    if (!EnsureInitialized(snapshot.error)) {
      return snapshot;
    }

    ThreadState &state = CurrentThreadState();
    if (!EnsureThreadProgram(state, requested, snapshot.error)) {
      return snapshot;
    }

    std::array<u64, KPC_MAX_COUNTERS> current{};
    if (!ReadThreadCounters(*state.installed_program, current, snapshot.error)) {
      return snapshot;
    }
    for (u8 i = 0; i < requested.count; ++i) {
      const int slot = LookupCounterSlot(*state.installed_program, requested.items[i]);
      if (slot >= 0) {
        snapshot.values[i] = current[static_cast<usize>(slot)];
      }
    }
    snapshot.valid = true;
    return snapshot;
  }

  ~Backend() {
    DumpJsonl();
    if (initialized_) {
      api_.kpc_set_thread_counting(0);
      api_.kpc_set_counting(0);
      api_.kpc_force_all_ctrs_set(0);
      if (db_ != nullptr) {
        api_.kpep_db_free(db_);
      }
    }
  }

 private:
  Api api_{};
  kpep_db *db_ = nullptr;
  bool initialized_ = false;
  bool attempted_ = false;
  std::string init_error_;
  std::mutex mutex_;
  std::unordered_map<std::string, Program> programs_;
  std::vector<ThreadState *> thread_states_;

  Backend() = default;

  static ThreadState &CurrentThreadState() {
    thread_local ThreadState *state = Instance().AllocateThreadState();
    return *state;
  }

  ThreadState *AllocateThreadState() {
    std::scoped_lock lock(mutex_);
    ThreadState *state = new ThreadState();
    thread_states_.push_back(state);
    return state;
  }

  bool EnsureInitialized(std::string &error) {
    std::scoped_lock lock(mutex_);
    if (initialized_) {
      return true;
    }
    if (attempted_) {
      error = init_error_;
      return false;
    }
    attempted_ = true;
    if (!LoadApi(api_, init_error_)) {
      error = init_error_;
      return false;
    }
    if (api_.kpep_db_create(nullptr, &db_) != 0 || db_ == nullptr) {
      init_error_ = "failed to open local kpep database";
      error = init_error_;
      return false;
    }
    if (api_.kpc_force_all_ctrs_set(1) != 0) {
      init_error_ = "kpc_force_all_ctrs_set(1) failed; run as root or blessed pid";
      error = init_error_;
      return false;
    }
    initialized_ = true;
    return true;
  }

  static int LookupCounterSlot(const Program &program, Counter counter) {
    for (u8 i = 0; i < program.set.count; ++i) {
      if (program.set.items[i] == counter) {
        return program.counter_slot_for_requested[i];
      }
    }
    return -1;
  }

  bool ReadThreadCounters(const Program &program, std::array<u64, KPC_MAX_COUNTERS> &out,
                          std::string &error) {
    out.fill(0);
    const int ret =
        api_.kpc_get_thread_counters(0, static_cast<u32>(program.active_count), out.data());
    if (ret != 0) {
      error = "kpc_get_thread_counters failed: " + std::to_string(ret);
      return false;
    }
    return true;
  }

  bool BuildProgram(const CounterSet &set, Program &program, std::string &error) {
    program = Program{};
    program.set = set;
    program.key = CanonicalCounterSetKey(set);
    program.fixed_count = api_.kpc_get_counter_count(KPC_CLASS_FIXED_MASK);
    program.classes = 0;
    bool need_config = false;
    for (u8 i = 0; i < set.count; ++i) {
      if (set.items[i].kind == CounterKind::Named && set.items[i].fixed) {
        program.classes |= KPC_CLASS_FIXED_MASK;
      } else {
        need_config = true;
      }
    }
    if (need_config) {
      program.classes |= KPC_CLASS_FIXED_MASK | KPC_CLASS_CONFIGURABLE_MASK;
    }
    if (program.classes == 0) {
      program.classes = KPC_CLASS_FIXED_MASK;
    }
    program.active_count = api_.kpc_get_counter_count(program.classes);
    if (program.active_count == 0 || program.active_count > KPC_MAX_COUNTERS) {
      error = "unexpected active counter count";
      return false;
    }

    std::array<bool, KPC_MAX_COUNTERS> used_slots{};
    used_slots.fill(false);
    program.regs.fill(0);

    std::vector<Counter> named_events;
    std::vector<u8> named_indices;
    for (u8 i = 0; i < set.count; ++i) {
      if (set.items[i].kind == CounterKind::Named) {
        named_events.push_back(set.items[i]);
        named_indices.push_back(i);
      }
    }

    if (!named_events.empty()) {
      kpep_config *config = nullptr;
      const int create_ret = api_.kpep_config_create(db_, &config);
      if (create_ret != 0 || config == nullptr) {
        error = "kpep_config_create failed: " + std::to_string(create_ret);
        return false;
      }
      struct Cleanup {
        Api &api;
        kpep_config *config = nullptr;
        ~Cleanup() {
          if (config != nullptr) {
            api.kpep_config_free(config);
          }
        }
      } cleanup{api_, config};

      const int force_ret = api_.kpep_config_force_counters(config);
      if (force_ret != 0) {
        error = "kpep_config_force_counters failed: " + std::to_string(force_ret);
        return false;
      }

      for (Counter counter : named_events) {
        kpep_event *event = nullptr;
        const int db_ret = api_.kpep_db_event(db_, counter.name, &event);
        if (db_ret != 0 || event == nullptr) {
          error = std::string("unknown event: ") + (counter.name != nullptr ? counter.name : "");
          return false;
        }
        const int add_ret = api_.kpep_config_add_event(config, &event, 0, nullptr);
        if (add_ret != 0) {
          if (add_ret == KPEP_CONFIG_ERROR_CONFLICTING_EVENTS) {
            error = std::string("conflicting events while building set: ") +
                    CanonicalCounterSetKey(set);
          } else {
            error = "kpep_config_add_event failed: " + std::to_string(add_ret);
          }
          return false;
        }
      }

      std::vector<usize> map(named_events.size());
      const int map_ret =
          api_.kpep_config_kpc_map(config, map.data(), map.size() * sizeof(map[0]));
      if (map_ret != 0) {
        error = "kpep_config_kpc_map failed: " + std::to_string(map_ret);
        return false;
      }
      const int kpc_ret =
          api_.kpep_config_kpc(config, program.regs.data(), program.regs.size() * sizeof(u64));
      if (kpc_ret != 0) {
        error = "kpep_config_kpc failed: " + std::to_string(kpc_ret);
        return false;
      }

      for (usize i = 0; i < named_events.size(); ++i) {
        const usize slot = map[i];
        if (slot >= KPC_MAX_COUNTERS) {
          error = "event mapped beyond KPC_MAX_COUNTERS";
          return false;
        }
        used_slots[slot] = true;
        program.counter_slot_for_requested[named_indices[i]] = static_cast<int>(slot);
        program.counter_name_for_requested[named_indices[i]] = CounterName(named_events[i]);
      }
    }

    int next_raw_slot = static_cast<int>(program.fixed_count);
    if ((program.classes & KPC_CLASS_CONFIGURABLE_MASK) == 0) {
      next_raw_slot = static_cast<int>(program.active_count);
    }
    for (u8 i = 0; i < set.count; ++i) {
      const Counter counter = set.items[i];
      if (counter.kind == CounterKind::Named) {
        continue;
      }
      while (next_raw_slot < static_cast<int>(program.active_count) &&
             used_slots[static_cast<usize>(next_raw_slot)]) {
        ++next_raw_slot;
      }
      if (next_raw_slot >= static_cast<int>(program.active_count)) {
        error = "too many simultaneous configurable counters in installed thread set";
        return false;
      }
      program.regs[static_cast<usize>(next_raw_slot)] = counter.raw_config;
      used_slots[static_cast<usize>(next_raw_slot)] = true;
      program.counter_slot_for_requested[i] = next_raw_slot;
      program.counter_name_for_requested[i] = CounterName(counter);
      ++next_raw_slot;
    }

    program.valid = true;
    return true;
  }

  Program *GetOrBuildProgram(const CounterSet &set, std::string &error) {
    std::scoped_lock lock(mutex_);
    const std::string key = CanonicalCounterSetKey(set);
    auto it = programs_.find(key);
    if (it != programs_.end()) {
      error = it->second.error;
      return it->second.valid ? &it->second : nullptr;
    }

    Program program;
    if (!BuildProgram(set, program, error)) {
      program.error = error;
      program.valid = false;
    }
    auto [inserted, _] = programs_.emplace(key, std::move(program));
    return inserted->second.valid ? &inserted->second : nullptr;
  }

  bool InstallProgram(ThreadState &state, const CounterSet &set, std::string &error) {
    if (set.count == 0) {
      state.installed_program = nullptr;
      state.installed_set = {};
      return true;
    }
    Program *program = GetOrBuildProgram(set, error);
    if (program == nullptr) {
      return false;
    }
    if ((program->classes & KPC_CLASS_CONFIGURABLE_MASK) != 0) {
      const int config_ret = api_.kpc_set_config(program->classes, program->regs.data());
      if (config_ret != 0) {
        error = "kpc_set_config failed: " + std::to_string(config_ret);
        return false;
      }
    }
    if (api_.kpc_set_counting(program->classes) != 0 ||
        api_.kpc_set_thread_counting(program->classes) != 0) {
      error = "failed to enable counting";
      return false;
    }
    state.installed_program = program;
    state.installed_set = set;
    return true;
  }

  bool EnsureThreadProgram(ThreadState &state, const CounterSet &requested, std::string &error) {
    if (requested.count == 0) {
      return true;
    }
    if (state.installed_program == nullptr) {
      return InstallProgram(state, requested, error);
    }
    if (IsSubset(requested, state.installed_set)) {
      return true;
    }
    if (!state.active_frames.empty()) {
      error =
          "requested counters are not a subset of this thread's installed counter set; prime a superset before nested use";
      return false;
    }
    CounterSet widened = state.installed_set | requested;
    if (widened.overflow) {
      error = "too many simultaneous configurable counters in installed thread set";
      return false;
    }
    return InstallProgram(state, widened, error);
  }

  Aggregate &GetOrCreateAggregate(ThreadState &state, std::string_view label,
                                  const CounterSet &set, const std::vector<Threshold> &thresholds,
                                  u32 sample_every) {
    std::ostringstream key_builder;
    key_builder << label << "::" << CanonicalCounterSetKey(set) << "::" << sample_every << "::";
    for (const Threshold &threshold : thresholds) {
      key_builder << CounterId(threshold.counter) << "<=" << threshold.max_value << '|';
    }
    const std::string key = key_builder.str();

    auto [it, inserted] = state.aggregates.try_emplace(key);
    Aggregate &aggregate = it->second;
    if (inserted) {
      aggregate.label = std::string(label);
      aggregate.set = set;
      aggregate.sample_every = sample_every;
      aggregate.thresholds = thresholds;
      for (u8 i = 0; i < set.count; ++i) {
        aggregate.counter_names.push_back(CounterName(set.items[i]));
      }
    }
    return aggregate;
  }

  static void RecordDropped(Aggregate &aggregate, const ScopeFrame &frame) {
    ++aggregate.dropped_count;
    aggregate.last_error = frame.error;
  }

  static void RecordComplete(Aggregate &aggregate, const ScopeFrame &frame, u64 wall_ticks,
                             const std::array<u64, PERF_MAX_SCOPE_EVENTS> &deltas) {
    ++aggregate.sampled_count;
    aggregate.total_wall_ticks += wall_ticks;
    aggregate.min_wall_ticks = std::min(aggregate.min_wall_ticks, wall_ticks);
    aggregate.max_wall_ticks = std::max(aggregate.max_wall_ticks, wall_ticks);

    for (u8 i = 0; i < frame.requested.count; ++i) {
      const u64 value = deltas[i];
      aggregate.total_counters[i] += value;
      aggregate.min_counters[i] = std::min(aggregate.min_counters[i], value);
      aggregate.max_counters[i] = std::max(aggregate.max_counters[i], value);
    }

    for (const Threshold &threshold : frame.thresholds) {
      for (u8 i = 0; i < frame.requested.count; ++i) {
        if (frame.requested.items[i] == threshold.counter && deltas[i] > threshold.max_value) {
          ++aggregate.threshold_violations;
        }
      }
    }
  }

  static void MergeAggregate(Aggregate &dst, const Aggregate &src) {
    if (dst.label.empty()) {
      dst = src;
      return;
    }
    dst.sampled_count += src.sampled_count;
    dst.dropped_count += src.dropped_count;
    dst.total_wall_ticks += src.total_wall_ticks;
    if (src.sampled_count != 0) {
      dst.min_wall_ticks = std::min(dst.min_wall_ticks, src.min_wall_ticks);
      dst.max_wall_ticks = std::max(dst.max_wall_ticks, src.max_wall_ticks);
    }
    for (u8 i = 0; i < dst.set.count; ++i) {
      dst.total_counters[i] += src.total_counters[i];
      if (src.sampled_count != 0) {
        dst.min_counters[i] = std::min(dst.min_counters[i], src.min_counters[i]);
        dst.max_counters[i] = std::max(dst.max_counters[i], src.max_counters[i]);
      }
    }
    dst.threshold_violations += src.threshold_violations;
    if (!src.last_error.empty()) {
      dst.last_error = src.last_error;
    }
  }

  static u64 TicksToNs(u64 ticks) {
    static const mach_timebase_info_data_t timebase = [] {
      mach_timebase_info_data_t info{};
      mach_timebase_info(&info);
      return info;
    }();
    return static_cast<u64>((static_cast<__uint128_t>(ticks) * timebase.numer) / timebase.denom);
  }

  void DumpJsonl() {
    std::unordered_map<std::string, Aggregate> snapshot;
    {
      std::scoped_lock lock(mutex_);
      for (ThreadState *state : thread_states_) {
        for (const auto &entry : state->aggregates) {
          Aggregate &merged = snapshot[entry.first];
          MergeAggregate(merged, entry.second);
        }
      }
    }
    if (snapshot.empty()) {
      return;
    }

    const char *path = std::getenv("PERF_OUTPUT");
    std::ostream *out = nullptr;
    std::ofstream file;
    if (path == nullptr || path[0] == '\0') {
      file.open("perf.jsonl", std::ios::out | std::ios::trunc);
      out = &file;
    } else if (std::string_view(path) == "-") {
      out = &std::cout;
    } else {
      file.open(path, std::ios::out | std::ios::trunc);
      out = &file;
    }
    if (out == nullptr || !(*out)) {
      return;
    }

    std::vector<std::string> keys;
    keys.reserve(snapshot.size());
    for (const auto &entry : snapshot) {
      keys.push_back(entry.first);
    }
    std::sort(keys.begin(), keys.end());

    for (const std::string &key : keys) {
      const Aggregate &aggregate = snapshot.at(key);
      (*out) << '{';
      (*out) << "\"label\":\"" << JsonEscape(aggregate.label) << "\"";
      (*out) << ",\"sample_every\":" << aggregate.sample_every;
      (*out) << ",\"sampled_count\":" << aggregate.sampled_count;
      (*out) << ",\"dropped_count\":" << aggregate.dropped_count;
      if (aggregate.sampled_count != 0) {
        const u64 total_ns = TicksToNs(aggregate.total_wall_ticks);
        const u64 min_ns = TicksToNs(aggregate.min_wall_ticks);
        const u64 max_ns = TicksToNs(aggregate.max_wall_ticks);
        (*out) << ",\"wall_ns\":{\"total\":" << total_ns << ",\"min\":" << min_ns
               << ",\"max\":" << max_ns << ",\"mean\":"
               << static_cast<double>(total_ns) / static_cast<double>(aggregate.sampled_count)
               << '}';
      }

      (*out) << ",\"counters\":[";
      for (u8 i = 0; i < aggregate.set.count; ++i) {
        if (i != 0) {
          (*out) << ',';
        }
        const u64 min_value = aggregate.sampled_count != 0 ? aggregate.min_counters[i] : 0;
        (*out) << '{';
        (*out) << "\"name\":\"" << JsonEscape(aggregate.counter_names[i]) << "\"";
        (*out) << ",\"total\":" << aggregate.total_counters[i];
        (*out) << ",\"min\":" << min_value;
        (*out) << ",\"max\":" << aggregate.max_counters[i];
        (*out) << ",\"mean\":"
               << (aggregate.sampled_count != 0
                       ? static_cast<double>(aggregate.total_counters[i]) /
                             static_cast<double>(aggregate.sampled_count)
                       : 0.0);
        (*out) << '}';
      }
      (*out) << ']';
      (*out) << ",\"threshold_violations\":" << aggregate.threshold_violations;
      if (!aggregate.last_error.empty()) {
        (*out) << ",\"last_error\":\"" << JsonEscape(aggregate.last_error) << '"';
      }
      (*out) << "}\n";
    }
  }
};

}  // namespace detail

class PerfScope {
 public:
  explicit PerfScope(const char *label, CounterSet counters = CounterSet{}, u32 sample_every = 1,
                     std::initializer_list<Threshold> thresholds = {},
                     std::source_location where = std::source_location::current())
      : label_(label),
        counters_(counters),
        sample_every_(sample_every == 0 ? 1 : sample_every) {
    if (label_ == nullptr || label_[0] == '\0') {
      return;
    }
    if (!detail::Backend::Instance().ShouldSample(sample_every_, where)) {
      return;
    }
    frame_.label = label_;
    frame_.requested = counters_;
    frame_.sample_every = sample_every_;
    frame_.thresholds.assign(thresholds.begin(), thresholds.end());
    detail::Backend::Instance().Enter(frame_);
    active_ = frame_.active || frame_.dropped;
  }

  ~PerfScope() {
    if (!active_) {
      return;
    }
    detail::Backend::Instance().Exit(frame_);
  }

  PerfScope(const PerfScope &) = delete;
  PerfScope &operator=(const PerfScope &) = delete;

 private:
  const char *label_ = nullptr;
  CounterSet counters_{};
  u32 sample_every_ = 1;
  detail::ScopeFrame frame_{};
  bool active_ = false;
};

struct PerfPointDelta {
  CounterSet set{};
  std::array<u64, PERF_MAX_SCOPE_EVENTS> values{};
  u8 count = 0;
  bool valid = false;
  std::string error;

  [[nodiscard]] std::string ToJson() const {
    std::ostringstream oss;
    oss << '{';
    oss << "\"valid\":" << (valid ? "true" : "false");
    if (!error.empty()) {
      oss << ",\"error\":\"" << detail::JsonEscape(error) << '"';
    }
    oss << ",\"counters\":[";
    for (u8 i = 0; i < count; ++i) {
      if (i != 0) {
        oss << ',';
      }
      oss << '{';
      oss << "\"name\":\"" << detail::JsonEscape(detail::CounterName(set.items[i])) << '"';
      oss << ",\"delta\":" << values[i];
      oss << '}';
    }
    oss << "]}";
    return oss.str();
  }
};

class PerfPoint {
 public:
  explicit PerfPoint(CounterSet counters = CounterSet{}) : set_(counters) {
    snapshot_ = detail::Backend::Instance().CapturePoint(counters);
  }

  [[nodiscard]] bool valid() const { return snapshot_.valid; }
  [[nodiscard]] const std::string &error() const { return snapshot_.error; }

  friend PerfPointDelta operator-(const PerfPoint &end, const PerfPoint &begin) {
    PerfPointDelta delta;
    delta.set = end.set_;
    delta.count = end.snapshot_.count;
    if (!end.snapshot_.valid || !begin.snapshot_.valid) {
      delta.error = !end.snapshot_.valid ? end.snapshot_.error : begin.snapshot_.error;
      return delta;
    }
    if (detail::CanonicalCounterSetKey(end.set_) != detail::CanonicalCounterSetKey(begin.set_)) {
      delta.error = "PerfPoint delta requires matching counter sets";
      return delta;
    }
    delta.valid = true;
    for (u8 i = 0; i < delta.count; ++i) {
      delta.values[i] = end.snapshot_.values[i] - begin.snapshot_.values[i];
    }
    return delta;
  }

 private:
  CounterSet set_{};
  detail::Backend::PointSnapshot snapshot_{};
};

inline bool PrimeThread(CounterSet counters, std::string *error = nullptr) {
  std::string local_error;
  const bool ok = detail::Backend::Instance().PrimeThread(counters, local_error);
  if (error != nullptr) {
    *error = local_error;
  }
  return ok;
}

}  // namespace perf

using PerfScope = perf::PerfScope;
using PerfPoint = perf::PerfPoint;
using PerfPointDelta = perf::PerfPointDelta;
using PerfCounter = perf::Counter;
using PerfCounterSet = perf::CounterSet;
using PerfThreshold = perf::Threshold;

inline constexpr auto CYCLES = perf::CYCLES;
inline constexpr auto INSTRUCTIONS = perf::INSTRUCTIONS;
inline constexpr auto BRANCHES = perf::BRANCHES;
inline constexpr auto BRANCH_MISS = perf::BRANCH_MISS;
inline constexpr auto L1_LOAD_MISS = perf::L1_LOAD_MISS;
inline constexpr auto L1_STORE_MISS = perf::L1_STORE_MISS;
inline constexpr auto L1_MISS = perf::L1_MISS;
inline constexpr auto DTLB_MISS = perf::DTLB_MISS;
inline constexpr auto ITLB_MISS = perf::ITLB_MISS;
inline constexpr auto TLB_MISS = perf::TLB_MISS;
inline constexpr auto L2_TLB_MISS = perf::L2_TLB_MISS;
inline constexpr auto L2_MISS = perf::L2_MISS;
inline constexpr auto RawEvent = perf::RawEvent;
inline constexpr auto MaxThreshold = perf::MaxThreshold;
inline bool PerfPrimeThread(PerfCounterSet counters, std::string *error = nullptr) {
  return perf::PrimeThread(counters, error);
}

#else

#include <cstdint>
#include <initializer_list>
#include <source_location>
#include <string>

namespace perf {

struct Counter {
  constexpr Counter() = default;
};

struct CounterSet {
  constexpr CounterSet() = default;
  constexpr CounterSet(Counter) {}
};

struct Threshold {
  Counter counter{};
  std::uint64_t max_value = 0;
};

constexpr CounterSet operator|(CounterSet lhs, CounterSet) { return lhs; }
constexpr CounterSet operator|(CounterSet lhs, Counter) { return lhs; }
constexpr CounterSet operator|(Counter, CounterSet rhs) { return rhs; }
constexpr CounterSet operator|(Counter, Counter) { return CounterSet{}; }

inline constexpr Counter CYCLES{};
inline constexpr Counter INSTRUCTIONS{};
inline constexpr Counter BRANCHES{};
inline constexpr Counter BRANCH_MISS{};
inline constexpr Counter L1_LOAD_MISS{};
inline constexpr Counter L1_STORE_MISS{};
inline constexpr Counter L1_MISS{};
inline constexpr Counter DTLB_MISS{};
inline constexpr Counter ITLB_MISS{};
inline constexpr Counter TLB_MISS{};
inline constexpr Counter L2_TLB_MISS{};
inline constexpr Counter L2_MISS{};

constexpr Counter RawEvent(std::uint64_t, const char * = nullptr) { return Counter{}; }
constexpr Threshold MaxThreshold(Counter counter, std::uint64_t max_value) {
  return Threshold{counter, max_value};
}

class PerfScope {
 public:
  explicit PerfScope(const char *, CounterSet = CounterSet{}, std::uint32_t = 1,
                     std::initializer_list<Threshold> = {},
                     std::source_location = std::source_location::current()) {}
};

struct PerfPointDelta {
  [[nodiscard]] std::string ToJson() const { return "{\"valid\":false}"; }
};

class PerfPoint {
 public:
  explicit PerfPoint(CounterSet = CounterSet{}) {}
  [[nodiscard]] bool valid() const { return false; }
  [[nodiscard]] const std::string &error() const {
    static const std::string empty;
    return empty;
  }
  friend PerfPointDelta operator-(const PerfPoint &, const PerfPoint &) { return PerfPointDelta{}; }
};

inline bool PrimeThread(CounterSet, std::string * = nullptr) { return true; }

}  // namespace perf

using PerfScope = perf::PerfScope;
using PerfPoint = perf::PerfPoint;
using PerfPointDelta = perf::PerfPointDelta;
using PerfCounter = perf::Counter;
using PerfCounterSet = perf::CounterSet;
using PerfThreshold = perf::Threshold;

inline constexpr auto CYCLES = perf::CYCLES;
inline constexpr auto INSTRUCTIONS = perf::INSTRUCTIONS;
inline constexpr auto BRANCHES = perf::BRANCHES;
inline constexpr auto BRANCH_MISS = perf::BRANCH_MISS;
inline constexpr auto L1_LOAD_MISS = perf::L1_LOAD_MISS;
inline constexpr auto L1_STORE_MISS = perf::L1_STORE_MISS;
inline constexpr auto L1_MISS = perf::L1_MISS;
inline constexpr auto DTLB_MISS = perf::DTLB_MISS;
inline constexpr auto ITLB_MISS = perf::ITLB_MISS;
inline constexpr auto TLB_MISS = perf::TLB_MISS;
inline constexpr auto L2_TLB_MISS = perf::L2_TLB_MISS;
inline constexpr auto L2_MISS = perf::L2_MISS;
inline constexpr auto RawEvent = perf::RawEvent;
inline constexpr auto MaxThreshold = perf::MaxThreshold;
inline bool PerfPrimeThread(PerfCounterSet counters, std::string *error = nullptr) {
  return perf::PrimeThread(counters, error);
}

#endif

#endif  // PERF_H_
