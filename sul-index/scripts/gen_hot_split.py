#!/usr/bin/env python3
"""
gen_hot_split.py
================

Split a 100K dataset into a base (first N_base points) and hot_ul (N_hot hot-write
points), then generate hot queries with the template query's selectivity.

Hot writes are defined by sorting the base by Z-order and dividing it into n_buckets.
Each hot bucket has Z-order bounds and a 2D bounding box. Pool points in those buckets
become hot writes; missing points are synthesized inside the boxes. Hot queries reuse
template rectangle sizes and move their centers into random hot-bucket boxes.

Usage:
  python3 scripts/gen_hot_split.py SRC OUT_BASE OUT_HOT_UL \
      [--template-query QUERY_CSV --out-hot-query OUT_HOT_QUERY] \
      [--n-base 20000] [--n-hot 10000] [--n-buckets 50] \
      [--hot-buckets 24,25,26] [--seed 42]
"""
import argparse
import csv
import random
import sys


def morton2d_16(x: int, y: int) -> int:
    """Encode a 16-bit-per-dimension Morton value compatible with C++ ZOrderEncoder."""
    def part(v: int) -> int:
        v &= 0xFFFF
        v = (v | (v << 8)) & 0x00FF00FF
        v = (v | (v << 4)) & 0x0F0F0F0F
        v = (v | (v << 2)) & 0x33333333
        v = (v | (v << 1)) & 0x55555555
        return v
    return part(x) | (part(y) << 1)


def z_of(xf: float, yf: float) -> int:
    xi = max(0, min(0xFFFF, int(xf * 65536)))
    yi = max(0, min(0xFFFF, int(yf * 65536)))
    return morton2d_16(xi, yi)


def load_points(path):
    """Read x,y,id CSV rows and return a list of (x, y, id)."""
    rows = []
    with open(path, newline='') as f:
        for r in csv.reader(f):
            if not r or r[0].startswith('#'):
                continue
            rows.append((float(r[0]), float(r[1]), int(r[2])))
    return rows


def build_buckets(base_sorted, n_buckets):
    """Return each bucket's Z-order interval and 2D bounding box."""
    n = len(base_sorted)
    bsize = n // n_buckets
    buckets = []
    for k in range(n_buckets):
        i0 = k * bsize
        i1 = (k + 1) * bsize if k < n_buckets - 1 else n
        seg = base_sorted[i0:i1]
        zs = [r[3] for r in seg]
        xs = [r[0] for r in seg]
        ys = [r[1] for r in seg]
        buckets.append((min(zs), max(zs), min(xs), max(xs), min(ys), max(ys)))
    return buckets


def write_csv(path, rows):
    with open(path, 'w', newline='') as f:
        w = csv.writer(f)
        for r in rows:
            w.writerow([f"{r[0]:.9g}", f"{r[1]:.9g}", int(r[2])])


def gen_hot_ul(pool, hot_buckets_info, n_hot, rng):
    """Select hot-region points from the pool and synthesize any shortfall in hot boxes."""
    def in_hot(z):
        for b in hot_buckets_info:
            if b[0] <= z <= b[1]:
                return True
        return False

    pool_hot = [p for p in pool if in_hot(p[3])]
    print(f"[info] pool_hot from real pool: {len(pool_hot)} (need {n_hot})", file=sys.stderr)

    if len(pool_hot) >= n_hot:
        hot = rng.sample(pool_hot, n_hot)
    else:
        hot = list(pool_hot)
        next_id = 10_000_000  # Synthetic ID range avoids collisions with 0..99999.
        while len(hot) < n_hot:
            b = hot_buckets_info[rng.randint(0, len(hot_buckets_info) - 1)]
            x = rng.uniform(b[2], b[3])
            y = rng.uniform(b[4], b[5])
            hot.append((x, y, next_id, z_of(x, y)))
            next_id += 1

    rng.shuffle(hot)
    return hot


