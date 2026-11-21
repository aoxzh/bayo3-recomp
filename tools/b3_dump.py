#!/usr/bin/env python3
"""Finding and checking the player's extracted game files, so a setup starts from whatever the player has without
them knowing the layout.

The project never decrypts a game: the player extracts the ExeFS and RomFS of their own Bayonetta 3 with update 1.2.0
using their emulator (Ryujinx: right-click the game > Extract Data > ExeFS, then RomFS; Eden / yuzu / Citron:
right-click > Dump RomFS, and Dump ExeFS with the debug option on). This module:

  find()            folders that look like such an extraction: the emulators' own dump folders and the usual places
                    on this computer or phone (Desktop, Downloads, Documents, Internal storage)
  inspect(folder)   where its exefs and romfs really are (an extra folder level, ExeFS/RomFS spelled any way, the
                    folder itself or one of the two picked, the ExeFS files loose beside romfs), and what is wrong in
                    plain words: missing modules, another game version, a RomFS without the update or cut short

  python tools/b3_dump.py [folder]     prints what it finds (no folder: searches)

Standard library only: it runs in a release's Python, on the phone's, and in the launcher.
"""
import hashlib
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / 'config/bayonetta3-1.2.0.json'
TITLE_ID = '01004a4010fea000'
MODULES = ('main', 'subsdk0', 'sdk')
EMULATORS = ('Ryujinx', 'eden', 'yuzu', 'suyu', 'sudachi', 'citron', 'torzu')
# root_000.pkz of 1.2.0 (tools/bayonetta3_shaders.py ARCHIVES): small, and different without the update.
ROOT_PKZ = '87e7d717d1ced64d155d07e0b5197b2b24c08317b03c89d7ad49671699b2ce88'
ROMFS_MIN_FILES, ROMFS_MIN_BYTES = 500, 13 * 10**9

HOW_TO_EXTRACT = (
    'Extract the game files with your emulator, after installing update 1.2.0 there:\n'
    '  Ryujinx: right-click Bayonetta 3 > Extract Data > ExeFS, pick a folder; then Extract Data > RomFS, same folder.\n'
    '  Eden / yuzu / Citron: right-click Bayonetta 3 > Dump RomFS > Full; for the ExeFS turn on Emulation > '
    'Configure > Debug > "Dump ExeFS" and start the game once.\n'
    'You need a folder with exefs (main, sdk, subsdk0, main.npdm) and romfs (about 15 GB of .pkz files).')


def _children(folder):
    try:
        return {p.name.lower(): p for p in Path(folder).iterdir()}
    except OSError:
        return {}


def _is_exefs(folder):
    c = _children(folder)
    return 'main' in c and ('main.npdm' in c or 'sdk' in c)


def _is_romfs(folder):
    c = _children(folder)
    return any(n.endswith('.pkz') for n in c) or 'movie' in c and any(n.endswith('.pkz') for n in c)


def resolve(folder):
    """(exefs, romfs) under `folder`, each None when not found. Looks at the folder, its exefs/romfs children in any
    case, one folder level down (a dump inside its own folder), and the folder's parent (the player picked the exefs
    or romfs folder itself)."""
    folder = Path(folder)
    candidates = [folder] + [p for p in _children(folder).values() if p.is_dir()]
    if folder.parent != folder:
        candidates.append(folder.parent)
    exefs = romfs = None
    for base in candidates:
        c = _children(base)
        if exefs is None:
            if 'exefs' in c and _is_exefs(c['exefs']):
                exefs = c['exefs']
            elif _is_exefs(base):
                exefs = base
        if romfs is None:
            if 'romfs' in c and _is_romfs(c['romfs']):
                romfs = c['romfs']
            elif 'romfs' in c and 'romfs' in _children(c['romfs']) and _is_romfs(_children(c['romfs'])['romfs']):
                romfs = _children(c['romfs'])['romfs']  # romfs/romfs: extracted into a folder of the same name
            elif _is_romfs(base):
                romfs = base
        if exefs and romfs:
            break
    return exefs, romfs


