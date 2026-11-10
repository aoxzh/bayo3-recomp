#!/usr/bin/env python3
"""The launcher's settings model: private/b3.conf, its switches, and the command that starts the game.

Pure python (no Tk, no game, no GPU) so tests/test_b3_launcher_config.py can exercise every rule without a display:
the file format is the one scripts/b3_play.sh reads (`key=value`, `#` comments, `NX_<KEY>` in the environment):
false/off/no/empty leave a switch unset, as does 0 except for settings that accept it as a numeric value.
`to_env()` mirrors the script's exports.

The switch names are the runtime's own (docs/nvn/SWITCHES.md), in lower case, which is exactly what the file holds.
"""
from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

GROUP_TITLES = {
    'video': 'Picture',
    'input': 'Controller',
    'audio': 'Audio',
    'system': 'System',
    'advanced': 'Advanced',
}
GROUPS = tuple(GROUP_TITLES)

LANGUAGES = (
    ('en-US', 'English (US)'),
    ('en-GB', 'English (UK)'),
    ('ja', 'Japanese'),
    ('fr', 'French'),
    ('fr-CA', 'French (Canada)'),
    ('de', 'German'),
    ('it', 'Italian'),
    ('es', 'Spanish'),
    ('es-419', 'Spanish (Latin America)'),
    ('ko', 'Korean'),
    ('ru', 'Russian'),
    ('zh-Hans', 'Chinese (Simplified)'),
    ('zh-Hant', 'Chinese (Traditional)'),
)

# Host window size. Its relationship to internal render targets differs by platform;
# see docs/PLAYING.md. `auto` requests the largest display-fitting 16:9 area.
# All listed sizes are 16:9.
RESOLUTIONS = (
    ('auto', 'Largest 16:9 that fits the display (auto)'),
    ('1280x720', '1280 x 720 (720p, fastest)'),
    ('1600x900', '1600 x 900'),
    ('1920x1080', '1920 x 1080 (1080p)'),
    ('2560x1440', '2560 x 1440 (1440p)'),
    ('3840x2160', '3840 x 2160 (4K, slowest)'),
)

# NX_RENDER_RES: resolution reported to the game and used by current render setup.
# Older macOS measurements differ from the current Windows path; see docs/PLAYING.md.
RENDER_RES = (('', 'Console size (1920 x 1080, 1280 x 720 handheld)'),) + tuple(
    (v, lbl.split(' (')[0]) for v, lbl in RESOLUTIONS if v != 'auto')

# A factor on the console size (NX_RENDER_SCALE): the cheap way to trade sharpness for frame rate on weak devices.
RENDER_SCALES = (
    ('', '1.0 (native)'),
    ('0.5', '0.5'),
    ('0.67', '0.67'),
    ('0.75', '0.75'),
    ('1.25', '1.25'),
    ('1.5', '1.5'),
)

FPS_CAPS = (('', "The game's own rule (60, 30 when heavy)"), ('30', '30 fps'))
# NX_DISPLAY_HZ: the display clock the game paces itself by. Above 60 the runtime converts the gameplay step and the
# game's frame-counted windows to real time (b3_scene_clock.cpp, b3_fps_counters.cpp). Absent: the panel's own rate.
DISPLAY_HZ = (('', "The display's own rate"), ('60', '60 Hz'), ('90', '90 Hz'), ('120', '120 Hz'), ('144', '144 Hz'))
ANISO = (('', "The game's own"), ('2', '2x'), ('4', '4x'), ('8', '8x'), ('16', '16x'))
ON_OFF = (('1', 'On'), ('0', 'Off'))
MOUSE_SENS = (('', '1.0 (default)'), ('0.5', '0.5'), ('0.75', '0.75'), ('1.5', '1.5'), ('2', '2.0'), ('3', '3.0'))
FSR_SHARPNESS = (('', '0.2 (default)'), ('0', '0.0'), ('1', '1.0'), ('2', '2.0'))
PIPELINED = (('', 'Off (synchronous)'), ('1', '1'), ('2', '2'), ('3', '3'), ('4', '4'))


@dataclass(frozen=True)
class Setting:
    key: str            # the b3.conf key, without NX_
    label: str
    kind: str           # bool | choice | int | text
    group: str
    default: str = ''   # '' means "the line is absent, the runtime's own default applies"
    help: str = ''
    choices: tuple = ()  # ((value, label), ...) for choice
    lo: int = 0
    hi: int = 0
    hint: str = ''      # what the runtime does while the line is absent, shown beside an empty field

    @property
    def env(self) -> str:
        return 'NX_' + self.key.upper()


