# Energy-aware RPL objective function for Contiki-NG

BACSE203 Computer Networks course project.
Team: Navika Kapoor (25BCE0835), Ananya Thapliyal (25BCE0695). Guide: Prof. Sarwesh P.

Default RPL objective functions (OF0: hop count, MRHOF: ETX) ignore battery level,
so some nodes are overused and die early. This project builds an energy-aware
objective function (OF) for Contiki-NG's RPL Lite and compares it with OF0 and
MRHOF on network lifetime, delivery ratio, delay and per-node energy.

The OF scores each candidate parent with a weighted sum of normalized ETX,
residual energy and hop count, and layers three mechanisms on top: energy-gated
filtering, depletion-rate tracking and local adaptive weighting. See
[CLAUDE.md](CLAUDE.md) for the full design, the decisions taken, and the status
of every checklist item (it is the living project log).

## Status

| Part | State |
|---|---|
| Baseline weighted-sum OF | done, validated in Cooja |
| Energy-gated filtering | done; fires as designed, effect on results not yet measured |
| Depletion-rate tracking, adaptive weighting | in progress (Ananya) |
| OF-independent battery + node death, duty-cycle energy model | done, frozen for all three OFs |
| Lifetime experiment tooling (scenarios, runner, batch runner) | done |
| Evaluation runs (10/30/50 nodes x 5 seeds x 3 OFs) | not started; waiting on the validation steps in CLAUDE.md |

No performance result is claimed anywhere in this repository yet.

## Layout

| Path | What it is |
|---|---|
| `of-energy/` | the energy-aware OF (`rpl-energy-of.{c,h}`), ablation toggles, install notes |
| `battery-sim/` | simulated battery and node death, independent of the OF so OF0, MRHOF and our OF run under the identical model |
| `rpl-udp/` | the example app (UDP client/server), `project-conf.h` with the frozen settings, small test scenarios |
| `experiments/` | `install.sh`, `make_csc.py` (scenario generator), `run_lifetime.sh` (one run), `run_batch.sh` (a matrix of runs) |
| `results/` | scenarios and summaries of runs; raw Cooja logs are git-ignored (tens of MB each) |
| `CLAUDE.md` | design, decisions, open questions, status log |

## Quick start (Linux / WSL2 with a Contiki-NG checkout at `~/contiki-ng`)

```bash
bash experiments/install.sh                       # copies everything into Contiki-NG (safe to rerun)

# one lifetime run: NODES SEED OF  (OF: 0 = our energy OF, 1 = MRHOF, 2 = OF0)
bash experiments/run_lifetime.sh 30 1 1

# a matrix, 3 runs at a time
bash experiments/run_batch.sh "30 50" "1" "0 1 2" 3 90000 scale-check
```

A run goes until the last client dies or the cutoff and prints one line:

```
LIFETIME nodes=30 clients=29 dead=29 cutoff=0 FND_s=... HND_s=... LND_s=...
```

FND, HND and LND are the first, half (the ceil(clients/2)-th) and last client death
in simulated seconds; the root is mains-powered and never dies. The same
`NODES` and `SEED` always produce the identical topology and traffic, whichever
OF is run, and every run verifies that the firmware really contains the OF it was
asked for.

## Energy model (identical for every OF)

Radio and CPU follow one duty-cycle assumption `D` (primary 1%, sensitivity 0.5%
and 2%); transmit time is measured and not scaled; capacity is a documented
simulation parameter, not a real battery. The formula and the reasons are in
[`battery-sim/battery-sim.c`](battery-sim/battery-sim.c) and CLAUDE.md. Per-run
overrides (`CAPACITY_MJ`, `DUTY_CYCLE`, `SPREAD_PCT`, `AVG_DEGREE`) are
documented in `experiments/run_lifetime.sh`.
