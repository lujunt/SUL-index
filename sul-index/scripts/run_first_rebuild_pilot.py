#!/usr/bin/env python3
"""串行运行四个数据集的原分布/热点 beta=1 首次重构预实验。"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SPECS = (
    ('SKE', 'datasets/SKE_20000_4_2.csv'),
    ('ABUS', 'datasets/ABUS_20000_0_2.csv'),
    ('USAC', 'datasets/USAC_20000_0_2.csv'),
    ('MBF', 'datasets/MBF_20000_0_2.csv'),
)


def data_rows(path):
    with Path(path).open(newline='') as handle:
        return sum(1 for row in csv.reader(handle)
                   if row and row[0].strip() and not row[0].lstrip().startswith('#'))


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def write_plan(path, rows):
    columns = ['dataset', 'distribution', 'max_updates', 'run_dir', 'status']
    with path.open('w', newline='') as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--repeat-id', type=int, default=1)
    parser.add_argument('--executable', type=Path, default=ROOT/'build'/'sul_update')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args(argv)

    try:
        if args.repeat_id < 1:
            raise ValueError('repeat-id must be positive')
        if not args.executable.exists():
            raise ValueError(f'missing executable: {args.executable}')
        if args.output_root.exists():
            raise ValueError(f'output root already exists: {args.output_root}')

        rows, commands = [], []
        for dataset, base_text in SPECS:
            base = ROOT/base_text
            eval_query = ROOT/f'query/{dataset}_20000_dim2_0.25.csv'
            monitor = ROOT/f'query/retrain_v2_monitor_pilot/{dataset}_20000_dim2_0.25.csv'
            if data_rows(base) != 20000 or data_rows(eval_query) != 100 or data_rows(monitor) < 400:
                raise ValueError(f'{dataset}: invalid base/evaluation/monitor row count')
            for distribution, suffix in [('matched', 'matched_source'), ('hotspot', 'hotspot')]:
                insert = ROOT/f'datasets/retrain_v2/pilot_beta1/{dataset}_{suffix}_20000.csv'
                manifest_path = Path(str(insert)+'.json')
                if data_rows(insert) != 20000 or not manifest_path.exists():
                    raise ValueError(f'{dataset}/{distribution}: invalid insert input')
                manifest = json.loads(manifest_path.read_text())
                if (manifest.get('mode') != distribution or manifest.get('insert_count') != 20000
                        or manifest.get('insert_sha256') != sha256(insert)
                        or manifest.get('base_sha256') != sha256(base)):
                    raise ValueError(f'{dataset}/{distribution}: manifest mismatch')
                run_dir = args.output_root/f'{dataset}_{distribution}_beta1_first_r{args.repeat_id}'
                command = [str(args.executable), str(base), str(insert), str(eval_query),
                           '1024', '4', '100', '--monitor-query', str(monitor),
                           '--sl-pct', '0.25', '--beta', '1', '--check-every', '200',
                           '--coarse-check-every', '1000', '--fine-threshold-ratio', '0.8',
                           '--max-rebuilds', '1', '--pilot-timing-only', '1',
                           '--monitor-warmup-rounds', '1', '--monitor-repeats', '1',
                           '--warmup-rounds', '0', '--eval-repeats', '1',
                           '--distribution', distribution, '--dataset-label', dataset,
                           '--repeat-id', str(args.repeat_id), '--phase', 'adaptive',
                           '--run-dir', str(run_dir)]
                rows.append({'dataset': dataset, 'distribution': distribution,
                             'max_updates': 20000, 'run_dir': str(run_dir), 'status': 'pending'})
                commands.append(command)

        if args.dry_run:
            for command in commands:
                print(shlex.join(command))
            return 0

        args.output_root.mkdir(parents=True)
        plan = args.output_root/'pilot_plan.csv'
        write_plan(plan, rows)
        for row, command in zip(rows, commands):
            row['status'] = 'running'
            write_plan(plan, rows)
            print(f"[pilot] dataset={row['dataset']} distribution={row['distribution']}", flush=True)
            result = subprocess.run(command)
            if result.returncode:
                row['status'] = f'failed:{result.returncode}'
                write_plan(plan, rows)
                return result.returncode
            with (Path(row['run_dir'])/'events.csv').open(newline='') as handle:
                events = list(csv.DictReader(handle))
            row['status'] = 'complete_first_rebuild' if events else 'complete_no_rebuild'
            write_plan(plan, rows)
        print(f'[complete] pilot={args.output_root}', flush=True)
        return 0
    except (ValueError, OSError, json.JSONDecodeError) as exc:
        parser.error(str(exc))


if __name__ == '__main__':
    sys.exit(main())