SETTINGS = (
    Setting('resolution', 'Window size', 'choice', 'video', default='1600x900', choices=RESOLUTIONS,
            help='The window. The game renders at 1600 x 900 (1280 x 720 in handheld mode) and is scaled to it.'),
    Setting('fullscreen', 'Fullscreen', 'bool', 'video', default='0',
            help='Start fullscreen; F11 or Alt+Enter toggles it.'),
    Setting('fsr', 'FSR 1 upscale', 'bool', 'video', default='0',
            help='Upscale and sharpen when the rendered picture is smaller than the output window.'),
    Setting('fsr_sharp', 'FSR sharpness', 'choice', 'video', choices=FSR_SHARPNESS, help='Only with FSR 1 on.'),
    Setting('fps_cap', 'Frame rate cap', 'choice', 'video', choices=FPS_CAPS,
            help='Optionally cap at 30 fps; used in the documented Snapdragon 8 Gen 2 / Adreno 740 combat tests. See Performance for heat-related limits.'),
    Setting('display_hz', 'Frame rate', 'choice', 'video', choices=DISPLAY_HZ,
            help='The rate the game paces itself by. Above 60 the gameplay and its timing windows are kept at real '
                 'time (hit-stops are a little shorter); fights that cannot keep up settle where they can.'),
    Setting('fifo', 'V-sync', 'bool', 'video', default='0',
            help='Wait for the display refresh (no tearing, a little more latency). Off shows the newest frame at each '
                 'refresh without waiting.'),
    Setting('aniso', 'Anisotropic filtering', 'choice', 'video', choices=ANISO,
            help='Sharper ground and walls seen at a slant, for a small GPU cost.'),
    Setting('pad_swap', 'Xbox button layout', 'bool', 'input', default='0',
            help='Swap A<->B and X<->Y, for players used to Xbox labels.'),
    Setting('pad_deadzone', 'Stick deadzone', 'int', 'input', lo=0, hi=20000, hint='7849',
            help='Default 7849 of 32767. 0: no deadzone.'),
    Setting('pad_trigger', 'Trigger threshold', 'int', 'input', lo=0, hi=32767, hint='12000', help='Default 12000 of 32767.'),
    Setting('pad_index', 'Player 1 controller', 'int', 'input', lo=0, hi=7, hint='0',
            help='Which connected controller plays (0 = the first). Default 0.'),
    Setting('pad_invert_x', 'Invert camera X', 'bool', 'input', default='0'),
    Setting('pad_invert_y', 'Invert camera Y', 'bool', 'input', default='0'),
    Setting('mouse_camera', 'Mouse camera', 'choice', 'input', default='1', choices=ON_OFF,
            help='Windows: click into the game window to turn the camera with the mouse; F10 releases it. Keys are '
                 'rebound with key_<input>= lines in the settings file.'),
    Setting('mouse_sens', 'Mouse speed', 'choice', 'input', choices=MOUSE_SENS),
    Setting('mouse_invert_y', 'Invert mouse Y', 'bool', 'input', default='0'),
    Setting('language', 'Language', 'choice', 'system', default='en-US', choices=LANGUAGES,
            help='The game text language.'),
    Setting('handheld', 'Handheld mode', 'bool', 'system', default='0',
            help='The game renders 1280 x 720 instead of docked 1600 x 900 (a lighter GPU load).'),
    Setting('log', 'Log file', 'text', 'system', help='Default: <repo>/private/b3.log.'),
    Setting('discord_app_id', 'Discord application id', 'text', 'system',
            help='Windows: Rich Presence ("Bayonetta 3" or "The Old Picture Book", and since when) through your own '
                 'Discord application (discord.com/developers). Empty: off.'),
    Setting('mute', 'Mute', 'bool', 'audio', default='0', help='No audio output.'),
    Setting('render_res', 'Display resolution answer', 'choice', 'advanced', choices=RENDER_RES,
            help='NX_RENDER_RES: the resolution reported to the game. Other ports render at it; Bayonetta 3 renders '
                 'at its own console size, so this changes nothing here.'),
    Setting('render_scale', 'Resolution scale', 'choice', 'advanced', choices=RENDER_SCALES,
            help='NX_RENDER_SCALE: a factor on the reported resolution. Same caveat as above.'),
    Setting('ultrawide', 'Ultrawide', 'bool', 'advanced', default='0',
            help='Experimental: widen the render targets and patch the camera aspect for windows wider than 16:9.'),
    Setting('mem_limit_gb', 'Memory limit (GB)', 'int', 'advanced', lo=1, hi=64, hint='platform default',
            help='The guard stops the process above this footprint. Empty uses the platform default; Windows uses 60% of RAM.'),
    Setting('pipelined', 'GPU submits in flight', 'choice', 'advanced', choices=PIPELINED,
            help='Experimental; absent is synchronous.'),
    Setting('pipeline_cache', 'Pipeline cache', 'bool', 'advanced', default='1',
            help='On-disk shader pipeline cache; off disables it.'),
    Setting('headless', 'Headless', 'bool', 'advanced', default='0',
            help='No window and no sound device (benchmarks only).'),
)

