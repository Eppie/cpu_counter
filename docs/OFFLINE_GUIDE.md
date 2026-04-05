# Offline Guide

This file is for "I have the repo but not the network and want enough useful work to do."

The project has two distinct surfaces:

1. `perf.h`
   The production-facing single-header library for PMU instrumentation.
2. `cpu_counter`
   The demo lab and validation CLI that explains counters with curated workloads.

Use this guide in order. Each section should leave you with a concrete question or code-reading task.

## 1. Fast Orientation

Read these files in this order:

1. `README.md`
2. `perf.h`
3. `main.cpp`
4. `demos/catalog.h`
5. `demos/catalog.cpp`
6. `demos/workloads.cpp`

What to look for:

- In `perf.h`, identify the three hot-path layers:
  - scope instrumentation
  - point measurement
  - backend PMU programming / thread state
- In `main.cpp`, identify the three runner layers:
  - CLI parsing
  - measurement / retry / placement policy
  - formatting / interpretation
- In `demos/catalog.cpp`, identify the three registry layers:
  - workload expectations
  - workload metadata
  - counter metadata

## 2. Current State Snapshot

Read `docs/CURRENT_STATUS.md` next.

That file captures:

- what passed in the latest large run
- which demos are still weak
- which failures look like runner-policy issues versus weak hardware signals
- where to read code for each unresolved problem

## 3. Reading Questions

Answer these while reading the code. Write the answers anywhere you like.

### Library questions

- How many times can the backend reprogram PMCs on one thread during a typical demo run?
- What exactly does `HasActiveConfigurableCounters()` prove, and what does it not prove?
- Which parts of the API are intentionally cheap enough for hot loops, and which are not?
- What assumptions does the library make about nested scopes and thread-local installed counter sets?

### Demo-runner questions

- Where is exact CPU preference enforced?
- Where is P-core range preference enforced?
- Which parts of the retry loop happen inside a worker thread versus outside it?
- Where do contrast demos reuse the primary measurement set, and why?
- Which warnings are currently comparison-based and which are single-demo zero checks?

### Demo-registry questions

- Which counters are stable tier?
- Which experimental counters are likely to be weak by design on this machine?
- Which workload pairs are symmetric comparisons and which are one-sided teaching cases?

## 4. Good Offline Work

If you want concrete tasks while offline, do them in this order.

### Task A: Audit the stable tier

Goal:
- convince yourself the stable counters really deserve to be stable

Read:
- `demos/catalog.cpp`
- `all2.log`

Check:
- does each stable counter have a believable high demo and low demo?
- does the explanation text actually match the workload mechanism?
- are any stable counters relying on fragile "high because it is not zero" reasoning?

Deliverable:
- a short note with any stable counter you think should be downgraded or clarified

### Task B: Tighten weak explanations

Goal:
- improve the human explanations without changing code

Look for:
- explanations that describe ratios but not mechanisms
- explanations that mention "more useful work per cycle" where "less stalled" or "more frontend churn" would be sharper
- counters whose notes are technically true but too generic

Best candidates:
- `nt-stream-read`
- `nt-stream-write`
- `first-touch-fault`
- `frontend-random-restart`
- `dispatch-memory-stream`

Deliverable:
- rewritten one-line expectation notes and one-line interpretation bullets

### Task C: Audit measurement-set design

Goal:
- understand which demo measurement sets are "headline counter only" and which include companion liveness counters

Read:
- `demos/catalog.cpp`
- `main.cpp`, especially `CounterMeasurementSet(...)`

Questions:
- which demos include helper counters only to prove the PMU is live?
- where should that be documented more explicitly?
- are there demos whose measurement set should be widened further to make inactive-PMU detection less ambiguous?

Deliverable:
- a list of demos whose measurement sets should probably be revised next

### Task D: Triage the remaining weak demos

Focus on these:

- `barrier-loop`
- `interrupt-storm`
- `store-order-friendly`
- `store-order-alias`
- `nt-stream-read`
- `first-touch-fault`

For each one, answer:

- Is the issue likely runner policy, event semantics, or workload shape?
- Is the counter meaningful enough to keep as a first-class demo?
- If not, should it stay experimental with a stronger caveat?

Deliverable:
- a "keep / retune / demote" decision for each of the six

## 5. Source Reading Path By Topic

### Production library

Read these sections in `perf.h`:

- public API and typedefs
- `PerfMeasurement`
- `PerfMeasure(...)`
- backend initialization and `BuildProgram(...)`
- thread installation / priming
- scope enter / exit

Questions:

- What are the real hot-path allocations, if any?
- Where can installation fail after `sudo`?
- How is event conflict surfaced to callers?

### Runner policy

Read these in `main.cpp`:

- `ApplyMeasuredRunDefaults(...)`
- `PrintExpectationWarnings(...)`
- `MismatchReason(...)`
- `CounterMeasurementSet(...)`
- `ExecuteWorkload(...)`
- `RunDemo(...)`
- `RunCounter(...)`
- `RunDemos(...)`
- `Validate(...)`

Questions:

- Which defaults are now "safe but strict"?
- Which defaults would be too strict for production use but fine for the lab?

### Demo registry

Read these in `demos/catalog.cpp`:

- all `k*Expectations`
- the workload array
- the counter array

Questions:

- Which expectations are absolute "high/low" claims?
- Which are really "higher than this contrast workload" claims?
- Which descriptions need machine-specific caveats for M4 Max P-cores?

### Workloads

Read these in `demos/workloads.cpp`:

- pointer chase pair
- stream read/write family
- page-stride / first-touch pair
- atomic pair
- store-order pair
- instruction-page pair
- frontend restart pair

Questions:

- Which loops are deliberately scalar?
- Which loops are deliberately SIMD?
- Which loops depend on OS behavior beyond pure user-space instruction mix?

## 6. Suggested Offline Notes To Write

If you want a productive artifact while offline, write one or more of these:

- "What should count as production-ready in this repo?"
- "Which experimental counters are worth keeping?"
- "What should `run demos` do by default on machines with heterogeneous cores?"
- "Which counters deserve a second contrast workload?"
- "Should low-baseline demos always include a live companion counter?"

## 7. When You Are Back Online

Good next commands:

```sh
make
sudo ./cpu_counter run demos --tier all --prefer-pcore --require-stable-cpu > all3.log
sudo ./cpu_counter validate --prefer-pcore --require-stable-cpu --require-active-pmu
```

If you want to focus only on the weak cases:

```sh
sudo ./cpu_counter run demo barrier-loop --prefer-pcore --require-stable-cpu
sudo ./cpu_counter run demo interrupt-storm --prefer-pcore --require-stable-cpu
sudo ./cpu_counter run demo store-order-friendly --prefer-pcore --require-stable-cpu
sudo ./cpu_counter run demo store-order-alias --prefer-pcore --require-stable-cpu
sudo ./cpu_counter run demo nt-stream-read --prefer-pcore --require-stable-cpu
sudo ./cpu_counter run demo first-touch-fault --prefer-pcore --require-stable-cpu
```

## 8. Short Version

If you only have limited attention:

1. Read `docs/CURRENT_STATUS.md`.
2. Read `perf.h` with a pen and trace the installation / measurement path.
3. Read `main.cpp` and decide whether the runner is still too strict.
4. Read the weak demo sections in `demos/catalog.cpp` and `demos/workloads.cpp`.
5. Write down what should be stable, what should stay experimental, and what should be removed.
