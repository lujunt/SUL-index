#!/usr/bin/env python3
"""Encrypted end-to-end test covering two cycles, fixed evaluation, persistence, and invalid input."""
import csv
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import statistics

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
from prepare_retrain_data import prepare, quantize, mapping, read_rows
from plot_retrain import load_run, export


def write(path, rows):
    with path.open('w', newline='') as f:
        csv.writer(f).writerows(rows)


def check_time_records(run):
    def close(actual, expected):
        assert math.isclose(float(actual), float(expected), rel_tol=1e-10, abs_tol=1e-9), (actual, expected)

    for mode, trace in run['traces'].items():
        with (run['path']/f'{mode}_monitor_samples.csv').open() as f:
            samples = list(csv.DictReader(f))
        assert len(samples) == sum(int(r['monitor_repeats']) for r in trace)
        baseline = None
        for row in trace:
            selected = [s for s in samples if all(s[k] == row[k] for k in ['stage','epoch','event_id','successful_updates'])]
            if row['stage'] in ['baseline','post_reset']:
                baseline = row
            assert row['baseline_successful_updates'] == baseline['successful_updates']
            close(row['monitor_baseline_total_ms'],baseline['monitor_query_total_ms'])
            if row.get('monitor_measurement') == 'candidate_only':
                assert row['stage'] == 'check' and row['triggered'] == '0'
                assert selected == [] and row['monitor_repeats'] == '0'
                for field in ['monitor_query_total_ms','monitor_query_avg_ms','monitor_delta_total_ms',
                              'monitor_actual_sl_pct','monitor_recall','monitor_precision',
                              'spi_filter_ms','spi_delta_ms']:
                    assert row[field] == '', (field,row[field])
                assert float(row['candidate_probe_ms']) > 0
                close(row['probe_ms'],row['candidate_probe_ms'])
                continue
            repeats = int(row['monitor_repeats'])
            assert len(selected) == repeats
            assert [int(s['round']) for s in selected] == list(range(1,repeats+1))
            assert all(s['Wt'] == row['Wt'] and s['Nt'] == row['Nt'] for s in selected)
            total = statistics.mean(float(s['query_total_ms']) for s in selected)
            assert all(float(s['query_total_ms']) > 0 for s in selected)
            close(row['monitor_query_total_ms'],total)
            close(row['monitor_query_avg_ms'],total/int(row['m']))
            close(row['monitor_delta_total_ms'],total-float(baseline['monitor_query_total_ms']))
            spi = statistics.mean(float(s['spi_filter_ms']) for s in selected)
            close(row['spi_filter_ms'],spi)
            close(row['spi_baseline_ms'],baseline['spi_filter_ms'])
            close(row['spi_delta_ms'],spi-float(baseline['spi_filter_ms']))
        with (run['path']/f'{mode}_eval_samples.csv').open() as f:
            eval_samples = list(csv.DictReader(f))
        assert all(s['eval_count'] == '100' for s in eval_samples)
        for s in eval_samples:
            close(s['query_total_ms'],100*float(s['query_avg_ms']))
        if mode == 'control':
            assert not any(s['stage'] in ['pre_rebuild','post_rebuild'] for s in eval_samples)
            continue
        for e in run['events']:
            pre = next(r for r in trace if r['triggered'] == '1' and r['event_id'] == e['event_id'])
            post = next(r for r in trace if r['stage'] == 'post_reset' and r['event_id'] == e['event_id'])
            assert e['epoch_before'] == pre['epoch']
            assert e['baseline_successful_updates'] == pre['baseline_successful_updates']
            for event_field, row, trace_field in [
                ('monitor_baseline_total_ms',pre,'monitor_baseline_total_ms'),
                ('monitor_pre_total_ms',pre,'monitor_query_total_ms'),
                ('monitor_delta_total_ms',pre,'monitor_delta_total_ms'),
                ('monitor_post_total_ms',post,'monitor_query_total_ms')]:
                close(e[event_field],row[trace_field])
            close(e['monitor_pre_query_avg_ms'],float(e['monitor_pre_total_ms'])/int(e['m']))
            close(e['monitor_post_query_avg_ms'],float(e['monitor_post_total_ms'])/int(e['m']))
            close(e['monitor_saved_total_ms'],float(e['monitor_pre_total_ms'])-float(e['monitor_post_total_ms']))
            cost = float(e['rebuild_build_ms'])
            close(e['beta_rebuild_ms'],float(e['beta'])*cost)
            close(e['monitor_delta_over_rebuild'],float(e['monitor_delta_total_ms'])/cost)
            assert int(e['time_condition_met']) == (float(e['monitor_delta_total_ms']) >= float(e['beta_rebuild_ms']))
            for event_field, row, trace_field in [
                ('spi_baseline_total_ms',pre,'spi_baseline_ms'),('spi_pre_total_ms',pre,'spi_filter_ms'),
                ('spi_delta_total_ms',pre,'spi_delta_ms'),('spi_post_total_ms',post,'spi_filter_ms')]:
                close(e[event_field],row[trace_field])
            close(e['spi_delta_avg_ms'],float(e['spi_delta_total_ms'])/int(e['m']))
            close(e['spi_saved_total_ms'],float(e['spi_pre_total_ms'])-float(e['spi_post_total_ms']))
            close(e['spi_delta_over_rebuild'],float(e['spi_delta_total_ms'])/cost)
            assert int(e['spi_time_condition_met']) == (float(e['spi_delta_total_ms']) >= float(e['beta_rebuild_ms']))
            assert e['configured_ul_pct'] == run['meta']['configured_ul_pct']
            close(e['trigger_insert_pct'],
                  100*int(e['successful_updates'])/int(run['meta']['N_init']))
            for stage, field in [('pre_rebuild','pre_eval_query_avg_ms'),('post_rebuild','post_eval_query_avg_ms')]:
                selected = [s for s in eval_samples if s['stage'] == stage and s['event_id'] == e['event_id']]
                assert len(selected) == int(run['meta']['eval_repeats'])
                assert all(s['Nt'] == e['Nt'] for s in selected)
                close(e[field],statistics.mean(float(s['query_avg_ms']) for s in selected))