SETTINGS_BY_KEY = {s.key: s for s in SETTINGS}

_FALSE_VALUES = ('0', 'false', 'off', 'no', '')
_KEY_RE = re.compile(r'^[A-Za-z0-9_]+$')


def group_settings(group):
    return tuple(s for s in SETTINGS if s.group == group)


def zero_is_value(setting):
    """Whether 0 is a valid explicit numeric value for a setting, rather than an off spelling."""
    if setting is None:
        return False
    if setting.kind == 'int':
        return setting.lo <= 0 <= setting.hi
    return setting.kind == 'choice' and any(value == '0' for value, _ in setting.choices)


def is_unset(setting, value):
    value = value.strip().lower()
    return value in _FALSE_VALUES and (value != '0' or not zero_is_value(setting))


def is_on(value):
    """The truth of a value, with the spellings scripts/b3_play.sh treats as off."""
    return value is not None and value.strip().lower() not in _FALSE_VALUES


# ---- the file -------------------------------------------------------------------------------------------------

@dataclass
class Pair:
    key: str
    value: str
    suffix: str = ''  # the inline comment the line had, kept so a save does not strip the template's notes


def _realign(item, value):
    """The same note, moved so the `#` keeps its column when the value changes length."""
    if not item.suffix:
        return ''  # a line with no note stays bare: no trailing space
    if not item.suffix.startswith((' ', '\t')):
        return ' ' + item.suffix  # `fps_cap=# 30` (glued): give the note the space it was missing
    pad = len(item.suffix) - len(item.suffix.lstrip(' \t'))
    column = len(item.key) + 1 + len(item.value) + pad
    return ' ' * max(1, column - len(item.key) - 1 - len(value)) + item.suffix.lstrip(' \t')


@dataclass
class Raw:
    text: str


@dataclass
class Conf:
    """private/b3.conf as an ordered list of pairs and verbatim lines, so comments and unknown keys survive."""

    items: list = field(default_factory=list)

    @classmethod
    def parse(cls, text):
        items = []
        for line in text.splitlines():
            stripped = line.strip()
            hash_at = line.find('#')
            if hash_at >= 0:
                # Keep the padding before the note, so a saved file stays aligned the way the template is.
                while hash_at > 0 and line[hash_at - 1] in ' \t':
                    hash_at -= 1
            body = (line[:hash_at] if hash_at >= 0 else line).strip()
            if '=' in body:
                key, value = body.split('=', 1)
                key, value = key.strip(), value.strip()
                if _KEY_RE.match(key):
                    items.append(Pair(key, value, line[hash_at:] if hash_at >= 0 else ''))
                    continue
            items.append(Raw(line))
        return cls(items)

    @classmethod
    def load(cls, path):
        path = Path(path)
        if not path.exists():
            return cls()
        return cls.parse(path.read_text(encoding='utf-8', errors='replace'))

    def get(self, key, default=''):
        """The value the runtime would see: the last matching line wins, as in scripts/b3_play.sh."""
        found = default
        for item in self.items:
            if isinstance(item, Pair) and item.key == key:
                found = item.value
        return found

    def effective(self, setting):
        """The setting's value, or its default when the line is absent or says "off"."""
        value = self.get(setting.key, None)
        if value is None:
            return setting.default
        # Older files can use 0/false/off/no to mean unset for choices that do not offer zero.
        if setting.kind == 'choice' and is_unset(setting, value):
            return setting.default
        return value

    def set(self, key, value):
        """Write one key: the first line is rewritten, later duplicates dropped, a new key is appended."""
        written = False
        kept = []
        for item in self.items:
            if isinstance(item, Pair) and item.key == key:
                if not written and value is not None:
                    kept.append(Pair(key, str(value), _realign(item, str(value))))
                    written = True
                continue
            kept.append(item)
        if not written and value is not None:
            if kept and isinstance(kept[-1], Raw) and kept[-1].text.strip() == '':
                kept.insert(len(kept) - 1, Pair(key, str(value)))
            else:
                kept.append(Pair(key, str(value)))
        self.items = kept
        return self

    def apply(self, values):
        """Write every key of a {key: value} mapping (None removes the line)."""
        for key, value in values.items():
            self.set(key, value)
        return self

    def render(self):
        return '\n'.join(i.text if isinstance(i, Raw) else f'{i.key}={i.value}{i.suffix}' for i in self.items) + '\n'

    def save(self, path):
        """Atomically: a crash while writing must not leave a half-read settings file."""
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_name(path.name + '.tmp')
        tmp.write_text(self.render(), encoding='utf-8')
        os.replace(tmp, path)
        return path


