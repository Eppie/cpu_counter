# perf.h

`perf.h` is a single-header Apple Silicon PMU profiling helper aimed at instrumenting real C++ code with minimal ceremony:

```cpp
#include "perf.h"

void MixerUpdate() {
  PERF_SCOPE("mixer_update", CYCLES | INSTRUCTIONS | L1_MISS | DTLB_MISS);
  // production work here
}
```

It is header-only in the normal build sense: no extra `.cpp`, no extra link flags, no required init/teardown call. The first sampled use lazily initializes the private `kperf` / `kperfdata` backend, and process shutdown writes JSONL output.

The library now uses a lower-overhead runtime model:

- counters are installed per thread and kept active
- scope enter/exit just snapshots counters and updates thread-local aggregates
- thread-local aggregates are merged only when dumping JSONL at shutdown

The important tradeoff is that nested scopes are expected to use subsets of the installed thread set. If you know a thread will use several different nested regions, prime a superset once with `PerfPrimeThread(...)`.

The lowest-overhead API is now macro-first:

- `PERF_SCOPE(...)` for normal scopes
- `PERF_SCOPE_SAMPLED(...)` for sampled hot loops

Those macros keep a static thread-local site cache, so they avoid the old per-callsite sampling hash map and also cache aggregate / slot metadata.
Use them when the label and counter set are fixed per callsite. If those vary at runtime, use the direct `PerfScope` constructor instead.

## Caveat First

- This uses private Apple APIs.
- Counter programming on this machine still requires `root` or a blessed pid.
- Without privilege, the process still runs, but the JSONL output records dropped scopes with a clear error instead of crashing.

## Build

```sh
make
```

The example binary is `cpu_counter`.

## Run

```sh
sudo ./cpu_counter
```

By default the library writes `perf.jsonl` in the current directory. Override with:

```sh
PERF_OUTPUT=/tmp/my_profile.jsonl sudo ./cpu_counter
PERF_OUTPUT=- sudo ./cpu_counter
```

## API

Basic scope:

```cpp
PERF_SCOPE("mixer_update", CYCLES | INSTRUCTIONS | L1_MISS);
```

Sampling:

```cpp
PERF_SCOPE_SAMPLED("hot_path", CYCLES | L1_MISS, 1024);
```

Fast sampled hot-loop path:

```cpp
PERF_SCOPE_SAMPLED("hot_path", CYCLES | L1_MISS, 1024);
```

`PERF_SCOPE(...)` and `PERF_SCOPE_SAMPLED(...)` both use a `static thread_local` site cache. `PERF_SCOPE_SAMPLED(...)` also uses a per-site sampled counter, so it avoids the old TLS hash-map lookup entirely. For hot code, use the macros.

Thresholds:

```cpp
PerfScope guard("cache_sensitive",
                CYCLES | L2_TLB_MISS,
                {MaxThreshold(L2_TLB_MISS, 1000)});
```

Dynamic or non-macro scope:

```cpp
PerfScope guard("cache_sensitive",
                CYCLES | L2_TLB_MISS,
                {MaxThreshold(L2_TLB_MISS, 1000)});
```

This still works, but it is not the lowest-overhead path because it does not get a static site cache.

Optional thread priming for low-overhead nested use:

```cpp
std::string error;
if (!PerfPrimeThread(CYCLES | INSTRUCTIONS | L1_MISS | DTLB_MISS, &error)) {
  // handle or log error
}
```

This is not required for basic use, but it is the recommended pattern when nested scopes on the same thread need different counters.

Manual snapshots:

```cpp
PerfPoint a(CYCLES | L1_MISS);
// weird control flow
PerfPoint b(CYCLES | L1_MISS);
PerfPointDelta d = b - a;
```

Compile out everything:

```cpp
#define PERF_DISABLE
#include "perf.h"
```

## Built-in Counters

Currently exposed built-ins:

- `CYCLES`
- `INSTRUCTIONS`
- `BRANCHES`
- `BRANCH_MISS`
- `L1_LOAD_MISS`
- `L1_STORE_MISS`
- `L1_MISS`
- `DTLB_MISS`
- `ITLB_MISS`
- `TLB_MISS`
- `L2_TLB_MISS`
- `L2_MISS`

Notes:

- `L1_MISS` currently aliases `L1_LOAD_MISS`.
- `L2_MISS` currently aliases `L2_TLB_MISS_DATA`, not a generic L2 cache miss. That is a convenience alias, not a fully validated semantic name.

Raw configurable events are also supported:

```cpp
PerfScope guard("raw_probe", CYCLES | RawEvent(0x1234, "my_raw_event"));
```

## Output

The library writes one JSON object per aggregate.

Each object contains:

- `label`
- `sample_every`
- `sampled_count`
- `dropped_count`
- `wall_ns`: `total`, `min`, `max`, `mean`
- `counters`: per-counter `total`, `min`, `max`, `mean`
- `threshold_violations`
- `last_error` when scopes were dropped

## Nesting and Thread Safety

- Active scopes are thread-local.
- Aggregation is thread-local on the hot path and merged at dump time.
- The PMU configuration is installed per thread and reused.

Important caveat:

- The first scope on a thread installs that scope's counter set. Later scopes on that thread may widen the installed set when no scope is active.
- Nested scopes are expected to request subsets of the installed thread set. If an inner scope needs extra counters that were not already installed, that inner scope is dropped and the JSONL output records the failure.
- `PerfPrimeThread(...)` is the intended fix for that case: prime the superset once per thread, then use cheap nested subsets.
- If a scope requests too many counters, or a conflicting combination, that scope is dropped and the JSONL output records the failure.
- This implementation reports that as a runtime error, not a compile-time error.

## Breaking Changes

If you were using the previous sampled constructor form:

```cpp
PerfScope guard("hot_path", CYCLES | L1_MISS, 1024);
```

update it to:

```cpp
PERF_SCOPE_SAMPLED("hot_path", CYCLES | L1_MISS, 1024);
```

If you want the new low-overhead cached path for ordinary scopes, update:

```cpp
PerfScope guard("mixer_update", CYCLES | INSTRUCTIONS);
```

to:

```cpp
PERF_SCOPE("mixer_update", CYCLES | INSTRUCTIONS);
```

The direct `PerfScope` constructor still exists for dynamic cases, but the macros are now the preferred production API.

## Files

- [perf.h](/Users/eppie/codex_projects/cpu_counter/perf.h): single-header library
- [main.cpp](/Users/eppie/codex_projects/cpu_counter/main.cpp): tiny usage example
- [Makefile](/Users/eppie/codex_projects/cpu_counter/Makefile): builds the example
