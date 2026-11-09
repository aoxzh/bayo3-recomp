#!/usr/bin/env python3
"""Bring a dump's exefs/ and romfs/ into this folder, so the folder holds the whole game and can be moved.

    python3 tools/b3_data.py --dump <folder with exefs/ and romfs/> [--to private/data/b3] [--mode link]

Modes: link (hard links: same drive, no extra space, the dump stays whole; the default), copy (another drive),
move (out of the dump). Files already there with the same size are kept, so a stopped run resumes. When it is done,
<to>/complete.json records what came from where.

On Windows `tools/b3_win.py setup` runs this as its `data` step. On Linux and the Steam Deck run it before the
generation and point that at the result (docs/PLAYING.md, "Linux / Steam Deck"):

    python3 tools/b3_data.py --dump ~/dumps/bayonetta3
    python3 tools/prepare_bayonetta3.py ... --exefs private/data/b3/exefs --romfs private/data/b3/romfs
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def child(folder, name):
    """The entry `name` under folder, case-insensitively (dumps use exefs/ExeFS, romfs/RomFS)."""
    if not folder or not Path(folder).is_dir():
        return None
    for entry in Path(folder).iterdir():
        if entry.name.lower() == name.lower():
            return entry
    return None


def same_drive_link(src, dst):
    """A hard link dst -> src; False when the two are on different drives (or the file system has no hard links)."""
    try:
        os.link(src, dst)
        return True
    except OSError as e:
        if getattr(e, 'winerror', None) == 17 or e.errno in (18,):  # ERROR_NOT_SAME_DEVICE / EXDEV
            return False
        raise


def bring_tree(src, dst, mode):
    """Every file of src into dst by `mode` (link / copy / move); returns (files, bytes), or None when link cannot
    work across drives. Files already there with the same size are kept, so an interrupted run resumes."""
    files = size = 0
    for folder, _, names in os.walk(src):
        rel = Path(folder).relative_to(src)
        (dst / rel).mkdir(parents=True, exist_ok=True)
        for name in names:
            a, b = Path(folder) / name, dst / rel / name
            n = a.stat().st_size
            if not (b.exists() and b.stat().st_size == n):
                if b.exists():
                    b.unlink()
                if mode == 'link':
                    if not same_drive_link(a, b):
                        return None
                elif mode == 'move':
                    shutil.move(a, b)
                else:
                    shutil.copy2(a, b)
            files += 1
            size += n
    return files, size


def bring_dump(exefs, romfs, here, mode, say=print):
    """exefs/ and romfs/ into `here`. Returns the record written to here/complete.json, or None when link mode met
    another drive (nothing is left behind then)."""
    done = {}
    for name, src in (('exefs', exefs), ('romfs', romfs)):
        got = bring_tree(Path(src), Path(here) / name, mode)
        if got is None:
            shutil.rmtree(here, ignore_errors=True)
            say(f'data: {src} is on another drive, so it cannot be hard-linked; it stays where it is '
                f'(--mode copy copies it instead, taking as much space again)')
            return None
        done[name] = {'files': got[0], 'bytes': got[1], 'from': str(src)}
    record = {'mode': mode, **done, 'at': time.strftime('%Y-%m-%d %H:%M:%S')}
    (Path(here) / 'complete.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
    gb = sum(d['bytes'] for d in done.values()) / 1e9
    say(f'data: {sum(d["files"] for d in done.values())} files, {gb:.1f} GB in {here} '
        f'({"no extra space" if mode == "link" else "moved" if mode == "move" else "copied"})')
    return record


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--dump', required=True, help='the folder with exefs/ and romfs/')
    p.add_argument('--to', default=str(ROOT / 'private/data/b3'), help='where (default: private/data/b3)')
    p.add_argument('--mode', choices=['link', 'copy', 'move'], default='link')
    a = p.parse_args(argv)
    exefs, romfs = child(a.dump, 'exefs'), child(a.dump, 'romfs')
    if exefs is None and child(a.dump, 'main') is not None:
        exefs = Path(a.dump)  # program 1 as some tools extract it: the ExeFS files beside romfs/
    if exefs is None or romfs is None:
        print(f'b3_data: {a.dump} must hold exefs/ and romfs/', file=sys.stderr)
        return 1
    here = Path(a.to).resolve()
    if (here / 'complete.json').exists():
        print(f'data: already in {here}')
        return 0
    return 0 if bring_dump(exefs, romfs, here, a.mode) else 1


if __name__ == '__main__':
    sys.exit(main())