def values_of(conf):
    """Every known setting's effective value, as the launcher's widgets hold them."""
    return {s.key: conf.effective(s) for s in SETTINGS}


def to_env(conf):
    """The environment scripts/b3_play.sh would build from this file (later lines win)."""
    env = {}
    for item in conf.items:
        if not isinstance(item, Pair):
            continue
        value = item.value
        name = item.key if item.key.startswith('NX_') else 'NX_' + item.key.upper()
        setting = SETTINGS_BY_KEY.get(name[3:].lower())
        if is_unset(setting, value):
            continue
        env[name] = value
    return env


# ---- validation -----------------------------------------------------------------------------------------------

def validate_value(setting, value):
    """None when the value is acceptable, else a sentence for the player."""
    value = '' if value is None else str(value).strip()
    if value == '':
        return None  # absent: the runtime's own default
    if setting.kind == 'choice' and value.lower() in _FALSE_VALUES:
        return None  # `fps_cap=0` in an older file means "unset", as it does for scripts/b3_play.sh
    if setting.kind == 'bool':
        return None if value.lower() in ('0', '1', 'true', 'false', 'off', 'on', 'yes', 'no') else 'Use on or off.'
    if setting.kind == 'choice':
        allowed = [v for v, _ in setting.choices]
        return None if value in allowed else f'Choose one of: {", ".join(v or "(default)" for v in allowed)}.'
    if setting.kind == 'int':
        try:
            number = int(value, 10)
        except ValueError:
            return 'Enter a whole number.'
        return None if setting.lo <= number <= setting.hi else f'Enter {setting.lo} to {setting.hi}.'
    return None


def validate(conf):
    """[(setting, message)] for every value the runtime would reject."""
    problems = []
    for setting in SETTINGS:
        message = validate_value(setting, conf.get(setting.key, None))
        if message:
            problems.append((setting, message))
    return problems


# ---- the install and the launch -------------------------------------------------------------------------------

def repo_root(start=None):
    here = Path(start or __file__).resolve()
    return here.parents[1] if here.parent.name == 'tools' else here.parent


# Which host this launcher is running on. The install layout and the entry script differ per platform: Deck/Linux
# starts through scripts/b3_play.sh (which execs run_linux.sh and loops the program-0/program-1 handoff), while
# Windows starts `tools/b3_win.py play`, the native runner over what `tools/b3_win.py setup` built in private/win
# (no Git Bash: it loops the same handoff itself). Resolved per call, but exposed as a function so a test can pin it.
def host_platform():
    if os.name == 'nt':
        return 'win'
    return 'linux'


def install_layout(host=None):
    """{entry script, binary, data root, log} relative to the repo, for one host."""
    host = host or host_platform()
    if host == 'win':
        return {
            'play': Path('tools') / 'b3_win.py',
            'binary': Path('private') / 'win' / 'build' / 'b3' / 'bayonetta3.exe',
            'data': Path('private') / 'win' / 'run' / 'b3',
            'log': Path('private') / 'win' / 'logs' / 'game.log',
        }
    return {
        'play': Path('scripts') / 'b3_play.sh',
        'binary': Path('private') / 'build-deck' / 'bayonetta3',
        'data': Path('private') / 'run-bayonetta3',
        'log': Path('private') / 'b3.log',
    }


