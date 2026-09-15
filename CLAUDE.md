# Project Context for Claude Code

## What this project is

We are building a custom RPL **Objective Function (OF)** for **Contiki-NG**, as a Computer Networks (BACSE203) course project. The goal is to make IoT/wireless sensor network routing energy-aware, current default Objective Functions (OF0, MRHOF) ignore battery level entirely when picking a routing parent, which causes some nodes to get overused and die early.

**Team:** Navika Kapoor (25BCE0835), Ananya Thapliyal (25BCE0695)
**Guide:** Prof. Sarwesh P.

## Environment

- Running inside **WSL2 Ubuntu** on Windows
- Contiki-NG is cloned at `~/contiki-ng` (cloned with `--recursive`)
- Cooja simulator is at `~/contiki-ng/tools/cooja`, launched via `./gradlew run`
- This repo was seeded from `~/contiki-ng/examples/rpl-udp` as a starting point
- Build target for quick tests: `make TARGET=native`, binaries land in `build/native/`
- Full simulation testing happens inside Cooja using the UDGM radio model

## Baseline (already confirmed working)

The stock `rpl-udp` example builds and runs correctly in Cooja with the default RPL setup (RPL Lite, MRHOF as default OF). This has been manually verified, do not need to re-verify basic Contiki-NG/Cooja functionality, it works.

## What we are building

A new Objective Function file, **`of-energy.c`**, to sit alongside the existing `of0.c` and `mrhof.c` inside Contiki-NG's RPL module (`os/net/routing/rpl-classic/` or the RPL-Lite equivalent, check which one this project's example uses first, RPL Lite vs RPL Classic have different OF interfaces).

### Core formula (baseline, already agreed on)

```
Composite Score = w1 * ETX(norm) + w2 * (1 - Energy(norm)) + w3 * HopCount(norm)
```

- Lower score = better parent choice
- All three normalized terms are scaled 0 to 1 (min-max normalization)
- Weights sum to 1
- Starting weights: w1 = 0.4 (reliability/ETX), w2 = 0.4 (energy), w3 = 0.2 (hop count)
- Energy term is inverted `(1 - Energy_norm)` because higher remaining battery should produce a *lower* (better) score contribution

### Three novel mechanisms to layer on top of the baseline formula

This is the actual research contribution, the plain weighted-sum formula above already exists in prior published work (WSM-OF, EC-OF, EAOF, and others), so it is **not** the novelty. These three mechanisms, combined, are:

1. **Energy-gated filtering (hard exclusion stage)**
   Before scoring any candidate parent, exclude any neighbor whose residual energy falls below a dynamic threshold (e.g., bottom 20% among currently visible neighbors). Only candidates that pass this filter get scored by the weighted-sum formula. This is a two-stage design: filter first, then score, not just a soft weight.

2. **Depletion-rate tracking (not just a snapshot)**
   Instead of only looking at a neighbor's current residual energy, track a short rolling history of each neighbor's reported energy (received via DIO messages) and compute a rate of depletion over time. A node whose battery is dropping fast relative to its traffic load should be penalized even if its absolute battery level is still okay, this is predictive rather than reactive.

3. **Local adaptive weighting**
   Every K DIO intervals, each node locally adjusts its own w2 (the energy weight) up or down based on the energy variance it observes among its immediate neighbors, no global coordination, no centralized computation, just a simple local heuristic. This needs the DIO message format extended (Contiki-NG supports custom metric containers) to actually carry residual energy and depletion-rate info between neighbors.

### Baselines to compare against

- **OF0** (hop count only)
- **MRHOF** (ETX only, current default)
- Our OF with **only** energy-gated filtering (ablation)
- Our OF with **only** depletion-rate tracking (ablation)
- Our OF with **only** local adaptive weighting (ablation)
- Our OF with **all three combined** (the full contribution)

Ablation studies matter, we need to be able to claim which specific mechanism contributes what, not just "our combined thing is better."

## Evaluation plan

- Network sizes: **10, 30, and 50 nodes**, random topology, UDGM radio model in Cooja
- Metrics to collect for every run:
  - Network lifetime: **FND** (First Node Death), **HNA** (Half Nodes Alive / 50% death), **LND** (Last Node Death)
  - **PDR** (Packet Delivery Ratio): packets received at root / packets sent
  - **Latency**: average time from sensor send to root receive
  - **Energy per node**: both average AND worst-case (max) consumption across all nodes, this matters because the whole point is fixing uneven drain, not just average drain
