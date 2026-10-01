#!/usr/bin/env python3
"""Run serial beta=1 query-workload hotspot experiments at ul=20..100 on five datasets."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
DATASETS = ('UNI', 'SKE', 'ABUS', 'USAC', 'MBF')
UL_VALUES = tuple(range(20, 101, 5))


def data_rows(path):
    with Path(path).open(newline='') as handle:
        return sum(1 for row in csv.reader(handle)
                   if row and row[0].strip() and not row[0].lstrip().startswith('#'))


def sha256(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def paths(dataset):
    prefix = ROOT/'datasets'/'retrain_v3'/'query_workload'
    query = ROOT/'query'/'retrain_v3'/'query_workload'
    return {
        'base': prefix/f'{dataset}_20000_base_seed42.csv',
        'insert': prefix/f'{dataset}_hotspot_query_workload_20000.csv',
        'monitor': query/f'{dataset}_monitor'/f'{dataset}_20000_dim2_0.25.csv',
        'eval': query/f'{dataset}_eval'/f'{dataset}_20000_dim2_0.25.csv',
    }


def validate(dataset):
    item = paths(dataset)
    for path in item.values():
        if not path.exists():
            raise ValueError(f'{dataset}: missing input {path}')
    if data_rows(item['base']) != 20000 or data_rows(item['insert']) != 20000:
        raise ValueError(f'{dataset}: base/insert must each contain 20000 rows')
    if data_rows(item['monitor']) != 400 or data_rows(item['eval']) != 100:
        raise ValueError(f'{dataset}: monitor/eval must contain 400/100 rows')
    manifest_path = Path(str(item['insert']) + '.json')
    manifest = json.loads(manifest_path.read_text())
    if (manifest.get('mode') != 'hotspot'
            or manifest.get('hotspot_policy') != 'query-workload'
            or manifest.get('insert_count') != 20000
            or manifest.get('selected_query_coverage_pct') != 100.0
            or manifest.get('insert_sha256') != sha256(item['insert'])
            or manifest.get('base_sha256') != sha256(item['base'])
            or manifest.get('query_sha256') != sha256(item['monitor'])):
        raise ValueError(f'{dataset}: query-workload manifest mismatch')
    return item


def write_plan(path, rows):
    fields = ['dataset', 'distribution', 'beta', 'max_ul_pct', 'ul_checkpoints',
              'target_updates', 'run_dir', 'status', 'rebuild_count']
    with path.open('w', newline='') as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', required=True, type=Path)
    parser.add_argument('--repeat-id', type=int, default=1)
    parser.add_argument('--monitor-repeats', type=int, default=3)
    parser.add_argument('--eval-repeats', type=int, default=3)
    parser.add_argument('--executable', type=Path, default=ROOT/'build'/'sul_update')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args(argv)
    try:
        if args.repeat_id < 1 or args.monitor_repeats < 1 or args.eval_repeats < 1:
            raise ValueError('repeat IDs and measurement repeats must be positive')
        if not args.executable.exists():
            raise ValueError(f'missing executable {args.executable}')
        if args.output_root.exists() and not args.dry_run:
            raise ValueError(f'output root already exists: {args.output_root}')
        inputs = {dataset: validate(dataset) for dataset in DATASETS}
        checkpoint_text = ','.join(map(str, UL_VALUES))
        rows, commands = [], []
        for dataset in DATASETS:
            item = inputs[dataset]
            run_dir = args.output_root/f'{dataset}_hotspot_beta1_ul20-100_r{args.repeat_id}'
            command = [str(args.executable), str(item['base']), str(item['insert']),
                       str(item['eval']), '1024', '4', '100',
                       '--monitor-query', str(item['monitor']), '--sl-pct', '0.25',
                       '--beta', '1', '--check-every', '200',
                       '--coarse-check-every', '1000', '--fine-threshold-ratio', '0.8',
                       '--ul-checkpoints', checkpoint_text, '--distribution', 'hotspot',
                       '--dataset-label', dataset, '--repeat-id', str(args.repeat_id),
                       '--phase', 'adaptive', '--warmup-rounds', '1',
                       '--eval-repeats', str(args.eval_repeats),
                       '--monitor-warmup-rounds', '1',
                       '--monitor-repeats', str(args.monitor_repeats),
                       '--run-dir', str(run_dir)]
            commands.append(command)
            rows.append({'dataset': dataset, 'distribution': 'query-workload-hotspot',
                         'beta': 1, 'max_ul_pct': 100,
                         'ul_checkpoints': checkpoint_text, 'target_updates': 20000,
                         'run_dir': str(run_dir), 'status': 'pending', 'rebuild_count': ''})
        if args.dry_run:
            for command in commands:
                print(subprocess.list2cmdline(command))
            return 0
        args.output_root.mkdir(parents=True)
        plan_path = args.output_root/'experiment_plan.csv'
        write_plan(plan_path, rows)
        for row, command in zip(rows, commands):
            row['status'] = 'running'
            write_plan(plan_path, rows)
            print(f"[matrix] dataset={row['dataset']} beta=1 ul=20..100", flush=True)
            result = subprocess.run(command)
            if result.returncode:
                row['status'] = f'failed:{result.returncode}'
                write_plan(plan_path, rows)
                return result.returncode
            events_path = Path(row['run_dir'])/'events.csv'
            with events_path.open(newline='') as handle:
                row['rebuild_count'] = sum(1 for _ in csv.DictReader(handle))
            row['status'] = 'complete'
            write_plan(plan_path, rows)
        print(f'[complete] matrix={args.output_root}', flush=True)
        return 0
    except (ValueError, OSError, json.JSONDecodeError) as exc:
        parser.error(str(exc))


if __name__ == '__main__':
    sys.exit(main())
