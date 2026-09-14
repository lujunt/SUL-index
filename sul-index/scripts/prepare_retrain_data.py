#!/usr/bin/env python3
"""从同源池生成新增数据，保留既有 base，使用与 C++ 一致的坐标量化。"""
import argparse
import bisect
import csv
import hashlib
import json
import math
import random
from pathlib import Path


def read_rows(path):
    rows = []
    with Path(path).open(newline='') as f:
        for row in csv.reader(f):
            if not row or not row[0].strip() or row[0].lstrip().startswith('#'):
                continue
            coords = tuple(float(x) for x in row[:-1])
            if not 1 <= len(coords) <= 6 or not all(map(math.isfinite, coords)):
                raise ValueError(f'{path}: invalid coordinates')
            if rows and len(coords) != len(rows[0][0]):
                raise ValueError(f'{path}: inconsistent dimension')
            source_id = int(row[-1])
            if not -(2**31) <= source_id < 2**31:
                raise ValueError(f'{path}: record ID is outside int32 range')
            rows.append((coords, source_id))
    if not rows:
        raise ValueError(f'{path}: empty dataset')
    return rows


def mapping(base):
    return tuple(map(min, zip(*(p for p, _ in base)))), tuple(map(max, zip(*(p for p, _ in base))))


def quantize(coords, low, high):
    if len(coords) != len(low):
        raise ValueError('source/base dimensions differ')
    if any(v < lo or v > hi for v, lo, hi in zip(coords, low, high)):
        return None
    return tuple(min(65535, math.floor((v-lo)/(hi-lo)*65536)) if hi > lo else 0
                 for v, lo, hi in zip(coords, low, high))


def morton(coords):
    return sum(((v >> b) & 1) << (b*len(coords)+d)
               for b in range(16) for d, v in enumerate(coords))


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_query_rects(path, dim, scale=65536):
    """Read normalized query rectangles using the same quantization as C++."""
    rects = []
    with Path(path).open(newline='') as f:
        for line_no, row in enumerate(csv.reader(f), 1):
            if not row or not row[0].strip() or row[0].lstrip().startswith('#'):
                continue
            if len(row) != 2 * dim:
                raise ValueError(f'{path}: query line {line_no} has {len(row)} fields, expected {2*dim}')
            values = [float(x) for x in row]
            if not all(math.isfinite(x) and 0 <= x <= 1 for x in values):
                raise ValueError(f'{path}: query line {line_no} coordinates must be in [0,1]')
            lo = tuple(min(scale-1, math.floor(x*scale)) for x in values[:dim])
            hi = tuple(min(scale-1, math.floor(x*scale)) for x in values[dim:])
            rects.append((tuple(min(a, b) for a, b in zip(lo, hi)),
                          tuple(max(a, b) for a, b in zip(lo, hi))))
    if not rects:
        raise ValueError(f'{path}: no query rectangles')
    return rects


def query_hit_score(key, rects):
    return sum(all(lo[d] <= key[d] <= hi[d] for d in range(len(key)))
               for lo, hi in rects)


