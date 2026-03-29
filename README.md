# CPU Counter Probe

This project is a rough proof of concept for accessing Apple Silicon PMU counters from C++ on macOS and then using them to characterize specific code patterns.

Current host:

- Chip: `Apple M4 Max`
- `hw.cpufamily`: `0x17d5b93a`
- `hw.cachelinesize`: `128`
- `kpc_cpu_string()`: `cpu_100000c_2_17d5b93a`
- Local PMU database: `/usr/share/kpep/cpu_100000c_2_17d5b93a.plist -> as4-1.plist`

The code uses:

- `kperf.framework` for the private `kpc_*` control/read APIs
- `kperfdata.framework` for the private `kpep_*` event database/config helpers

Build:

```sh
make
```

Run:

```sh
sudo ./cpu_counter
```

Optional focused sampling controls:

```sh
sudo ./cpu_counter --prefer-cpu 14 --max-attempts 25 --require-stable-cpu
sudo ./cpu_counter --scan-cpus --max-attempts 25
sudo ./cpu_counter --prefer-pcore --require-stable-cpu --require-active-pmu --max-attempts 40
```

Useful flags:

- `--prefer-cpu N`: keep retrying each workload until it runs entirely on CPU `N`
- `--prefer-pcore`: best-effort performance-core mode
  - requests `QOS_CLASS_USER_INTERACTIVE` on the measuring thread
  - accepts only samples whose `cpu_before` and `cpu_after` fall in the heuristic performance-core range inferred from `hw.perflevel*`
- `--prefer-ecore`: best-effort efficiency-core mode using a lower-QoS bias
- `--max-attempts N`: cap retries per workload
- `--require-stable-cpu`: reject samples that migrate during the measurement window
- `--require-active-pmu`: reject samples whose configurable counters are all zero
- `--allow-migration`: accept migrated samples again
- `--scan-cpus`: run a compact per-CPU sweep with representative workloads to see which logical CPUs produce live counts

The current binary is a cache/TLB/I-side expansion suite. It reprograms the configurable PMCs at runtime and currently focuses on the remaining memory-side nonspec counters plus instruction-side cache/TLB counters.

- scalar streaming reads
- random pointer chasing
- scalar streaming writes
- random page writes
- hot-code loop for low instruction-side pressure
- sequential executable-page sweep
- randomized executable-page sweep

The suite is intentionally split into multiple passes because Apple only exposes a small number of configurable PMCs at once, and some event combinations interact badly. The current focused groups are:

- extra L1D cache counters:
  - `L1D_CACHE_MISS_LD`
  - `L1D_CACHE_MISS_LD_NONSPEC`
  - `L1D_CACHE_MISS_ST`
  - `L1D_CACHE_MISS_ST_NONSPEC`
- extra DTLB counters:
  - `L1D_TLB_ACCESS`
  - `L1D_TLB_FILL`
  - `L1D_TLB_MISS`
  - `L1D_TLB_MISS_NONSPEC`
  - `MMU_TABLE_WALK_DATA`
- instruction-side counters:
  - `L1I_CACHE_MISS_DEMAND`
  - `L1I_TLB_FILL`
  - `L1I_TLB_MISS_DEMAND`
  - `L2_TLB_MISS_INSTRUCTION`
  - `MMU_TABLE_WALK_INSTRUCTION`

Notes:

- Counter programming still requires privileged access on this machine.
- This relies on private Apple interfaces, not public SDK APIs.
- The code stays intentionally direct and single-file so it is easy to keep iterating during reverse engineering.
- There is no clean public macOS API here for exact logical-CPU pinning. The new `--prefer-pcore` and `--prefer-ecore` modes are best-effort:
  - they bias scheduling through thread QoS
  - they filter accepted samples by the heuristic logical-CPU ranges derived from `hw.perflevel*`
- Instruction-side workloads use generated executable stubs in an RX mapping. A standalone local smoke test confirmed that the `mmap` + `mprotect(PROT_EXEC)` + `sys_icache_invalidate()` path works on this host.
- If a requested event group cannot be programmed together on this machine, the tool prints that group as skipped and continues with the rest of the suite.
- `LDST_X64_UOP` is a 64-byte split-access counter. On this M4 Max, `hw.cachelinesize` is `128`, so the X64 workloads are about 64-byte boundaries, not full L1 cache lines.
- Each benchmark sample now records the current CPU before and after the workload so scheduler migration can be correlated with suspicious all-zero measurements.
- Several debug-only workload variants use `optnone` so the optimizer can be ruled out without dropping the whole binary to `-O0`.
- The current startup log prints the inferred perflevel layout, for example:
  - `hw.perflevel0.name: Performance logicalcpu=12 heuristic_cpu_range=0-11`
  - `hw.perflevel1.name: Efficiency logicalcpu=4 heuristic_cpu_range=12-15`
