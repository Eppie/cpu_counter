# Limitations and hardware notes

`perf.h` itself is deliberately small and portable across Apple Silicon
generations. Most of the caveats below are about the **demo lab**: which
counters teach cleanly, and which depend on the specific chip, the OS, or
scheduling luck.

## Privilege and best-effort programming

- Programming counters requires `root` (or a blessed pid). Without it the binary
  still runs; measured commands fail with a clear error.
- Some machines reject `kpc_force_all_ctrs_set(1)` even under `sudo`. The library
  treats that as best-effort and reports the later programming failure directly
  if configurable PMCs still cannot be installed.

## Counter limits and conflicts

The Apple Silicon PMU has a small, fixed set of counters and some non-obvious
rules about which events can run together. `perf.h` handles these for you; this
section documents what happens underneath (verified on the reference M4).

- **How many at once.** Two *fixed* counters (cycles, instructions) plus eight
  *configurable* counters on the M4's performance cores — the count is queried at
  runtime (`kpc_get_counter_count`), not hardcoded. The fixed counters are
  "free": they use dedicated slots and never cost a configurable slot. A single
  `CounterSet` may name at most `PERF_MAX_SCOPE_EVENTS` (10) counters.
- **Slot masks → conflicts.** Each configurable event may only occupy a subset of
  the physical counter slots (a per-event bitmask). Events whose masks can't be
  packed into distinct slots cannot be programmed together — for example, the
  four events that each require a single specific slot are mutually exclusive.
  `perf.h` surfaces this as a *"conflicting events"* error that tells you to split
  the set across separate measurements.
- **Order independence (handled for you).** The hardware's slot allocator is
  greedy and order-sensitive: the *same* set can succeed or fail depending on the
  order events are added. `perf.h` reorders internally (most-constrained events
  first), so any programmable set programs regardless of how you list its
  counters.
- **Configurable-only sets (handled for you).** The underlying API numbers slots
  differently when no fixed counter is present, which would misread a set like
  `PERF_SCOPE("x", L1_LOAD_MISS)`. `perf.h` transparently presents the (free)
  fixed counters to the allocator so configurable-only sets read correctly.

Net: build any `CounterSet` of up to 10 counters in any order — it either just
works, or fails with a message naming the conflicting counters and what to do.

Note that the *specific* per-event slot masks differ between chip generations
(e.g. the M2 Pro and M4 group events differently), so the exact set of mutually
exclusive events is chip-specific; the model above is not.

## Hardware coverage

The library auto-selects the PMU event database for the current chip, so the
public counter constants are not tied to one generation. However:

- The **demos were tuned and validated on an M4 Max**, with interpretation
  oriented toward its performance (P) cores.
- On other Apple Silicon chips (M1–M4 and their Pro/Max/Ultra variants) some
  experimental event names may differ, and some counters may separate less
  cleanly between their high and low demos.
- Counter behavior differs across logical CPUs and perflevels, which is why the
  runner defaults to best-effort P-core placement and why `validate` forces
  `--require-stable-cpu` and `--require-active-pmu`.

If you run on a different chip and a demo doesn't separate, that is expected for
the experimental tier — please open an issue with the chip and the output rather
than assuming the counter is broken.

## Stability tiers

- **Stable** — curated, validated showcase pairs that teach the counter clearly.
  These are the ones to trust as examples.
- **Experimental** — visible and runnable, but not yet trusted as production-ready
  teaching examples. Some are structurally sound but machine-specific; others are
  genuinely weak on current hardware.

## Counters that are weak or hardware-sensitive

These are the experimental cases most likely to disappoint on a given machine,
and why. They remain in the catalog as research probes.

- **`inst-barrier` (`barrier-loop`)** — separation depends on the barrier
  instruction mix being counted distinctly from surrounding work. The demo adds
  an `INST_ALL` companion so the low baseline still proves the PMU is live.
- **`interrupt-pending` (`interrupt-storm`)** — driven by helper-thread signal
  delivery, which is OS-sensitive. Whether it moves enough to be a reliable
  teaching case varies by system load.
- **`st-memory-order-violation` (`store-order-friendly` / `store-order-alias`)**
  — a 4 KiB-alias store/load ordering stress pair. Whether this event fires at
  all is highly core-specific.
- **`ld-nt-uop` (`nt-stream-read`)** — uses explicit `ldnp` pair loads on Arm64.
  Whether the event responds to `ldnp` depends on how narrowly the core defines
  the non-temporal load path.
- **`mmu-virtual-memory-fault` (`first-touch-fault`)** — faults pages in from a
  fresh mapping using signal recovery and `mprotect`. Whether the event observes
  user-space protection-fault-driven page materialization is uncertain on some
  cores.

## Runner policy notes

These defaults were chosen from batch-run experience and are worth keeping in
mind when interpreting results:

- `--prefer-pcore` (P-core *range*) is more robust for batch runs than pinning to
  one logical CPU with `--prefer-cpu N`.
- `--require-stable-cpu` rejects samples that migrated across cores mid-measurement.
- `--require-active-pmu` is valuable only when the measurement set includes at
  least one counter expected to be live even in the low case — hence the liveness
  companion counters in several demos.
- Compare-mode warnings fire only when the contrast workload expects the opposite
  direction, not when both workloads intentionally expect "high".

## Open questions

Genuinely unresolved on the reference hardware:

- Is `interrupt-pending` inherently too OS-sensitive for a reliable demo?
- Is `st-memory-order-violation` meaningful enough on current cores to deserve a
  first-class demo?
- Do `ldnp` / `stnp` map to the PMU's `LD_NT_UOP` / `ST_NT_UOP` definitions?
- Does `MMU_VIRTUAL_MEMORY_FAULT_NONSPEC` observe user-space protection-fault
  page materialization at all?
