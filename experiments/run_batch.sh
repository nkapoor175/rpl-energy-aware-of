#!/bin/bash
# Run a matrix of lifetime experiments, several at a time.
#
#   experiments/run_batch.sh "NODES..." "SEEDS..." "VARIANTS..." JOBS [MAX_SECONDS] [EXPERIMENT_NAME]
#
# Example: the six scale checks (one 30-node and one 50-node run per OF):
#   experiments/run_batch.sh "30 50" "1" "0 1 2" 3 90000 scale-check
# Example: the full planned set (45 runs):
#   experiments/run_batch.sh "10 30 50" "1 2 3 4 5" "0 1 2" 4 90000 main
#
#   JOBS   how many runs execute at the same time. Memory is the limit: measure
#          one 50-node run first (see CLAUDE.md) before raising this.
#
# The optional per-run overrides of run_lifetime.sh (CAPACITY_MJ, DUTY_CYCLE,
# SPREAD_PCT, AVG_DEGREE) are passed through the environment, e.g. a capacity
# sensitivity batch:  CAPACITY_MJ=120000.0 experiments/run_batch.sh ... sens-2x
#
# RESUME=1 (default) skips runs whose summary.txt already has a LIFETIME line, so an
# interrupted batch can simply be started again; RESUME=0 reruns everything.
#
# Every run uses the identical scenario for a given (nodes, seed), whatever the
# OF. Output: results/<name>/<date>/... (see run_lifetime.sh) plus
# results/<name>/<date>/batch.txt with one summary line per run.
NODES_LIST="$1"; SEEDS="$2"; OFS="$3"; JOBS="$4"; MAX="${5:-90000}"; NAME="${6:-lifetime}"
[ -n "$NODES_LIST" ] && [ -n "$SEEDS" ] && [ -n "$OFS" ] && [ -n "$JOBS" ] || { sed -n 2,19p "$0"; exit 1; }

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DAY="$(date +%F)"
mkdir -p "$REPO/results/$NAME/$DAY"
BATCH="$REPO/results/$NAME/$DAY/batch.txt"
: > "$BATCH"

JOBLIST="$(mktemp)"
for n in $NODES_LIST; do
  for s in $SEEDS; do
    for of in $OFS; do
      echo "$n $s $of" >> "$JOBLIST"
    done
  done
done
TOTAL=$(wc -l < "$JOBLIST")
echo "$TOTAL runs, $JOBS at a time -> $BATCH"

export REPO MAX NAME BATCH
export RESUME="${RESUME:-1}"
< "$JOBLIST" xargs -P "$JOBS" -L 1 bash -c '
  n="$0"; sd="$1"; v="$2"
  case "$v" in 0) label=energy;; 1) label=mrhof;; 2) label=of0;; *) label="$v";; esac
  if [ "$RESUME" = "1" ]; then
    prev="$(ls "$REPO"/results/"$NAME"/*/N"$n"_s"$sd"_"$label"/summary.txt 2>/dev/null | head -1)"
    if [ -n "$prev" ] && grep -q "^LIFETIME" "$prev"; then
      echo "nodes=$n seed=$sd of=$v rc=0 SKIPPED_ALREADY_DONE $(grep "^LIFETIME" "$prev")" | tee -a "$BATCH"
      exit 0
    fi
  fi
  out="$(bash "$REPO/experiments/run_lifetime.sh" "$n" "$sd" "$v" "$MAX" "$NAME" 2>&1)"
  rc=$?
  line="$(echo "$out" | grep -E "^LIFETIME" | head -1)"
  echo "nodes=$n seed=$sd of=$v rc=$rc ${line:-NO_LIFETIME_LINE}" | tee -a "$BATCH"
'
rm -f "$JOBLIST"
echo "done: $(grep -c 'rc=0' "$BATCH") of $TOTAL runs finished with exit code 0, $(grep -c NO_LIFETIME_LINE "$BATCH") without a LIFETIME line"
