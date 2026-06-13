# Architecture

This repository has two surfaces that share only `perf.h`:

1. **`perf.h`** — the product: a single-header PMU instrumentation library for
   macOS on Apple Silicon.
2. **`cpu_counter`** — a demo/validation CLI (`main.cpp` + `demos/`) that
   teaches what each hardware counter means by running curated workloads and
   measuring them *through the public `perf.h` API only*.

The demo lab never reaches into library internals; it consumes the same surface
a production user would.

## The library (`perf.h`)

The header is organized in three layers.

### 1. Scope and point instrumentation (the public hot path)

- `PERF_SCOPE(label, counters)` / `PERF_SCOPE_SAMPLED(label, counters, every)`
  are the intended production entry points. Each expands to a
  `static thread_local PerfScopeSite` at the call site so per-callsite state
  (sample cadence, aggregate histogram) is cached across calls.
- `PerfScope` (the underlying RAII type) is still usable directly for dynamic
  cases that can't use a macro.
- `PerfPoint` / `PerfPointDelta` capture a counter reading and subtract two
  readings for ad-hoc measurement.
- `PerfMeasure(counters, fn)` runs `fn` once and returns a `PerfMeasurement`
  with immediate deltas. This is the explicit, no-aggregation path — the demo
  runner uses it.

Counters are `perf::Counter` values combined with `operator|` into a
`CounterSet`. Public constants (`CYCLES`, `L1_LOAD_MISS`, …) and curated bundles
(`CACHE_PROFILE`, `BRANCH_PROFILE`, `FRONTEND_PROFILE`, `EXECUTION_PROFILE`)
build those sets at compile time.

### 2. Thread state and priming

The runtime keeps **one installed configurable-counter set per thread**
(`perf::ThreadState`). Nested scopes are expected to request a *subset* of the
installed set. If a thread uses several nested regions with different counters,
prime a superset once up front:

```cpp
std::string error;
PerfPrimeThread(CYCLES | INSTRUCTIONS | L1_MISS | DTLB_MISS, &error);
```

### 3. Backend PMU programming

`perf::Backend` is a singleton that, at first use, `dlopen`s Apple's private
`kperf` and `kperfdata` frameworks (see `perf::Api`), builds a `Program` of
events, and programs/reads the hardware counters. `kpep_db_create(nullptr, …)`
lets kperfdata pick the event database for the current chip, so the counter
constants are not bound to one Apple Silicon generation.

### Compile-out path

The entire public API is defined **twice**: the real implementation under
`#ifndef PERF_DISABLE`, and a no-op mirror under `#else`. Building with
`-DPERF_DISABLE` turns every scope, measurement, and counter constant into
nothing, so instrumented code compiles and links on any platform with zero
overhead. **Any change to the public API must be mirrored in both paths** (and
in the trailing `using`-alias blocks); `make test-disable` guards against
divergence.

### Output

Production `PerfScope` aggregation writes schema-v2 JSONL on shutdown, controlled
by `PERF_OUTPUT` (`/path` or `-` for stdout). `cpu_counter summary` and
`cpu_counter diff` consume that JSONL.

## The demo lab (`demos/` + `main.cpp`)

The lab is **registry-driven**, with two parallel registries in
`demos/catalog.cpp`:

- **`WorkloadDefinition[]`** — each demo: id, title, mechanism text, group,
  tier, default repeats/warmups, a curated `measurement_counters` set,
  `WorkloadExpectation`s (relative `high`/`low`, approximate value, or
  near-zero), and an optional `contrast_demo_id` for side-by-side tables.
- **`CounterDefinition[]`** — each hardware counter: CLI name, public
  `PerfCounter` mapping, description, caveats, tier, a curated high demo + low
  demo id, and a `ValidationRule`.

`demos/catalog.h` declares the registry types, the `DemoEnvironment`
(preallocated buffers plus mmap'd page/exec regions the kernels run against),
and every workload function. `demos/workloads.cpp` implements the actual
microarchitecture kernels: pointer-chase, stream read/write, page-stride,
atomics, store-order, instruction-page, and frontend-restart families. Kernels
guard Arm64-specific code with `#if defined(__aarch64__)` and provide portable
fallbacks.

`main.cpp` has three layers — CLI parsing → measurement / retry / placement
policy → formatting / interpretation — and dispatches a `CommandKind` in
`main()`. Policy functions worth knowing:

- `ApplyMeasuredRunDefaults(...)` — measured runs default to best-effort P-core
  placement, aggressive retry, and rejecting samples where the requested
  configurable PMCs stayed inactive.
- `SampleMatches(...)` / `MismatchReason(...)` — accept or reject a measured
  sample (stable-CPU, active-PMU checks).
- `CounterMeasurementSet(...)` — builds the counter set for `run counter`, often
  adding a companion "liveness" counter so a low-baseline workload can still
  prove the PMU is live.
- `PrintExpectationWarnings(...)` — compare-based warnings fire only when the
  contrast workload expects the opposite direction.

## Tiers and core policy

`Tier::Stable` are validated teaching pairs; `Tier::Experimental` are runnable
but not yet trusted. P-core preference is a **demo-runner policy** (the demos
were tuned on an M4 Max), **not** a `perf.h` requirement — the library stays
core-neutral. See [`LIMITATIONS.md`](LIMITATIONS.md) for counters that are weak
or hardware-sensitive.

## Reading path for contributors

1. `README.md`
2. `perf.h` — trace the install/measure path: public API → `PerfMeasurement` /
   `PerfMeasure` → `Backend` init and `BuildProgram` → thread priming → scope
   enter/exit.
3. `main.cpp` — the runner policy functions above.
4. `demos/catalog.h`, then `demos/catalog.cpp` (the two registries).
5. `demos/workloads.cpp` (the kernels).

## Common changes

- **Add/change a counter:** add the public constant in `perf.h` (both the active
  and `PERF_DISABLE` paths, plus the alias blocks), then add a
  `CounterDefinition` in `demos/catalog.cpp`.
- **Add/change a demo:** implement the kernel in `demos/workloads.cpp`, declare
  it in `demos/catalog.h`, and register a `WorkloadDefinition` in
  `demos/catalog.cpp`.

## Tests

`make test` builds and runs two binaries:

- `tests/api_compile.cpp` — public-API smoke test (also compiled with
  `-DPERF_DISABLE` via `make test-disable`).
- `tests/registry_check.cpp` — registry invariants: no duplicate demo/counter
  ids, measurement sets hold ≤ 5 configurable counters, and every stable counter
  names a valid high *and* low demo. Run it after editing the registries.
