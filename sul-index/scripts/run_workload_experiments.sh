#!/usr/bin/env bash
# 场景实验（点查询 + 插入混合负载）批量执行脚本
#
# 流程:
#   单一阶段 - 跑 5 场景（train/insert 由调用方提前用 sul_split 准备好）:
#       100%R / 80R20W / 50R50W / 20R80W / 100%W
#       每场景 ops=2000 (read_pct + write_pct = 100)
#
# 数据集准备（如未切割，先执行）:
#   for stem in UNI SKE ABUS MBF PLUT USAC; do
#       ./build/sul_split datasets/${stem}_20000_<sfx>_2.csv datasets 0.9 42 64
#   done
#
# 用法（从 sul-index/ 目录执行）:
#   bash scripts/run_workload_experiments.sh                 # 5 数据集 × 5 场景
#   bash scripts/run_workload_experiments.sh "UNI MBF"       # 仅 UNI+MBF
#   bash scripts/run_workload_experiments.sh UNI "100 50 0"  # UNI 仅 3 场景
#
# 参数（与 SUL-indx 实验设置.md 一致）:
#   N_total = 20000, ratio = 0.9 → train=18000 insert=2000
#   K=1024, err=4, ops=2000
#
# 产出:
#   datasets/<stem>_<N>_dim2_N18000_train.csv     (sul_split 输出)
#   datasets/<stem>_<N>_dim2_N2000_insert.csv     (sul_split 输出)
#   record/workload_<stem>_K1024_err4_dim2.csv    (每场景 1 行追加)
#   logs/run_workload_<STEM>_R<rpct>.log
#   logs/split_<STEM>.log

set -uo pipefail
cd "$(dirname "$0")/.." || { echo "[error] cannot cd to sul-index root"; exit 1; }

EXE=./build/sul_workload
[ -x "$EXE" ] || { echo "[error] $EXE 不存在；请先 cmake --build build"; exit 1; }

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

# 场景百分比 → 中文标签
scenario_label() {
    case "$1" in
        100) echo "全读" ;;
        80)  echo "多读少写" ;;
        50)  echo "读写均衡" ;;
        20)  echo "少读多写" ;;
        0)   echo "全写" ;;
        *)   echo "R=${1}%" ;;
    esac
}

# 前置检查：train/insert 必须已存在（由 sul_split 预先生成）
echo "===== 前置检查: train/insert 文件 ====="
missing_any=0
for stem in $STEMS; do
    train=datasets/${stem}_${N_TOTAL}_dim2_N${N_TRAIN}_train.csv
    insert=datasets/${stem}_${N_TOTAL}_dim2_N${N_INSERT}_insert.csv
    if [ -f "$train" ] && [ -f "$insert" ]; then
        printf "  %-6s OK   (train=%d insert=%d)\n" "$stem" \
            "$(wc -l <"$train")" "$(wc -l <"$insert")"
    else
        printf "  %-6s MISS (缺 %s 或 %s)\n" "$stem" "$train" "$insert"
        missing_any=1
    fi
done
if [ "$missing_any" = "1" ]; then
    echo ""
    echo "[error] 部分 stem 缺 train/insert，请先执行："
    echo "  ./build/sul_split datasets/<stem>_${N_TOTAL}_<sfx>_2.csv datasets 0.9 42 64"
    exit 3
fi

# 跑场景
echo ""
echo "===== 场景执行 (K=$DEFAULT_K err=$DEFAULT_ERR ops=$DEFAULT_OPS) ====="
total=$(($(echo $STEMS | wc -w) * $(echo $SCENARIOS | wc -w)))
i=0
ALL_START=$SECONDS

printf "%-7s %-6s %-4s %-10s %-12s %s\n" "#" "STEM" "R%" "scenario" "elapsed" "log"
echo "--------------------------------------------------------------------------"

for stem in $STEMS; do
    train=datasets/${stem}_${N_TOTAL}_dim2_N${N_TRAIN}_train.csv
    insert=datasets/${stem}_${N_TOTAL}_dim2_N${N_INSERT}_insert.csv
    if [ ! -f "$train" ] || [ ! -f "$insert" ]; then
        echo "[warn] $stem 缺 train/insert，跳过"
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
echo "全部完成，总耗时 $((SECONDS - ALL_START))s"
echo "workload CSV: $(ls record/workload_*.csv 2>/dev/null | wc -l)"
