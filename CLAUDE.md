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
   Every K DIO intervals, each node locally adjusts its own w2 (the energy weight) up or down based on the energy variance it observes among its immediate neighbors, no global coordination, no centralized computation, just a simple local heuristic. The implementation reuses the standard RPL energy metric container (`energy_est`), so no new DIO field is needed.

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

## Current status (2026-10-26)

Full dated history, numbers and review rounds: `docs/decision-log.md`. This section is the
compact current state. **No comparative performance result has been produced yet; do not
claim any benefit of the proposed OF until the main matrix has been run and analysed.**

### Done (all verified; see the log for the evidence)
- RPL Lite confirmed; the OF lives in `of-energy/rpl-energy-of.{c,h}`. The ONLY change to
  Contiki-NG core is `patches/energy-of-core.patch` (OF registration, the DIO hook, and the
  per-neighbour energy history in `struct rpl_nbr`). `bash experiments/install.sh` installs
  everything and is safe to repeat. There are no copies of core files in this repo.
- The three mechanisms exist, each behind a flag: energy-gated filtering (bottom 20% of the
  neighbours' energy, needs >= 5 neighbours), depletion-rate tracking (60 min window, samples
  at least 10 min apart, 8 samples, penalty added after the score division, capped at 180 < the
  rank hysteresis of 192) and local adaptive weighting. It reuses the standard RPL energy
  metric container `energy_est` (8 bit): it is NOT a new DIO field, do not call it a "DIO
  extension" in the paper.
- `battery-sim/` is independent of the OF, so OF0, MRHOF and our OF run under the identical
  battery and death rule (`NODE_DEAD` line, radio off, all processes exited). The root is a
  mains-powered sink and never dies.
- Experiment tooling in `experiments/`: `make_csc.py` (seeded random connected topology),
  `run_lifetime.sh` (one run, parallel-safe, builds the firmware itself and checks which OF it
  contains), `run_batch.sh` (resumable matrix), `run_all.sh` (the stages below, writes
  `FROZEN.txt` first), `parse_run.py` (per-run metrics), `parse_energy_log.py` and
  `analyze_energy.py` (debug records and drain plots).

### Frozen settings (identical for every variant; change nothing here without rerunning everything)
| Setting | Value |
|---|---|
| Energy model | duty-cycle assumption D = 0.01 (sensitivity 0.005 and 0.02); `I_cpu_eff = 1.8 D + 0.0545 (1 - D)` mA, `I_listen_eff = 20 D` mA, `I_tx = 17.7` mA on measured TX time (not scaled), V = 3 V, no separate RX cost |
| Capacity | 60000 mJ per node, a simulation parameter (not a real battery); PROVISIONAL, sensitivity 0.5x / 2x |
| Batteries | identical in all main results; the 30% spread is ONLY the recovery experiment |
| Topologies | 10 / 30 / 50 nodes, seeds 1-5, seeded random connected, root at the centre, nominal density 10 (`make_csc.py` default), UDGM 50 m / 100 m, links ideal |
| Traffic | every client sends a UDP request every 10 s, the root answers |
| Run length | until the last client dies or 90000 s (safety cutoff only, never a design parameter) |
| Lifetime | FND = first client death, HND = ceil(clients/2)-th, LND = last; root excluded |
| Weights | 0.4 / 0.4 / 0.2 (ETX / energy / hop), per-mille in code |
| Gate | bottom 20% of neighbour energy, minimum 5 neighbours |
| Depletion | window 60 min (30 min sensitivity), spacing 10 min, 8 samples, reference rate 10 milli-units/s, max penalty 180 |
| Adaptive | every 4 updates, step 50 per-mille on w2 (w1 -40, w3 -10 / +40, +10), w2 in [300, 600], spread thresholds 100 (high) / 40 (low) per-mille (lowered from 250 / 100 in `e779e35` without being mentioned in its message; with identical batteries the observed spread is 0-15 per-mille, so it stays in the low branch either way) |

### Variants (`run_lifetime.sh NODES SEED VARIANT`)
`of0`, `mrhof` (baselines); `base` (our weighted sum, no mechanism); `gating`, `depletion`,
`adaptive` (single-mechanism ablations, each WITHOUT the others); `full` (all three);
`fixedw` (base with FIXED weights 0.48/0.30/0.22: the control that separates "different
weights" from "adapting"). `WEIGHTS=w1/w2/w3`, `DUTY_CYCLE`, `CAPACITY_MJ`, `SPREAD_PCT`,
`AVG_DEGREE` override a run (recorded in `summary.txt`; the exact compile-time settings are
saved as `project-conf.used.h`).

### Stages (`experiments/run_all.sh STAGE [JOBS]`)
`scale` (six runs: 30 and 50 nodes, seed 1, of0 / mrhof / full: check runtime, logs, deaths and
parser before anything else) -> `main` (10/30/50 x seeds 1-5 x 8 variants) -> `weights`
(weight-sensitivity sweep of `full`, 30 nodes) -> `duty`, `capacity` (sensitivity, of0 / mrhof
/ full) -> `spread` (recovery experiment) -> `parse`. Finished runs are skipped on restart.

### Findings so far (measurements about the model, NOT results about the OF)
- With an always-on radio the drain is time-driven and all three OFs die at identical times;
  that is why the duty-cycle model exists. Cooja counts the CPU as always on, so the duty-cycle
  assumption also covers the CPU.
- Under identical batteries all neighbours drain at 3.5-4.8 milli-units/s, so the depletion
  penalty (60-84) is almost a common offset (a relay and a leaf differ by ~20, the hysteresis
  is 192) and depletion changes no parent choice: expect no effect except in the 30% spread
  experiment. Penalising only the excess over the neighbour-median rate would make it
  meaningful (not implemented).
- With identical batteries the adaptive weights sit at 0.48 / 0.30 / 0.22 (energy spread is
  ~0), so `adaptive` vs `base` mostly measures a weight change; `fixedw` is the control.
- A 15-min depletion window cannot resolve the relay-vs-leaf difference (1.1 vs 1.2
  milli-units/s of resolution vs signal); 60 min can (99% correct on clearly different pairs).
- A 10-node random field is nearly a star (1.0-1.3 hops), so parent choice is trivial there.
- Lessons that cost time: Cooja's own `make` ignores `EXPERIMENT_OF` (the run script builds
  first and verifies the firmware); `project-conf.h` must stay plain `#define`s; radio-off alone
  does not silence a Cooja node.

### Open items
- [ ] PR #1 (mechanisms + repository fixes) is mergeable; Navika merges it.
- [~] Capacity 60000 mJ is provisional: freeze it after the `capacity` sensitivity stage.
- [ ] Run the stages above, parse, analyse (means and spread over 5 seeds, not only means),
      write up. Report honestly if the OF only wins under one setting.
- [ ] Case-study document (`docs/Case-Study-Energy-Aware-RPL-OF.docx`) must be updated with the results.

## Working conventions

- Keep `of-energy.c` and any supporting files clearly commented, this needs to be explainable to a professor and eventually written up as a paper, code clarity matters as much as correctness
- Any new simulation `.csc` files should be named descriptively (e.g., `of-energy-30node-random.csc`), not left as generic defaults
- Log/CSV outputs should be saved into a `results/` folder, organized by experiment name and date, not overwritten between runs
- We are two people working on this (Navika: core OF logic, Ananya: simulation/DIO/adaptive weighting, see full split in the team's build plan doc), so keep commits scoped and named clearly enough that either of us can follow what changed and why
