# CPU Counter Probe

This is a rough proof of concept for accessing Apple Silicon PMU counters from C++ on macOS.

Current findings on this machine:

- Chip: `Apple M4 Max`
- `hw.cpufamily`: `0x17d5b93a`
- `kpc_cpu_string()`: `cpu_100000c_2_17d5b93a`
- Local PMU database: `/usr/share/kpep/cpu_100000c_2_17d5b93a.plist -> as4-1.plist`

The probe uses:

- `kperf.framework` for the private `kpc_*` control/read APIs
- `kperfdata.framework` for the private `kpep_*` event database/config helpers

Build:

```sh
make
```

Run:

```sh
./cpu_counter
```

At the moment, the unprivileged run reaches the private framework successfully but fails at:

```text
kpc_force_all_ctrs_set(1)
```

That strongly suggests real counter programming on this machine requires elevated privileges. The next intended run is:

```sh
sudo ./cpu_counter
```

What the program currently does:

1. Resolves the current Apple PMU database from the local `/usr/share/kpep` files.
2. Dynamically loads `kperf.framework` and `kperfdata.framework`.
3. Configures these events:
   - `FIXED_CYCLES`
   - `FIXED_INSTRUCTIONS`
   - `INST_BRANCH`
   - `BRANCH_MISPRED_NONSPEC`
4. Tries to enable counting for the current thread and report counter deltas around a synthetic workload.

Notes:

- This relies on private Apple interfaces, not public SDK APIs.
- The current code is intentionally single-file and direct so it is easy to iterate on during reverse engineering.
