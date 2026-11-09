#!/usr/bin/env python3
"""The launcher's setup/update progress (tools/b3_launcher.py Launcher._task_line) from b3_win.py's own lines,
without opening a window."""
from pathlib import Path
import sys
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import b3_launcher  # noqa: E402

task_line = b3_launcher.Launcher._task_line


def run(kind, total, lines):
    s = SimpleNamespace(task_kind=kind, task_log=[], task_step='', task_done=0, task_total=total)
    seen = []
    for line in lines:
        task_line(s, line)
        seen.append((s.task_step, s.task_done, s.task_total))
    return s, seen


setup_lines = [
    'b3: log: C:\\x\\setup.log',
    'b3: deps: Python packages (venv)',
    'b3: deps: done in 40 s',
    'b3: verify: b3',
    'b3: verify: done in 3 s',
    'b3: data: b3 by link into C:\\x\\data\\b3',
    'b3: data: b3: 2 files, 15.0 GB in C:\\x (no extra space)',
    'b3: data: done in 0 s',
    'b3: cpu: generating b3 (10-40 minutes)',
]
s, seen = run('setup', 7, setup_lines)
assert (s.task_step, s.task_done, s.task_total) == ('cpu', 3, 7), (s.task_step, s.task_done, s.task_total)

update_lines = [
    'b3: update: hooks: new runtime hooks applied to the generated code in place',
    'b3: update: build: only the changed files compile again',
    'b3: update: link: the data roots',
    'b3: update: hooks: done in 4 s',
    'b3: build: bayonetta3.exe (the generated code: 10-30 minutes the first time)',
    'b3: update: build: done in 40 s',
]
s, seen = run('update', 1, update_lines)
assert s.task_total == 3, s.task_total
assert s.task_done == 2 and s.task_step == 'build', (s.task_done, s.task_step)
print('PASS launcher setup/update progress')
