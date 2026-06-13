# perf.h + cpu_counter

This project now has two jobs:

1. `perf.h` is the production-facing single-header PMU instrumentation library.
2. `cpu_counter` is a demo and validation binary that explains counters with small curated workloads.

The library stays as one shipped header. The demo lab is built from the `demos/` subtree and uses the public library API only.

For a deeper tour, see:

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — how the library and demo lab fit together, and a reading path for contributors
- [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md) — counters that are weak or hardware-sensitive, and what to expect off the reference machine

## Requirements

- **macOS on Apple Silicon.** `perf.h` is built on the private Apple `kperf` / `kperfdata` frameworks and Arm64 PMU events; it does not compile or run elsewhere.
- A C++20 compiler. The stock `clang++` from the Xcode command line tools works; override `make CXX=...` for another toolchain.
- `root` (or a blessed pid) to *program* counters. Building, `list`, `explain`, `summary`, and `diff` need no privilege.

The library auto-selects the PMU event database for whatever Apple Silicon chip it runs on, so the counter constants are not tied to one generation. The **demo interpretations and tuning, however, were validated on an M4 Max** (P-core oriented). On other chips some experimental counters may be named differently or behave differently — see [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md).

**Counter limits:** you can measure the two fixed counters (cycles, instructions) plus up to ~8 configurable counters at once, in any order — `perf.h` handles slot allocation, event ordering, and fixed-counter requirements for you, and fails with an actionable message if a set genuinely can't be programmed together. See [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md#counter-limits-and-conflicts) for the details.

## Caveat First

- This project uses private Apple `kperf` / `kperfdata` APIs. They are undocumented and can change between macOS releases; do not ship this in software you submit to the App Store.
- Programming counters requires `root` or a blessed pid.
- Some machines also reject `kpc_force_all_ctrs_set(1)` even under `sudo`; the library treats that as best-effort and reports the later programming failure directly if configurable PMCs still cannot be installed.
- Without privilege, the binary still runs, but measured commands fail with a clear error instead of crashing.

## Integration

`perf.h` is a single header with no build step of its own. Pick whichever path fits your project; the CMake paths give you a `perf::perf` target that carries the include path and the C++20 requirement.

### Vendor the header

Copy `perf.h` into your tree and include it (C++20 required):

```cpp
#include "perf.h"
```

### CMake — FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(perf
  GIT_REPOSITORY https://github.com/Eppie/cpu_counter.git
  GIT_TAG main)
FetchContent_MakeAvailable(perf)
target_link_libraries(your_target PRIVATE perf::perf)
```

### CMake — add_subdirectory

```cmake
add_subdirectory(third_party/cpu_counter)
target_link_libraries(your_target PRIVATE perf::perf)
```

(Consuming via `add_subdirectory`/FetchContent builds only the library; the demo, tests, and examples are skipped.)

### CMake — installed package

```sh
cmake -B build && cmake --build build && cmake --install build --prefix /your/prefix
```

```cmake
find_package(perf CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE perf::perf)
```

Notes:

- Define `PERF_DISABLE` to compile all instrumentation out to no-ops (the public API still exists, so instrumented code builds anywhere — handy for non-Apple targets).
- `PERF_VERSION` (plus `PERF_VERSION_MAJOR` / `_MINOR` / `_PATCH`) exposes the header version.
- A minimal usage example lives in [`examples/quickstart.cpp`](examples/quickstart.cpp).

## Build

```sh
make
```

Useful targets:

```sh
make test
make test-disable
make live-smoke
```

## 1. Using `perf.h` in Production Code

`perf.h` is still the stable public surface. The recommended path is macro-first because the macros keep a static thread-local site cache.

```cpp
#include "perf.h"

void MixerUpdate() {
  PERF_SCOPE("mixer_update", CYCLES | INSTRUCTIONS | L1_MISS);
}

void HotLoop() {
  for (std::size_t i = 0; i < 1'000'000; ++i) {
    PERF_SCOPE_SAMPLED("hot_loop", CYCLES | L1_MISS, 1024);
  }
}
```

### Recommended API

- `PERF_SCOPE(...)`
- `PERF_SCOPE_SAMPLED(...)`
- `PerfPrimeThread(...)`
- `PerfPoint`
- `PerfMeasure(...)`
- curated counter bundles: `CACHE_PROFILE`, `BRANCH_PROFILE`, `FRONTEND_PROFILE`, `EXECUTION_PROFILE`

### Explicit Measurement API

The demo runner uses `PerfMeasure(...)`, and advanced users can use it directly when they want immediate deltas instead of aggregate JSONL output.

```cpp
std::uint64_t sink = 0;
PerfMeasurement measurement = PerfMeasure(CYCLES | INSTRUCTIONS | L1_LOAD_MISS, [&] {
  sink = RunKernel();
});

