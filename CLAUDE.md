# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

Two surfaces in one repo, sharing only `perf.h`:

1. **`perf.h`** — the product. A single shipped header providing PMU (performance counter) instrumentation for macOS on Apple Silicon. Everything else exists to demo and validate it.
2. **`cpu_counter`** — a demo/validation CLI (built from `main.cpp` + `demos/`) that teaches what each hardware counter means by running small curated workloads and measuring them through the *public* `perf.h` API only.

The demo lab must never reach into `perf.h` internals — it consumes the same public surface a production user would.

## Build / test / run

```sh
make                # build cpu_counter (uses Homebrew LLVM clang++, C++20)
make test           # build + run both test binaries (api_compile, registry_check)
make test-disable   # compile the API smoke test with -DPERF_DISABLE (no-op build path)
make live-smoke     # sudo validate run on P-cores (needs hardware + root)
make clean
```

Run a single test binary directly after building it:

```sh
make test_registry_check && ./test_registry_check
make test_api_compile && ./test_api_compile
```

`CXX` defaults to `clang++` on PATH (stock Apple Clang works); override with `make CXX=...`. The codebase is AppleClang/LLVM-sensitive (see the constexpr string-comparison handling in `perf.h` — `CStringEqual` exists because `std::char_traits` comparison isn't reliably constexpr across these compilers). It requires macOS on Apple Silicon: `perf.h`'s active path has an `#error` guard, and building elsewhere needs `-DPERF_DISABLE`.

### Measured runs need root

Programming the PMU uses **private Apple `kperf`/`kperfdata` frameworks** (loaded via `dlopen` at runtime — see `perf::Api` in `perf.h`) and requires `root` or a blessed pid:

```sh
sudo ./cpu_counter run demo random-pointer-chase
sudo ./cpu_counter run demos --tier stable
sudo ./cpu_counter validate
```

Non-measuring commands work without sudo:

```sh
./cpu_counter list counters | list demos
./cpu_counter explain counter l1-load-miss | explain demo random-pointer-chase
./cpu_counter summary /tmp/profile.jsonl     # analyze production JSONL output
./cpu_counter diff baseline.jsonl candidate.jsonl
```

Without privilege the binary still runs; measured commands fail with a clear error instead of crashing.

## Architecture

### `perf.h` (the library)

A header-only library defined entirely **twice**, gated on `PERF_DISABLE`:

- **Active path** (`#ifndef PERF_DISABLE`, lines ~30–1550): real implementation. `perf::Backend` is the singleton that loads the private frameworks, builds an event `Program`, and programs/reads the PMCs. State lives in a per-thread `ThreadState` — the runtime keeps **one installed configurable-counter set per thread**, and nested scopes are expected to request a *subset* of that installed set. If a thread uses multiple nested regions with different counters, call `PerfPrimeThread(superset)` first.
- **Disabled path** (`#else`, lines ~1552–1708): every public type/constant/function re-declared as a no-op so instrumented code compiles to nothing. **Any change to the public API must be mirrored in both paths**, plus the `using`-alias block and `test-disable` will catch divergence.

The macros at the bottom (`PERF_SCOPE`, `PERF_SCOPE_SAMPLED`) are the intended production entry points — they stamp a `static thread_local PerfScopeSite` per call site so per-callsite state (sample cadence, aggregate) is cached. The bare `PerfScope` constructor still exists for dynamic use. `PerfMeasure(...)` is the explicit one-shot path returning immediate deltas (a `PerfMeasurement`); the demo runner uses this rather than the JSONL aggregation path.

Counters are `perf::Counter` values (named kpep events or raw configs). Public counter constants (`CYCLES`, `L1_LOAD_MISS`, …) and curated bundles (`CACHE_PROFILE`, `BRANCH_PROFILE`, `FRONTEND_PROFILE`, `EXECUTION_PROFILE`) combine with `operator|` into a `CounterSet`. **Naming note:** `L2_TLB_MISS` maps to the TLB event `L2_TLB_MISS_DATA`, not a generic L2 cache miss.

**Design principle — "just work, or fail loudly":** the library hides Apple Silicon PMU quirks so callers never need to know them. `Backend::BuildProgram` (in `perf.h`) does this deliberately and should not be "simplified" away: it (1) adds events to the kpep config **most-constrained-first** so an order-induced slot conflict auto-recovers, and (2) **transparently injects the fixed counters** into configurable-only sets so slot numbering stays in the `FIXED|CONFIG` layout the read path expects (without it, e.g. `PERF_SCOPE("x", L1_LOAD_MISS)` silently misreads). It reads `kpep_event.mask` (guarded by a `sizeof` `static_assert`) only to order events — never for correctness. When a set genuinely can't be programmed, errors name the cause and the remedy.

Production aggregation writes schema-v2 JSONL on shutdown; control with `PERF_OUTPUT=/path` or `PERF_OUTPUT=-` (stdout).

### Demo lab (`demos/` + `main.cpp`)

The lab is **registry-driven**. Two parallel registries in `demos/catalog.cpp`:

- **`WorkloadDefinition[]`** — each demo: id, title, mechanism text, group, tier, default repeats/warmups, a curated `measurement_counters` set, `WorkloadExpectation`s (relative `high`/`low`, approximate value, or near-zero), and an optional `contrast_demo_id` for side-by-side compare tables.
- **`CounterDefinition[]`** — each hardware counter: CLI name, public `PerfCounter` mapping, description, caveats, tier, a curated high demo + low demo id, and a `ValidationRule`.

`demos/catalog.h` declares the registry types, the `DemoEnvironment` (preallocated buffers + mmap'd page/exec regions the workloads run against), and every workload function. `demos/workloads.cpp` implements the actual microarchitecture kernels (pointer chase, stream read/write, page-stride, atomics, store-order, instruction-page, frontend-restart families).

`main.cpp` is three layers: CLI parsing → measurement/retry/placement policy → formatting/interpretation. It dispatches a `CommandKind` in `main()`. Key policy functions to know:

- `ApplyMeasuredRunDefaults(...)` — measured runs default to best-effort **P-core** placement, aggressive retry, and rejecting samples where the requested configurable PMCs stayed inactive.
- `SampleMatches(...)` / `MismatchReason(...)` — accept/reject a measured sample (stable-CPU, active-PMU checks).
- `CounterMeasurementSet(...)` — builds the counter set for a `run counter`, often adding a companion "liveness" counter so a low-baseline workload can still prove the PMU is live.
- `PrintExpectationWarnings(...)` — compare-based warnings only fire when the contrast workload expects the opposite direction.

### Tiers and core-type policy

`Tier::Stable` = validated teaching pairs; `Tier::Experimental` = runnable but not yet trusted. The full counter table and per-counter high/low demos live in `README.md`; the weak/hardware-sensitive cases are documented in `docs/LIMITATIONS.md` — consult those rather than re-deriving. P-core preference is a **demo-runner policy on this M4 Max machine, not a `perf.h` requirement**; the library stays core-neutral.

## Where to read for a task

- Adding/changing a counter: `perf.h` public constants (both paths) + `demos/catalog.cpp` `CounterDefinition[]`.
- Adding/changing a demo: `demos/workloads.cpp` (kernel) + `demos/catalog.cpp` `WorkloadDefinition[]` + declare it in `demos/catalog.h`.
- Runner behavior/placement/retry: `main.cpp` policy functions above.
- `docs/ARCHITECTURE.md` is the human-facing architecture + contributor reading path; `docs/LIMITATIONS.md` lists known-weak / hardware-sensitive counters.

`registry_check` enforces invariants: no duplicate demo/counter ids, measurement sets ≤5 configurable counters, and every stable counter must name a valid high *and* low demo. Run it after registry edits.

## Conventions

- `.gitignore` excludes the built binaries (`cpu_counter`, `test_*`), `perf.jsonl`, `all.log`, and `logs/` — these are run artifacts, not source. Don't commit them.
- Counter CLI names are kebab-case (`l1-load-miss`); the internal kpep event names are SCREAMING_CASE (`L1D_CACHE_MISS_LD`).