def main():
    executable = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='sul-retrain-test-') as tmp:
        d=Path(tmp)
        write(d/'base.csv', [(0,0,90),(1,1,91)]+[(.5,.5,92+i) for i in range(4)])
        write(d/'insert.csv', [(i/25,i/25,100+i) for i in range(1,9)])
        eval_rows=[(0,0,1,1),(.45,.45,.55,.55),(.7,0,.8,.1),(0,0,.25,.25)]*25
        write(d/'eval.csv',eval_rows)
        write(d/'monitor.csv',[(0,0,1,1),(0,0,.9,.9)])
        command=[executable,str(d/'base.csv'),str(d/'insert.csv'),str(d/'eval.csv'),
                 '1024','4','100','--monitor-query',str(d/'monitor.csv'),
                 '--sl-pct','50','--beta','0.5','--check-every','2']
        # target ceil(6 * 100%) = 6; two cycles required, including the final check.
        result=subprocess.run(command+['--repeat-id','7','--monitor-warmup-rounds','1',
            '--monitor-repeats','3','--eval-repeats','2','--run-dir',str(d/'run')],capture_output=True,text=True)
        assert result.returncode == 0, result.stdout+result.stderr
        run=load_run(d/'run',min_events=2)
        assert len(run['events'])==2, run['events']
        assert run['meta']['base_duplicate_coordinates']=='3'
        assert run['meta']['eval_count']=='100' and run['meta']['m']=='2'
        assert run['meta']['monitor_repeats']=='3' and run['meta']['monitor_warmup_rounds']=='1'
        assert run['meta']['time_measurement_schema']=='5'
        assert run['meta']['configured_ul_pct']=='100' and run['meta']['repeat_id']=='7'
        assert run['meta']['ul_mode']=='single_target' and run['meta']['ul_checkpoints']=='100'
        for mode in run['traces']:
            with (d/'run'/f'{mode}_ul_checkpoints.csv').open() as f:
                checkpoints=list(csv.DictReader(f))
            assert len(checkpoints)==1 and checkpoints[0]['checkpoint_ul_pct']=='100'
            assert checkpoints[0]['successful_updates']=='6'
            assert all(r['check_interval'] for r in run['traces'][mode])
        check_time_records(run)
        for row in run['summary']:
            assert row['Nt']=='12' and row['eval_recall']=='1' and row['eval_precision']=='1'
            assert (d/'run'/f"{row['mode']}_final.scidx").exists()
        assert len(list((d/'run').glob('event*.scidx')))==2
        export([run],d/'export',figures=False)
        assert (d/'export'/'retrain_summary.xls').exists()
        assert (d/'export'/'threshold_table.csv').exists()
        # Rebuild count is observational: a no-trigger run still completes the configured ul budget.
        no_event=subprocess.run(command+['--beta','100','--phase','adaptive','--run-dir',str(d/'no_event')],capture_output=True,text=True)
        assert no_event.returncode==0, no_event.stdout+no_event.stderr
        no_event_run=load_run(d/'no_event')
        assert no_event_run['events'] == []
        assert no_event_run['summary'][0]['successful_updates']=='6'
        assert no_event_run['traces']['adaptive'][-1]['successful_updates']=='6'
        check_time_records(no_event_run)
        hybrid=subprocess.run(command+['--beta','100','--phase','adaptive',
            '--coarse-check-every','4','--fine-threshold-ratio','0.8',
            '--ul-checkpoints','50,100','--run-dir',str(d/'hybrid')],capture_output=True,text=True)
        assert hybrid.returncode==0, hybrid.stdout+hybrid.stderr
        hybrid_run=load_run(d/'hybrid')
        assert hybrid_run['meta']['check_schedule']=='hybrid'
        with (d/'hybrid'/'adaptive_ul_checkpoints.csv').open() as f:
            checkpoints=list(csv.DictReader(f))
        assert [r['checkpoint_ul_pct'] for r in checkpoints]==['50','100']
        assert [r['successful_updates'] for r in checkpoints]==['3','6']
        pilot=subprocess.run(command+['--phase','adaptive','--max-rebuilds','1','--pilot-timing-only','1',
            '--run-dir',str(d/'pilot')],capture_output=True,text=True)
        assert pilot.returncode==0, pilot.stdout+pilot.stderr
        pilot_run=load_run(d/'pilot',min_events=1)
        assert len(pilot_run['events'])==1
        assert int(pilot_run['summary'][0]['successful_updates']) < 6
        assert pilot_run['summary'][0]['target_reached']=='0'
        assert pilot_run['summary'][0]['stop_reason']=='max_rebuilds'
        assert pilot_run['summary'][0]['status']=='complete_pilot'
        assert pilot_run['meta']['pilot_timing_only']=='1'
        assert not list((d/'pilot').glob('*.scidx'))
        with (d/'pilot'/'adaptive_eval_samples.csv').open() as f:
            assert list(csv.DictReader(f))==[]
        assert pilot_run['events'][0]['pre_eval_query_avg_ms']==''
        for trace in run['traces'].values():
            assert all(r['monitor_measurement'] == 'candidate_only'
                       for r in trace if r['stage']=='check' and r['triggered']=='0')
        def fail(args):
            p=subprocess.run(args,capture_output=True,text=True)
            assert p.returncode!=0,p.stdout
        fail(command+['--run-dir',str(d/'run')])
        for key,value in [('--beta','nan'),('--sl-pct','0'),('--sl-pct','0.25'),('--check-every','0'),
                          ('--monitor-repeats','0'),('--monitor-repeats','101'),
                          ('--monitor-warmup-rounds','-1'),('--monitor-warmup-rounds','11')]:
            fail(command+[key,value,'--run-dir',str(d/'bad')])
        for extra in [['--coarse-check-every','1'],['--fine-threshold-ratio','0'],
                      ['--fine-threshold-ratio','1.1'],['--ul-checkpoints','75,50'],
                      ['--ul-checkpoints','50,101'],['--pilot-timing-only','1']]:
            fail(command+extra+['--run-dir',str(d/'bad')])
        for removed in ['--min-rebuilds','--stop-after-rebuilds','--full-monitor-every-check']:
            fail(command+[removed,'1','--run-dir',str(d/'bad')])
        fail(command[:7]+['1.2']+command[7:])
        write(d/'short.csv',eval_rows[:99])
        bad=command.copy();bad[3]=str(d/'short.csv');fail(bad)
        bad=command.copy();bad[6]='200';fail(bad)
        write(d/'overlap.csv',[(.5,.5,900)]*8)
        bad=command.copy();bad[2]=str(d/'overlap.csv');fail(bad)
        write(d/'outside.csv',[(-1,.5,900)]*8)
        bad=command.copy();bad[2]=str(d/'outside.csv');fail(bad)
        assert not (d/'bad').exists()
        # Preparation retains base duplicates, excludes existing coordinates and domain outliers.
        write(d/'source.csv',[(0,0,0),(1,1,1),(.5,.5,2)]+[(i/100,i/100,100+i) for i in range(1,100)])
        rows,meta=prepare(d/'base.csv',d/'source.csv','all','hotspot',42,4,0,1)
        assert meta['synthetic_rows']==0 and meta['base_duplicate_coordinates']==3
        low,high=mapping(read_rows(d/'base.csv'))
        keys=[quantize(p,low,high) for p,_ in rows]
        assert len(keys)==len(set(keys))
        assert quantize((.5,.5),low,high) not in keys
        try:
            prepare(d/'base.csv',d/'source.csv','100000','hotspot',42,4,0,1)
        except ValueError:
            pass
        else:
            raise AssertionError('insufficient hotspot pool not rejected')
        rows,meta=prepare(d/'base.csv',d/'source.csv','20','hotspot',42,4,.45,.55,'auto-center')
        assert len(rows)==20 and meta['hotspot_policy']=='auto-center'
        assert int(meta['hot_z_low']) <= int(meta['hotspot_center_z']) <= int(meta['hot_z_high'])
        write(d/'workload.csv', [(0,0,.49,.49),(.01,.01,.8,.8)])
        rows,meta=prepare(d/'base.csv',d/'source.csv','20','hotspot',42,4,0,1,
                          'query-workload',d/'workload.csv')
        assert len(rows)==20 and meta['hotspot_policy']=='query-workload'
        assert meta['selected_query_coverage_pct']==100
        assert meta['selected_query_covered_keys']==20
        assert meta['selected_point_window_incidences'] >= 20
        assert meta['selected_windows_touched'] > 0
        print(result.stdout)
        print('cipher rebuild, duplicate records, snapshot roundtrip, input rejection, export and preparation passed')


if __name__=='__main__':
    main()
