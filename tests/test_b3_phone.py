#!/usr/bin/env python3
"""tools/b3_phone.py without a phone: the release APK's name, and what push_tree sends (only missing or changed files;
a folder that is mostly new in one adb push)."""
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import b3_phone  # noqa: E402

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print(f'FAIL: {what}')


class FakePhone:
    def __init__(self, there):
        self.there, self.pushes, self.commands = there, [], []

    def shell(self, command, check=True):
        self.commands.append(command)
        if command.startswith('find '):
            return ''.join(f'{size} {path}\n' for path, size in self.there.items())
        return ''

    def run(self, *args, check=True, capture=True, timeout=None):
        self.pushes.append(tuple(map(str, args)))
        return ''


def main():
    check(b3_phone.apk_version(Path('Bayonetta3Recomp-v0.3.14-android-arm64.apk')) == 'v0.3.14', 'version from name')
    check(b3_phone.apk_version(Path('my.apk')) is None, 'no version from another name')
    with tempfile.TemporaryDirectory() as tmp:
        local = Path(tmp) / 'spirv'
        (local / 'sub').mkdir(parents=True)
        (local / 'a.spv').write_bytes(b'1234')
        (local / 'b.spv').write_bytes(b'12')
        (local / 'sub/c.spv').write_bytes(b'123')
        remote = '/sdcard/Bayonetta 3/spirv'
        same = FakePhone({f'{remote}/a.spv': 4, f'{remote}/b.spv': 2, f'{remote}/sub/c.spv': 3})
        b3_phone.push_tree(same, local, remote, 'shaders')
        check(not same.pushes, 'nothing sent when every file is there with its size')
        changed = FakePhone({f'{remote}/a.spv': 4, f'{remote}/b.spv': 9})
        b3_phone.push_tree(changed, local, remote, 'shaders')
        sent = sorted(p[2] for p in changed.pushes if p[0] == 'push')
        check(sent == [f'{remote}/b.spv', f'{remote}/sub/c.spv'], f'only the changed and missing files: {sent}')
        for i in range(250):
            (local / f'n{i}.spv').write_bytes(b'x')
        fresh = FakePhone({})
        b3_phone.push_tree(fresh, local, remote, 'shaders')
        check(len(fresh.pushes) == 1 and fresh.pushes[0][1].endswith('/.'), 'a mostly new folder goes in one push')
    if failures:
        print(f'b3_phone: {len(failures)} failure(s)')
        return 1
    print('b3_phone: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
