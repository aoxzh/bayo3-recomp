#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A bug report in one file: the game's logs, crash reports, the settings file and a short system summary, zipped into
private/reports/b3-report-<time>.zip for an issue. Never anything from the game (no dump, no generated code, no
shaders, no saves).

    python tools/b3_report.py            # prints the path of the new zip
The launcher's "Make a bug report" entry calls make_report() and opens the folder.
"""
from __future__ import annotations

import os
import platform
import re
import sys
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import b3_launcher_config as cfg  # noqa: E402

MAX_FILE = 16 << 20  # a log over this keeps only its end: what led to the problem
NEWEST = 3           # setup/update logs and crash reports: the newest few
# Lines of the game log that describe the machine (GPU, driver, memory) for the summary.
SYSTEM_LINES = re.compile(r'^(nvn/vk: (GPU \d|using GPU|extensions (present|missing))|memory: guard|'
                          r'Bayonetta 3 \(Switch recomp\)|B3 runtime:|android: profile)')


def _tail(path: Path) -> bytes:
    size = path.stat().st_size
    with path.open('rb') as f:
        if size > MAX_FILE:
            f.seek(size - MAX_FILE)
            return b'[... the first %d bytes are cut ...]\n' % (size - MAX_FILE) + f.read()
        return f.read()


def _newest(folder: Path, patterns, count=NEWEST):
    found = []
    for pattern in patterns:
        found += [p for p in folder.glob(pattern) if p.is_file()]
    return sorted(set(found), key=lambda p: p.stat().st_mtime, reverse=True)[:count]


def collect(root, host=None):
    """[(name in the zip, file)] for this install, in a stable order."""
    p = cfg.paths(root, host)
    log = p['log']
    files = []
    for candidate in (log, log.with_name(log.name + '.prev'), log.with_name(log.stem + '-prog1' + log.suffix)):
        if candidate.is_file():
            files.append(('logs/' + candidate.name, candidate))
    folder = log.parent
    if folder.is_dir():
        for f in _newest(folder, ('setup-*.log', 'update-*.log', 'patches-*.log')):
            files.append(('logs/' + f.name, f))
        for f in _newest(folder, ('crash-*.txt',)):
            files.append(('crash/' + f.name, f))
    if p['conf'].is_file():
        files.append(('b3.conf', p['conf']))
    return files


def summary(root, files, host=None):
    lines = [
        'Bayonetta 3 Recomp bug report',
        f'made: {time.strftime("%Y-%m-%d %H:%M:%S %z")}',
        f'host: {cfg.host_platform() if host is None else host}',
        f'os: {platform.platform()}',
        f'machine: {platform.machine()}, {os.cpu_count()} logical CPUs',
        f'python: {platform.python_version()}',
    ]
    version = Path(root) / 'VERSION'
    if version.is_file():
        lines.append(f'release: {version.read_text(encoding="utf-8", errors="replace").strip()}')
    log = cfg.paths(root, host)['log']
    if log.is_file():
        seen = []
        with log.open('r', encoding='utf-8', errors='replace') as f:
            for i, line in enumerate(f):
                if i > 400:
                    break
                if SYSTEM_LINES.match(line) and line not in seen:
                    seen.append(line)
        if seen:
            lines.append('from the game log:')
            lines += ['  ' + s.rstrip() for s in seen]
    lines.append('files:')
    lines += [f'  {name} ({path.stat().st_size} bytes)' for name, path in files]
    return '\n'.join(lines) + '\n'


def make_report(root, host=None, now=None) -> Path:
    root = Path(root)
    files = collect(root, host)
    out_dir = root / 'private' / 'reports'
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime('%Y%m%d-%H%M%S', time.localtime(now))
    out = out_dir / f'b3-report-{stamp}.zip'
    with zipfile.ZipFile(out, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr('system.txt', summary(root, files, host))
        for name, path in files:
            try:
                z.writestr(name, _tail(path))
            except OSError as e:  # a log the game still holds open, a file removed meanwhile
                z.writestr(name + '.error.txt', f'could not read {path}: {e}\n')
    return out


ISSUES = 'https://github.com/aoxzh/bayo3-recomp/issues/new'
GPU_LINE = re.compile(r'^nvn/vk: using GPU \d+, (.+?) \(NX_GPU')


def issue_url(root, report: Path, host=None) -> str:
    """GitHub's new-issue page for this project, its form filled in: what to write, where the zip is, and the
    version, system and GPU (nothing else: no paths, no user name). The player signs in, writes what happened, drags
    the zip in and submits."""
    from urllib.parse import urlencode
    lines = []
    version = Path(root) / 'VERSION'
    lines.append(f'- version: {version.read_text(encoding="utf-8", errors="replace").strip()}'
                 if version.is_file() else '- version: from source')
    lines.append(f'- system: {platform.system()} {platform.release()} ({platform.machine()}, '
                 f'{os.cpu_count()} logical CPUs)')
    log = cfg.paths(root, host)['log']
    if log.is_file():
        with log.open('r', encoding='utf-8', errors='replace') as f:
            for i, line in enumerate(f):
                if i > 400:
                    break
                m = GPU_LINE.match(line)
                if m:
                    lines.append(f'- GPU: {m.group(1)}')
                    break
    # The issue form (.github/ISSUE_TEMPLATE/bug.yml): What happened and Where are required there, so an issue
    # always says something; the zip's name and the details are filled in.
    report_text = (f'Drag {report.name} here. It just opened in a folder on your computer; it holds the logs and '
                   'settings, no game files.')
    return ISSUES + '?' + urlencode({'template': 'bug.yml', 'title': '[Bug] ', 'report': report_text,
                                     'device': '\n'.join(x[2:] for x in lines)})


def main():
    root = Path(__file__).resolve().parent.parent
    print(make_report(root))


if __name__ == '__main__':
    main()