def build_id(nso):
    """An NSO's module id (build id), the 32 bytes at 0x40 of its header, as hex; None when it is not an NSO."""
    try:
        with open(nso, 'rb') as f:
            head = f.read(0x60)
    except OSError:
        return None
    if head[:4] != b'NSO0':
        return None
    return head[0x40:0x60].hex()


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def inspect(folder, deep=True):
    """{'folder', 'exefs', 'romfs', 'ok', 'problems': [plain sentences], 'summary'} for a picked or found folder.
    deep also hashes the RomFS's small root archive (a fraction of a second) to tell 1.2.0 from the base game."""
    folder = Path(folder)
    exefs, romfs = resolve(folder)
    problems = []
    config = json.loads(CONFIG.read_text(encoding='utf-8')) if CONFIG.is_file() else {'modules': {}}
    if exefs is None:
        problems.append('No ExeFS found (a folder with main, sdk, subsdk0 and main.npdm). Extract it with your '
                        'emulator as well: it is a separate step from the RomFS.')
    else:
        c = _children(exefs)
        missing = [m for m in MODULES + ('main.npdm',) if m not in c]
        if missing:
            problems.append(f'The ExeFS in {exefs} lacks {", ".join(missing)}: extract the ExeFS again.')
        for m in MODULES:
            want = config['modules'].get(m, {}).get('build_id')
            if m in c and want:
                got = build_id(c[m])
                if got is None:
                    problems.append(f'{c[m]} is not a game module (still compressed or encrypted?): extract the '
                                    'ExeFS with your emulator.')
                    break
                if got != want:
                    problems.append('The ExeFS is not from version 1.2.0 (another update, or the base game): install '
                                    'update 1.2.0 in your emulator, then extract the ExeFS again.')
                    break
    if romfs is None:
        problems.append('No RomFS found (a folder of .pkz files, about 15 GB). Extract the RomFS with your emulator '
                        '(Ryujinx: Extract Data > RomFS; Eden / yuzu: Dump RomFS > Full).')
    else:
        files = total = 0
        for d, _, names in os.walk(romfs):
            for n in names:
                files += 1
                try:
                    total += os.path.getsize(os.path.join(d, n))
                except OSError:
                    pass
        c = _children(romfs)
        if files < ROMFS_MIN_FILES or total < ROMFS_MIN_BYTES:
            problems.append(f'The RomFS in {romfs} looks cut short ({files} files, {total / 1e9:.1f} GB; a complete '
                            'one is about 557 files, 15 GB): extract or copy it again.')
        elif deep and 'root_000.pkz' in c and sha256(c['root_000.pkz']) != ROOT_PKZ:
            problems.append('The RomFS is not from version 1.2.0 (the base game without the update?): install update '
                            '1.2.0 in your emulator and extract the RomFS again ("Full" in Eden / yuzu).')
    ok = not problems
    where = f'{exefs.parent if exefs and romfs and exefs.parent == romfs.parent else folder}'
    return {'folder': str(folder), 'exefs': str(exefs) if exefs else None, 'romfs': str(romfs) if romfs else None,
            'ok': ok, 'problems': problems,
            'summary': f'Ready: {where}' if ok else problems[0]}


def search_roots():
    """Where extractions usually end up, most likely first."""
    home = Path.home()
    roots = []
    if hasattr(sys, 'getandroidapilevel') or Path('/sdcard').is_dir() and not os.name == 'nt':
        sd = Path('/sdcard')
        roots += [sd / 'Bayonetta 3/dump', sd / 'Download', sd / 'Documents', sd]
    appdata = os.environ.get('APPDATA')
    data_homes = [Path(appdata)] if appdata else []
    data_homes += [home / '.local/share', home / 'Library/Application Support',
                   home / '.var/app/org.ryujinx.Ryujinx/config']
    for base in data_homes:
        for emu in EMULATORS:
            for case in (emu, emu.lower(), emu.capitalize()):
                d = base / case / 'dump'
                for tid in (TITLE_ID, TITLE_ID.upper()):
                    roots.append(d / tid)
    for name in ('Desktop', 'Downloads', 'Documents', 'Games'):
        roots.append(home / name)
    roots.append(home)
    seen, out = set(), []
    for r in roots:
        key = str(r).lower()
        if key not in seen and r.is_dir():
            seen.add(key)
            out.append(r)
    return out


def find(max_depth=3, limit=8):
    """[inspect(folder, deep=False)] for the folders that look like an extraction (both parts found), best first:
    ready ones before ones with problems."""
    found, seen = [], set()
    for root in search_roots():
        stack = [(root, 0)]
        while stack and len(found) < limit:
            d, depth = stack.pop()
            c = _children(d)
            if ('exefs' in c or 'romfs' in c) and str(d).lower() not in seen:
                exefs, romfs = resolve(d)
                if exefs and romfs:
                    seen.add(str(d).lower())
                    found.append(inspect(d, deep=False))
                    continue
            if depth < max_depth:
                for name, p in c.items():
                    if name.startswith('.') or name in ('appdata', 'library', 'windows', 'node_modules', 'android'):
                        continue
                    try:
                        if p.is_dir() and not p.is_symlink():
                            stack.append((p, depth + 1))
                    except OSError:
                        pass
    found.sort(key=lambda r: not r['ok'])
    return found


def main(argv):
    if len(argv) > 1:
        r = inspect(argv[1])
        print(json.dumps(r, indent=2))
        return 0 if r['ok'] else 1
    hits = find()
    if not hits:
        print('No extracted Bayonetta 3 found.\n' + HOW_TO_EXTRACT)
        return 1
    for r in hits:
        print(('OK   ' if r['ok'] else 'FIX  ') + r['folder'] + ('' if r['ok'] else '\n     ' + r['summary']))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
