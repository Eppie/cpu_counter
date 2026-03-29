# perf.h

`perf.h` is a single-header Apple Silicon PMU profiling helper aimed at instrumenting real C++ code with minimal ceremony:

```cpp
#include "perf.h"

void MixerUpdate() {
  PerfScope guard("mixer_update", CYCLES | INSTRUCTIONS | L1_MISS | DTLB_MISS);
  // production work here
}
```

It is header-only in the normal build sense: no extra `.cpp`, no extra link flags, no explicit init/teardown call. The first sampled use lazily initializes the private `kperf` / `kperfdata` backend, and process shutdown writes JSONL output.

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
PerfScope guard("mixer_update", CYCLES | INSTRUCTIONS | L1_MISS);
```

Sampling:

```cpp
PerfScope guard("hot_path", CYCLES | L1_MISS, 1024);
```

Thresholds:

```cpp
PerfScope guard("cache_sensitive",
                CYCLES | L2_TLB_MISS,
                1,
                {MaxThreshold(L2_TLB_MISS, 1000)});
```

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
- Aggregation is process-global and mutex-protected.
- Nested scopes are supported by reprogramming to the union of counters requested by the active stack on that thread.

Important caveat:

- Nested scopes only work while the union fits what the PMU can program simultaneously.
- If a scope or nested union requests too many counters, or requests a conflicting combination, that scope is dropped and the JSONL output records the failure.
- This implementation reports that as a runtime error, not a compile-time error.

## Files

- [perf.h](/Users/eppie/codex_projects/cpu_counter/perf.h): single-header library
- [main.cpp](/Users/eppie/codex_projects/cpu_counter/main.cpp): tiny usage example
- [Makefile](/Users/eppie/codex_projects/cpu_counter/Makefile): builds the example
