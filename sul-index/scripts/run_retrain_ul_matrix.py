#!/usr/bin/env python3
"""串行执行两种插入分布；每个跑次连续记录 11 档 ul 检查点。"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
UL_VALUES = (20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70)


def data_rows(path):
    with Path(path).open(newline='') as f:
        return sum(1 for row in csv.reader(f)
                   if row and row[0].strip() and not row[0].lstrip().startswith('#'))


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def validate_insert(path, distribution, base):
    path = Path(path)
    if data_rows(path) != 20000:
        raise ValueError(f'{path}: insert file must contain exactly 20000 data rows')
    manifest_path = Path(str(path) + '.json')
    if not manifest_path.exists():
        raise ValueError(f'{path}: missing manifest {manifest_path}')
    manifest = json.loads(manifest_path.read_text())
    if manifest.get('mode') != distribution or manifest.get('insert_count') != 20000:
        raise ValueError(f'{manifest_path}: distribution/count mismatch')
    if manifest.get('insert_sha256') != sha256(path):
        raise ValueError(f'{manifest_path}: insert hash mismatch')
    if manifest.get('base_sha256') != sha256(base):
        raise ValueError(f'{manifest_path}: base hash mismatch')


def write_plan(path, rows):
    fields = ['distribution', 'max_ul_pct', 'ul_checkpoints', 'target_updates', 'run_dir', 'status']
    with path.open('w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', required=True, type=Path)
    ap.add_argument('--matched-insert', required=True, type=Path)
    ap.add_argument('--hotspot-insert', required=True, type=Path)
    ap.add_argument('--eval-query', required=True, type=Path)
    ap.add_argument('--monitor-query', required=True, type=Path)
    ap.add_argument('--output-root', required=True, type=Path)
    ap.add_argument('--dataset-label', required=True)
    ap.add_argument('--beta', required=True, type=float)
    ap.add_argument('--repeat-id', type=int, default=1)
    ap.add_argument('--K', type=int, default=1024)
    ap.add_argument('--err', type=int, default=4)
    ap.add_argument('--sl-pct', type=float, default=.25)
    ap.add_argument('--monitor-warmup-rounds', type=int, default=1)
    ap.add_argument('--monitor-repeats', type=int, default=3)
    ap.add_argument('--eval-warmup-rounds', type=int, default=1)
    ap.add_argument('--eval-repeats', type=int, default=3)
    ap.add_argument('--check-every', type=int, default=200)
    ap.add_argument('--coarse-check-every', type=int, default=0)
    ap.add_argument('--fine-threshold-ratio', type=float, default=.8)
    ap.add_argument('--executable', type=Path, default=ROOT/'build'/'sul_update')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args(argv)

    try:
        if not math.isfinite(args.beta) or args.beta <= 0:
            raise ValueError('beta must be finite and positive')
        if args.repeat_id < 1:
            raise ValueError('repeat-id must be positive')
        if args.check_every < 1:
            raise ValueError('check-every must be positive')
        if args.coarse_check_every and args.coarse_check_every < args.check_every:
            raise ValueError('coarse-check-every must be zero or >= check-every')
        if not math.isfinite(args.fine_threshold_ratio) or not 0 < args.fine_threshold_ratio <= 1:
            raise ValueError('fine-threshold-ratio must be in (0,1]')
        for path in [args.base, args.eval_query, args.monitor_query, args.executable]:
            if not path.exists():
                raise ValueError(f'missing input: {path}')
        if data_rows(args.base) != 20000:
            raise ValueError(f'{args.base}: base file must contain exactly 20000 data rows')
        validate_insert(args.matched_insert, 'matched', args.base)
        validate_insert(args.hotspot_insert, 'hotspot', args.base)
        if args.output_root.exists():
            raise ValueError(f'output root already exists: {args.output_root}')

        runs = []
        commands = []
        beta_tag = format(args.beta, 'g').replace('.', 'p')
        checkpoint_text = ','.join(map(str, UL_VALUES))
        for distribution, insert in [('matched', args.matched_insert),
                                     ('hotspot', args.hotspot_insert)]:
            run_dir = args.output_root / (
                f'{args.dataset_label}_{distribution}_beta{beta_tag}_ul20-70_r{args.repeat_id}')
            command = [str(args.executable), str(args.base), str(insert), str(args.eval_query),
                       str(args.K), str(args.err), str(UL_VALUES[-1]),
                       '--monitor-query', str(args.monitor_query),
                       '--sl-pct', format(args.sl_pct, 'g'), '--beta', format(args.beta, 'g'),
                       '--check-every', str(args.check_every),
                       '--ul-checkpoints', checkpoint_text,
                       '--distribution', distribution,
                       '--dataset-label', args.dataset_label, '--repeat-id', str(args.repeat_id),
                       '--phase', 'adaptive', '--warmup-rounds', str(args.eval_warmup_rounds),
                       '--eval-repeats', str(args.eval_repeats),
                       '--monitor-warmup-rounds', str(args.monitor_warmup_rounds),
                       '--monitor-repeats', str(args.monitor_repeats),
                       '--run-dir', str(run_dir)]
            if args.coarse_check_every:
                command.extend(['--coarse-check-every', str(args.coarse_check_every),
                                '--fine-threshold-ratio', format(args.fine_threshold_ratio, 'g')])
            runs.append({'distribution': distribution, 'max_ul_pct': UL_VALUES[-1],
                         'ul_checkpoints': checkpoint_text,
                         'target_updates': 200 * UL_VALUES[-1], 'run_dir': str(run_dir),
                         'status': 'pending'})
            commands.append(command)

        if args.dry_run:
            for command in commands:
                print(shlex.join(command))
            return 0

        args.output_root.mkdir(parents=True)
        plan_path = args.output_root / 'matrix_plan.csv'
        write_plan(plan_path, runs)
        for row, command in zip(runs, commands):
            row['status'] = 'running'
            write_plan(plan_path, runs)
            print(f"[matrix] distribution={row['distribution']} ul=20..{row['max_ul_pct']}%",
                  flush=True)
            result = subprocess.run(command)
            if result.returncode:
                row['status'] = f'failed:{result.returncode}'
                write_plan(plan_path, runs)
                return result.returncode
            row['status'] = 'complete'
            write_plan(plan_path, runs)
        print(f'[complete] matrix={args.output_root}', flush=True)
        return 0
    except (ValueError, OSError, json.JSONDecodeError) as exc:
        ap.error(str(exc))


if __name__ == '__main__':
    sys.exit(main())
