#!/usr/bin/env python3
"""tools/b3_dump.py on made-up folders: the layouts it resolves and the problems it names (no game files)."""
import json
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import b3_dump  # noqa: E402

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print(f'FAIL: {what}')


def nso(path, build_hex):
    path.parent.mkdir(parents=True, exist_ok=True)
    head = bytearray(0x100)
    head[:4] = b'NSO0'
    head[0x40:0x60] = bytes.fromhex(build_hex)
    path.write_bytes(bytes(head))


def exefs(folder, version_ok=True):
    config = json.loads((ROOT / 'config/bayonetta3-1.2.0.json').read_text(encoding='utf-8'))
    for m in b3_dump.MODULES:
        want = config['modules'][m]['build_id']
        nso(folder / m, want if version_ok else 'ab' * 32)
    (folder / 'main.npdm').write_bytes(b'META')


def romfs(folder, files=3):
    folder.mkdir(parents=True, exist_ok=True)
    for i in range(files):
        (folder / f'x_{i:03}.pkz').write_bytes(b'.')


def main():
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        # The usual layout, ExeFS/RomFS in capitals one level down, and romfs/romfs.
        exefs(t / 'a/exefs'); romfs(t / 'a/romfs')
        exefs(t / 'b/Bayonetta 3/ExeFS'); romfs(t / 'b/Bayonetta 3/RomFS')
        exefs(t / 'c/exefs'); romfs(t / 'c/romfs/romfs')
        for name in ('a', 'b', 'c'):
            e, r = b3_dump.resolve(t / name)
            check(e is not None and r is not None, f'{name}: both parts found ({e}, {r})')
        e, r = b3_dump.resolve(t / 'a/romfs')
        check(e == t / 'a/exefs' and r == t / 'a/romfs', 'the romfs folder itself picked: its parent is used')
        r = b3_dump.inspect(t / 'a', deep=False)
        check(not r['ok'] and any('cut short' in p for p in r['problems']), 'a tiny RomFS is called cut short')
        check(not any('ExeFS' in p for p in r['problems']), f'a 1.2.0 ExeFS has no problem: {r["problems"]}')
        exefs(t / 'd/exefs', version_ok=False); romfs(t / 'd/romfs')
        r = b3_dump.inspect(t / 'd', deep=False)
        check(any('not from version 1.2.0' in p for p in r['problems']), 'another version is named as such')
        romfs(t / 'e/romfs')
        r = b3_dump.inspect(t / 'e', deep=False)
        check(any('No ExeFS found' in p for p in r['problems']), 'a missing ExeFS is named')
        (t / 'f/exefs').mkdir(parents=True); (t / 'f/exefs/main').write_bytes(b'not an nso')
        (t / 'f/exefs/main.npdm').write_bytes(b'')
        romfs(t / 'f/romfs')
        r = b3_dump.inspect(t / 'f', deep=False)
        check(any('not a game module' in p for p in r['problems']), 'a non-NSO main (still packed) is named')
    if failures:
        print(f'b3_dump: {len(failures)} failure(s)')
        return 1
    print('b3_dump: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