def gen_hot_query(template_path, hot_buckets_info, rng):
    """Reuse template rectangle sizes and move centers into random hot-bucket boxes."""
    header_comment = None
    rects = []
    with open(template_path, newline='') as f:
        for line in f:
            s = line.strip()
            if not s:
                continue
            if s.startswith('#'):
                if header_comment is None:
                    header_comment = line.rstrip('\n')
                continue
            parts = [float(x) for x in s.split(',')]
            if len(parts) != 4:
                raise ValueError(f"unexpected query line (need 2D 4 cols): {s}")
            rects.append(parts)

    new_rects = []
    for lo_x, lo_y, hi_x, hi_y in rects:
        dx = hi_x - lo_x
        dy = hi_y - lo_y
        b = hot_buckets_info[rng.randint(0, len(hot_buckets_info) - 1)]
        cx = rng.uniform(b[2], b[3])
        cy = rng.uniform(b[4], b[5])
        new_lo_x = max(0.0, cx - dx / 2)
        new_lo_y = max(0.0, cy - dy / 2)
        new_hi_x = min(1.0 - 1e-9, new_lo_x + dx)
        new_hi_y = min(1.0 - 1e-9, new_lo_y + dy)
        # Shift the lower bound after upper-bound clipping to preserve edge length.
        if new_hi_x - new_lo_x < dx - 1e-12:
            new_lo_x = max(0.0, new_hi_x - dx)
        if new_hi_y - new_lo_y < dy - 1e-12:
            new_lo_y = max(0.0, new_hi_y - dy)
        new_rects.append((new_lo_x, new_lo_y, new_hi_x, new_hi_y))

    return header_comment, new_rects


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src', help='100K source CSV')
    ap.add_argument('out_base', help='output base CSV')
    ap.add_argument('out_hot_ul', help='output hot-insertion CSV')
    ap.add_argument('--n-base', type=int, default=20000)
    ap.add_argument('--n-hot', type=int, default=10000)
    ap.add_argument('--n-buckets', type=int, default=50)
    ap.add_argument('--hot-buckets', default='24,25,26')
    ap.add_argument('--seed', type=int, default=42)
    ap.add_argument('--template-query', default=None,
                    help='template query path; also generates hot queries when provided')
    ap.add_argument('--out-hot-query', default=None,
                    help='hot-query output path; requires --template-query')
    args = ap.parse_args()

    if (args.template_query is None) != (args.out_hot_query is None):
        ap.error('--template-query and --out-hot-query must be provided together')

    rng = random.Random(args.seed)
    rows = load_points(args.src)
    print(f"[info] loaded {len(rows)} rows from {args.src}", file=sys.stderr)
    if len(rows) < args.n_base + 1:
        ap.error(f'source has {len(rows)} rows, fewer than n_base={args.n_base}')

    # Attach a Z-order value to every point as (x, y, id, z).
    pts = [(x, y, i, z_of(x, y)) for (x, y, i) in rows]

    base = pts[:args.n_base]
    pool = pts[args.n_base:]

    base_sorted = sorted(base, key=lambda r: r[3])
    buckets = build_buckets(base_sorted, args.n_buckets)

    hot_idx = [int(s) for s in args.hot_buckets.split(',')]
    bad = [k for k in hot_idx if not (0 <= k < args.n_buckets)]
    if bad:
        ap.error(f'hot-buckets out of range: {bad} (n-buckets={args.n_buckets})')
    hot_info = [buckets[k] for k in hot_idx]

    print(f"[info] hot buckets={hot_idx}", file=sys.stderr)
    for k, b in zip(hot_idx, hot_info):
        print(f"  bucket[{k:>3}] z=[{b[0]}, {b[1]}]  "
              f"x=[{b[2]:.4f}, {b[3]:.4f}]  y=[{b[4]:.4f}, {b[5]:.4f}]",
              file=sys.stderr)

    write_csv(args.out_base, base)
    print(f"[ok] base={len(base)} -> {args.out_base}")

    hot = gen_hot_ul(pool, hot_info, args.n_hot, rng)
    write_csv(args.out_hot_ul, hot)
    print(f"[ok] hot_ul={len(hot)} -> {args.out_hot_ul}")

    if args.template_query is not None:
        hdr, hot_rects = gen_hot_query(args.template_query, hot_info, rng)
        with open(args.out_hot_query, 'w', newline='') as f:
            if hdr is not None:
                f.write(hdr + '  hot_buckets=' + ','.join(str(k) for k in hot_idx) + '\n')
            w = csv.writer(f)
            for lo_x, lo_y, hi_x, hi_y in hot_rects:
                w.writerow([f"{lo_x:.9g}", f"{lo_y:.9g}",
                            f"{hi_x:.9g}", f"{hi_y:.9g}"])
        print(f"[ok] hot_query={len(hot_rects)} -> {args.out_hot_query}")


if __name__ == '__main__':
    main()
