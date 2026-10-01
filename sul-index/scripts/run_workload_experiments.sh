#!/usr/bin/env bash
# Batch runner for mixed point-query and insertion workloads.
#
# Workflow:
#   Run five scenarios using training/insertion files prepared with sul_split:
#       100%R / 80R20W / 50R50W / 20R80W / 100%W
#       2,000 operations per scenario (read_pct + write_pct = 100)
#
# Dataset preparation when not already split:
#   for stem in UNI SKE ABUS MBF PLUT USAC; do
#       ./build/sul_split datasets/${stem}_20000_<sfx>_2.csv datasets 0.9 42 64
#   done
#
# Usage from sul-index/:
#   bash scripts/run_workload_experiments.sh                 # five datasets, five scenarios
#   bash scripts/run_workload_experiments.sh "UNI MBF"       # UNI and MBF only
#   bash scripts/run_workload_experiments.sh UNI "100 50 0"  # three UNI scenarios
#
# Parameters:
#   N_total = 20000, ratio = 0.9 → train=18000 insert=2000
#   K=1024, err=4, ops=2000
#
# Output:
#   datasets/<stem>_<N>_dim2_N18000_train.csv     (from sul_split)
#   datasets/<stem>_<N>_dim2_N2000_insert.csv     (from sul_split)
#   record/workload_<stem>_K1024_err4_dim2.csv    (one row per scenario)
#   logs/run_workload_<STEM>_R<rpct>.log
#   logs/split_<STEM>.log

set -uo pipefail
cd "$(dirname "$0")/.." || { echo "[error] cannot cd to sul-index root"; exit 1; }

EXE=./build/sul_workload
[ -x "$EXE" ] || { echo "[error] $EXE is missing; run cmake --build build first"; exit 1; }

STEMS="${1:-UNI SKE ABUS MBF USAC}"
SCENARIOS="${2:-100 80 50 20 0}"

N_TOTAL=20000
N_TRAIN=$((N_TOTAL * 9 / 10))
N_INSERT=$((N_TOTAL - N_TRAIN))
DEFAULT_K=1024
DEFAULT_ERR=4
DEFAULT_OPS=2000

mkdir -p logs record

suffix_for() {
    case "$1" in
        UNI) echo 1 ;;
        ABUS|MBF|PLUT|USAC) echo 0 ;;
        SKE) echo 4 ;;
        *) echo "[error] unknown stem: $1" >&2; exit 2 ;;
    esac
}

# Map read percentages to scenario labels.
scenario_label() {
    case "$1" in
        100) echo "read-only" ;;
        80)  echo "read-heavy" ;;
        50)  echo "balanced" ;;
        20)  echo "write-heavy" ;;
        0)   echo "write-only" ;;
        *)   echo "R=${1}%" ;;
    esac
}

# Verify that sul_split has already generated train/insert files.
echo "===== Prerequisite check: train/insert files ====="
missing_any=0
for stem in $STEMS; do
    train=datasets/${stem}_${N_TOTAL}_dim2_N${N_TRAIN}_train.csv
    insert=datasets/${stem}_${N_TOTAL}_dim2_N${N_INSERT}_insert.csv
    if [ -f "$train" ] && [ -f "$insert" ]; then
        printf "  %-6s OK   (train=%d insert=%d)\n" "$stem" \
            "$(wc -l <"$train")" "$(wc -l <"$insert")"
    else
        printf "  %-6s MISS (missing %s or %s)\n" "$stem" "$train" "$insert"
        missing_any=1
    fi
done
if [ "$missing_any" = "1" ]; then
    echo ""
    echo "[error] some datasets lack train/insert files; run:"
    echo "  ./build/sul_split datasets/<stem>_${N_TOTAL}_<sfx>_2.csv datasets 0.9 42 64"
    exit 3
fi

# Run scenarios.
echo ""
echo "===== Scenario runs (K=$DEFAULT_K err=$DEFAULT_ERR ops=$DEFAULT_OPS) ====="
total=$(($(echo $STEMS | wc -w) * $(echo $SCENARIOS | wc -w)))
i=0
ALL_START=$SECONDS

printf "%-7s %-6s %-4s %-10s %-12s %s\n" "#" "STEM" "R%" "scenario" "elapsed" "log"
echo "--------------------------------------------------------------------------"

for stem in $STEMS; do
    train=datasets/${stem}_${N_TOTAL}_dim2_N${N_TRAIN}_train.csv
    insert=datasets/${stem}_${N_TOTAL}_dim2_N${N_INSERT}_insert.csv
    if [ ! -f "$train" ] || [ ! -f "$insert" ]; then
        echo "[warn] $stem lacks train/insert files; skipping"
        continue
    fi
    for rpct in $SCENARIOS; do
        i=$((i+1))
        log=logs/run_workload_${stem}_R${rpct}.log
        : >"$log"
        ts=$SECONDS
        "$EXE" "$train" "$insert" "$DEFAULT_K" "$DEFAULT_ERR" "$rpct" "$DEFAULT_OPS" \
            >"$log" 2>&1
        rc=$?
        elapsed=$((SECONDS - ts))
        printf "[%2d/%2d] %-6s %-3s %-10s %-12s rc=%d %s\n" \
            "$i" "$total" "$stem" "${rpct}%" "$(scenario_label "$rpct")" \
            "${elapsed}s" "$rc" "$log"
    done
done

echo "--------------------------------------------------------------------------"
echo "All runs completed in $((SECONDS - ALL_START))s"
echo "workload CSV: $(ls record/workload_*.csv 2>/dev/null | wc -l)"
