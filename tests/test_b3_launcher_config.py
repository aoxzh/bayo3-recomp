#!/usr/bin/env python3
"""Contract tests for the launcher's settings model (tools/b3_launcher_config.py).

WHY THESE CHECKS EXIST
  The launcher and scripts/b3_play.sh both read private/b3.conf, and a player edits the same file by hand. A model that
  reorders lines, drops a comment, keeps a stale duplicate, or exports a switch b3_play.sh would skip turns a working
  launch into a silent misconfiguration (window at 4K, handheld off, pad deadzone 0). These checks pin the file format
  against the script's own rules, so the GUI, the command line and the script cannot drift apart.

Run: python3 tests/test_b3_launcher_config.py     (ctest name: b3_launcher_config)
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

import b3_launcher_config as cfg  # noqa: E402

FAILED = []


def _bash():
    r"""A bash that actually runs the scripts, which is not always the `bash` found on PATH.

    On Git for Windows, PATH resolves `bash` to the WSL launcher (C:\Windows\system32\bash.exe) before the Git Bash
    that implements the shell these scripts are written in; the stub exits 1 with "no installed distributions" and
    the check would fail for a reason unrelated to the contract. Probe by running each candidate, as
    tests/test_cross_platform_harness.py does, and let B3_BASH override. The probe opens this file by its path: once
    WSL has a distribution its bash answers `echo ok` too, but reads a C:\... argument as C:Users...
    """
    import re
    candidates = [os.environ.get('B3_BASH'), 'bash', 'bash.exe', '/usr/bin/bash', '/bin/bash']
    for entry in os.environ.get('PATH', '').split(os.pathsep):
        entry = entry.strip().strip('"')
        if not entry:
            continue
        candidates.append(os.path.join(entry, 'bash.exe'))
        candidates.append(os.path.join(entry, 'usr', 'bin', 'bash.exe'))
        if re.match(r'^/[A-Za-z]/', entry):   # Git Bash reports PATH in POSIX spelling
            candidates.append(entry[1].upper() + ':' + entry[2:].replace('/', '\\') + '\\bin\\bash.exe')
    git = shutil.which('git')
    if git:
        root = os.path.dirname(os.path.dirname(os.path.abspath(git)))
        candidates += [os.path.join(root, 'bin', 'bash.exe'), os.path.join(root, 'usr', 'bin', 'bash.exe')]
    seen = set()
    for candidate in candidates:
        if not candidate or candidate in seen:
            continue
        seen.add(candidate)
        try:
            probe = subprocess.run([candidate, '-c', '[ -f "$1" ] && echo ok', 'bash', str(Path(__file__).resolve())],
                                   capture_output=True, timeout=60, encoding='utf-8', errors='replace')
        except (OSError, subprocess.SubprocessError):
            continue
        if probe.returncode == 0 and probe.stdout.strip() == 'ok':
            return candidate
    raise RuntimeError('no working bash interpreter found (set B3_BASH=/path/to/bash)')


def check(ok, why):
    if not ok:
        FAILED.append(why)
        print(f'FAIL: {why}')


def eq(actual, expected, why):
    check(actual == expected, f'{why}: got {actual!r}, want {expected!r}')


def test_parse_preserves_comments_and_unknown_keys():
    text = '# header\nresolution=1600x900   # inline\n\nweird_key=1\n  spaced = 2  \n'
    conf = cfg.Conf.parse(text)
    eq(conf.get('resolution'), '1600x900', 'inline comment is not part of the value')
    eq(conf.get('weird_key'), '1', 'unknown key kept')
    eq(conf.get('spaced'), '2', 'whitespace around key and value stripped')
    rendered = conf.render()
    check('# header' in rendered, 'comment line survives')
    check('# inline' in rendered, 'inline comment survives')
    check('weird_key=1' in rendered, 'unknown key survives a render')
    eq(conf.render(), cfg.Conf.parse(rendered).render(), 'render is a fixed point')


def test_last_duplicate_wins_like_the_script():
    # scripts/b3_play.sh exports every line in order, so a later line overrides an earlier one.
    conf = cfg.Conf.parse('resolution=1280x720\nresolution=1920x1080\n')
    eq(conf.get('resolution'), '1920x1080', 'last duplicate wins')
    # Writing collapses the duplicates and keeps the meaning.
    conf.set('resolution', '1600x900')
    eq(conf.render(), 'resolution=1600x900\n', 'set collapses duplicates')
    eq(conf.get('resolution'), '1600x900', 'set is what the runtime then reads')


def test_set_rewrites_in_place_and_appends_new_keys():
    conf = cfg.Conf.parse('# top\nresolution=1600x900\n# tail\n\n')
    conf.set('resolution', '1920x1080').set('pad_swap', '1')
    lines = conf.render().splitlines()
    eq(lines[0], '# top', 'header stays first')
    eq(lines[1], 'resolution=1920x1080', 'existing key rewritten in place')
    check('pad_swap=1' in lines, 'new key appended')
    check(lines.index('pad_swap=1') < lines.index('') if '' in lines else True, 'new key goes before the trailing blank')
    eq(conf.get('pad_swap'), '1', 'appended key readable')
    # None removes the line entirely.
    conf.set('pad_swap', None)
    check('pad_swap' not in conf.render(), 'None removes the key')


def test_effective_values_use_defaults():
    conf = cfg.Conf.parse('resolution=1280x720\n')
    eq(conf.effective(cfg.SETTINGS_BY_KEY['resolution']), '1280x720', 'explicit value')
    eq(conf.effective(cfg.SETTINGS_BY_KEY['language']), 'en-US', 'absent key uses the default')
    eq(conf.get('language', None), None, 'absent key is not invented by get()')
    values = cfg.values_of(conf)
    eq(values['resolution'], '1280x720', 'values_of reads the file')
    eq(values['mute'], '0', 'values_of falls back to the default')


def test_env_mirrors_the_script():
    conf = cfg.Conf.parse(
        'resolution=1920x1080\n'
        'fullscreen=0\n'
        'handheld=off\n'
        'mute=\n'
        'pad_swap=1\n'
        'NX_CUSTOM=keep\n'
        'log=/tmp/x.log\n'
    )
    env = cfg.to_env(conf)
    eq(env['NX_RESOLUTION'], '1920x1080', 'NX_ prefix added')
    eq(env['NX_PAD_SWAP'], '1', 'switch kept')
    eq(env['NX_CUSTOM'], 'keep', 'a key that already starts with NX_ is exported verbatim')
    eq(env['NX_LOG'], '/tmp/x.log', 'path value')
    for off in ('NX_FULLSCREEN', 'NX_HANDHELD', 'NX_MUTE'):
        check(off not in env, f'{off} is left unset, as the script does')
    # Later lines win, exactly as the sequential exports do.
    eq(cfg.to_env(cfg.Conf.parse('resolution=1280x720\nresolution=1920x1080\n'))['NX_RESOLUTION'], '1920x1080',
       'later line wins')


def test_numeric_zero_is_an_explicit_value():
    zero_keys = {s.key for s in cfg.SETTINGS if cfg.zero_is_value(s)}
    eq(zero_keys, {'pad_deadzone', 'pad_trigger', 'pad_index', 'fsr_sharp', 'mouse_camera'},
       'the shell numeric-zero list covers every zero-valued setting')
    conf = cfg.Conf.parse(
        'pad_deadzone=0\n'
        'pad_trigger=0\n'
        'pad_index=0\n'
        'fsr_sharp=0\n'
        'mouse_camera=0\n'
        'fullscreen=0\n'
        'pad_swap=off\n'
        'fps_cap=0\n'
        'unknown_switch=0\n'
    )
    for key in ('pad_deadzone', 'pad_trigger', 'pad_index', 'fsr_sharp', 'mouse_camera'):
        setting = cfg.SETTINGS_BY_KEY[key]
        eq(cfg.validate_value(setting, '0'), None, f'{key}=0 is legal')
        eq(conf.effective(setting), '0', f'{key}=0 is visible to the launcher')
        eq(cfg.to_env(conf).get(setting.env), '0', f'{key}=0 reaches the environment')
    for key in ('NX_FULLSCREEN', 'NX_PAD_SWAP', 'NX_FPS_CAP', 'NX_UNKNOWN_SWITCH'):
        check(key not in cfg.to_env(conf), f'{key} remains unset')
    eq(cfg.launch_env(ROOT, conf, base={}).get('NX_PAD_DEADZONE'), '0', 'launcher passes 0 to the game')
    with tempfile.TemporaryDirectory(prefix='b3-zero-config-') as tmp:
        saved = Path(tmp) / 'settings.conf'
        conf.save(saved)
        reloaded = cfg.Conf.load(saved)
        for key in zero_keys:
            setting = cfg.SETTINGS_BY_KEY[key]
            eq(reloaded.effective(setting), '0', f'{key}=0 survives saving')
            eq(cfg.to_env(reloaded).get(setting.env), '0', f'saved {key}=0 reaches the environment')
    for spelling in ('', 'false', 'off', 'no', 'OFF', 'No'):
        for key in ('pad_deadzone', 'pad_trigger', 'fsr_sharp'):
            env = cfg.to_env(cfg.Conf.parse(f'{key}={spelling}\n'))
            check(cfg.SETTINGS_BY_KEY[key].env not in env, f'{key}={spelling!r} remains unset')


def test_shell_exports_match_python_model_without_touching_real_config():
    # A disposable copy of b3_play.sh execs a fake run_linux.sh, which records the environment and arguments.
    # No game, user config, or save directory is read or written.
    with tempfile.TemporaryDirectory(prefix='b3-play-config-') as tmp:
        root = Path(tmp)
        scripts = root / 'scripts'
        scripts.mkdir()
        shutil.copyfile(ROOT / 'scripts' / 'b3_play.sh', scripts / 'b3_play.sh')
        capture = root / 'environment'
        args = root / 'arguments'
        run = scripts / 'run_linux.sh'
        run.write_text('#!/usr/bin/env bash\nprintenv -0 > "$CAPTURE_ENV"\nprintf "%s\\0" "$@" > "$CAPTURE_ARGS"\n')
        run.chmod(0o755)
        config = root / 'fixture.conf'
        cases = (
            ('pad_deadzone=5\npad_deadzone=0 # disable\npad_trigger=0\npad_index=0\nfsr_sharp=0\n'
             'fullscreen=0\npad_swap=off\nhandheld=FALSE\nmute=no\nheadless=\n'
             'pad_invert_x=1\nfps_cap=0\nmem_limit_gb=0\nunknown_switch=0\n'),
            ('NX_PAD_DEADZONE=0\nNX_PAD_TRIGGER=0\nPAD_INDEX=0\nFSR_SHARP=0\n'
             'nx_pad_deadzone=0\nnx_custom=kept\nNX_pad_index=0\n'
             'pad_deadzone=off\npad_trigger=NO\n'),
        )
        base = {key: value for key, value in os.environ.items() if not key.startswith('NX_')}
        for text in cases:
            config.write_text(text)
            env = {**base, 'B3_CONFIG': str(config), 'CAPTURE_ENV': str(capture), 'CAPTURE_ARGS': str(args)}
            proc = subprocess.run([_bash(), str(scripts / 'b3_play.sh'), '--sentinel', 'two words'],
                                  env=env, text=True, capture_output=True, timeout=10,
                                  encoding='utf-8', errors='replace')
            eq(proc.returncode, 0, f'isolated b3_play.sh exits successfully: {proc.stderr}')
            raw = capture.read_bytes().split(b'\0')
            exported = dict(item.decode().split('=', 1) for item in raw if item)
            shell_env = {key: value for key, value in exported.items() if key.startswith('NX_')}
            expected = cfg.to_env(cfg.Conf.parse(text))
            # NX_LOG is the script's own default, not a line in the file; it is checked separately because the
            # script spells a path the way its shell does (POSIX under Git Bash, native on Linux).
            log = shell_env.pop('NX_LOG', '')
            expected.pop('NX_LOG', None)
            eq(Path(log).name, 'b3.log', 'the script defaults NX_LOG to b3.log')
            eq(shell_env, expected, 'shell export matches Python model')
            eq(args.read_bytes(), b'--sentinel\0two words\0', 'runtime arguments are preserved')
            eq(config.read_text(), text, 'launch does not modify its config')
            for key in ('NX_PAD_DEADZONE', 'NX_PAD_TRIGGER', 'NX_PAD_INDEX', 'NX_FSR_SHARP'):
                eq(shell_env.get(key), '0', f'shell exports {key}=0')


def test_validation():
    eq(cfg.validate_value(cfg.SETTINGS_BY_KEY['resolution'], '1280x720'), None, 'known choice')
    check(cfg.validate_value(cfg.SETTINGS_BY_KEY['resolution'], '1024x600') is not None, 'unknown choice rejected')
    eq(cfg.validate_value(cfg.SETTINGS_BY_KEY['pad_deadzone'], ''), None, 'absent value is fine')
    eq(cfg.validate_value(cfg.SETTINGS_BY_KEY['pad_deadzone'], '7849'), None, 'in range')
    check(cfg.validate_value(cfg.SETTINGS_BY_KEY['pad_deadzone'], '20001') is not None, 'out of range rejected')
    check(cfg.validate_value(cfg.SETTINGS_BY_KEY['pad_deadzone'], 'abc') is not None, 'not a number rejected')
    eq(cfg.validate_value(cfg.SETTINGS_BY_KEY['fullscreen'], '1'), None, 'bool on')
    check(cfg.validate_value(cfg.SETTINGS_BY_KEY['fullscreen'], 'maybe') is not None, 'bool junk rejected')
    eq(cfg.validate_value(cfg.SETTINGS_BY_KEY['log'], '/tmp/b3.log'), None, 'text is free form')
    # Every default and every offered choice must pass the setting's own validation.
    for setting in cfg.SETTINGS:
        eq(cfg.validate_value(setting, setting.default), None, f'default of {setting.key} is valid')
        for value, _ in setting.choices:
            eq(cfg.validate_value(setting, value), None, f'choice {value!r} of {setting.key} is valid')
    problems = cfg.validate(cfg.Conf.parse('pad_deadzone=99999\nresolution=nope\n'))
    eq(sorted(s.key for s, _ in problems), ['pad_deadzone', 'resolution'], 'validate lists every problem')


def test_switch_names_are_the_documented_ones():
    # The runtime reads NX_<KEY>; these are the names in docs/nvn/SWITCHES.md and the runtime's getenv calls.
    expected = {
        'resolution': 'NX_RESOLUTION', 'render_res': 'NX_RENDER_RES', 'render_scale': 'NX_RENDER_SCALE',
        'pad_swap': 'NX_PAD_SWAP', 'pad_deadzone': 'NX_PAD_DEADZONE', 'handheld': 'NX_HANDHELD',
        'fps_cap': 'NX_FPS_CAP', 'mute': 'NX_MUTE',
    }
    for key, env in expected.items():
        eq(cfg.SETTINGS_BY_KEY[key].env, env, f'{key} env name')
    # Every setting belongs to a tab the GUI draws.
    for setting in cfg.SETTINGS:
        check(setting.group in cfg.GROUPS, f'{setting.key} group {setting.group} is a known tab')
    for group in cfg.GROUPS:
        check(cfg.group_settings(group), f'tab {group} is not empty')


def test_save_is_atomic_and_round_trips():
    with tempfile.TemporaryDirectory(prefix='bayo3-launcher-conf-') as tmp:
        path = Path(tmp) / 'private' / 'b3.conf'
        conf = cfg.Conf.parse('# keep me\nresolution=1600x900\n')
        conf.set('fps_cap', '30').save(path)
        check(path.is_file(), 'saved')
        check(not (Path(tmp) / 'private' / 'b3.conf.tmp').exists(), 'no temporary file left behind')
        again = cfg.Conf.load(path)
        eq(again.get('fps_cap'), '30', 'value survives a round trip')
        check('# keep me' in again.render(), 'comment survives a round trip')
        eq(cfg.Conf.load(Path(tmp) / 'missing.conf').render(), '\n', 'a missing file loads as empty')


def test_off_spellings_in_a_list_mean_unset():
    # An older private/b3.conf has fps_cap=0 (the old template wrote it): the runtime sees no switch at all, so the
    # launcher shows its default instead of flagging the file as invalid.
    conf = cfg.Conf.parse('fps_cap=0\nrender_scale=0\n')
    fps = cfg.SETTINGS_BY_KEY['fps_cap']
    eq(conf.effective(fps), '', 'a 0 in a list means unset')
    eq(cfg.validate_value(fps, '0'), None, 'and it is not an error')
    eq(cfg.validate_value(cfg.SETTINGS_BY_KEY['pad_deadzone'], '0'), None, 'a 0 deadzone is a real value')
    eq(cfg.to_env(conf), {}, 'and nothing reaches the environment')
    conf.set('fps_cap', '30')
    eq(conf.effective(fps), '30', 'a real choice still wins')


def test_comments_keep_their_column():
    # The template lines its notes up at one column; a saved file must still look like the template.
    conf = cfg.Conf.parse('resolution=1600x900  # render + window size\npad_swap=0           # 1: swap\n')
    conf.set('resolution', '1280x720')
    conf.set('pad_swap', 'on')
    eq(conf.render().splitlines()[0], 'resolution=1280x720  # render + window size', 'shorter value keeps the column')
    eq(conf.render().splitlines()[1], 'pad_swap=on          # 1: swap', 'longer value keeps the column')
    # The '#' stays in its column when the value length changes.
    columns = [line.index('#') for line in cfg.Conf.parse(conf.render()).render().splitlines() if '#' in line]
    eq(len(set(columns)), 1, f'one column ({columns})')
    # A note written straight after the value gets the padding back.
    glued = cfg.Conf.parse('fps_cap=# 30: hold it\n')
    glued.set('fps_cap', '30')
    check('fps_cap=30 # 30: hold it' in glued.render(), f'a glued note gets its space: {glued.render()!r}')


def test_the_shipped_template_is_understood_and_complete():
    template = ROOT / 'config' / 'b3.conf.example'
    check(template.is_file(), 'config/b3.conf.example exists')
    conf = cfg.Conf.load(template)
    eq(cfg.validate(conf), [], 'the template has no invalid value')
    text = template.read_text(encoding='utf-8')
    for setting in cfg.SETTINGS:
        check(setting.key + '=' in text, f'the template documents {setting.key}')
    # A file the template produced is already what the launcher would write: no churn on the first save.
    first = conf.render()
    second = cfg.Conf.parse(first).render()
    eq(second, first, 'template round trip is stable')


def test_install_state_launch_and_summary():
    with tempfile.TemporaryDirectory(prefix='bayo3-launcher-root-') as tmp:
        root = Path(tmp)
        (root / 'scripts').mkdir()
        (root / 'private').mkdir()
        play = root / 'scripts' / 'b3_play.sh'
        play.write_text('#!/bin/sh\n')
        state = cfg.install_state(root, 'linux')
        check(state['play'], 'play script found')
        check(not state['built'], 'no binary yet')
        eq(cfg.launch_argv(root, host='linux'), [str(play)], 'the launcher starts the same entry point as the shell')
        eq(cfg.launch_argv(root, ['--flag'], host='linux'), [str(play), '--flag'],
           'extra arguments are passed through')
        conf = cfg.Conf.parse('resolution=1280x720\nfps_cap=30\n')
        env = cfg.launch_env(root, conf, base={'PATH': '/bin'}, host='linux')
        eq(env['NX_RESOLUTION'], '1280x720', 'the file reaches the environment')
        eq(env['NX_FPS_CAP'], '30', 'switches reach the environment')
        eq(env['NX_LOG'], str(root / 'private' / cfg.install_layout('linux')['log'].name),
           'log defaults to the Linux log file')
        eq(env['PATH'], '/bin', 'the host environment is kept')
        line = cfg.summary(conf)
        check('window 1280x720' in line and '30 fps cap' in line, f'summary mentions the choices: {line}')


def test_install_layout_per_host():
    # Windows has no run_linux.sh, so its entry point and install paths differ; the launcher must follow the host it
    # runs on rather than the Deck layout, or Play would start a script that cannot run there.
    win = cfg.install_layout('win')
    eq(win['play'].name, 'b3_win.py', 'Windows starts the native runner (no Git Bash)')
    eq(win['binary'].name, 'bayonetta3.exe', 'the Windows binary is a .exe')
    eq(win['binary'].parent.as_posix(), 'private/win/build/b3', 'Windows plays what b3_win.py setup built')
    eq(win['data'].as_posix(), 'private/win/run/b3', 'the Windows data root')
    eq(win['log'].name, 'game.log', 'the Windows log')
    argv = cfg.launch_argv(Path('/repo'), ['--x'], host='win')
    eq(argv[0], sys.executable, 'the runner is started with this Python')
    eq(argv[2:], ['play', '--x'], 'the runner plays, with the extra arguments')
    linux = cfg.install_layout('linux')
    eq(linux['play'].name, 'b3_play.sh', 'Linux keeps b3_play.sh')
    eq(linux['binary'].parent.name, 'build-deck', 'Linux keeps the Deck build directory')
    eq(linux['data'].name, 'run-bayonetta3', 'Linux keeps the Deck data root')
    eq(linux['log'].name, 'b3.log', 'Linux keeps private/b3.log')
    check('b3_win.py setup' in cfg.build_hint('win'), 'the Windows build hint names the setup')
    eq(cfg.build_hint('linux'), 'cmake --build private/build-deck', 'the Linux build hint names build-deck')


def test_reset_keeps_comments():
    conf = cfg.Conf.parse('# a\nresolution=1600x900\npad_swap=1\n# b\n')
    cfg.reset_to_defaults(conf)
    rendered = conf.render()
    check('resolution' not in rendered and 'pad_swap' not in rendered, 'known keys dropped')
    check('# a' in rendered and '# b' in rendered, 'comments kept')


def test_repo_paths():
    p = cfg.paths(cfg.repo_root())
    eq(p['conf'].name, 'b3.conf', 'settings file name')
    eq(p['conf'].parent.name, 'private', 'settings live in private/')
    check(p['template'].is_file(), 'the template the script copies is there')
    check(p['play'].is_file(), 'the play script is there')


def main():
    tests = [value for name, value in sorted(globals().items()) if name.startswith('test_') and callable(value)]
    for test in tests:
        test()
    if FAILED:
        print(f'{len(FAILED)} check(s) failed')
        return 1
    print(f'b3_launcher_config: {len(tests)} tests ok')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
