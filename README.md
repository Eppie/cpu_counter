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

The current binary is an issue-focused memory-counter suite. It reprograms the configurable PMCs at runtime, fixes the raw-slot versus packed-counter indexing issue in the readout path, and temporarily disables the known-good passes so the output stays concentrated on the remaining inconsistencies.

- scalar streaming reads
- alternate scalar streaming reads compiled `optnone`
- explicit NEON streaming reads
- page-stride reads
- alternate page-stride reads compiled `optnone`
- scalar streaming writes
- explicit NEON streaming writes
- random page writes
- alternate random page writes compiled `optnone`
- aligned vs 64-byte-split loads/stores
- aligned vs page-crossing loads/stores
- alternate cross-page stores compiled `optnone`

The suite is intentionally split into multiple passes because Apple only exposes a small number of configurable PMCs at once, and some event combinations interact badly. The current focused groups are:

- translation with and without `INST_LDST`, to check whether `INST_LDST` is what poisons page-stride and random-page-write samples
- scalar stream debug:
  - `INST_INT_LD`
  - `LD_UNIT_UOP`
  - `L1D_CACHE_MISS_LD`
  - `L1D_TLB_MISS`
  - `MMU_TABLE_WALK_DATA`
- SIMD stream debug:
  - `INST_SIMD_LD`
  - `LD_UNIT_UOP`
  - `L1D_CACHE_MISS_LD`
- deeper translation counters:
  - `L1D_TLB_FILL`
  - `L2_TLB_MISS_DATA`
- separate scalar and SIMD store passes:
  - scalar stores avoid the `INST_LDST` + `INST_INT_ST` conflict
  - SIMD stores are measured in their own pass
- split-boundary diagnostics:
  - `LDST_X64_UOP`
  - `LDST_XPG_UOP`
- store-path diagnostics:
  - `L1D_CACHE_WRITEBACK`

Notes:

- Counter programming still requires privileged access on this machine.
- This relies on private Apple interfaces, not public SDK APIs.
- The code stays intentionally direct and single-file so it is easy to keep iterating during reverse engineering.
- There is no clean public macOS API here for exact logical-CPU pinning. The new `--prefer-pcore` and `--prefer-ecore` modes are best-effort:
  - they bias scheduling through thread QoS
  - they filter accepted samples by the heuristic logical-CPU ranges derived from `hw.perflevel*`
- If a requested event group cannot be programmed together on this machine, the tool prints that group as skipped and continues with the rest of the suite.
- `LDST_X64_UOP` is a 64-byte split-access counter. On this M4 Max, `hw.cachelinesize` is `128`, so the X64 workloads are about 64-byte boundaries, not full L1 cache lines.
- Each benchmark sample now records the current CPU before and after the workload so scheduler migration can be correlated with suspicious all-zero measurements.
- Several debug-only workload variants use `optnone` so the optimizer can be ruled out without dropping the whole binary to `-O0`.
- The current startup log prints the inferred perflevel layout, for example:
  - `hw.perflevel0.name: Performance logicalcpu=12 heuristic_cpu_range=0-11`
  - `hw.perflevel1.name: Efficiency logicalcpu=4 heuristic_cpu_range=12-15`
