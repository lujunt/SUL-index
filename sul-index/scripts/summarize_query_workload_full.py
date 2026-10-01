#!/usr/bin/env python3
"""Validate and combine continuous query-workload hotspot records for five datasets."""
import argparse
import csv
import json
import math
from pathlib import Path


DATASETS = ('UNI', 'SKE', 'ABUS', 'USAC', 'MBF')
CHECKPOINTS = list(range(20, 101, 5))


def read_csv(path):
    with path.open(newline='') as handle:
        return list(csv.DictReader(handle))


def write_csv(path, fields, rows):
    with path.open('w', newline='') as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def close(a, b):
    return math.isclose(float(a), float(b), rel_tol=1e-9, abs_tol=1e-6)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    combined_events, combined_checkpoints = [], []
    validation = {'schema': 'query_workload_full_validation_v1', 'root': str(root),
                  'datasets': {}, 'valid': False}
    for dataset in DATASETS:
        run = root/f'{dataset}_hotspot_beta1_ul20-100_r1'
        if not run.exists() or (run/'status.txt').read_text().strip() != 'complete':
            raise ValueError(f'{dataset}: run is not complete')
        events = read_csv(run/'events.csv')
        checkpoints = read_csv(run/'adaptive_ul_checkpoints.csv')
        monitor = read_csv(run/'adaptive_monitor_samples.csv')
        eval_samples = read_csv(run/'adaptive_eval_samples.csv')
        summary = read_csv(run/'summary.csv')
        if len(summary) != 1 or summary[0]['successful_updates'] != '20000' \
                or summary[0]['Nt'] != '40000' or summary[0]['status'] != 'complete':
            raise ValueError(f'{dataset}: invalid final summary')
        if [int(row['checkpoint_ul_pct']) for row in checkpoints] != CHECKPOINTS:
            raise ValueError(f'{dataset}: expected 17 checkpoints from 20 to 100')
        if len(events) != 2 or summary[0]['rebuild_count'] != '2':
            raise ValueError(f'{dataset}: expected exactly two rebuild events')
        if len(monitor) != 15 or len(eval_samples) != 21:
            raise ValueError(f'{dataset}: unexpected raw timing sample count')
        for event in events:
            if not close(event['rho_before'], float(event['delta_W_before'])/float(event['Nt'])):
                raise ValueError(f'{dataset}: rho mismatch at event {event["event_id"]}')
            if event['time_condition_met'] != '1' or event['spi_time_condition_met'] != '1':
                raise ValueError(f'{dataset}: time condition failed at event {event["event_id"]}')
            if event['pre_eval_recall'] != '1' or event['post_eval_recall'] != '1' \
                    or event['pre_eval_precision'] != '1' or event['post_eval_precision'] != '1':
                raise ValueError(f'{dataset}: query correctness failed at event {event["event_id"]}')
            event_id = event['event_id']
            for stage, total_field, avg_field in (
                    ('check', 'monitor_pre_total_ms', 'monitor_pre_query_avg_ms'),
                    ('post_reset', 'monitor_post_total_ms', 'monitor_post_query_avg_ms')):
                values = [float(row['query_total_ms']) for row in monitor
                          if row['event_id'] == event_id and row['stage'] == stage]
                if len(values) != 3 or not close(sum(values)/3, event[total_field]) \
                        or not close(sum(values)/3/400, event[avg_field]):
                    raise ValueError(f'{dataset}: monitor mean mismatch at event {event_id}/{stage}')
            for stage, field in (('pre_rebuild', 'pre_eval_query_avg_ms'),
                                 ('post_rebuild', 'post_eval_query_avg_ms')):
                values = [float(row['query_avg_ms']) for row in eval_samples
                          if row['event_id'] == event_id and row['stage'] == stage]
                if len(values) != 3 or not close(sum(values)/3, event[field]):
                    raise ValueError(f'{dataset}: eval mean mismatch at event {event_id}/{stage}')
            if not (run/event['snapshot_path']).exists():
                raise ValueError(f'{dataset}: missing snapshot for event {event_id}')
            combined_events.append({'dataset': dataset, **event})
        for checkpoint in checkpoints:
            combined_checkpoints.append({'dataset': dataset, **checkpoint})
        validation['datasets'][dataset] = {
            'successful_updates': 20000, 'Nt': 40000, 'checkpoint_count': 17,
            'event_count': 2, 'monitor_sample_count': 15,
            'eval_sample_count': 21, 'status': 'complete'}
    event_fields = ['dataset'] + list(combined_events[0].keys())[1:]
    checkpoint_fields = ['dataset'] + list(combined_checkpoints[0].keys())[1:]
    write_csv(root/'combined_events.csv', event_fields, combined_events)
    write_csv(root/'combined_ul_checkpoints.csv', checkpoint_fields, combined_checkpoints)
    validation['event_rows'] = len(combined_events)
    validation['checkpoint_rows'] = len(combined_checkpoints)
    validation['valid'] = True
    with (root/'validation.json').open('w') as handle:
        json.dump(validation, handle, ensure_ascii=False, indent=2)
    print(f'[validated] datasets=5 events={len(combined_events)} checkpoints={len(combined_checkpoints)}')
    print(f'[output] {root}/combined_events.csv')
    print(f'[output] {root}/combined_ul_checkpoints.csv')


if __name__ == '__main__':
    main()
