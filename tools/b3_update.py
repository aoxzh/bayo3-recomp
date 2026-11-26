"""Is there a newer release? The launcher asks GitHub's public releases API once at start, in the background, when the
install is a release (a VERSION file at its root) and Settings > System > Check for updates is on. It sends nothing
but the request itself (a generic User-Agent, no machine or user details) and only ever opens the release page in the
browser: it downloads and installs nothing by itself.
"""
from __future__ import annotations

import json
import re
import urllib.request
from pathlib import Path

REPO = 'aoxzh/bayo3-recomp'
RELEASES = f'https://github.com/{REPO}/releases/latest'
API = f'https://api.github.com/repos/{REPO}/releases/latest'
AUTHOR_URL = 'https://x.com/aoxzh'  # the author's X (Twitter) profile: the launchers' Follow link


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


def latest_release(timeout: float = 6.0) -> tuple[str, str] | None:
    """(tag, page URL) of the latest release, or None when GitHub cannot be reached."""
    req = urllib.request.Request(API, headers={'Accept': 'application/vnd.github+json',
                                               'User-Agent': 'bayo3-recomp-launcher'})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            data = json.loads(r.read().decode('utf-8'))
    except (OSError, ValueError):
        return None
    tag = data.get('tag_name') or ''
    return (tag, data.get('html_url') or RELEASES) if version_tuple(tag) else None


def check(root) -> tuple[str, str] | None:
    """(tag, URL) when a newer release than this install is out, else None."""
    current = installed_version(root)
    if not current:
        return None
    found = latest_release()
    return found if found and newer(found[0], current) else None
