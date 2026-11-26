"""The launchers' translations: every text the Android app (Tr.t / Tr.f in games/bayonetta3/java) and the desktop
launcher (_() in tools/b3_launcher*.py) shows has its Chinese and Japanese line, with the same placeholders.

    python tests/test_launcher_i18n.py          # the check
    python tests/test_launcher_i18n.py --list   # the keys with no translation yet
"""
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / 'games/bayonetta3/java/io/github/bayonetta3recomp'
sys.path.insert(0, str(ROOT / 'tools'))

_JSTR = r'"(?:[^"\\]|\\.)*"'


def _unescape(lit: str) -> str:
    return bytes(lit[1:-1], 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')


def _groups(text: str) -> list[str]:
    """The string literals in `text`, those joined by + as one."""
    out, pos = [], 0
    pat = re.compile(rf'{_JSTR}(?:\s*\+\s*{_JSTR})*')
    for m in pat.finditer(text):
        out.append(''.join(_unescape(x) for x in re.findall(_JSTR, m.group(0))))
    return out


def _call_args(src: str, start: int) -> str:
    """The text of a call's arguments, from just after its '(' to the matching ')'."""
    depth, i, n = 1, start, len(src)
    while i < n and depth:
        c = src[i]
        if c == '"':
            i += 1
            while src[i] != '"':
                i += 2 if src[i] == '\\' else 1
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        i += 1
    return src[start:i - 1]


def java_keys() -> set[str]:
    keys = set()
    for f in JAVA.glob('*.java'):
        if f.name in ('Tr.java', 'TrText.java'):
            continue
        src = f.read_text(encoding='utf-8')
        for m in re.finditer(r'\bTr\.[tf]\(', src):
            for k in _groups(_call_args(src, m.end())):
                # A text, not a config key such as on("NX_MUTE") inside the call.
                if re.search(r'[A-Za-z]', k) and not re.fullmatch(r'[A-Z0-9_]*_[A-Z0-9_]*', k):
                    keys.add(k)
    return keys


def java_table() -> dict[str, list[str]]:
    src = (JAVA / 'TrText.java').read_text(encoding='utf-8')
    rows = {}
    for m in re.finditer(r'\{\s*(' + _JSTR + r'(?:\s*\+\s*' + _JSTR + r')*(?:\s*,\s*' + _JSTR + r'(?:\s*\+\s*' + _JSTR
                         + r')*)*)\s*\}', src):
        cells = _groups(m.group(1))
        if len(cells) == 3:
            rows[cells[0]] = cells
    return rows


def python_keys() -> set[str]:
    keys = set()
    for name in ('b3_launcher.py', 'b3_launcher_config.py'):
        src = (ROOT / 'tools' / name).read_text(encoding='utf-8')
        for m in re.finditer(r"\b_\(\s*((?:'(?:[^'\\]|\\.)*'|\"(?:[^\"\\]|\\.)*\")(?:\s*(?:'(?:[^'\\]|\\.)*'|\"(?:[^\"\\]|\\.)*\"))*)", src):
            parts = re.findall(r"'(?:[^'\\]|\\.)*'|\"(?:[^\"\\]|\\.)*\"", m.group(1))
            keys.add(''.join(eval(p) for p in parts))  # noqa: S307 -- our own source's string literals
    # Translated where they are shown (_(setting.label) and the like): the settings model's own texts.
    import b3_dump
    import b3_launcher_config as cfg
    import b3_spec
    for s in cfg.SETTINGS:
        keys.add(s.label)
        if s.help:
            keys.add(s.help)
        keys.update(label for _v, label in s.choices)
    keys.update(cfg.GROUP_TITLES.values())
    keys.update((b3_dump.HOW_TO_EXTRACT, b3_spec.MINIMUM, 'Setup', 'Update', 'Phone'))  # kind.capitalize()
    return {k for k in keys if re.search(r'[A-Za-z]{2}', k)}  # not the plain numbers: '0.5', '2x'


def placeholders(s: str) -> list[str]:
    """The placeholders, without their argument positions (a translation may reorder them as %2$s %1$.1f)."""
    return sorted(re.sub(r'%\d+\$', '%', p) for p in re.findall(r'%(?:\d+\$)?[-#+ 0,(]*\d*(?:\.\d+)?[sdf]|\{\w*\}', s))


class TestLauncherI18n(unittest.TestCase):
    def test_android_every_text_translated(self):
        table = java_table()
        missing = sorted(java_keys() - table.keys())
        self.assertEqual(missing, [], f'{len(missing)} texts without a translation in TrText.java')

    def test_android_placeholders_match(self):
        for en, zh, ja in java_table().values():
            for t in (zh, ja):
                self.assertEqual(placeholders(en), placeholders(t), f'{en!r} -> {t!r}')

    def test_desktop_every_text_translated(self):
        import b3_i18n
        missing = sorted(python_keys() - b3_i18n.TEXT.keys())
        self.assertEqual(missing, [], f'{len(missing)} texts without a translation in tools/b3_i18n.py')

    def test_desktop_placeholders_match(self):
        import b3_i18n
        for en, (zh, ja) in b3_i18n.TEXT.items():
            for t in (zh, ja):
                self.assertEqual(placeholders(en), placeholders(t), f'{en!r} -> {t!r}')


if __name__ == '__main__':
    if '--list' in sys.argv:
        have = java_table() if (JAVA / 'TrText.java').exists() else {}
        for k in sorted(java_keys() - have.keys()):
            print('java', repr(k))
        try:
            import b3_i18n
            text = b3_i18n.TEXT
        except ImportError:
            text = {}
        for k in sorted(python_keys() - text.keys()):
            print('py', repr(k))
        sys.exit(0)
    unittest.main()
