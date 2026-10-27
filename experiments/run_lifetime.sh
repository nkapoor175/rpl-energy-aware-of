#!/bin/bash
# Run ONE lifetime experiment headless in Cooja: until the last client dies or
# the cutoff, whichever comes first.
#
#   experiments/run_lifetime.sh NODES SEED VARIANT [MAX_SECONDS] [EXPERIMENT_NAME]
#
#   NODES        total nodes including the root (e.g. 10, 30, 50)
#   SEED         scenario seed; with NODES it fixes topology + Cooja seed, so
#                every variant sees the identical scenario
#   VARIANT      what runs (all use the same battery, traffic and topology):
#                  of0        OF0 (hop count)                          EXPERIMENT_OF=2
#                  mrhof      MRHOF (ETX)                              EXPERIMENT_OF=1
#                  base       our weighted sum only, no mechanisms     weights 0.4/0.4/0.2
#                  gating     base + energy-gated filtering (ablation)
#                  depletion  base + depletion-rate tracking (ablation)
#                  adaptive   base + adaptive weighting (ablation)
#                  full       all three mechanisms
#                  fixedw     base with FIXED weights 0.48/0.30/0.22 (control for 'adaptive':
#                             separates "different weights" from "adapting")
#                Legacy numeric codes still work: 0 = our OF with the project-conf.h
#                defaults (gating only), 1 = MRHOF, 2 = OF0.
#   MAX_SECONDS  cutoff in simulated seconds (default 90000); a safety limit,
#                not a design parameter
#   EXPERIMENT_NAME  folder under results/ (default "lifetime")
#
# Output: results/<EXPERIMENT_NAME>/<date>/N<nodes>_s<seed>_<OF label>/
#   scenario.csc   the exact scenario that was run
#   project-conf.used.h   the exact compile-time settings of this run
#   cooja.log      every node's log line with its simulated time (microseconds)
#   run.out        Cooja's console output
#   summary.txt    the LIFETIME line + wall-clock time + simulation speed-up
#
# The firmware is built HERE with EXPERIMENT_OF set, before Cooja starts: Cooja's
# own internal make does not see the variable, and would otherwise silently build
# the default (our energy OF) for every run.
# Runs are independent (own copy of the example dir, own Cooja log dir), so
# several can run in parallel. Needs experiments/install.sh to have been run.
set -e
NODES="$1"; SEED="$2"; OF="$3"; MAX="${4:-90000}"; NAME="${5:-lifetime}"
[ -n "$NODES" ] && [ -n "$SEED" ] && [ -n "$OF" ] || { sed -n 2,32p "$0"; exit 1; }
# Variant -> (EXPERIMENT_OF code, mechanism toggles; "-" = leave the project-conf.h default)
VARIANT="$OF"
G=-; D=-; A=-; WPRESET=""
case "$VARIANT" in
  0)         OF=0; LABEL=energy ;;
  1|mrhof)   OF=1; LABEL=mrhof ;;
  2|of0)     OF=2; LABEL=of0 ;;
  base)      OF=0; LABEL=base;      G=0; D=0; A=0 ;;
  gating)    OF=0; LABEL=gating;    G=1; D=0; A=0 ;;
  depletion) OF=0; LABEL=depletion; G=0; D=1; A=0 ;;
  adaptive)  OF=0; LABEL=adaptive;  G=0; D=0; A=1 ;;
  full)      OF=0; LABEL=full;      G=1; D=1; A=1 ;;
  fixedw)    OF=0; LABEL=fixedw;    G=0; D=0; A=0; WPRESET="480/300/220" ;;
  *) echo "unknown variant '$VARIANT' (of0 mrhof base gating depletion adaptive full fixedw, or 0/1/2)"; exit 1 ;;
esac
WEIGHTS="${WEIGHTS:-$WPRESET}"      # WEIGHTS=w1/w2/w3 in per-mille (sum 1000), e.g. 500/300/200

REPO="$(cd "$(dirname "$0")/.." && pwd)"
CONTIKI="${CONTIKI:-$HOME/contiki-ng}"
TAG="N${NODES}_s${SEED}_${LABEL}_$$"
RUNDIR="examples/rpl-udp-run-$TAG"
OUT="$REPO/results/$NAME/$(date +%F)/N${NODES}_s${SEED}_${LABEL}"
mkdir -p "$OUT"

# private copy of the example, so parallel runs never share a build directory
rm -rf "$CONTIKI/$RUNDIR"
mkdir "$CONTIKI/$RUNDIR"
cp "$CONTIKI/examples/rpl-udp/"{Makefile,project-conf.h,udp-client.c,udp-server.c} "$CONTIKI/$RUNDIR/"

