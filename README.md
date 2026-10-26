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
| The three mechanisms (gating, depletion tracking, adaptive weighting) | implemented, each behind a flag; build-tested in every configuration |
| OF-independent battery + node death, duty-cycle energy model | done, frozen for all variants |
| Experiment tooling (scenarios, runners, parsers, stage runner) | done |
| Evaluation runs (10/30/50 nodes x 5 seeds x 8 variants, plus sensitivity runs) | running; see `results/` and `docs/decision-log.md` |

No performance result is claimed anywhere in this repository until the evaluation has been
run and analysed. `CLAUDE.md` holds the frozen settings and the compact current state;
`docs/decision-log.md` holds the dated reasoning and measurements.

## Layout

| Path | What it is |
|---|---|
| `of-energy/` | the energy-aware OF (`rpl-energy-of.{c,h}`), mechanism flags, install notes |
| `patches/` | the single patch to Contiki-NG core (OF registration, DIO hook, neighbour energy history) |
| `docs/` | `decision-log.md` (dated reasoning and measurements) and the case-study report |
| `battery-sim/` | simulated battery and node death, independent of the OF so OF0, MRHOF and our OF run under the identical model |
| `rpl-udp/` | the example app (UDP client/server), `project-conf.h` with the frozen settings, small test scenarios |
| `experiments/` | installer, scenario generator, one-run / batch / stage runners, and the log parsers |
| `results/` | scenarios and summaries of runs; raw Cooja logs are git-ignored (tens of MB each) |
| `CLAUDE.md` | design, decisions, open questions, status log |

## Quick start (Linux / WSL2 with a Contiki-NG checkout at `~/contiki-ng`)

```bash
bash experiments/install.sh                       # installs the OF, the battery model and the one core patch (safe to rerun)

# one lifetime run: NODES SEED VARIANT
#   VARIANT: of0 | mrhof | base | gating | depletion | adaptive | full | fixedw
bash experiments/run_lifetime.sh 30 1 full

# the whole evaluation in stages (see the header of the script)
bash experiments/run_all.sh scale 3               # six scale checks first
bash experiments/run_all.sh main 5                # then the main matrix
python3 experiments/parse_run.py --all results/main    # -> results/main/all_runs.csv
```

A run goes until the last client dies or the cutoff and prints one line:

```
LIFETIME nodes=30 clients=29 dead=29 cutoff=0 FND_s=... HND_s=... LND_s=...
```

FND, HND and LND are the first, half (the ceil(clients/2)-th) and last client death in
simulated seconds; the root is mains-powered and never dies. The same `NODES` and `SEED`
always give the identical topology and traffic whichever variant runs, and every run verifies
that the firmware really contains the OF it was asked for.

## Energy model (identical for every OF)

Radio and CPU follow one duty-cycle assumption `D` (primary 1%, sensitivity 0.5%
and 2%); transmit time is measured and not scaled; capacity is a documented
simulation parameter, not a real battery. The formula and the reasons are in
[`battery-sim/battery-sim.c`](battery-sim/battery-sim.c) and CLAUDE.md. Per-run
overrides (`CAPACITY_MJ`, `DUTY_CYCLE`, `SPREAD_PCT`, `AVG_DEGREE`) are
documented in `experiments/run_lifetime.sh`.
