#!/usr/bin/env python3
"""Create initial retraining data by splitting a 100K source dataset with a fixed seed."""
import argparse
import csv
import hashlib
import json
import random
from pathlib import Path


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', required=True)
    ap.add_argument('--output', required=True)
    ap.add_argument('--count', type=int, default=20000)
    ap.add_argument('--seed', type=int, default=42)
    args = ap.parse_args()
    source, output = Path(args.source), Path(args.output)
    manifest = Path(str(output) + '.json')
    try:
        if output.exists() or manifest.exists():
            raise ValueError('output/manifest already exists; choose a new path')
        with source.open(newline='') as f:
            rows = [row for row in csv.reader(f)
                    if row and row[0].strip() and not row[0].lstrip().startswith('#')]
        if args.count < 1 or args.count >= len(rows):
            raise ValueError(f'count must be in [1,{len(rows)-1}]')
        order = list(range(len(rows)))
        random.Random(args.seed).shuffle(order)
        selected = [rows[i] for i in order[:args.count]]
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open('x', newline='') as f:
            csv.writer(f).writerows(selected)
        metadata = {
            'schema': 'retrain_base_v1',
            'source': str(source.resolve()),
            'source_sha256': digest(source),
            'source_rows': len(rows),
            'split_seed': args.seed,
            'base_count': args.count,
            'selection': 'fixed-seed shuffle without replacement',
            'base_sha256': digest(output),
        }
        with manifest.open('x') as f:
            json.dump(metadata, f, ensure_ascii=False, indent=2)
        print(f'[prepared] {output}: N={len(selected)}, split_seed={args.seed}')
        print(f'[manifest] {manifest}')
    except (ValueError, OSError) as exc:
        ap.exit(2, f'[error] {exc}\n')


if __name__ == '__main__':
    main()
