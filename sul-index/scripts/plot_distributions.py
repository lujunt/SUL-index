#!/usr/bin/env python3
"""Visualize the 2D distributions of UNI, ABUS, MBF, PLUT, and USAC.

Dependencies:
    pip install matplotlib numpy

Usage from sul-index/:
    python3 scripts/plot_distributions.py
    python3 scripts/plot_distributions.py --N 100000        # use larger datasets
    python3 scripts/plot_distributions.py --out figs/dist.png

Output:
    scripts/distributions.png (default)

Each subplot is a scatter plot labeled with its coordinate range. The shared overlay
highlights clustering differences across all five datasets.
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
    print(f"[error] missing dependency: {e.name}", file=sys.stderr)
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
                    help="dataset size (default: 20000)")
    ap.add_argument("--datasets-dir", type=Path, default=Path("datasets"))
    ap.add_argument("--out", type=Path, default=Path("scripts/distributions.png"))
    ap.add_argument("--alpha", type=float, default=0.25)
    ap.add_argument("--marker-size", type=float, default=2.5)
    args = ap.parse_args()

    args.out.parent.mkdir(parents=True, exist_ok=True)

    # Two rows by three columns; the last panel overlays all five datasets.
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

    # Sixth panel: overlay all five datasets.
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
