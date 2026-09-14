#!/usr/bin/env python3
"""验证 ul 矩阵使用两条固定插入流生成两个连续检查点跑次。"""
import csv
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


UL_VALUES = [20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70]


def write_rows(path, count):
    with path.open('w', newline='') as f:
        writer = csv.writer(f)
        for i in range(count):
            writer.writerow([i, i, i])


def main():
    script = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='sul-ul-matrix-test-') as tmp:
        root = Path(tmp)
        base = root/'base.csv'
        matched = root/'matched.csv'
        hotspot = root/'hotspot.csv'
        eval_query = root/'eval.csv'
        monitor_query = root/'monitor.csv'
        executable = root/'sul_update'
        write_rows(base, 20000)
        write_rows(matched, 20000)
        write_rows(hotspot, 20000)
        write_rows(eval_query, 100)
        write_rows(monitor_query, 400)
        executable.touch()
        for path, mode in [(matched, 'matched'), (hotspot, 'hotspot')]:
            Path(str(path)+'.json').write_text(json.dumps({
                'mode':mode, 'insert_count':20000,
                'insert_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
                'base_sha256':hashlib.sha256(base.read_bytes()).hexdigest()}))
        command = [sys.executable, str(script), '--base', str(base),
                   '--matched-insert', str(matched), '--hotspot-insert', str(hotspot),
                   '--eval-query', str(eval_query), '--monitor-query', str(monitor_query),
                   '--output-root', str(root/'runs'), '--dataset-label', 'UNI',
                   '--beta', '.5', '--repeat-id', '2', '--executable', str(executable), '--dry-run']
        result = subprocess.run(command, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        commands = [shlex.split(line) for line in result.stdout.splitlines() if line.strip()]
        assert len(commands) == 2
        assert all(int(c[6]) == 70 for c in commands)
        assert all(c[c.index('--ul-checkpoints')+1] == ','.join(map(str,UL_VALUES)) for c in commands)
        assert all(c[c.index('--check-every')+1] == '200' for c in commands)
        assert all('--coarse-check-every' not in c for c in commands)
        assert all(c[c.index('--phase')+1] == 'adaptive' for c in commands)
        assert all(c[c.index('--repeat-id')+1] == '2' for c in commands)
        assert all('--stop-after-rebuilds' not in c and '--full-monitor-every-check' not in c
                   for c in commands)
        assert not (root/'runs').exists()


if __name__ == '__main__':
    main()