def centered_hotspot(pool, count, center):
    """Return the contiguous Z window of count points closest to the fixed center."""
    if count < 1 or count > len(pool):
        raise ValueError(f'available unique new points={len(pool)}, requested={count}')
    ordered = sorted(pool, key=lambda row: (row[2], row[1]))
    best = None
    for start in range(len(ordered) - count + 1):
        zl, zh = ordered[start][2], ordered[start + count - 1][2]
        contains_center = zl <= center <= zh
        distance = 0 if contains_center else min(abs(zl-center), abs(zh-center))
        score = (not contains_center, distance, zh-zl,
                 abs((zl+zh)//2-center), start)
        if best is None or score < best[0]:
            best = (score, start)
    start = best[1]
    return ordered[start:start+count]


def prepare(base_path, source_path, count, mode, seed, strata, hot_low, hot_high,
            hotspot_policy='fixed', query_file=None):
    base, source = read_rows(base_path), read_rows(source_path)
    low, high = mapping(base)
    base_keys = {quantize(p, low, high) for p, _ in base}
    seen = set(base_keys)
    pool = []
    excluded_domain = excluded_key = 0
    for p, original_id in source:
        key = quantize(p, low, high)
        if key is None:
            excluded_domain += 1
        elif key in seen:
            excluded_key += 1
        else:
            seen.add(key)
            pool.append((p, original_id, morton(key)))
    zbase = sorted(morton(quantize(p, low, high)) for p, _ in base)
    rng = random.Random(seed)
    metadata = {
        'schema': 'retrain_data_v2', 'base': str(Path(base_path).resolve()),
        'source': str(Path(source_path).resolve()), 'base_sha256': digest(base_path),
        'source_sha256': digest(source_path), 'mode': mode, 'seed': seed,
        'N_init': len(base), 'dim': len(low), 'source_rows': len(source),
        'base_duplicate_coordinates': len(base)-len(base_keys),
        'excluded_outside_base_domain': excluded_domain,
        'excluded_base_or_duplicate_keys': excluded_key, 'available_new_keys': len(pool),
        'normalization': {'low': low, 'high': high, 'scale': 65536},
        'synthetic_rows': 0,
    }
    if mode == 'hotspot':
        if hotspot_policy == 'query-workload':
            if query_file is None:
                raise ValueError('query-workload hotspot requires --query-file')
            if count == 'all':
                raise ValueError('query-workload hotspot requires an explicit count')
            n = int(count)
            if n < 1 or n > len(pool):
                raise ValueError(f'available unique new points={len(pool)}, requested={n}')
            rects = read_query_rects(query_file, len(low))
            scored = [(query_hit_score(quantize(row[0], low, high), rects), row)
                      for row in pool]
            positive = [item for item in scored if item[0] > 0]
            if len(positive) < n:
                raise ValueError(
                    f'query workload covers only {len(positive)} unique new points; requested={n}')
            # Prefer points queried more frequently. Random tie-breaking avoids an unrelated
            # Morton/source-order bias; the final shuffle fixes the insertion order as well.
            ranked = [(score, rng.random(), row) for score, row in positive]
            ranked.sort(key=lambda item: (-item[0], item[1]))
            chosen = ranked[:n]
            selected = [row for _, _, row in chosen]
            selected_scores = [score for score, _, _ in chosen]
            window_counts = [0] * len(rects)
            for row in selected:
                key = quantize(row[0], low, high)
                for i, (lo, hi) in enumerate(rects):
                    if all(lo[d] <= key[d] <= hi[d] for d in range(len(key))):
                        window_counts[i] += 1
            metadata.update(
                hotspot_policy=hotspot_policy,
                query_file=str(Path(query_file).resolve()),
                query_sha256=digest(query_file),
                query_count=len(rects),
                query_positive_pool_keys=len(positive),
                selected_query_covered_keys=len(selected_scores),
                selected_query_coverage_pct=100.0,
                selected_query_hit_score_min=min(selected_scores),
                selected_query_hit_score_mean=sum(selected_scores) / len(selected_scores),
                selected_query_hit_score_max=max(selected_scores),
                selected_point_window_incidences=sum(selected_scores),
                selected_windows_touched=sum(x > 0 for x in window_counts),
                selected_window_hit_min=min(window_counts),
                selected_window_hit_mean=sum(window_counts) / len(window_counts),
                selected_window_hit_max=max(window_counts))
        elif hotspot_policy == 'auto-center':
            if count == 'all':
                raise ValueError('auto-center hotspot requires an explicit count')
            n = int(count)
            center = zbase[len(zbase)//2]
            selected = centered_hotspot(pool, n, center)
            zl, zh = selected[0][2], selected[-1][2]
            capacity = sum(zl <= row[2] <= zh for row in pool)
            metadata.update(hotspot_policy=hotspot_policy, hotspot_center_z=str(center),
                            hot_z_low=str(zl), hot_z_high=str(zh),
                            available_hot_keys=capacity, available_pool_keys=len(pool))
        else:
            zl = zbase[min(len(zbase)-1, math.floor(hot_low*len(zbase)))];
            zh = zbase[min(len(zbase)-1, math.floor(hot_high*len(zbase)))]
            hot_pool = [r for r in pool if zl <= r[2] <= zh]
            metadata.update(hotspot_policy=hotspot_policy, hot_low=hot_low, hot_high=hot_high,
                            hot_z_low=str(zl), hot_z_high=str(zh), available_hot_keys=len(hot_pool))
            n = len(hot_pool) if count == 'all' else int(count)
            if n < 1 or n > len(hot_pool):
                raise ValueError(f'hotspot has {len(hot_pool)} unique new points; requested {count}. '
                                 'No synthetic fill. Use --count all, --hotspot-policy auto-center, '
                                 'expand the source, or declare a different hot interval.')
            selected = rng.sample(hot_pool, n)
    else:
        if count == 'all':
            raise ValueError('matched requires an explicit count for proportional sampling')
        n = int(count)
        if n < 1 or n > len(pool):
            raise ValueError(f'available unique new points={len(pool)}, requested={n}')
        bounds = sorted(set(zbase[min(len(zbase)-1, i*len(zbase)//strata)] for i in range(1, strata)))
        buckets = [[] for _ in range(len(bounds)+1)]
        weights = [0]*len(buckets)
        for z in zbase:
            weights[bisect.bisect_right(bounds, z)] += 1
        for row in pool:
            buckets[bisect.bisect_right(bounds, row[2])].append(row)
        # Largest-remainder allocation, preserving base bucket proportions.
        quota = [n*w//len(base) for w in weights]
        remainder_order = sorted(range(len(quota)), key=lambda i: (-(n*weights[i] % len(base)), i))
        for i in remainder_order[:n-sum(quota)]:
            quota[i] += 1
        shortage = [(i, quota[i], len(buckets[i])) for i in range(len(quota)) if quota[i] > len(buckets[i])]
        if shortage:
            raise ValueError(f'matched strata shortage (bucket, need, available): {shortage}; use fewer points or a larger source')
        selected = [r for bucket, q in zip(buckets, quota) for r in rng.sample(bucket, q)]
        metadata.update(strata=len(buckets), z_bounds=list(map(str, bounds)),
                        base_bucket_counts=weights, insert_bucket_counts=quota)
    rng.shuffle(selected)
    metadata['insert_count'] = len(selected)
    metadata['base_coordinate_overlap'] = 0
    # Source ID retained only as provenance; sul_update assigns stable internal IDs.
    return [(p, source_id) for p, source_id, _ in selected], metadata


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', required=True); ap.add_argument('--source', required=True)
    ap.add_argument('--output', required=True)
    ap.add_argument('--mode', choices=['matched', 'hotspot'], required=True)
    ap.add_argument('--count', required=True, help='新增条数；hotspot 可用 all')
    ap.add_argument('--seed', type=int, default=42)
    ap.add_argument('--strata', type=int, default=64)
    ap.add_argument('--hot-low', type=float, default=.45)
    ap.add_argument('--hot-high', type=float, default=.55)
    ap.add_argument('--hotspot-policy', choices=['fixed', 'auto-center', 'query-workload'],
                    default='fixed', help='fixed 使用给定分位区间；auto-center 选择靠近 base Z '
                    '中位数的最小连续窗口；query-workload 按监测窗口覆盖次数选择热点')
    ap.add_argument('--query-file', help='query-workload 热点使用的固定监测窗口')
    args = ap.parse_args()
    try:
        if args.strata < 1 or not 0 <= args.hot_low < args.hot_high <= 1:
            raise ValueError('invalid strata or hotspot quantiles')
        output = Path(args.output); manifest = Path(str(output)+'.json')
        if output.exists() or manifest.exists():
            raise ValueError('output/manifest already exists; choose a new path')
        rows, metadata = prepare(args.base, args.source, args.count, args.mode,
                                 args.seed, args.strata, args.hot_low, args.hot_high,
                                 args.hotspot_policy, args.query_file)
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open('x', newline='') as f:
            w = csv.writer(f)
            for p, source_id in rows:
                w.writerow([format(x, '.17g') for x in p] + [source_id])
        metadata['insert_sha256'] = digest(output)
        with manifest.open('x') as f:
            json.dump(metadata, f, ensure_ascii=False, indent=2)
        print(f'[prepared] {output}: N={len(rows)}, mode={args.mode}, synthetic=0')
        print(f'[manifest] {manifest}')
    except (ValueError, OSError) as e:
        ap.exit(2, f'[error] {e}\n')


if __name__ == '__main__':
    main()
