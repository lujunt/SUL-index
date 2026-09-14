#!/usr/bin/env python3
"""校验新重训练记录，生成两图一表和独立 SpreadsheetML 工作簿。"""
import argparse
import csv
import math
import os
from pathlib import Path
import statistics
import tempfile
from xml.sax.saxutils import escape


def read_csv(path):
    with Path(path).open(newline='') as f:
        return list(csv.DictReader(f))


def load_run(path, allow_incomplete=False, min_events=0):
    path = Path(path)
    status = (path/'status.txt').read_text().strip()
    if status != 'complete' and not (allow_incomplete and status.startswith('incomplete:')):
        raise ValueError(f'{path}: status={status}')
    meta = {r['key']: r['value'] for r in read_csv(path/'metadata.csv')}
    if meta.get('schema') != 'retrain_v2':
        raise ValueError(f'{path}: incompatible schema')
    summary = read_csv(path/'summary.csv')
    events = read_csv(path/'events.csv')
    expected_modes = ({'control', 'adaptive'} if meta.get('phase', 'both') == 'both'
                      else {meta['phase']})
    if {r['mode'] for r in summary} != expected_modes:
        raise ValueError(f'{path}: incomplete phases')
    traces = {mode: read_csv(path/f'{mode}_trace.csv') for mode in expected_modes}
    checkpoints = {}
    if meta.get('time_measurement_schema') == '5':
        for mode in expected_modes:
            rows = read_csv(path/f'{mode}_ul_checkpoints.csv')
            completed = next(int(row['successful_updates']) for row in summary if row['mode'] == mode)
            n_init = int(meta['N_init'])
            expected_pcts = [pct for pct in meta['ul_checkpoints'].split(',')
                             if math.ceil(n_init*float(pct)/100) <= completed]
            if [row['checkpoint_ul_pct'] for row in rows] != expected_pcts:
                raise ValueError(f'{path}: incomplete or reordered ul checkpoints for {mode}')
            for row in rows:
                delta = int(row['Wt']) - int(row['W0_before'])
                if delta != int(row['delta_W']) or not math.isclose(
                        float(row['rho']), delta/int(row['Nt']), abs_tol=1e-12):
                    raise ValueError(f'{path}: bad ul checkpoint arithmetic')
            checkpoints[mode] = rows
    adaptive = next((r for r in summary if r['mode'] == 'adaptive'), None)
    if ((adaptive is None and events) or
            (adaptive is not None and int(adaptive['rebuild_count']) != len(events)) or
            len(events) < min_events):
        raise ValueError(f'{path}: event count mismatch or less than {min_events}')
    adaptive_trace = traces.get('adaptive', [])
    triggers = [r for r in adaptive_trace if r['triggered']=='1']
    resets = [r for r in adaptive_trace if r['stage']=='post_reset']
    if len(triggers) != len(events) or len(resets) != len(events):
        raise ValueError(f'{path}: event/trace mismatch')
    for i, (e, t, reset) in enumerate(zip(events, triggers, resets), 1):
        delta = int(e['Wt_before']) - int(e['W0_before'])
        if int(e['event_id']) != i or delta != int(e['delta_W_before']):
            raise ValueError(f'{path}: invalid event {i}')
        if delta + 1e-10 < float(e['beta'])*int(e['Nt']):
            raise ValueError(f'{path}: event {i} did not reach threshold')
        if (e['successful_updates'] != t['successful_updates'] or e['Wt_before'] != t['Wt']
                or e['W0_before'] != t['W0'] or e['W_after'] != reset['W0']):
            raise ValueError(f'{path}: event {i} does not match trace')
        if int(reset['delta_W']) != 0 or reset['W0'] != reset['Wt']:
            raise ValueError(f'{path}: baseline not reset')
    for mode, rows in traces.items():
        baseline = None
        last_check = 0
        for row in rows:
            if row['stage'] in ['baseline','post_reset']:
                baseline = int(row['W0'])
            if int(row['W0']) != baseline:
                raise ValueError(f'{path}: baseline drift without rebuild')
            delta = int(row['Wt']) - baseline
            if delta != int(row['delta_W']) or not math.isclose(float(row['rho']), delta/int(row['Nt']), abs_tol=1e-12):
                raise ValueError(f'{path}: bad trace arithmetic')
            if mode == 'control' and row['triggered'] != '0':
                raise ValueError(f'{path}: control rebuilt')
            if meta.get('time_measurement_schema') == '5' and row['stage'] == 'check':
                if int(row['check_interval']) != int(row['successful_updates']) - last_check:
                    raise ValueError(f'{path}: invalid check interval')
                last_check = int(row['successful_updates'])
    return {'path':path, 'meta':meta, 'summary':summary, 'events':events, 'traces':traces,
            'checkpoints':checkpoints, 'status':status}


