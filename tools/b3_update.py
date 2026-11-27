"""Updates for a release install (one with a VERSION file at its root), from this project's GitHub releases.

check(root)       asks GitHub's public releases API whether a newer release is out (the launcher does this once at
                  start when Settings > System > Check for updates is on). Only the request goes out: a generic
                  User-Agent, no machine or user details.
prepare(...)      the player said "Update now": downloads this platform's package, checks it against the release's
                  .sha256, and unpacks it into private/update/new.
apply_and_restart(...)  writes a small helper script and starts it detached; the launcher then quits. The helper waits
                  for the launcher to exit, copies the new files over the install, removes private/update and starts
                  the launcher again with --after-update, which rebuilds the game (the game is compiled from the
                  player's own dump, so new runtime code reaches it only through a rebuild).

Nothing under private/ is in a package, so settings, saves, the dump's data and the built game stay as they are. A file
the new release no longer has is left in place (harmless: nothing refers to it).
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

REPO = 'aoxzh/bayo3-recomp'
RELEASES = f'https://github.com/{REPO}/releases/latest'
API = f'https://api.github.com/repos/{REPO}/releases/latest'
AUTHOR_URL = 'https://x.com/aoxzh'  # the author's X (Twitter) profile: the launchers' Follow link
USER_AGENT = 'bayo3-recomp-launcher'


def version_tuple(v: str) -> tuple[int, int, int] | None:
    m = re.match(r'v?(\d+)\.(\d+)\.(\d+)', (v or '').strip())
    return (int(m[1]), int(m[2]), int(m[3])) if m else None


def newer(latest: str, current: str) -> bool:
    a, b = version_tuple(latest), version_tuple(current)
    return bool(a and b and a > b)


def installed_version(root) -> str | None:
    """The release this install came from (its VERSION file), or None for a source checkout."""
    f = Path(root) / 'VERSION'
    try:
        return f.read_text(encoding='utf-8', errors='replace').strip() or None
    except OSError:
        return None


def package_suffix(platform: str | None = None) -> str:
    platform = platform or sys.platform
    return '-windows-x64.zip' if platform == 'win32' else '-linux-x86_64.tar.gz'


def _get(url: str, timeout: float):
    req = urllib.request.Request(url, headers={'Accept': 'application/vnd.github+json', 'User-Agent': USER_AGENT})
    return urllib.request.urlopen(req, timeout=timeout)


def latest_release(timeout: float = 6.0) -> dict | None:
    """{'tag', 'url', 'assets': {name: (download url, size)}} of the latest release, or None when unreachable."""
    try:
        with _get(API, timeout) as r:
            data = json.loads(r.read().decode('utf-8'))
    except (OSError, ValueError):
        return None
    tag = data.get('tag_name') or ''
    if not version_tuple(tag):
        return None
    assets = {a.get('name', ''): (a.get('browser_download_url', ''), int(a.get('size') or 0))
              for a in data.get('assets') or []}
    return {'tag': tag, 'url': data.get('html_url') or RELEASES, 'assets': assets}


def check(root) -> dict | None:
    """The latest release (latest_release) when it is newer than this install, else None."""
    current = installed_version(root)
    if not current:
        return None
    found = latest_release()
    return found if found and newer(found['tag'], current) else None


def package_asset(release: dict, platform: str | None = None) -> tuple[str, str, int] | None:
    """(name, url, size) of this platform's package in a release, or None."""
    suffix = package_suffix(platform)
    for name, (url, size) in release.get('assets', {}).items():
        if name.endswith(suffix):
            return name, url, size
    return None


def download(url: str, dest: Path, progress=None, timeout: float = 30.0) -> None:
    """url into dest (through dest.part), calling progress(done_bytes, total_bytes) as it goes."""
    part = dest.with_name(dest.name + '.part')
    with _get(url, timeout) as r, open(part, 'wb') as f:
        total = int(r.headers.get('Content-Length') or 0)
        done = 0
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
            done += len(chunk)
            if progress:
                progress(done, total)
    part.replace(dest)


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def _unpack(archive: Path, into: Path) -> Path:
    """The package into `into`; returns its top folder (Bayonetta3Recomp-<version>-<platform>)."""
    if into.exists():
        shutil.rmtree(into)
    into.mkdir(parents=True)
    if archive.name.endswith('.zip'):
        with zipfile.ZipFile(archive) as z:
            base = into.resolve()
            for name in z.namelist():  # no entry may leave the folder
                if not (into / name).resolve().is_relative_to(base):
                    raise ValueError(f'the package has a bad path: {name}')
            z.extractall(into)
    else:
        with tarfile.open(archive) as t:
            t.extractall(into, filter='data')  # refuses paths and links that leave the folder; keeps exec bits
    tops = [p for p in into.iterdir() if p.is_dir()]
    if len(tops) != 1 or not (tops[0] / 'VERSION').is_file() or not (tops[0] / 'tools' / 'b3_launcher.py').is_file():
        raise ValueError('the package does not look like a Bayonetta 3 Recomp release')
    return tops[0]


