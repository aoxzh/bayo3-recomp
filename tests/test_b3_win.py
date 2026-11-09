#!/usr/bin/env python3
"""tools/b3_win.py without game data: the settings file rules, the dump layouts, the junctions and the program switch.

The program switch is what makes the picture book work: program 0 exits with 86 after writing relaunch.txt
("0 1": start program 1) and its user channel, program 1 must get that channel as its launch parameter and return
the same way. Here the game is a stand-in (subprocess.run replaced), so the loop is checked without building it.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

FAILED = []


def check(ok, what):
    if not ok:
        FAILED.append(what)
        print(f'FAIL: {what}')


def eq(got, want, what):
    check(got == want, f'{what}: got {got!r}, want {want!r}')


def fresh(tmp):
    os.environ['B3_WIN_ROOT'] = str(Path(tmp) / 'win')
    sys.modules.pop('b3_win', None)
    import b3_win
    return b3_win


def test_conf_env():
    with tempfile.TemporaryDirectory() as tmp:
        w = fresh(tmp)
        conf = Path(tmp) / 'b3.conf'
        conf.write_text('resolution=1920x1080  # window\nfullscreen=1\nmute=0\npad_deadzone=0\nfps_cap=\n'
                        'NX_HANDHELD=1\nbad key=1\nfsr=off\n', encoding='utf-8')
        env = w.conf_env(conf)
        eq(env.get('NX_RESOLUTION'), '1920x1080', 'key=value becomes NX_KEY, comment dropped')
        eq(env.get('NX_FULLSCREEN'), '1', 'a switch')
        check('NX_MUTE' not in env, '0 turns a switch off')
        eq(env.get('NX_PAD_DEADZONE'), '0', '0 is a value for the pad settings')
        check('NX_FPS_CAP' not in env and 'NX_FSR' not in env, 'empty and off unset')
        eq(env.get('NX_HANDHELD'), '1', 'an NX_ name is kept as it is')
        check(not any(k.startswith('NX_BAD') for k in env), 'a bad key is ignored')


def test_dump_layouts():
    with tempfile.TemporaryDirectory() as tmp:
        w = fresh(tmp)
        a = Path(tmp) / 'a'
        (a / 'ExeFS').mkdir(parents=True)
        (a / 'RomFS').mkdir()
        exefs, romfs = w.dump_layout(a, 'b3')
        eq((exefs.name, romfs.name), ('ExeFS', 'RomFS'), 'ExeFS/RomFS')
        b = Path(tmp) / 'b'
        (b / 'romfs').mkdir(parents=True)
        (b / 'main').write_bytes(b'')
        exefs, romfs = w.dump_layout(b, 'prog1')
        eq(exefs, b, 'program 1 extracted with its ExeFS files beside romfs/')
        try:
            w.dump_layout(Path(tmp) / 'nothing', 'b3')
            check(False, 'a folder without exefs/romfs is refused')
        except w.SetupError:
            pass


def test_junctions():
    with tempfile.TemporaryDirectory() as tmp:
        w = fresh(tmp)
        t1, t2 = Path(tmp) / 't1', Path(tmp) / 't2'
        t1.mkdir(), t2.mkdir()
        (t1 / 'file').write_text('x')
        link = Path(tmp) / 'run' / 'analysis'
        w.junction(link, t1)
        check((link / 'file').exists(), 'the link reaches its target')
        w.junction(link, t2)
        check(not (link / 'file').exists() and (t1 / 'file').exists(), 'a link of ours is retargeted, not followed')
        real = Path(tmp) / 'real'
        real.mkdir()
        (real / 'save').write_text('keep')
        try:
            w.junction(real, t1)
            check(False, 'a real directory in the way is refused')
        except w.SetupError:
            pass
        check((real / 'save').exists(), 'and left alone')


def test_program_switch():
    with tempfile.TemporaryDirectory() as tmp:
        w = fresh(tmp)
        b3, p1 = w.data_roots()
        for exe in (w.WIN / 'build/b3/bayonetta3.exe', w.WIN / 'build/prog1/bayonetta3prog1.exe'):
            exe.parent.mkdir(parents=True)
            exe.write_bytes(b'')
        b3.mkdir(parents=True), p1.mkdir(parents=True)
        os.environ['B3_CONFIG'] = str(Path(tmp) / 'b3.conf')  # created from the template; never the real one
        calls = []

        def fake_run(argv, env=None, cwd=None):
            root = Path(env['NX_DATA_ROOT'])
            eq(Path(env['NX_SETTINGS_FILE']).name, 'b3.conf', 'the game is told where the panel saves')
            calls.append((Path(argv[0]).name, root.name, (root / 'launch_parameter.txt').read_bytes()
                          if (root / 'launch_parameter.txt').exists() else None))
            if len(calls) == 1:  # program 0: the book
                (root / 'relaunch.txt').write_text('0 1\r\n')  # Windows text mode
                (root / 'user_channel.bin').write_bytes(b'P' * 32)
                return subprocess.CompletedProcess(argv, 86)
            if len(calls) == 2:  # program 1: the prologue's end
                (root / 'relaunch.txt').write_text('0 0\n')
                (root / 'user_channel.bin').write_bytes(b'back')
                return subprocess.CompletedProcess(argv, 86)
            return subprocess.CompletedProcess(argv, 0)

        real_run = w.subprocess.run
        w.subprocess.run = fake_run
        try:
            rc = w.main(['play'])
        finally:
            w.subprocess.run = real_run
            os.environ.pop('B3_CONFIG', None)
        check((Path(tmp) / 'b3.conf').exists(), 'a missing settings file is created from the template')
        eq(rc, 0, 'the last program exits normally')
        eq([c[:2] for c in calls], [('bayonetta3.exe', 'b3'), ('bayonetta3prog1.exe', 'prog1'),
                                    ('bayonetta3.exe', 'b3')], 'program 0 -> 1 -> 0, each in its own data root')
        eq(calls[1][2], b'P' * 32, 'program 1 gets program 0 user channel')
        eq(calls[2][2], b'back', 'program 0 gets program 1 user channel back')
        check(not (b3 / 'relaunch.txt').exists(), 'relaunch.txt is consumed')


def test_data_step_and_relink():
    with tempfile.TemporaryDirectory() as tmp:
        w = fresh(tmp)
        dump = Path(tmp) / 'dump'
        (dump / 'exefs').mkdir(parents=True)
        (dump / 'exefs/main').write_bytes(b'M' * 10)
        (dump / 'romfs/sub').mkdir(parents=True)
        (dump / 'romfs/sub/a.pkz').write_bytes(b'A' * 100)
        settings = {'dump': str(dump)}
        eq(w.local_layout(settings, 'b3')[1], dump / 'romfs', 'before the data step the dump is read')
        w.step_data(settings, None)
        here = w.WIN / 'data/b3'
        eq((here / 'romfs/sub/a.pkz').read_bytes(), b'A' * 100, 'romfs file brought in')
        eq(os.stat(here / 'romfs/sub/a.pkz').st_nlink, 2, 'same drive: a hard link, no extra space')
        check((dump / 'romfs/sub/a.pkz').exists(), 'link mode leaves the dump whole')
        eq(w.local_layout(settings, 'b3'), (here / 'exefs', here / 'romfs'), 'later steps read the local copy')
        w.step_data(settings, None)  # done: nothing to redo
        check(w.load(here / 'complete.json', {})['romfs']['files'] == 1, 'the marker counts the files')
        for d in ('cpu/b3/analysis', 'shaders/b3/spirv'):
            (w.WIN / d).mkdir(parents=True)
        check(not w.links_ok(settings), 'no data roots yet')
        w.step_link(settings, None)
        check(w.links_ok(settings), 'linked data roots are in order')
        # a junction left pointing elsewhere (as after moving the folder) is found and made again
        w.junction(w.WIN / 'run/b3/extract/romfs', dump / 'romfs')
        check(not w.links_ok(settings), 'a data root that points outside is noticed')
        w.step_link(settings, None)
        check(w.links_ok(settings), 'relinked')


def test_update_plan():
    with tempfile.TemporaryDirectory() as tmp:
        w = fresh(tmp)
        same = {s: {'fingerprint': w.fingerprint(s)} for s in w.FINGERPRINTED}
        eq([s for s, _ in w.update_plan(same)], ['hooks', 'build', 'link'], 'unchanged tools: hooks, build, link')
        changed = dict(same, cpu={'fingerprint': 'old'})
        eq([s for s, _ in w.update_plan(changed)], ['cpu', 'build', 'link'], 'changed recompiler: cpu again')
        legacy = {'cpu': {}, 'shaders': {}}
        eq([s for s, _ in w.update_plan(legacy)], ['hooks', 'build', 'link'], 'no fingerprint yet: kept')
        check('recomp/b3_hooks.py' in w.NOT_FINGERPRINTED, 'hook table changes do not regenerate')


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith('test_') and callable(v)]
    for test in tests:
        test()
    if FAILED:
        print(f'b3_win: {len(FAILED)} failure(s)')
        return 1
    print(f'b3_win: {len(tests)} tests ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