- Energy numbers should come from Contiki-NG's built-in **Energest** module, not a custom estimate
- Weight-sensitivity sweep: also test a few alternate weight combinations (e.g., 0.5/0.3/0.2, 0.3/0.5/0.2, 0.33/0.33/0.34) at the 30-node size, to show how sensitive results are to weight choice

## Current status / what to do next

<!-- Update this section as work progresses, so Claude Code always knows where things actually stand -->

- [x] Confirm whether this Contiki-NG checkout uses RPL Classic or RPL Lite, and locate the correct OF interface files — confirmed RPL Lite (default `MAKE_ROUTING_RPL_LITE` in `Makefile.include`, no override in the example). OF interface is `struct rpl_of` in `os/net/routing/rpl-lite/rpl-types.h`; reference implementations are `rpl-mrhof.c`/`rpl-of0.c` in the same directory.
- [x] Build `of-energy.c` skeleton with the baseline static weighted-sum formula (no novel mechanisms yet) — done as `of-energy/rpl-energy-of.c` + `rpl-energy-of.h`, see `of-energy/README.md` for the integration steps (copy into Contiki-NG + one-line patch to `rpl-dag.c`). Builds clean with `make TARGET=native`. All three novel mechanisms are marked as TODOs at their hook points, not yet implemented.
- [x] Wire up Energest-based residual energy tracking per node — `rpl_energy_of_get_own_energy_percent()` now converts CPU/LPM/TX/LISTEN on-time into consumed mJ (assumed Tmote Sky-class current draw, overridable) against an assumed starting battery capacity, also overridable. Verified with a temporary debug log that values are real and increase over time (not stuck at 0). Needs `ENERGEST_CONF_ON 1` (added to `project-conf.h`, off by default on cooja/native). The battery-capacity assumption is a placeholder tuned to deplete visibly within a short Cooja run — revisit once we agree on actual evaluation run durations.
- [x] Get baseline weighted-sum OF running end-to-end in a small Cooja test (5 to 10 nodes) before scaling up — `rpl-udp/of-energy-6node-multihop.csc` (1 root + 5 clients, positions force node-6 into a multi-hop path). Ran headless (`gradlew run --no-gui --autostart`) for 180s simulated: all 5 clients joined the DODAG and exchanged request/response traffic with the root (node-6 got 10/10 with zero drops). Log saved at `results/baseline-of-energy-6node-2026-09-15/cooja-run.log`.
- [x] Implement energy-gated filtering — `compute_energy_gate_threshold()` + the exclusion check in `nbr_is_acceptable_parent()` in `of-energy/rpl-energy-of.c`. Excludes neighbors in the bottom 20% of currently visible neighbors' residual energy (disabled below 5 visible neighbors, since "bottom 20%" isn't meaningful with fewer). Sort/percentile logic verified correct with a standalone unit check (3 cases: below-minimum disables the gate, a clear low outlier gets excluded while the rest are kept, all-equal excludes nobody — all passed). Re-ran the 6-node Cooja test: no regression, though that topology never has ≥5 neighbors on any node so it doesn't actually exercise the gate live — worth a bigger topology (10+ nodes) later to see it fire in situ.
- [ ] Implement depletion-rate tracking (requires DIO extension first) — Ananya's, depends on her DIO work
- [ ] Extend DIO messages to carry energy + depletion-rate data — Ananya's
- [ ] Implement local adaptive weighting — Ananya's
- [ ] Integrate all three mechanisms into one working `of-energy.c`
- [ ] Set up automated Cooja log parsing into CSV (Python script) for repeatable data collection
- [ ] Run baseline (OF0, MRHOF) at 10/30/50 nodes
- [ ] Run full OF and each ablation at 10/30/50 nodes
- [ ] Run weight-sensitivity sweep at 30 nodes

## Working conventions

- Keep `of-energy.c` and any supporting files clearly commented, this needs to be explainable to a professor and eventually written up as a paper, code clarity matters as much as correctness
- Any new simulation `.csc` files should be named descriptively (e.g., `of-energy-30node-random.csc`), not left as generic defaults
- Log/CSV outputs should be saved into a `results/` folder, organized by experiment name and date, not overwritten between runs
- We are two people working on this (Navika: core OF logic, Ananya: simulation/DIO/adaptive weighting, see full split in the team's build plan doc), so keep commits scoped and named clearly enough that either of us can follow what changed and why
