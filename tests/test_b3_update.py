"""tools/b3_update.py without the network: version order, the package for each platform, unpacking (a release's
layout, a path that leaves the folder refused, something that is not a release refused)."""
from __future__ import annotations

import io
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import b3_update as u  # noqa: E402

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print('FAIL:', what)


check(u.newer('v0.4.4', 'v0.4.3'), '0.4.4 is newer than 0.4.3')
check(u.newer('v0.10.0', 'v0.9.9'), 'numbers compare as numbers')
check(not u.newer('v0.4.3', 'v0.4.3'), 'the same version is not newer')
check(not u.newer('v0.4.3', 'dev'), 'a source checkout never updates')
release = {'tag': 'v1.0.0', 'url': '', 'assets': {
    'Bayonetta3Recomp-v1.0.0-windows-x64.zip': ('w', 1), 'Bayonetta3Recomp-v1.0.0-linux-x86_64.tar.gz': ('l', 2),
    'Bayonetta3Recomp-v1.0.0-android-arm64.apk': ('a', 3)}}
check(u.package_asset(release, 'win32')[1] == 'w', 'Windows takes the zip')
check(u.package_asset(release, 'linux')[1] == 'l', 'Linux takes the tar.gz')

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    good = tmp / 'good.zip'
    with zipfile.ZipFile(good, 'w') as z:
        z.writestr('Bayonetta3Recomp-v1.0.0-windows-x64/VERSION', 'v1.0.0\n')
        z.writestr('Bayonetta3Recomp-v1.0.0-windows-x64/tools/b3_launcher.py', '#\n')
    top = u._unpack(good, tmp / 'a')
    check(top.name == 'Bayonetta3Recomp-v1.0.0-windows-x64', 'the release folder is found')

    escape = tmp / 'escape.zip'
    with zipfile.ZipFile(escape, 'w') as z:
        z.writestr('../outside', 'x')
    try:
        u._unpack(escape, tmp / 'b')
        check(False, 'a zip path leaving the folder is refused')
    except ValueError:
        pass

    other = tmp / 'other.tar.gz'
    with tarfile.open(other, 'w:gz') as t:
        info = tarfile.TarInfo('something/readme.txt')
        info.size = 2
        t.addfile(info, io.BytesIO(b'hi'))
    try:
        u._unpack(other, tmp / 'c')
        check(False, 'an archive that is not a release is refused')
    except ValueError:
        pass

if failures:
    print(f'{len(failures)} check(s) failed')
    sys.exit(1)
print('b3_update: ok')
