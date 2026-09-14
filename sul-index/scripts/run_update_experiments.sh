#!/usr/bin/env bash
# 新重训练连续流入口：直接传入已准备的 base/insert/eval/monitor 文件。
# 批量实验通过循环调用本脚本；每次调用保留独立 run-dir，失败码原样传播。
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $# -lt 4 ]]; then
    echo "Usage: $0 BASE INSERT EVAL_100 MONITOR [sul_update options...]" >&2
    echo "Example: $0 base.csv insert.csv eval.csv monitor.csv 1024 4 50 --sl-pct 0.25 --run-dir record/retrain_v2/run1" >&2
    exit 2
fi
BASE=$1
INSERT=$2
EVAL=$3
MONITOR=$4
shift 4
exec "$ROOT/build/sul_update" "$BASE" "$INSERT" "$EVAL" "$@" --monitor-query "$MONITOR"
