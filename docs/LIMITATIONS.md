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

## Counters that are dead or unreachable from userspace on M4 (and why)

Each of these has a *correct* trigger in the catalog and the event exists in this
machine's kpep database (`as4-1`), yet it reads zero or noise. The reasons come
from the reverse-engineered event descriptions (see links at the end) plus
measured runs on the reference M4:

- **`interrupt-pending` (`interrupt-storm`)** — the event is literally *"cycles
  while an interrupt was pending **because it was masked**."* User code (EL0)
  cannot mask interrupts, so there is never a masked-pending window to count.
  Plain software signals (delivered as ASTs on return-to-userspace) read ~24, and
  a cross-core TLB-shootdown IPI storm read **0** — both confirmed empirically. The
  demo therefore teaches asynchronous-preemption cost through **cycles** (a real
  ~5x blowup vs. the same loop uninterrupted) and keeps `interrupt-pending` only
  as a documented zero probe.
- **`st-memory-order-violation` (`store-order-alias` / `store-order-friendly`)** —
  the event counts *"retired store uops that triggered memory order violations
  with load uops,"* i.e. an architectural memory-consistency squash, which is
  genuinely rare in single-threaded code. The redesigned pair uses unpredictable,
  data-dependent store/load aliasing to provoke load/store-unit replays; that
  penalty is **real and shows up as ~1.6x cycles** over an otherwise identical
  instruction stream, but the replays are forwarding/disambiguation stalls rather
  than architectural violations, so the dedicated PMC stays at zero. Cycles are the
  signal; the event is kept as a documented zero probe.
- **`mmu-virtual-memory-fault` (`first-touch-fault`)** — described as *"memory
  accesses that reached retirement that triggered MMU virtual-memory faults."*
  Demand-paging first touches are resolved by the kernel VM layer and do not
  register as retirement-time MMU faults on M4 P-cores. The first-touch demo's
  real, large signals are `dtlb-miss` and `mmu-table-walk-data` (which separate by
  tens of thousands of x), so the fault event is not part of its claimed
  expectations.
- **`st-nt-uop` (`nt-stream-write`)** — *not dead, but non-discriminating.* Unlike
  its load-side twin `ld-nt-uop` (which is `ldnp`-specific and separates ~340,000x),
  `ST_NT_UOP` increments roughly once per scalar store **regardless of the
  non-temporal hint**: a `stnp` stream and a plain-store loop both read ≈ their
  `INST_INT_ST` count (~33M vs ~31M, a 1.07x "separation"). On M4 P-cores it tracks
  store-unit uops broadly, so it cannot isolate the non-temporal store path and is
  effectively redundant with `inst-int-st`. The demo is kept as a probe.

## Counters that initially looked weak but are validated on M4

Earlier drafts flagged these as uncertain; fresh measured runs prove they work,
and the reverse-engineered descriptions explain why:

- **`ld-nt-uop` (`nt-stream-read`)** — *"load uops that executed with non-temporal
  hint."* Explicit `ldnp` pair loads carry the hint, so it separates ~340,000x
  against the temporal baseline. (An earlier `__builtin_nontemporal_load` version
  did **not** lower to `ldnp` and read ~80 — the inline-asm rewrite is what fixed
  it.) Its store-side twin `st-nt-uop` does **not** behave the same way — see below.
- **`inst-barrier` (`barrier-loop`)** — *"retired **data** barrier instructions."*
  `dmb ish` is a data barrier, so the count lands exactly on the 20,000,000 loop
  trips, infinitely separated from the barrier-free baseline.
- **`flush-restart-other` (`frontend-self-modifying-restart`)** — *"pipeline flush
  and restarts **not** due to branch mispredictions or memory order violations."*
  Rewriting live code forces true pipeline flushes (~200k counts, ~27,000x
  separation). Plain code-page churn only triggers *fetch* restarts, which is why
  `frontend-random-restart` headlines `fetch-restart` instead of this event.

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

## Resolved questions

Previously open, now answered on the reference M4 — from measured runs combined
with the reverse-engineered event table:

- **Does `interrupt-pending` move from userspace?** No. It counts only cycles
  where an interrupt is pending *because masked*, and EL0 cannot mask interrupts.
  Neither signal storms nor TLB-shootdown IPIs reach it.
- **Is `st-memory-order-violation` reachable single-threaded?** Not as the
  dedicated event. Unpredictable store/load aliasing does cost ~1.6x cycles, but
  that is a forwarding/replay stall, not the architectural ordering violation the
  PMC counts.
- **Do `ldnp` / `stnp` map to `LD_NT_UOP` / `ST_NT_UOP`?** Yes — they carry the
  non-temporal hint and the events count them (~400,000x separation).
- **Does `MMU_VIRTUAL_MEMORY_FAULT_NONSPEC` observe userspace demand faults?** No.
  First-touch faulting drives `dtlb-miss` and `mmu-table-walk-data` instead.

Event descriptions were cross-referenced against the community reverse-engineering
at <https://github.com/jiegec/apple-pmu> (its `as4` table matches this machine's
`as4-1` database) and <https://github.com/dougallj/applecpu>. The per-event "which
ELx modes count" gating lives in `PMCR1_EL1`; the demo lab measures EL0 (user)
activity, which is why the masked-interrupt event is structurally out of reach.
