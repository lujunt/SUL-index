#!/usr/bin/env python3
"""5 个数据集（UNI / ABUS / MBF / PLUT / USAC）二维分布可视化

依赖:
    pip install matplotlib numpy

用法（从 sul-index/ 目录执行）:
    python3 scripts/plot_distributions.py
    python3 scripts/plot_distributions.py --N 100000        # 改用更大数据集
    python3 scripts/plot_distributions.py --out figs/dist.png

产出:
    scripts/distributions.png   (默认输出路径)

读图要点:
    - 每个子图: 散点 + 标题写坐标范围
    - 散点 alpha=0.25, 直观看聚簇程度
    - 5 数据集横向对比: UNI 几乎填满 [0,1], PLUT 极度聚簇在角落
"""
from __future__ import annotations
import argparse
import csv
import sys
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError as e:
    print(f"[error] 缺少依赖: {e.name}", file=sys.stderr)
    print("[fix] pip install matplotlib numpy", file=sys.stderr)
    sys.exit(1)


DATASETS = [
    ("UNI",  1, "tab:blue",   "Uniform [0,1]"),
    ("ABUS", 0, "tab:orange", "ABUS"),
    ("MBF",  0, "tab:green",  "MBF (Berlin Mod F)"),
    ("PLUT", 0, "tab:red",    "PLUT (Pluto)"),
    ("USAC", 0, "tab:purple", "USAC (US Cities)"),
]


def load_xy(path: Path) -> tuple[np.ndarray, np.ndarray]:
    xs, ys = [], []
    with path.open() as f:
        for row in csv.reader(f):
            if len(row) < 3:
                continue
            try:
                xs.append(float(row[0]))
                ys.append(float(row[1]))
            except ValueError:
                continue
    return np.asarray(xs), np.asarray(ys)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--N", type=int, default=20000,
                    help="数据集规模（默认 20000）")
    ap.add_argument("--datasets-dir", type=Path, default=Path("datasets"))
    ap.add_argument("--out", type=Path, default=Path("scripts/distributions.png"))
    ap.add_argument("--alpha", type=float, default=0.25)
    ap.add_argument("--marker-size", type=float, default=2.5)
    args = ap.parse_args()

    args.out.parent.mkdir(parents=True, exist_ok=True)

    # 2 行 × 3 列；最后 1 格作为 5 数据集散点叠加对比
    fig, axes = plt.subplots(2, 3, figsize=(16, 10))
    fig.suptitle(f"5 datasets coordinate distribution (dim=2, N={args.N})",
                 fontsize=14, y=0.995)

    for idx, (stem, sfx, color, label) in enumerate(DATASETS):
        r, c = divmod(idx, 3)
        ax = axes[r][c]
        path = args.datasets_dir / f"{stem}_{args.N}_{sfx}_2.csv"
        if not path.exists():
            ax.set_title(f"{label}\n[MISSING] {path}")
            ax.axis("off")
            continue

        x, y = load_xy(path)
        ax.scatter(x, y, s=args.marker_size, c=color, alpha=args.alpha,
                   edgecolors="none")
        ax.set_xlim(-0.02, 1.02)
        ax.set_ylim(-0.02, 1.02)
        ax.set_aspect("equal")
        ax.grid(True, alpha=0.3, linewidth=0.5)
        ax.set_title(
            f"{label}\n"
            f"x:[{x.min():.4g}, {x.max():.4g}]  "
            f"y:[{y.min():.4g}, {y.max():.4g}]  "
            f"N={len(x)}",
            fontsize=10,
        )

    # 第 6 格：所有 5 数据集叠加散点
    ax = axes[1][2]
    for stem, sfx, color, label in DATASETS:
        path = args.datasets_dir / f"{stem}_{args.N}_{sfx}_2.csv"
        if not path.exists():
            continue
        x, y = load_xy(path)
        ax.scatter(x, y, s=1.2, c=color, alpha=0.12, label=label,
                   edgecolors="none")
    ax.set_xlim(-0.02, 1.02)
    ax.set_ylim(-0.02, 1.02)
    ax.set_aspect("equal")
    ax.grid(True, alpha=0.3, linewidth=0.5)
    ax.set_title("All 5 overlaid", fontsize=10)
    leg = ax.legend(loc="lower right", fontsize=7, markerscale=4,
                    framealpha=0.85)
    handles = getattr(leg, "legend_handles", None) or leg.legendHandles
    for h in handles:
        h.set_alpha(1.0)

    plt.tight_layout()
    plt.savefig(args.out, dpi=120, bbox_inches="tight")
    print(f"[ok] saved {args.out}  ({args.out.stat().st_size / 1024:.1f} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