# Optional per-run overrides (environment variables), recorded in summary.txt.
# They change only THIS run's private copy of project-conf.h:
#   CAPACITY_MJ  battery capacity per node        (e.g. 30000.0 / 120000.0 for 0.5x / 2x)
#   DUTY_CYCLE   duty-cycle assumption D          (e.g. 0.005 / 0.02)
#   SPREAD_PCT   per-node capacity spread percent (recovery experiment only)
#   AVG_DEGREE   nominal neighbour density for make_csc.py
OVR=""
add_override() { OVR="$OVR$1"$'\n'; }
[ -n "$CAPACITY_MJ" ] && add_override "#undef BATTERY_SIM_CONF_CAPACITY_MJ"$'\n'"#define BATTERY_SIM_CONF_CAPACITY_MJ $CAPACITY_MJ"
[ -n "$DUTY_CYCLE" ]  && add_override "#undef BATTERY_SIM_CONF_DUTY_CYCLE"$'\n'"#define BATTERY_SIM_CONF_DUTY_CYCLE $DUTY_CYCLE"
[ -n "$SPREAD_PCT" ]  && add_override "#undef BATTERY_SIM_CONF_CAPACITY_SPREAD_PCT"$'\n'"#define BATTERY_SIM_CONF_CAPACITY_SPREAD_PCT $SPREAD_PCT"
if [ "$OF" = "0" ]; then
  [ "$G" != "-" ] && add_override "#undef RPL_ENERGY_OF_CONF_ENABLE_GATING"$'
'"#define RPL_ENERGY_OF_CONF_ENABLE_GATING $G"
  [ "$D" != "-" ] && add_override "#undef RPL_ENERGY_OF_CONF_ENABLE_DEPLETION"$'
'"#define RPL_ENERGY_OF_CONF_ENABLE_DEPLETION $D"
  [ "$A" != "-" ] && add_override "#undef RPL_ENERGY_OF_CONF_ENABLE_ADAPTIVE_WEIGHTING"$'
'"#define RPL_ENERGY_OF_CONF_ENABLE_ADAPTIVE_WEIGHTING $A"
  if [ -n "$WEIGHTS" ]; then
    IFS=/ read -r W1 W2 W3 <<< "$WEIGHTS"
    [ $((W1 + W2 + W3)) -eq 1000 ] || { echo "WEIGHTS must sum to 1000 (per-mille), got $WEIGHTS"; exit 1; }
    add_override "#undef RPL_ENERGY_OF_CONF_W1"$'
'"#define RPL_ENERGY_OF_CONF_W1 $W1"
    add_override "#undef RPL_ENERGY_OF_CONF_W2"$'
'"#define RPL_ENERGY_OF_CONF_W2 $W2"
    add_override "#undef RPL_ENERGY_OF_CONF_W3"$'
'"#define RPL_ENERGY_OF_CONF_W3 $W3"
  fi
fi
if [ -n "$OVR" ]; then
  PC="$CONTIKI/$RUNDIR/project-conf.h"
  head -n -1 "$PC" > "$PC.new"        # drop the closing #endif of the include guard
  { printf '%s' "$OVR"; echo "#endif /* PROJECT_CONF_H_ */"; } >> "$PC.new"
  mv "$PC.new" "$PC"
fi

cp "$CONTIKI/$RUNDIR/project-conf.h" "$OUT/project-conf.used.h"   # exact settings of this run (toggles, weights, energy model)

python3 "$REPO/experiments/make_csc.py" --nodes "$NODES" --seed "$SEED" \
  --max-seconds "$MAX" --example-dir "$RUNDIR" --out "$OUT/scenario.csc" \
  ${AVG_DEGREE:+--avg-degree "$AVG_DEGREE"}

LOGDIR="$(mktemp -d)"
export EXPERIMENT_OF="$OF"
( cd "$CONTIKI/$RUNDIR" && make TARGET=cooja udp-server.cooja udp-client.cooja ) > "$OUT/build.log" 2>&1   || { echo "firmware build failed, see $OUT/build.log"; exit 1; }
START=$(date +%s)
( cd "$CONTIKI/tools/cooja" && ./gradlew run --args="--no-gui --autostart --contiki=$CONTIKI --logdir=$LOGDIR $OUT/scenario.csc" ) > "$OUT/run.out" 2>&1 || true
END=$(date +%s)

cp "$LOGDIR/COOJA.testlog" "$OUT/cooja.log" 2>/dev/null || echo "no COOJA.testlog produced; see run.out"
# the root prints EXPERIMENT_OF=<n> at boot: proof of which OF was really built
BUILT="$(grep -o 'EXPERIMENT_OF=[0-9]*' "$OUT/cooja.log" 2>/dev/null | head -1 | cut -d= -f2)"
if [ "$BUILT" != "$OF" ]; then
  echo "OF MISMATCH: requested $OF but firmware reports '${BUILT:-nothing}'" | tee "$OUT/summary.txt"
  rm -rf "$CONTIKI/$RUNDIR" "$LOGDIR"
  exit 1
fi
{
  echo "nodes=$NODES seed=$SEED of=$OF ($LABEL) of_built_in_firmware=$BUILT max_seconds=$MAX"
  echo "variant=$VARIANT toggles: gating=$G depletion=$D adaptive=$A (- = project-conf.h default) weights=${WEIGHTS:-default}"
  echo "overrides: capacity_mj=${CAPACITY_MJ:-default} duty_cycle=${DUTY_CYCLE:-default} spread_pct=${SPREAD_PCT:-default} avg_degree=${AVG_DEGREE:-default}"
  grep "^LIFETIME" "$OUT/cooja.log" || echo "NO LIFETIME LINE (run failed?)"
  grep -o "Speedup: [0-9.]*" "$OUT/run.out" | tail -1
  echo "wall_seconds=$((END - START))"
} | tee "$OUT/summary.txt"

rm -rf "$CONTIKI/$RUNDIR" "$LOGDIR"