def write_csv(path, rows, columns):
    with path.open('w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=columns); w.writeheader(); w.writerows(rows)


def spreadsheet(path, tables):
    parts = ['<?xml version="1.0" encoding="UTF-8"?>',
             '<Workbook xmlns="urn:schemas-microsoft-com:office:spreadsheet" xmlns:ss="urn:schemas-microsoft-com:office:spreadsheet">']
    for title, rows, columns in tables:
        parts.append(f'<Worksheet ss:Name="{escape(title)}"><Table>')
        for values in [columns]+[[r.get(c,'') for c in columns] for r in rows]:
            parts.append('<Row>')
            for value in values:
                try:
                    numeric = math.isfinite(float(value))
                except (ValueError, TypeError):
                    numeric = False
                kind = 'Number' if numeric else 'String'
                parts.append(f'<Cell><Data ss:Type="{kind}">{escape(str(value))}</Data></Cell>')
            parts.append('</Row>')
        parts.append('</Table></Worksheet>')
    parts.append('</Workbook>')
    path.write_text('\n'.join(parts))


def export(runs, output, event_id=1, figures=True):
    output = Path(output)
    if output.exists():
        raise ValueError('output directory exists; choose a new directory')
    signature = ('distribution','N_init','K','err','dim','sl_pct','beta','check_every',
                 'coarse_check_every','fine_threshold_ratio','check_schedule','ul_mode','ul_checkpoints','max_rebuilds',
                 'warmup_rounds','eval_repeats','time_measurement_schema','monitor_schedule',
                 'monitor_warmup_rounds','monitor_repeats')
    configs = {tuple(r['meta'].get(k,'') for k in signature) for r in runs}
    if len(configs) != 1:
        raise ValueError('mixed experiment configurations; export matched/hotspot and parameter settings separately')
    if len({str(r['path'].resolve()) for r in runs}) != len(runs):
        raise ValueError('duplicate run input would double-count a repeat')
    # For repeated runs of one dataset, keep inputs and query workloads fixed.
    hashes = {}
    for r in runs:
        m = r['meta']; ds = m['dataset']
        hs = tuple(m[k] for k in ['base_fnv1a64','insert_fnv1a64','eval_fnv1a64','monitor_fnv1a64','target_updates'])
        if ds in hashes and hs != hashes[ds]:
            raise ValueError(f'{ds}: repeats use different input/query files or update counts')
        hashes[ds] = hs
    output.mkdir(parents=True)
    summaries, events, threshold = [], [], []
    grouped = {}
    for r in runs:
        common = {'dataset':r['meta']['dataset'],'run':r['path'].name,'run_status':r['status']}
        summaries.extend(dict(common, **row) for row in r['summary'])
        events.extend(dict(common, **row) for row in r['events'])
        selected = next((e for e in r['events'] if int(e['event_id']) == event_id), None)
        grouped.setdefault(common['dataset'], []).append(selected)
    mean = statistics.mean
    std = lambda values: statistics.stdev(values) if len(values)>1 else 0
    for ds, selected in grouped.items():
        measured = [e for e in selected if e is not None]
        row = {'dataset':ds,'event_id':event_id,'runs':len(selected),'triggered_runs':len(measured),
               'C_rbd_ms':'','C_rbd_std_ms':'','trg_point_pct':'','delta_W':'',
               'beta':runs[0]['meta']['beta'],'pre_eval_ms':'','post_eval_ms':'','pre_std_ms':'','post_std_ms':''}
        if measured:
            for field, source in [('C_rbd_ms','rebuild_build_ms'),('trg_point_pct','trigger_insert_pct'),
                                  ('delta_W','delta_W_before'),('pre_eval_ms','pre_eval_query_avg_ms'),
                                  ('post_eval_ms','post_eval_query_avg_ms')]:
                row[field] = mean(float(e[source]) for e in measured)
            for field, source in [('C_rbd_std_ms','rebuild_build_ms'),('pre_std_ms','pre_eval_query_avg_ms'),
                                  ('post_std_ms','post_eval_query_avg_ms')]:
                row[field] = std([float(e[source]) for e in measured])
        threshold.append(row)
    summary_columns = list(summaries[0])
    event_columns = list(events[0]) if events else ['dataset','run','event_id']
    table_columns = list(threshold[0])
    for name, rows, columns in [('summary',summaries,summary_columns),('events',events,event_columns),('threshold_table',threshold,table_columns)]:
        write_csv(output/f'{name}.csv', rows, columns)
    spreadsheet(output/'retrain_summary.xls', [('Runs',summaries,summary_columns),('Events',events,event_columns),('Threshold',threshold,table_columns)])
    if figures:
        os.environ.setdefault('MPLCONFIGDIR', str(Path(tempfile.gettempdir())/'sul-matplotlib'))
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        fig, axes = plt.subplots(len(runs),1,figsize=(8,3.5*len(runs)),squeeze=False)
        for ax, r in zip(axes[:,0], runs):
            for mode, trace in r['traces'].items():
                ax.plot([float(x['insert_pct']) for x in trace], [float(x['rho']) for x in trace], '.-', label=mode)
            ax.axhline(float(r['meta']['beta']),color='black',ls='--',label='beta')
            for e in r['events']:
                ax.plot(float(e['trigger_insert_pct']),float(e['rho_before']),'rx')
            ax.set(title=f"{r['meta']['dataset']} / {r['path'].name}", xlabel='Inserted records / initial N (%)', ylabel='(Wt - W0) / Nt')
            ax.legend()
        fig.tight_layout()
        for suffix in ['png','pdf']: fig.savefig(output/f'figure_a.{suffix}',dpi=180)
        plt.close(fig)
        fig, ax = plt.subplots(figsize=(max(6,len(threshold)*1.6),4))
        for i,row in enumerate(threshold):
            if row['triggered_runs']:
                ax.bar(i-.18,row['pre_eval_ms'],.36,yerr=row['pre_std_ms'],color='#4878a8',label='Before' if i==0 else None)
                ax.bar(i+.18,row['post_eval_ms'],.36,yerr=row['post_std_ms'],color='#59a14f',label='After' if i==0 else None)
            else: ax.text(i,0,'Not triggered',ha='center',va='bottom',rotation=90)
        ax.set_xticks(range(len(threshold)),[r['dataset'] for r in threshold])
        ax.set_ylabel('Mean evaluation latency (ms/query, 100 queries)')
        ax.set_title(f'Rebuild event {event_id}; error bars: sample standard deviation')
        from matplotlib.patches import Patch
        ax.legend(handles=[Patch(color='#4878a8',label='Before'),Patch(color='#59a14f',label='After')])
        fig.tight_layout()
        for suffix in ['png','pdf']: fig.savefig(output/f'figure_b.{suffix}',dpi=180)
        plt.close(fig)
    print(f'[exported] {output}')


def main(argv=None):
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('runs',nargs='+',type=Path); ap.add_argument('--output',required=True,type=Path)
    ap.add_argument('--event',type=int,default=1); ap.add_argument('--min-events',type=int,default=0)
    ap.add_argument('--allow-incomplete',action='store_true'); ap.add_argument('--no-figures',action='store_true')
    a=ap.parse_args(argv)
    try:
        if a.event < 1 or a.min_events < 0: raise ValueError('invalid event count')
        runs=[load_run(p,a.allow_incomplete,a.min_events) for p in a.runs]
        export(runs,a.output,a.event,not a.no_figures)
    except (ValueError,OSError,KeyError,ImportError) as e:
        ap.exit(2,f'[error] {e}\n')


if __name__=='__main__':
    main()
