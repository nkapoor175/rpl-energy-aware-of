# of-energy: installing the objective function into Contiki-NG

`rpl-energy-of.c` / `rpl-energy-of.h` implement the energy-aware RPL Lite
objective function. They do not live inside `~/contiki-ng` (a separate clone,
not tracked here), so they are installed into it. **Use the installer:**

```
bash experiments/install.sh [CONTIKI_DIR]      # default ~/contiki-ng; safe to run again
```

It does exactly the following (do it by hand only if you must):

1. Copies `of-energy/rpl-energy-of.{c,h}` into
   `os/net/routing/rpl-lite/` (RPL Lite compiles every `.c` there).
2. Copies `battery-sim/battery-sim.{c,h}` into `os/sys/` (the
   objective-function-independent battery model; the apps call
   `battery_sim_start()`, the server passes `true` = mains-powered sink).
3. Applies `patches/energy-of-core.patch` (`git apply`) to two Contiki-NG core
   files. This is the ONLY change to Contiki-NG core, and the single source of
   truth for it (there are no copies of core files in this repository):
   * `rpl-dag.c`: includes `rpl-energy-of.h`, adds `rpl_energy_of` to the
     `extern rpl_of_t rpl_of0, rpl_mrhof;` line (the same pattern Contiki-NG
     uses for OF0/MRHOF) and calls `rpl_energy_of_update_neighbor_energy()`
     after a DIO has stored the neighbour's metric container (only when
     `RPL_ENERGY_OF_CONF_ENABLE_DEPLETION` is set and our OF is in use).
   * `rpl-types.h`: adds the per-neighbour energy history (8 timestamped
     samples) and the depletion-rate fields to `struct rpl_nbr`.
   If the patch is already applied the installer says so and carries on.
4. Copies `rpl-udp/{Makefile,project-conf.h,udp-client.c,udp-server.c}` into
   `examples/rpl-udp/`.

If you change the core files, regenerate the patch from a pristine
Contiki-NG (`git diff -- os/net/routing/rpl-lite/rpl-dag.c
os/net/routing/rpl-lite/rpl-types.h > patches/energy-of-core.patch`), keep it
LF-terminated, and check that `git apply --check` passes on a clean tree.

## Choosing the objective function and the mechanisms

The same experiment builds with any of the three functions (same topology,
traffic and battery):

```
rm -rf build && make TARGET=cooja EXPERIMENT_OF=0   # our energy OF (default)
rm -rf build && make TARGET=cooja EXPERIMENT_OF=1   # MRHOF
rm -rf build && make TARGET=cooja EXPERIMENT_OF=2   # OF0
```

`experiments/run_lifetime.sh` does the build itself and checks that the
firmware really contains the function it was asked for (Cooja's own internal
make does not see `EXPERIMENT_OF`). Each mechanism of our OF is a flag in
`project-conf.h` (all `RPL_ENERGY_OF_CONF_*`):

| Flag | Default | Meaning |
|---|---|---|
| `ENABLE_GATING` | 1 | hard-exclude the lowest-energy 20% of neighbours (needs >= 5 neighbours) |
| `ENABLE_DEPLETION` | 0 | penalise neighbours that drain faster than the reference rate |
| `ENABLE_ADAPTIVE_WEIGHTING` | 0 | adapt the energy weight to the energy spread of the neighbourhood |
| `DEPLETION_WINDOW_MINUTES` | 60 | window of the depletion estimate (30 for the sensitivity run) |
| `DEPLETION_SAMPLE_SPACING_MINUTES` | 10 | minimum spacing between stored samples |

The energy model (duty cycle D, capacity) is set in `project-conf.h`
(`BATTERY_SIM_CONF_DUTY_CYCLE`, `BATTERY_SIM_CONF_CAPACITY_MJ`) and is
identical for every OF; see CLAUDE.md for the frozen values.

**Why `project-conf.h` does not `#include "rpl-energy-of.h"`:** it is pulled in
very early in Contiki-NG's build (before `stdint.h`/`process.h` are set up),
and including `rpl.h`'s transitive headers that early breaks the build
(missing `uint8_t`, undeclared `PROCESS_NONE`, macro redefinitions). Keep it
to plain `#define`s; the symbol is declared in `rpl-dag.c` by the patch.

## Reading the logs

* `experiments/parse_run.py RUN_DIR` (or `--all RESULTS_ROOT`): per-run metrics
  (lifetimes, delivery ratios, latency, energy balance, parent switches).
* `experiments/parse_energy_log.py LOG`: the `[DEPLETION]`, `[ADAPTIVE]`,
  `ENERGY` and parent-switch records as CSV (needs RPL logging at DBG/INFO).
* `experiments/analyze_energy.py ENERGY_CSV`: per-node drain table and plot.
