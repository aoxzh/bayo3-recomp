#!/usr/bin/env python3
"""Add verified B3 runtime hooks without regenerating or replacing other code.

Defaults to a dry run. --apply requires a fresh backup directory; every changed
translation unit is retained byte-for-byte before publication. The exact input
NSO must match the version supported by recomp.b3_hooks.
"""
import argparse
import hashlib
from pathlib import Path
import re
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from recomp import b3_hooks


def function_match(text, opening):
    matches = list(re.finditer(r'^' + re.escape(opening) + r'\n.*?^}', text, re.M | re.S))
    if len(matches) != 1:
        raise ValueError('Expected one complete function: ' + opening)
    if re.search(r'^(?:static )?void main_', matches[0].group(0)[len(opening):], re.M):
        raise ValueError('Function boundary crossed: ' + opening)
    return matches[0]


def refresh(text, source_sha256):
    if source_sha256 != b3_hooks.MAIN_SHA256:
        raise ValueError('Unsupported NSO identity; generated code was not changed')
    found = set()
    updated = text
    for entry in b3_hooks.ENTRIES:
        opening = b3_hooks.signature(entry)
        if opening not in updated:
            continue
        found.add(entry)
        match = function_match(updated, opening)
        body = match.group(0)
        if entry == b3_hooks.SCENE_CLOCK and 'nx::b3::scene_clock_after(' in body:
            original_opening = opening.replace('void main_', 'static void main_', 1).replace(
                '(nx::CPU&', '_original(nx::CPU&', 1)
            original = function_match(updated, original_opening).group(0)
            expected = b3_hooks.apply(original.replace(original_opening, opening, 1),
                                      'main', entry, source_sha256)
            if original + '\n\n' + body != expected:
                raise ValueError('Locally modified scene-clock wrapper; refusing replacement')
            continue
        replacement = b3_hooks.apply(body, 'main', entry, source_sha256)
        updated = updated[:match.start()] + replacement + updated[match.end():]
    if found:
        includes = b3_hooks.includes(updated)
        missing = ''.join(line for line in includes.splitlines(keepends=True)
                          if line.strip() not in updated.splitlines())
        if missing:
            marker = '#include "funcs.h"\n'
            if updated.count(marker) != 1:
                raise ValueError('Expected exactly one funcs.h include')
            updated = updated.replace(marker, marker + missing, 1)
    return updated, found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('generated', type=Path)
    parser.add_argument('--nso', type=Path, required=True)
    parser.add_argument('--apply', action='store_true')
    parser.add_argument('--backup', type=Path)
    args = parser.parse_args()
    root = args.generated.resolve(strict=True)
    main_dir = root / 'main'
    if not main_dir.is_dir() or main_dir.is_symlink():
        parser.error('generated/main must be a real directory')
    digest = hashlib.sha256(args.nso.read_bytes()).hexdigest()
    if digest != b3_hooks.MAIN_SHA256:
        parser.error('NSO SHA256 does not match supported Bayonetta 3 1.2.0 main')
    if args.apply and (args.backup is None or args.backup.exists() or args.backup.is_symlink()):
        parser.error('--apply requires a fresh --backup directory')
    seen = set()
    changes = []
    for path in sorted(main_dir.glob('code_*.cpp')):
        if path.is_symlink() or not path.resolve().is_relative_to(root):
            raise ValueError('Refusing generated symlink: ' + str(path))
        original = path.read_bytes()
        text = original.decode('utf-8')
        if '\r' in text:
            raise ValueError('Expected LF generated source: ' + str(path))
        updated, found = refresh(text, digest)
        if seen & found:
            raise ValueError('Repeated hook entry across translation units')
        seen.update(found)
        if updated != text:
            changes.append((path, original, updated.encode('utf-8')))
    if seen != set(b3_hooks.ENTRIES):
        raise ValueError('Generated tree lacks one or more supported entry points')
    print(f'{len(seen)} verified hook entries; {len(changes)} changed translation units; root={root}', flush=True)
    if not args.apply or not changes:
        return
    backup = args.backup.absolute()
    backup.mkdir(parents=True, exist_ok=False)
    for path, original, _ in changes:
        if path.read_bytes() != original:
            raise ValueError('Source changed while preparing: ' + str(path))
        saved = backup / path.relative_to(root)
        saved.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, saved)
    for path, original, updated in changes:
        if path.read_bytes() != original:
            raise ValueError('Source changed before publication: ' + str(path))
        path.write_bytes(updated)
    print(f'Preserved unrelated code; originals retained at {backup}', flush=True)


if __name__ == '__main__':
    main()