if (measurement.valid) {
  auto cycles = measurement.Get(CYCLES);
  auto misses = measurement.Get(L1_LOAD_MISS);
}
```

`PerfMeasurement` includes:

- `valid`
- `error`
- `wall_ns`
- `cpu_before`
- `cpu_after`
- `count`
- `set`
- raw counter deltas in `values`
- `Get(Counter)`
- `HasActiveConfigurableCounters()`

### Thread Priming

The runtime keeps one installed counter set per thread. Nested scopes are expected to use subsets of that installed set.

If you know a thread will use multiple nested regions with different counters, prime a superset first:

```cpp
std::string error;
if (!PerfPrimeThread(CYCLES | INSTRUCTIONS | L1_MISS | DTLB_MISS, &error)) {
  // handle error
}
```

### Output

Production-style `PerfScope` aggregation writes JSONL on shutdown, but only when you point it somewhere with `PERF_OUTPUT` — it never drops a file into your working directory on its own. If scopes were recorded and `PERF_OUTPUT` is unset, it prints a one-line hint to stderr instead.

```sh
PERF_OUTPUT=/tmp/profile.jsonl ./your_binary   # write to a file
PERF_OUTPUT=- ./your_binary                    # write to stdout
```

Each scope aggregate record is schema-marked JSONL. Existing total/min/max/mean fields remain, and schema v2 adds:

- `schema: 2` and `type: "scope_aggregate"`
- stable counter `id` values alongside display names
- `estimated_count` for sampled scopes
- approximate `p50`, `p95`, and `p99` for wall time and counters
- `distribution: "log2_upper_bound"` to describe the quantile approximation

The demo binary does not rely on that aggregated output. It measures workloads explicitly with `PerfMeasure(...)` and prints human-facing explanations.

The `cpu_counter` binary can also inspect production JSONL output:

```sh
./cpu_counter summary /tmp/profile.jsonl
./cpu_counter diff baseline.jsonl candidate.jsonl
```

`summary` sorts scopes by estimated cycle hotness and prints derived metrics such as IPC, L1 misses per kilo-instruction, and TLB pressure per kilo-instruction. `diff` matches scope aggregates by label, sample rate, and counter set, then reports cycle-per-call, IPC, and L1-miss deltas. Its verdicts use aggregate means plus p50/p95 direction when present; formal statistical tests still require repeated run-level samples, not just one aggregate JSONL file.

## 2. Running the Demo Lab

One binary drives the whole demo system:

```sh
sudo ./cpu_counter help
```

### CLI

```sh
./cpu_counter list counters
./cpu_counter list demos
./cpu_counter explain counter l1-load-miss
./cpu_counter explain demo random-pointer-chase
sudo ./cpu_counter run counter l1-load-miss
sudo ./cpu_counter run demo page-stride-read
sudo ./cpu_counter run demos --tier stable
./cpu_counter summary /tmp/profile.jsonl
./cpu_counter diff baseline.jsonl candidate.jsonl
sudo ./cpu_counter validate
```

Supported flags:

- `--tier stable|experimental|all`
- `--group <group>`
- `--repeat <N>`
- `--warmup <N>`
- `--prefer-pcore`
- `--prefer-ecore`
- `--prefer-cpu <N>`
- `--require-stable-cpu`
- `--require-active-pmu`

Measured commands (`run counter`, `run demo`, and `run demos`) now default to:

- best-effort P-core preference on this machine
- retrying more aggressively before giving up on a sample
- rejecting samples where the requested configurable PMCs stayed inactive

You can still tighten runs further with `--require-stable-cpu`.

Measured demo output now includes:

- raw counter means
- normalized derived metrics such as IPC/CPI, branch miss per kbranch, miss per kinst, uops per instruction, and percentage shares when the required counters are present
- side-by-side compare tables for demos that have an explicit contrast workload

### Demo Structure

The demo lab is registry-driven.

Each workload has:

- stable id
- title
- summary
- mechanism explanation
- group
- tier
- default repeats / warmups
- curated measurement set
- expected counter behaviors, which can be relative (`high` / `low`) or approximate count targets for the demos with stable loop-level expectations
- normalized derived metrics printed opportunistically from the measured counter set

Each counter has:

- CLI name
- public `Counter` mapping
- description
- caveats
- tier
- curated high demo
- curated low demo
- validation rule

### Current Stable Demos

Stable showcase workloads currently include:

- `dense-integer-alu`
- `hot-seq-read`
- `linear-pointer-chase`
- `random-pointer-chase`
- `hot-seq-write`
- `random-page-write`
- `page-stride-read`
- `predictable-branch`
- `unpredictable-branch`
- `hot-instruction-loop`
- `random-instruction-pages`

Experimental showcase workloads currently include:

- `scalar-stream-read`
- `nt-stream-read`
- `simd-stream-read`
- `aligned-x64-load`
- `cross-x64-load`
- `aligned-page-load`
- `cross-page-load`
- `scalar-stream-write`
- `nt-stream-write`
- `simd-stream-write`
- `uncontended-atomic-cas`
- `contended-atomic-cas`
- `barrier-loop`
- `interrupt-storm`
- `store-order-friendly`
- `store-order-alias`
- `first-touch-fault`
- `aligned-x64-store`
- `cross-x64-store`
- `aligned-page-store`
- `cross-page-store`
- `simd-vector-alu`
- `dispatch-int-alu`
- `dispatch-memory-stream`
- `dispatch-simd-alu`
- `frontend-hot-restart`
- `frontend-random-restart`
- `frontend-self-modifying-restart`

### Counter Support Table

The registry now covers the full 63-counter target list from the original research brief.

| CLI name | Tier | Meaning | High demo | Low demo | Notes |
| --- | --- | --- | --- | --- | --- |
| `cycles` | stable | fixed cycle count | `random-pointer-chase` | `dense-integer-alu` | best read with instruction or miss context |
| `instructions` | stable | fixed retired instructions | `dense-integer-alu` | `random-pointer-chase` | shows instruction density, not stalls |
| `branches` | stable | retired branch instructions | `unpredictable-branch` | `dense-integer-alu` | branch-heavy vs straighter-line code |
| `branch-miss` | stable | branch mispredicts | `unpredictable-branch` | `predictable-branch` | strongest when branch count is already high |
| `l1-load-miss` | stable | L1D load misses | `random-pointer-chase` | `hot-seq-read` | clear memory-latency showcase |
| `l1-store-miss` | stable | L1D store misses | `random-page-write` | `hot-seq-write` | store-heavy cold-page pattern |
| `dtlb-miss` | stable | first-level data TLB misses | `page-stride-read` | `hot-seq-read` | page-granular data access |
| `itlb-miss` | stable | instruction TLB misses | `random-instruction-pages` | `hot-instruction-loop` | generated executable stubs |
| `l2-tlb-miss` | stable | second-level data TLB misses | `page-stride-read` | `hot-seq-read` | a TLB event (`L2_TLB_MISS_DATA`), not a generic L2 cache miss |
| `l1i-cache-miss` | experimental | L1I demand misses | `random-instruction-pages` | `hot-instruction-loop` | more code-shape sensitive |
| `l1-load-miss-nonspec` | experimental | nonspec load misses | `random-pointer-chase` | `hot-seq-read` | semantics are more implementation-specific |
| `l1-store-miss-nonspec` | experimental | nonspec store misses | `random-page-write` | `hot-seq-write` | write-allocate effects can complicate reading |
| `inst-all` | experimental | aggregate retired instructions | `dense-integer-alu` | `random-pointer-chase` | PMU-event view of overall retired instructions |
| `core-active-cycle` | experimental | cycles where the core stayed actively busy | `dispatch-int-alu` | `random-pointer-chase` | dense compute versus latency-stalled pointer chasing |
| `interrupt-pending` | experimental | pending-interrupt pressure on the measured core | `interrupt-storm` | `dense-integer-alu` | driven by helper-thread signal delivery rather than background system noise |
| `inst-int-alu` | experimental | retired integer ALU instructions | `dense-integer-alu` | `hot-seq-read` | pure compute versus memory-heavy access |
| `retire-uop` | experimental | retired micro-ops | `dispatch-int-alu` | `random-pointer-chase` | lower-level counterpart to instruction retirement |
| `map-uop` | experimental | mapped micro-ops | `dispatch-int-alu` | `random-pointer-chase` | broad frontend-to-backend uop flow |
| `map-int-uop` | experimental | mapped integer-class uops | `dispatch-int-alu` | `dispatch-simd-alu` | scalar integer mapping versus SIMD mapping |
| `inst-simd-alu` | experimental | retired SIMD arithmetic instructions | `simd-vector-alu` | `dense-integer-alu` | vector-compute counterpart to scalar integer ALU |
| `map-simd-uop` | experimental | mapped SIMD-class uops | `dispatch-simd-alu` | `dispatch-int-alu` | SIMD mapping versus scalar integer mapping |
| `inst-simd-ld` | experimental | retired SIMD load instructions | `simd-stream-read` | `scalar-stream-read` | same stream, different load instruction class |
| `inst-int-ld` | experimental | retired integer load instructions | `random-pointer-chase` | `dense-integer-alu` | good load-heavy versus register-only contrast |
| `ld-nt-uop` | experimental | non-temporal load uops | `nt-stream-read` | `scalar-stream-read` | same stream, but explicitly requests the NT load path |
| `inst-simd-st` | experimental | retired SIMD store instructions | `simd-stream-write` | `scalar-stream-write` | same stream, different store instruction class |
| `atomic-succ` | experimental | successful atomic/exclusive ops | `uncontended-atomic-cas` | `dense-integer-alu` | direct atomic-success teaching counter |
| `atomic-fail` | experimental | failed atomic/exclusive ops | `contended-atomic-cas` | `uncontended-atomic-cas` | retry-heavy compare-exchange under contention |
| `inst-int-st` | experimental | retired integer store instructions | `random-page-write` | `dense-integer-alu` | clearest store-side trigger in the current lab |
| `st-nt-uop` | experimental | non-temporal store uops | `nt-stream-write` | `scalar-stream-write` | same stream, but explicitly requests the NT store path |
| `st-memory-order-violation` | experimental | non-speculative store/load ordering violations | `store-order-alias` | `store-order-friendly` | experimental 4 KiB-alias stress pair |
| `inst-ldst` | experimental | retired load/store instructions | `random-pointer-chase` | `dense-integer-alu` | broad memory-instruction mix signal |
| `map-ldst-uop` | experimental | mapped load/store uops | `dispatch-memory-stream` | `dispatch-int-alu` | separates memory-stream mapping from scalar compute |
| `ld-unit-uop` | experimental | load-unit micro-ops | `random-pointer-chase` | `dense-integer-alu` | often tracks load pressure more directly than retired instructions |
| `st-unit-uop` | experimental | store-unit micro-ops | `random-page-write` | `dense-integer-alu` | store pressure teaching counter |
| `l1d-writeback` | experimental | L1D writebacks | `random-page-write` | `hot-seq-write` | useful for dirty-line eviction behavior |
| `inst-barrier` | experimental | retired barrier instructions | `barrier-loop` | `dense-integer-alu` | ordering-heavy versus straight compute |
| `dtlb-access` | experimental | DTLB accesses | `page-stride-read` | `hot-seq-read` | shows translation activity before misses alone |
| `dtlb-fill` | experimental | DTLB fills | `page-stride-read` | `hot-seq-read` | emphasizes refills of translation entries |
| `mmu-table-walk-data` | experimental | data-side page table walks | `page-stride-read` | `hot-seq-read` | clearest real table-walk trigger in the lab |
| `mmu-virtual-memory-fault` | experimental | data-side virtual-memory faults | `first-touch-fault` | `hot-seq-read` | deliberately faults in a fresh anonymous mapping |
| `dtlb-miss-nonspec` | experimental | nonspec DTLB misses | `page-stride-read` | `hot-seq-read` | alternate view of the same sparse-page translation story |
| `branch-cond-miss` | experimental | conditional branch mispredicts | `unpredictable-branch` | `predictable-branch` | branch-miss story limited to conditional branches |
| `map-rewind` | experimental | mapper rewind events | `unpredictable-branch` | `predictable-branch` | intended to show speculative work getting thrown away |
| `inst-branch-cond` | experimental | retired conditional branches | `unpredictable-branch` | `dense-integer-alu` | branch-heavy versus mostly straight-line code |
| `inst-branch-taken` | experimental | retired taken branches | `predictable-branch` | `dense-integer-alu` | biased-taken branch pattern is the clean high case |
| `inst-branch-call` | experimental | retired call branches | `hot-instruction-loop` | `dense-integer-alu` | direct way to expose repeated stub calls |
| `inst-branch-ret` | experimental | retired return branches | `hot-instruction-loop` | `dense-integer-alu` | pairs naturally with the same stub-call loop |
| `inst-branch-indir` | experimental | retired indirect branches | `hot-instruction-loop` | `dense-integer-alu` | function-pointer call is the teaching case |
| `branch-call-indir-miss` | experimental | indirect-call mispredicts | `random-instruction-pages` | `hot-instruction-loop` | changing function-pointer targets versus one stable target |
| `branch-indir-miss` | experimental | indirect-branch mispredicts | `random-instruction-pages` | `hot-instruction-loop` | general indirect-target churn showcase |
| `branch-ret-indir-miss` | experimental | return-side indirect-branch mispredicts | `random-instruction-pages` | `hot-instruction-loop` | best available return-target stress case in the lab |
| `fetch-restart` | experimental | frontend fetch restart events | `frontend-random-restart` | `frontend-hot-restart` | code-page churn versus one hot stub |
| `flush-restart-other` | experimental | non-branch frontend flush/restart events | `frontend-self-modifying-restart` | `frontend-hot-restart` | explicit code rewriting plus I-cache invalidation is a better probe than plain code-page churn |
| `map-dispatch-bubble` | experimental | mapper/dispatch bubbles | `frontend-random-restart` | `frontend-hot-restart` | hot versus randomized code-page locality |
| `map-dispatch-bubble-ic` | experimental | instruction-cache-driven dispatch bubbles | `random-instruction-pages` | `hot-instruction-loop` | explicit code-cache churn teaching case |
| `map-dispatch-bubble-itlb` | experimental | instruction-TLB-driven dispatch bubbles | `random-instruction-pages` | `hot-instruction-loop` | explicit ITLB-churn teaching case |
| `map-stall` | experimental | mapper stall events | `frontend-random-restart` | `frontend-hot-restart` | frontend-locality contrast for mapper stalls |
| `map-stall-dispatch` | experimental | dispatch-facing mapper stalls | `frontend-random-restart` | `frontend-hot-restart` | same hot-versus-randomized frontend pair |
| `l1i-tlb-fill` | experimental | instruction-side TLB fills | `random-instruction-pages` | `hot-instruction-loop` | code-page churn refill signal |
| `l2-tlb-miss-instruction` | experimental | second-level instruction TLB misses | `random-instruction-pages` | `hot-instruction-loop` | instruction-side analogue of the data TLB story |
| `mmu-table-walk-instruction` | experimental | instruction-side page walks | `random-instruction-pages` | `hot-instruction-loop` | frontend page-walk teaching counter |
| `ldst-x64-uop` | experimental | 64-byte split load/store uops | `cross-x64-load` | `aligned-x64-load` | teaches 64B split accesses directly |
| `ldst-xpg-uop` | experimental | cross-page load/store uops | `cross-page-load` | `aligned-page-load` | teaches page-spanning accesses directly |

## 3. Stability Tiers and Core-Type Caveats

The project now uses explicit tiers:

- `stable`: curated, validated showcase pairs intended to teach the counter clearly
- `experimental`: visible and runnable, but not yet trusted as “production-ready teaching examples”

On this M4 Max machine, demo interpretation is P-core-oriented by default.

Why:

- earlier reverse-engineering work showed counter behavior differs across logical CPUs and perflevels
- the demo runner therefore defaults to best-effort P-core scheduling
- `validate` also forces `--require-stable-cpu` and `--require-active-pmu`

The library itself stays neutral. These P-core preferences are a demo-runner policy, not a `perf.h` requirement.

## 4. Migration Notes

The old constructor-heavy sampled API is no longer the recommended path.

### Old sampled code

```cpp
PerfScope guard("hot_path", CYCLES | L1_MISS, 1024);
```

### New sampled code

```cpp
PERF_SCOPE_SAMPLED("hot_path", CYCLES | L1_MISS, 1024);
```

### Old ordinary scope

```cpp
PerfScope guard("mixer_update", CYCLES | INSTRUCTIONS);
```

### New ordinary scope

```cpp
PERF_SCOPE("mixer_update", CYCLES | INSTRUCTIONS);
```

The direct `PerfScope` constructor still exists for dynamic cases, but the macros are now the intended production API because they cache per-callsite state.

## Repo Layout

- [perf.h](perf.h): shipped single-header library
- [main.cpp](main.cpp): CLI demo runner
- [demos/catalog.h](demos/catalog.h): registry types and shared demo declarations
- [demos/catalog.cpp](demos/catalog.cpp): counter catalog and workload registry
- [demos/workloads.cpp](demos/workloads.cpp): curated microarchitecture workloads
- [tests/api_compile.cpp](tests/api_compile.cpp): public API smoke test
- [tests/registry_check.cpp](tests/registry_check.cpp): stable registry validation

## License

MIT. See [LICENSE](LICENSE).
