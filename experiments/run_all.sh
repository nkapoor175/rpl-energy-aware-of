#!/bin/bash
# Run the whole evaluation in stages. Needs experiments/install.sh to have been run
# on this machine, and the frozen settings to be the ones in CLAUDE.md.
#
#   experiments/run_all.sh STAGE [JOBS]
#
#   scale     the six scale checks: 30 and 50 nodes, seed 1, of0 / mrhof / full
#             -> results/main        (these runs are part of the main matrix)
#   main      main matrix: 10/30/50 nodes x seeds 1-5 x 8 variants
#             (of0 mrhof base gating depletion adaptive full fixedw)
#             -> results/main        (already finished runs are skipped)
#   weights   weight-sensitivity sweep of the full OF: 30 nodes, seeds 1-5,
#             weights 500/300/200, 300/500/200, 333/333/334
#             -> results/weights-<w1>-<w2>-<w3>
#   duty      duty-cycle sensitivity: D = 0.005 and 0.02, 30 nodes, seeds 1-5, of0 mrhof full
#             -> results/duty-0.005, results/duty-0.02
#   capacity  capacity sensitivity: 0.5x (30000) and 2x (120000) mJ, same runs
#             -> results/capacity-30000, results/capacity-120000
#   spread    recovery experiment (NOT a main result): 30% capacity spread, 30 nodes,
#             seeds 1-5, of0 mrhof full -> results/spread-30
#   parse     parse every run folder found under results/ into all_runs.csv files
#
# JOBS = runs at the same time (default 5; ~0.6 GB of RAM each). Every stage first
# writes results/<name>/FROZEN.txt (git commit, settings, constants) and can be
# restarted: finished runs are skipped. The cutoff (MAX_SECONDS) is a safety limit,
# chosen per stage so that nodes die before it is reached.
set -e
STAGE="$1"; JOBS="${2:-5}"
[ -n "$STAGE" ] || { sed -n 2,26p "$0"; exit 1; }
REPO="$(cd "$(dirname "$0")/.." && pwd)"
B="$REPO/experiments/run_batch.sh"
SEEDS="1 2 3 4 5"
CORE="of0 mrhof full"

freeze() {   # freeze NAME : record exactly what is being run, before it runs
  local d="$REPO/results/$1"; mkdir -p "$d"
  {
    echo "stage=$STAGE name=$1 started=$(date -Is)"
    echo "git_commit=$(git -C "$REPO" rev-parse HEAD 2>/dev/null) dirty=$(git -C "$REPO" status --porcelain 2>/dev/null | grep -vc '^??')"
    echo "--- project-conf.h (frozen energy model) ---"
    grep -E "BATTERY_SIM_CONF_|EXPERIMENT_OF|ENERGEST" "$REPO/rpl-udp/project-conf.h" | grep -v '^ \*' | grep "#define"
    echo "--- objective-function constants ---"
    grep -hE "define (DEPLETION_RATE_REF_MILLI|DEPLETION_PENALTY_MAX_MILLI|ADAPTIVE_[A-Z0-9_]+|ENERGY_GATE_[A-Z_]+|RANK_THRESHOLD|RPL_ENERGY_OF_CONF_DEPLETION_[A-Z_]+|W[123]_[A-Z]+_MILLI)" "$REPO/of-energy/rpl-energy-of.c" | sed 's/^[ ]*//'
    echo "--- topology generator default ---"
    grep -n "avg-degree" "$REPO/experiments/make_csc.py" | head -2
  } > "$d/FROZEN.txt"
}

case "$STAGE" in
  scale)
    freeze main
    "$B" "30 50" "1" "$CORE" "$JOBS" 90000 main ;;
  main)
    freeze main
    "$B" "10 30 50" "$SEEDS" "of0 mrhof base gating depletion adaptive full fixedw" "$JOBS" 90000 main ;;
  weights)
    for w in 500/300/200 300/500/200 333/333/334; do
      n="weights-${w//\//-}"; freeze "$n"
      WEIGHTS="$w" "$B" "30" "$SEEDS" "full" "$JOBS" 90000 "$n"
    done ;;
  duty)
    for d in 0.005 0.02; do
      n="duty-$d"; freeze "$n"
      cut=90000; [ "$d" = "0.005" ] && cut=250000
      DUTY_CYCLE="$d" "$B" "30" "$SEEDS" "$CORE" "$JOBS" "$cut" "$n"
    done ;;
  capacity)
    for c in 30000.0 120000.0; do
      n="capacity-${c%.0}"; freeze "$n"
      cut=90000; [ "$c" = "120000.0" ] && cut=200000
      CAPACITY_MJ="$c" "$B" "30" "$SEEDS" "$CORE" "$JOBS" "$cut" "$n"
    done ;;
  spread)
    freeze spread-30
    SPREAD_PCT=30 "$B" "30" "$SEEDS" "$CORE" "$JOBS" 90000 spread-30 ;;
  parse)
    for d in "$REPO"/results/*/; do
      ls "$d"*/*/cooja.log >/dev/null 2>&1 && python3 "$REPO/experiments/parse_run.py" --all "$d" | tail -1
    done ;;
  *) echo "unknown stage '$STAGE'"; sed -n 2,26p "$0"; exit 1 ;;
esac
