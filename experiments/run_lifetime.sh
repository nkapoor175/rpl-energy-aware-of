#!/bin/bash
# Run ONE lifetime experiment headless in Cooja: until the last client dies or
# the cutoff, whichever comes first.
#
#   experiments/run_lifetime.sh NODES SEED OF [MAX_SECONDS] [EXPERIMENT_NAME]
#
#   NODES        total nodes including the root (e.g. 10, 30, 50)
#   SEED         scenario seed; with NODES it fixes topology + Cooja seed, so
#                the three OFs see the identical scenario
#   OF           0 = our energy-aware OF, 1 = MRHOF, 2 = OF0
#   MAX_SECONDS  cutoff in simulated seconds (default 90000); a safety limit,
#                not a design parameter
#   EXPERIMENT_NAME  folder under results/ (default "lifetime")
#
# Output: results/<EXPERIMENT_NAME>/<date>/N<nodes>_s<seed>_<OF label>/
#   scenario.csc   the exact scenario that was run
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
[ -n "$NODES" ] && [ -n "$SEED" ] && [ -n "$OF" ] || { sed -n 2,22p "$0"; exit 1; }
case "$OF" in 0) LABEL=energy;; 1) LABEL=mrhof;; 2) LABEL=of0;; *) echo "OF must be 0, 1 or 2"; exit 1;; esac

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

python3 "$REPO/experiments/make_csc.py" --nodes "$NODES" --seed "$SEED" \
  --max-seconds "$MAX" --example-dir "$RUNDIR" --out "$OUT/scenario.csc"

LOGDIR="$(mktemp -d)"
export EXPERIMENT_OF="$OF"
( cd "$CONTIKI/$RUNDIR" && make TARGET=cooja udp-server.cooja udp-client.cooja ) > "$OUT/build.log" 2>&1   || { echo "firmware build failed, see $OUT/build.log"; exit 1; }
START=$(date +%s)
( cd "$CONTIKI/tools/cooja" && ./gradlew run --args="--no-gui --autostart --contiki=$CONTIKI --logdir=$LOGDIR $OUT/scenario.csc" ) > "$OUT/run.out" 2>&1 || true
END=$(date +%s)

cp "$LOGDIR/COOJA.testlog" "$OUT/cooja.log" 2>/dev/null || echo "no COOJA.testlog produced; see run.out"
{
  echo "nodes=$NODES seed=$SEED of=$OF ($LABEL) max_seconds=$MAX"
  grep "^LIFETIME" "$OUT/cooja.log" || echo "NO LIFETIME LINE (run failed?)"
  grep -o "Speedup: [0-9.]*" "$OUT/run.out" | tail -1
  echo "wall_seconds=$((END - START))"
} | tee "$OUT/summary.txt"

rm -rf "$CONTIKI/$RUNDIR" "$LOGDIR"
