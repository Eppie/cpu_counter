# CPU Counter Probe

This project is a rough proof of concept for accessing Apple Silicon PMU counters from C++ on macOS and then using them to characterize specific code patterns.

Current host:

- Chip: `Apple M4 Max`
- `hw.cpufamily`: `0x17d5b93a`
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

The current binary is a small memory-counter suite. It reprograms the configurable PMCs at runtime, fixes the raw-slot versus packed-counter indexing issue in the readout path, and runs several benchmark patterns, including:

- hot sequential reads
- scalar streaming reads
- explicit NEON streaming reads
- random pointer chasing
- page-stride reads
- hot sequential writes
- scalar streaming writes
- explicit NEON streaming writes
- random page writes
- aligned vs cache-line-crossing loads/stores
- aligned vs page-crossing loads/stores

The suite is intentionally split into multiple passes because Apple only exposes a small number of configurable PMCs at once, and some event combinations interact badly. Current groups focus on:

- primary memory-boundness counters:
  - `INST_LDST`
  - `L1D_CACHE_MISS_LD`
  - `L1D_CACHE_MISS_ST`
  - `L1D_TLB_MISS`
  - `MMU_TABLE_WALK_DATA`
- deeper translation counters:
  - `L1D_TLB_FILL`
  - `L2_TLB_MISS_DATA`
- scalar vs SIMD mixes:
  - `INST_INT_LD`
  - `INST_INT_ST`
  - `INST_SIMD_LD`
  - `INST_SIMD_ST`
- boundary-crossing diagnostics:
  - `LDST_X64_UOP`
  - `LDST_XPG_UOP`
- store-path diagnostics:
  - `L1D_CACHE_WRITEBACK`

Notes:

- Counter programming still requires privileged access on this machine.
- This relies on private Apple interfaces, not public SDK APIs.
- The code stays intentionally direct and single-file so it is easy to keep iterating during reverse engineering.
- If a requested event group cannot be programmed together on this machine, the tool prints that group as skipped and continues with the rest of the suite.
