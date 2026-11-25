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
    # The minimum (DeviceCheck.java has the same rules), on the phones the issues and our tests have met.
    rules = b3_phone.spec_rules
    below, _ = rules(7_700_000_000, 2_200_000, 0x403000, 'Mali-G57 MC2')  # issues #9 and #11
    check(len(below) == 2 and any('2.2 GHz' in b for b in below) and any('Mali-G57' in b for b in below),
          f'the MT6789 / Mali-G57 MC2 is below the minimum: {below}')
    check(rules(11_500_000_000, 4_320_000, 0x403000, 'Adreno 830') == ([], []), 'a Snapdragon 8 Elite passes')
    below, weak = rules(7_000_000_000, 3_600_000, 0x403000, 'Adreno 825')  # issue #10: worked, swapping 2 GB
    check(not below and len(weak) == 1 and 'memory' in weak[0], f'7 GB is a warning, not a stop: {weak}')
    below, _ = rules(3_800_000_000, 3_000_000, 0x403000, 'Adreno 740')
    check(len(below) == 1 and 'memory' in below[0], '4 GB of memory is below the minimum')
    below, _ = rules(12_000_000_000, 3_000_000, 0x402000, 'Adreno 740')
    check(len(below) == 1 and 'Vulkan' in below[0], 'Vulkan 1.2 is below the minimum')
    check(rules(8_000_000_000, 2_400_000, 0x403000, 'Adreno 619')[0] == ['Adreno 619 (Adreno 640 or newer)'],
          'an Adreno 6xx below 640 is below the minimum')
    below, weak = rules(8_000_000_000, 3_000_000, 0x403000, 'Mali-G715 MC7')
    check(not below and weak == ['Mali-G715 MC7: not tested yet'], 'a newer Mali is untested, not refused')
    check(rules(0, 0, 0, '') == ([], []), 'nothing read: nothing refused')
    for gpu, want in (('Mali-G76 MP10', 'below'), ('Mali-G52 MC2', 'below'), ('Mali-G78 MP20', 'weak'),
                      ('Mali-G68 MC4', 'weak'), ('Immortalis-G720 MC12', 'weak'), ('Maleoon 910', 'below'),
                      ('Adreno 650', 'weak'), ('Adreno 725', 'weak'), ('Adreno 750', 'ok')):
        below, weak = rules(12_000_000_000, 3_200_000, 0x403000, gpu)
        got = 'below' if below else 'weak' if weak else 'ok'
        check(got == want, f'{gpu}: {got}, expected {want}')
    if failures:
        print(f'b3_phone: {len(failures)} failure(s)')
        return 1
    print('b3_phone: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
