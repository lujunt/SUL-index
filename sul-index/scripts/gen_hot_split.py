#!/usr/bin/env python3
"""
gen_hot_split.py
================

从 100K 数据集中拆出 base（前 N_base 点）+ hot_ul（N_hot 个热写点），
并基于模板查询文件生成同 selectivity 的热查询文件。

热写定义：
  对 base 按 z 排序后等分 n_buckets 个桶，每个桶有 z 边界 [z_lo, z_hi]
  与 2D 包围盒 [x_lo, x_hi] × [y_lo, y_hi]。
  hot_buckets 指定的桶视为"热区"。
  - 热写点 = 从 pool（100K\base）中筛 z 值落入任一热区的点；不足则在 2D
    包围盒内合成补充。
  - 热查询 = 复用模板查询的每条矩形边长，把中心重锚到随机热桶 2D bbox 内。

用法:
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
    """16-bit per dim Morton (Z-order) 编码，对齐 C++ 端 ZOrderEncoder。"""
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
    """读 CSV：每行 x,y,id（float, float, int）。返回 list[(x, y, id)]."""
    rows = []
    with open(path, newline='') as f:
        for r in csv.reader(f):
            if not r or r[0].startswith('#'):
                continue
            rows.append((float(r[0]), float(r[1]), int(r[2])))
    return rows


def build_buckets(base_sorted, n_buckets):
    """返回 list[(z_lo, z_hi, x_lo, x_hi, y_lo, y_hi)]，每个桶含 z 区间 + 2D bbox。"""
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
    """从 pool 筛热区点，不足则在 hot bbox 内合成补充。"""
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
        next_id = 10_000_000  # 合成点 id 段，避免与原 0..99999 冲突
        while len(hot) < n_hot:
            b = hot_buckets_info[rng.randint(0, len(hot_buckets_info) - 1)]
            x = rng.uniform(b[2], b[3])
            y = rng.uniform(b[4], b[5])
            hot.append((x, y, next_id, z_of(x, y)))
            next_id += 1

    rng.shuffle(hot)
    return hot


def gen_hot_query(template_path, hot_buckets_info, rng):
    """复用模板矩形边长，把中心重锚到随机热桶 2D bbox 内。"""
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
        # 若上界裁剪后变小，下界也要相应回退保持边长
        if new_hi_x - new_lo_x < dx - 1e-12:
            new_lo_x = max(0.0, new_hi_x - dx)
        if new_hi_y - new_lo_y < dy - 1e-12:
            new_lo_y = max(0.0, new_hi_y - dy)
        new_rects.append((new_lo_x, new_lo_y, new_hi_x, new_hi_y))

    return header_comment, new_rects


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src', help='100K source CSV')
    ap.add_argument('out_base', help='输出 base CSV')
    ap.add_argument('out_hot_ul', help='输出 hot insert CSV')
    ap.add_argument('--n-base', type=int, default=20000)
    ap.add_argument('--n-hot', type=int, default=10000)
    ap.add_argument('--n-buckets', type=int, default=50)
    ap.add_argument('--hot-buckets', default='24,25,26')
    ap.add_argument('--seed', type=int, default=42)
    ap.add_argument('--template-query', default=None,
                    help='模板查询文件路径；提供时会同步生成热查询')
    ap.add_argument('--out-hot-query', default=None,
                    help='热查询输出路径；与 --template-query 配套')
    args = ap.parse_args()

    if (args.template_query is None) != (args.out_hot_query is None):
        ap.error('--template-query 与 --out-hot-query 必须同时提供')

    rng = random.Random(args.seed)
    rows = load_points(args.src)
    print(f"[info] loaded {len(rows)} rows from {args.src}", file=sys.stderr)
    if len(rows) < args.n_base + 1:
        ap.error(f'src 行数 {len(rows)} 小于 n_base={args.n_base}')

    # 给所有点附加 z-value，统一保留 (x, y, id, z) 元组
    pts = [(x, y, i, z_of(x, y)) for (x, y, i) in rows]

    base = pts[:args.n_base]
    pool = pts[args.n_base:]

    base_sorted = sorted(base, key=lambda r: r[3])
    buckets = build_buckets(base_sorted, args.n_buckets)

    hot_idx = [int(s) for s in args.hot_buckets.split(',')]
    bad = [k for k in hot_idx if not (0 <= k < args.n_buckets)]
    if bad:
        ap.error(f'hot-buckets 越界: {bad} (n-buckets={args.n_buckets})')
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
