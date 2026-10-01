#!/usr/bin/env bash
# Batch parameter-experiment runner.
#
# Usage from sul-index/ (the script changes to the project directory automatically):
#   bash scripts/run_param_experiments.sh                    # all datasets and sweeps
#   bash scripts/run_param_experiments.sh "UNI MBF"          # all sweeps for UNI and MBF
#   bash scripts/run_param_experiments.sh UNI sl             # selectivity sweep for UNI
#   bash scripts/run_param_experiments.sh "UNI MBF" "sl d"   # custom subset
#   bash scripts/run_param_experiments.sh SKE                # all sweeps for SKE
#
# Parameter matrix:
#   Default: N=20000, d=2, K=1024, err=4, sl=0.25%.
#   sl  : 0.25 / 0.5 / 1 / 2 / 4
#   N   : 20000 / 40000 / 60000 / 80000 / 100000
#   d   : 2 / 3 / 4 / 5 / 6
#   err : 1 / 2 / 4 / 8 / 16
#   K   : 1024 / 2048 / 3072 / 4096
#
# Output:
#   record/build_<stem>_K{K}_err{err}_dim{d}.csv
#   record/rangequery_<stem>_K{K}_err{err}_dim{d}_sl{tag}.csv
#   logs/run_<STEM>_<SWEEP>.log    complete stdout/stderr for each sweep
#
# Prerequisites:
#   1. Build sul_compare_demo with cmake --build build.
#   2. Prepare datasets/<STEM>_<N>_<suffix>_<d>.csv.
#   3. Prepare query/<STEM>_<N>_dim<d>_<sl>.csv.
#      (suffix: UNI=1, ABUS/MBF/PLUT/USAC=0, SKE=4)

set -uo pipefail
cd "$(dirname "$0")/.." || { echo "[error] cannot cd to sul-index root"; exit 1; }

EXE=./build/sul_compare_demo
if [ ! -x "$EXE" ]; then
    echo "[error] $EXE is missing or not executable; run cmake --build build first"; exit 1
fi

DATASETS="${1:-UNI ABUS MBF PLUT USAC SKE}"
SWEEPS="${2:-sl N d err K}"

DEFAULT_N=20000
DEFAULT_D=2
DEFAULT_K=1024
DEFAULT_ERR=4
DEFAULT_SL=0.25

SL_VALUES="0.25 0.5 1 2 4"
N_VALUES="20000 40000 60000 80000 100000"
D_VALUES="2 3 4 5 6"
ERR_VALUES="1 2 4 8 16"
K_VALUES="1024 2048 3072 4096"

mkdir -p logs record

suffix_for() {
    case "$1" in
        UNI) echo 1 ;;
        ABUS|MBF|PLUT|USAC) echo 0 ;;
        SKE) echo 4 ;;
        *) echo "[error] unknown stem: $1" >&2; exit 2 ;;
    esac
}

# A failed sul_compare_demo invocation logs a warning without stopping the sweep.
run_compare() {
    local label=$1 ds=$2 q=$3 k=$4 err=$5 log=$6
    if [ ! -f "$ds" ]; then echo "[warn] missing dataset $ds (skipping $label)" | tee -a "$log"; return; fi
    if [ ! -f "$q"  ]; then echo "[warn] missing query file $q (skipping $label)" | tee -a "$log"; return; fi
    echo "===== [$(date +%T)] $label =====" >>"$log"
    "$EXE" "$ds" "$q" "$k" "$err" >>"$log" 2>&1
    local rc=$?
    echo "===== [$(date +%T)] $label DONE rc=$rc =====" >>"$log"
}

run_sweep() {
    local stem=$1 sweep=$2 log=$3
    local sfx; sfx=$(suffix_for "$stem")

    case "$sweep" in
      sl)
        for sl in $SL_VALUES; do
            run_compare "${stem} sl=${sl}%" \
                "datasets/${stem}_${DEFAULT_N}_${sfx}_${DEFAULT_D}.csv" \
                "query/${stem}_${DEFAULT_N}_dim${DEFAULT_D}_${sl}.csv" \
                "$DEFAULT_K" "$DEFAULT_ERR" "$log"
        done ;;
      N)
        for N in $N_VALUES; do
            run_compare "${stem} N=${N}" \
                "datasets/${stem}_${N}_${sfx}_${DEFAULT_D}.csv" \
                "query/${stem}_${N}_dim${DEFAULT_D}_${DEFAULT_SL}.csv" \
                "$DEFAULT_K" "$DEFAULT_ERR" "$log"
        done ;;
      d)
        for d in $D_VALUES; do
            run_compare "${stem} d=${d}" \
                "datasets/${stem}_${DEFAULT_N}_${sfx}_${d}.csv" \
                "query/${stem}_${DEFAULT_N}_dim${d}_${DEFAULT_SL}.csv" \
                "$DEFAULT_K" "$DEFAULT_ERR" "$log"
        done ;;
      err)
        for err in $ERR_VALUES; do
            run_compare "${stem} err=${err}" \
                "datasets/${stem}_${DEFAULT_N}_${sfx}_${DEFAULT_D}.csv" \
                "query/${stem}_${DEFAULT_N}_dim${DEFAULT_D}_${DEFAULT_SL}.csv" \
                "$DEFAULT_K" "$err" "$log"
        done ;;
      K)
        for K in $K_VALUES; do
            run_compare "${stem} K=${K}" \
                "datasets/${stem}_${DEFAULT_N}_${sfx}_${DEFAULT_D}.csv" \
                "query/${stem}_${DEFAULT_N}_dim${DEFAULT_D}_${DEFAULT_SL}.csv" \
                "$K" "$DEFAULT_ERR" "$log"
        done ;;
      *)
        echo "[error] unknown sweep '$sweep' (valid: sl N d err K)" >&2; exit 2 ;;
    esac
}

total=$(($(echo $DATASETS | wc -w) * $(echo $SWEEPS | wc -w)))
i=0
ALL_START=$SECONDS

printf "%-6s %-4s %-8s %-12s %-12s %s\n" "#" "STEM" "SWEEP" "elapsed" "PASS/FAIL" "log"
echo "--------------------------------------------------------------------------"

for stem in $DATASETS; do
    for sweep in $SWEEPS; do
        i=$((i+1))
        log=logs/run_${stem}_${sweep}.log
        : >"$log"
        ts=$SECONDS
        run_sweep "$stem" "$sweep" "$log"
        elapsed=$((SECONDS - ts))
        pass=$(grep -c "Result: PASS" "$log" || true)
        fail=$(grep -c "Result: FAIL" "$log" || true)
        printf "[%2d/%2d] %-4s %-8s %-12s P=%-3d/F=%-3d %s\n" \
            "$i" "$total" "$stem" "$sweep" "${elapsed}s" "$pass" "$fail" "$log"
    done
done

echo "--------------------------------------------------------------------------"
echo "All runs completed in $((SECONDS - ALL_START))s"
echo "record/ CSV count: $(ls record/*.csv 2>/dev/null | wc -l)"
