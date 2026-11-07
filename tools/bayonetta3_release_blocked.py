#!/usr/bin/env python3
"""Build a run data root in which the shaders the translator only warned about are released.

The translation tool (tools/bayonetta3_shaders.py) refuses to publish a module when the
translator logged anything, because a logged message means the translation may be
incomplete. That rule is right for a cache meant to be trusted blindly, but it makes the
runtime stop dead on the first draw that needs such a stage. This script produces a
*separate* data root for an experiment: the published cache is linked as-is, and only the
named hashes get a real .spv plus a results.tsv row. Nothing under private/shader-work is
modified, so the published cache keeps its meaning.

    python3 tools/bayonetta3_release_blocked.py --data-root <existing run root> \\
        --out <new run root> [--hashes <hash>,<hash>... | --all-warned]

--all-warned takes every row of private/shader-work/results-all.tsv whose status is
translator-warning and whose raw module exists. Each released module is recorded in
<out>/released.json with its sha256 and the translator message it carries, so the run's
log can be tied to exactly which warnings were accepted.
"""
import argparse
import csv
import hashlib
import json
import os
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'private/shader-work'
LINKS = ['analysis', 'extract', 'extract_exefs', 'extract_tmp', 'user']


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--data-root', type=Path, required=True, help='an existing run data root to clone')
    p.add_argument('--out', type=Path, required=True, help='new data root (must not exist)')
    p.add_argument('--hashes', default='', help='comma separated shader hashes to release')
    p.add_argument('--all-warned', action='store_true', help='release every translator-warning row')
    a = p.parse_args()

    src, out = a.data_root.resolve(), a.out.resolve()
    if out.exists():
        raise SystemExit(f'{out} exists; refusing to overwrite')
    if not (src / 'spirv').is_dir():
        raise SystemExit(f'{src}/spirv is missing')

    all_rows = {r['hash']: r for r in csv.DictReader((WORK / 'results-all.tsv').open(), delimiter='\t')}
    raw = {}
    for d in (WORK / 'translation-raw').iterdir():
        if d.is_dir():
            for f in d.glob('*.spv'):
                raw[f.stem] = f

    if a.all_warned:
        wanted = [h for h, r in all_rows.items() if r['status'] == 'translator-warning']
    else:
        wanted = [h for h in a.hashes.split(',') if h]
    if not wanted:
        raise SystemExit('no hashes selected')
    missing = [h for h in wanted if h not in raw]
    if missing:
        raise SystemExit(f'no raw module for: {", ".join(missing)}')

    out.mkdir(parents=True)
    (out / 'spirv').mkdir()
    for name in LINKS:
        if (src / name).exists():
            os.symlink((src / name).resolve(), out / name)
    for name in ('users.txt',):
        if (src / name).exists():
            shutil.copyfile(src / name, out / name)

    # The published cache is linked, not copied: 5,700 modules, and a copy would drift.
    published = src / 'spirv'
    linked = 0
    for f in published.iterdir():
        if f.is_file() and f.name != 'results.tsv':
            os.symlink(f.resolve(), out / 'spirv' / f.name)
            linked += 1
    released = []
    for h in wanted:
        target = out / 'spirv' / f'{h}.spv'
        if target.exists() or target.is_symlink():
            target.unlink()
        shutil.copyfile(raw[h], target)
        released.append(dict(hash=h, sha256=sha256(target), stage=all_rows[h]['stage'],
                             translator_messages=all_rows[h]['translator_messages'],
                             spv_bytes=int(all_rows[h]['spv_bytes'])))

    header = (published / 'results.tsv').read_text().splitlines()
    cols = header[0].split('\t')
    with (out / 'spirv' / 'results.tsv').open('w') as f:
        f.write('\n'.join(header) + '\n')
        for h in wanted:
            f.write('\t'.join(all_rows[h].get(c, '') for c in cols) + '\n')

    (out / 'released.json').write_text(json.dumps(dict(source=str(src), linked=linked, released=released), indent=2) + '\n')
    print(f'{out}: {linked} published modules linked, {len(released)} released')
    for r in released:
        print(f"  {r['hash']} {r['stage']:24s} {r['spv_bytes']:6d} B  {r['translator_messages']}")


if __name__ == '__main__':
    main()
