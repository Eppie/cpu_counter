# Current Status

This file is a snapshot for offline review.

It reflects the repo after:

- full demo coverage was added for all 63 target counters
- the runner was hardened against dead configurable PMCs
- exact-CPU retries were relaxed toward P-core-range execution
- the weak NT / first-touch / store-order / barrier / interrupt demos were retuned

## Latest Useful Batch Runs

### `all.log`

Command style:

- exact logical CPU targeting (`--prefer-cpu 8`)
- stable CPU required
- active PMU required

Result:

- successes: 25
- failures: 13

Main takeaway:

- exact CPU targeting is too strict for large batch runs
- many failures were placement noise, not bad demos

### `all2.log`

Command style:

- P-core range targeting (`--prefer-pcore`)
- stable CPU required
- active PMU required

Result:

- successes: 34
- failures: 4

Main takeaway:

- P-core range is the right default for this machine
- most earlier failures were retry / placement artifacts
- the remaining issues are much more informative

## What Looks Good

These demo families looked healthy in `all2.log`:

- pointer-chase memory-latency demos
- scalar vs SIMD stream classification
- hot vs random page-memory demos
- branch predictability demos
- instruction-side / ITLB / I-cache demos
- mapper / uop / active-cycle demos
- atomic contention demos
- frontend restart demos
- cross-page and split-boundary store demos

This means the overall lab architecture is now sound enough to trust for most counters.

## Remaining Weak Or Risky Areas

### 1. `barrier-loop`

Previous symptom:

- low-baseline contrast got rejected as "inactive PMU"

Recent fix:

- added `INST_ALL` companion activity so the demo no longer relies on `INST_BARRIER` being nonzero in both cases

What to verify next:

- does `inst-barrier` separate cleanly?
- does the low baseline still show live configurable PMCs?

Relevant files:

- `demos/catalog.cpp`
- `demos/workloads.cpp`
- `main.cpp`

### 2. `interrupt-storm`

Previous symptom:

- contrast baseline got rejected as "inactive PMU"

Recent fix:

- added `INST_ALL` companion activity so the low baseline can still prove the configurable PMU is live

What to verify next:

- does `interrupt-pending` move enough to justify keeping this demo?
- is signal delivery too OS-sensitive to be a reliable first-class teaching case?

Relevant files:

- `demos/workloads.cpp`
- `demos/catalog.cpp`

### 3. `store-order-friendly` / `store-order-alias`

Previous symptom:

- low case failed as "inactive PMU"
- the high case was not yet proven to separate clearly

Recent fix:

- added `INST_LDST` companion activity
- made the workloads more store-heavy before the aliased load

What to verify next:

- does `st-memory-order-violation` separate at all on this M4 Max?
- if not, should this stay in the catalog but be explicitly documented as hardware-weak?

Relevant files:

- `demos/workloads.cpp`
- `demos/catalog.cpp`

### 4. `ld-nt-uop`

Observed in `all2.log` before retuning:

- `ld-nt-uop` did not separate from scalar read

Recent fix:

- NT read now uses explicit `ldnp` pair loads on arm64

What to verify next:

- does the event actually respond to `ldnp` on this core?
- if not, is the counter semantically narrower than "generic non-temporal load path"?

Relevant files:

- `demos/workloads.cpp`
- `demos/catalog.cpp`

### 5. `mmu-virtual-memory-fault`

Observed in `all2.log` before retuning:

- remained zero in both the first-touch and baseline cases

Recent fix:

- the workload now faults in pages from a `PROT_NONE` mapping using signal recovery and `mprotect`

What to verify next:

- does this event respond to true protection faults on this machine?
- if not, is the PMU event simply not exposed in a useful way for user-space page-instantiation faults?

Relevant files:

- `demos/workloads.cpp`
- `demos/catalog.cpp`

## Runner Policy Lessons

These are already supported by the code and are worth remembering:

- `--prefer-pcore` is better than `--prefer-cpu 8` for batch runs.
- `--require-stable-cpu` is useful and should stay.
- `require_active_pmu` is valuable, but only if the measurement set includes at least one counter that is expected to be alive even in the low case.
- compare-mode warnings should only fire when the contrast workload expects the opposite direction, not when both workloads intentionally expect "high."

Read these functions:

- `ApplyMeasuredRunDefaults(...)` in `main.cpp`
- `PrintExpectationWarnings(...)` in `main.cpp`
- `MismatchReason(...)` in `main.cpp`
- `ExecuteWorkload(...)` in `main.cpp`
- `SampleMatches(...)` in `demos/workloads.cpp`

## Recommended Review Order

If you want a crisp offline review loop:

1. Read the `main.cpp` runner path.
2. Read the weak workload implementations.
3. Read the matching entries in `demos/catalog.cpp`.
4. Compare each weak demo against its `all2.log` output.
5. Decide whether the demo should be:
   - kept as-is
   - retuned
   - downgraded with a stronger caveat
   - removed from the default narrative but kept experimental

## Open Questions Worth Answering

These are the best remaining questions in the repo:

- Is `interrupt-pending` inherently too OS-sensitive for a reliable teaching demo?
- Is `st-memory-order-violation` meaningful enough on this core to justify a first-class demo?
- Do `ldnp` / `stnp` actually map to the PMU's `LD_NT_UOP` / `ST_NT_UOP` definitions on M4 Max?
- Does `MMU_VIRTUAL_MEMORY_FAULT_NONSPEC` observe user-space protection-fault-driven page materialization at all?
- Should "experimental" be split into:
  - structurally sound but machine-specific
  - currently weak / not yet trusted

## Suggested Offline Decisions

Write down decisions for these now, even before rerunning:

- Which counters are strong enough to promote from experimental to stable?
- Which counters are better treated as "research probes" than teaching demos?
- Which demos need a second contrast workload?
- Which interpretation text should become more machine-specific?

## If You Want One Concrete Goal

Use this one:

"Decide which experimental counters should remain in the demo lab after one more rerun."

That forces you to read both the code and the results with a useful filter.