def paths(root, host=None):
    root = Path(root)
    layout = install_layout(host)
    return {
        'root': root,
        'conf': root / 'private' / 'b3.conf',
        'template': root / 'config' / 'b3.conf.example',
        'play': root / layout['play'],
        'log': root / layout['log'],
        'binary': root / layout['binary'],
        'data': root / layout['data'],
    }


def build_hint(host=None):
    """The cmake target directory to build when the launcher finds no binary."""
    if (host or host_platform()) == 'win':
        return 'python tools/b3_win.py setup --dump <your dump folder>'
    return 'cmake --build private/build-deck'


def install_state(root, host=None):
    """What the launcher's status line reports: is the game built, is the data there, is a settings file present."""
    p = paths(root, host)
    return {
        'built': p['binary'].is_file(),
        'data': p['data'].is_dir(),
        'conf': p['conf'].is_file(),
        'play': p['play'].is_file(),
        'log': p['log'].is_file(),
    }


def launch_argv(root, extra=(), host=None):
    p = paths(root, host)
    if p['play'].suffix == '.py':  # the Windows runner: this same Python, no shell in between
        return [sys.executable, str(p['play']), 'play', *extra]
    return [str(p['play']), *extra]


def launch_env(root, conf, base=None, host=None):
    """The environment the launcher hands the entry script: its own switches, plus this file's."""
    env = dict(os.environ if base is None else base)
    env.update(to_env(conf))
    env.setdefault('NX_LOG', str(paths(root, host)['log']))
    return env


def summary(conf):
    """One line for the status bar."""
    parts = []
    resolution = conf.get('resolution', '') or SETTINGS_BY_KEY['resolution'].default
    parts.append('window ' + resolution)
    render = conf.get('render_res', '')
    if render:
        parts.append('render ' + render)
    scale = conf.get('render_scale', '')
    if scale:
        parts.append('scale ' + scale)
    if is_on(conf.get('fullscreen', '0')):
        parts.append('fullscreen')
    if is_on(conf.get('handheld', '0')):
        parts.append('handheld')
    if is_on(conf.get('pad_swap', '0')):
        parts.append('Xbox layout')
    if conf.get('fps_cap', '') == '30':
        parts.append('30 fps cap')
    return ' · '.join(parts)


def defaults():
    """A settings model with every setting at its default (nothing written)."""
    return Conf()


def reset_to_defaults(conf):
    """Drop every known key from the file, leaving the comments and unknown lines."""
    for setting in SETTINGS:
        conf.set(setting.key, None)
    return conf


def main(argv=None):  # pragma: no cover - a small command-line view of the same model
    import argparse
    import sys

    parser = argparse.ArgumentParser(description='Read or write private/b3.conf without the GUI.')
    parser.add_argument('--root', default=str(repo_root()))
    parser.add_argument('--show', action='store_true', help='print the effective settings and environment')
    parser.add_argument('--set', action='append', default=[], metavar='KEY=VALUE', help='write one setting')
    parser.add_argument('--reset', action='store_true', help='remove every known key')
    args = parser.parse_args(argv)

    p = paths(args.root)
    conf = Conf.load(p['conf'])
    if args.reset:
        reset_to_defaults(conf)
    for assignment in args.set:
        key, _, value = assignment.partition('=')
        key = key.strip()
        setting = SETTINGS_BY_KEY.get(key.lower().removeprefix('nx_'))
        if setting is None:
            parser.error(f'unknown setting: {key}')
        message = validate_value(setting, value)
        if message:
            parser.error(f'{key}: {message}')
        conf.set(setting.key, value.strip() or None)
    if args.set or args.reset:
        conf.save(p['conf'])
        print(f'wrote {p["conf"]}')
    if args.show or not (args.set or args.reset):
        for setting in SETTINGS:
            print(f'{setting.key:16} {conf.effective(setting) or "(default)":28} {setting.env}')
        problems = validate(conf)
        for setting, message in problems:
            print(f'problem: {setting.key}: {message}', file=sys.stderr)
        return 1 if problems else 0
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
