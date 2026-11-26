"""The desktop launcher's text in the player's language: English (as written in the code), Simplified Chinese or
Japanese, after the system's display language (B3_LANG=en|zh|ja overrides). The English text is the key: _('Settings')
is '设置' on a Chinese system and 'Settings' on any other, also for a text with no translation yet.
tests/test_launcher_i18n.py checks that every _('...') in tools/b3_launcher*.py and every setting's label, help and
choice (tools/b3_launcher_config.py) has its line in b3_i18n_text.py, with the same {placeholders}.
"""
from __future__ import annotations

import locale
import os
import sys

from b3_i18n_text import TEXT  # noqa: F401 -- {English: (Chinese, Japanese)}

LANGS = ('en', 'zh', 'ja')


def _system_language() -> str:
    if sys.platform == 'win32':
        try:
            import ctypes
            lcid = ctypes.windll.kernel32.GetUserDefaultUILanguage()
            name = locale.windows_locale.get(lcid, '')
            if name:
                return name
        except (AttributeError, OSError):
            pass
    for var in ('LANGUAGE', 'LC_ALL', 'LC_MESSAGES', 'LANG'):
        v = os.environ.get(var, '')
        if v and v not in ('C', 'POSIX'):
            return v.split(':')[0]
    try:
        return locale.getlocale()[0] or ''
    except ValueError:
        return ''


def language() -> str:
    forced = os.environ.get('B3_LANG', '').lower()[:2]
    if forced in LANGS:
        return forced
    code = _system_language().lower()
    return 'zh' if code.startswith('zh') or code.startswith('chinese') else \
        'ja' if code.startswith('ja') or code.startswith('japanese') else 'en'


LANG = language()


def _(en: str) -> str:
    if LANG == 'en':
        return en
    row = TEXT.get(en)
    if not row:
        return en
    return row[LANGS.index(LANG) - 1] or en