def prepare(root, release: dict, progress=None, platform: str | None = None) -> Path:
    """Download, check and unpack this platform's package of `release`; returns the unpacked release folder.
    progress(done_bytes, total_bytes) during the download. Raises with a plain message when something is wrong."""
    asset = package_asset(release, platform)
    if not asset:
        raise ValueError(f'{release["tag"]} has no package for this system')
    name, url, size = asset
    sums = release['assets'].get(name + '.sha256')
    if not sums:
        raise ValueError(f'{release["tag"]} has no checksum for {name}')
    stage = Path(root) / 'private' / 'update'
    stage.mkdir(parents=True, exist_ok=True)
    archive = stage / name
    expected_sum = stage / (name + '.sha256')
    download(sums[0], expected_sum)
    expected = expected_sum.read_text(encoding='utf-8', errors='replace').split()[0].lower()
    if not (archive.is_file() and sha256_of(archive) == expected):  # a finished earlier download is reused
        download(url, archive, progress)
        got = sha256_of(archive)
        if got != expected:
            archive.unlink(missing_ok=True)
            raise ValueError(f'the download does not match its checksum ({got[:12]} instead of {expected[:12]}); '
                             'try again')
    new = _unpack(archive, stage / 'new')
    archive.unlink(missing_ok=True)  # unpacked: the archive is not needed again
    return new


WIN_HELPER = r'''@echo off
rem Bayonetta 3 Recomp update (tools/b3_update.py): wait for the launcher to close, copy the new release over this
rem install, then start the launcher again, which rebuilds the game.
setlocal
:wait
tasklist /FI "PID eq {pid}" 2>nul | find "{pid}" >nul && (timeout /t 1 /nobreak >nul & goto wait)
robocopy "{new}" "{root}" /E /R:5 /W:1 /NFL /NDL /NJH /NJS /NP >"{log}" 2>&1
if %ERRORLEVEL% GEQ 8 (
    start "" "{python}" "{root}\tools\b3_launcher.py" --update-failed
    exit /b 1
)
start "" "{python}" "{root}\tools\b3_launcher.py" --after-update
rmdir /s /q "{stage}\new" 2>nul
endlocal
'''

SH_HELPER = r'''#!/bin/sh
# Bayonetta 3 Recomp update (tools/b3_update.py): wait for the launcher to close, copy the new release over this
# install, then start the launcher again, which rebuilds the game.
while kill -0 {pid} 2>/dev/null; do sleep 1; done
if cp -a "{new}/." "{root}/" >"{log}" 2>&1; then
    rm -rf "{stage}/new"
    exec "{root}/Bayonetta3Recomp.sh" --after-update
fi
exec "{root}/Bayonetta3Recomp.sh" --update-failed
'''


def _python_for_helper() -> str:
    """The Python that starts the launcher again: pythonw on Windows (no console window)."""
    exe = Path(sys.executable)
    w = exe.with_name('pythonw.exe')
    return str(w if w.is_file() else exe)


def apply_and_restart(root, new: Path, platform: str | None = None) -> Path:
    """Start the helper that puts `new` in place once this process (the launcher) has exited. The caller quits right
    after. Returns the helper's path."""
    platform = platform or sys.platform
    root = Path(root).resolve()
    stage = root / 'private' / 'update'
    log = stage / 'apply.log'
    if platform == 'win32':
        helper = stage / 'apply.cmd'
        helper.write_text(WIN_HELPER.format(pid=os.getpid(), new=new, root=root, log=log, stage=stage,
                                            python=_python_for_helper()).replace('\n', '\r\n'), encoding='utf-8')
        # Its own process group, no console window: it outlives the launcher that starts it.
        flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0) | getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)
        subprocess.Popen(['cmd', '/c', str(helper)], cwd=str(stage), creationflags=flags, close_fds=True)
    else:
        helper = stage / 'apply.sh'
        helper.write_text(SH_HELPER.format(pid=os.getpid(), new=new, root=root, log=log, stage=stage), encoding='utf-8')
        helper.chmod(0o755)
        subprocess.Popen(['/bin/sh', str(helper)], cwd=str(stage), start_new_session=True, close_fds=True,
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return helper
