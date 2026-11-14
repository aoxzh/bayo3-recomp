#!/usr/bin/env python3
"""Make a program's generated code and runtime image match the code patches (.ips, .pchtxt) of its mods.

    python tools/apply_code_patches.py --cpu private/win/cpu/b3 --exefs <exefs> --mods private/win/run/b3/mods \
        [--module main ...] [--check]

For each module (default: those whose patches differ from `--recorded`), the recompiler runs again on the module with
its patches (recomp/patches.py) into gen/<module>.new; files that came out the same are left alone, so the build
compiles only the functions a patch touched, and analysis/<module>.bin (the image the runtime loads) is written with
the same patches. A module whose mods no longer patch it goes back to the plain code the same way.

Prints one line per module, `patches <module> <fingerprint or ->`, which tools/b3_win.py keeps in its state.
Run with the tools venv's Python (the NSO reader needs lz4).
"""
import argparse
import filecmp
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from recomp import nso, patches  # noqa: E402


def fingerprints(exefs, mods, names):
    files = patches.find_files(mods) if mods else []
    out = {}
    for name in names:
        module_id = patches.build_id((exefs / name).read_bytes()[:0x60])
        out[name] = patches.fingerprint(module_id, files)
    return out, files


def sync(new, old):
    """Make `old` hold what `new` holds, touching only files that differ; returns how many changed."""
    old.mkdir(exist_ok=True)
    changed = 0
    for f in sorted(new.iterdir()):
        target = old / f.name
        if target.is_file() and filecmp.cmp(f, target, shallow=False):
            continue
        os.replace(f, target)
        changed += 1
    return changed


def regenerate(cpu, exefs, name, files, base):
    gen = cpu / 'gen' / name
    new = cpu / 'gen' / f'.{name}.new'
    shutil.rmtree(new, ignore_errors=True)
    cmd = [sys.executable, '-m', 'recomp', str(exefs / name), str(new), '--name', name, '--base', base]
    if files:
        cmd += ['--patch', *map(str, files)]
    subprocess.run(cmd, cwd=ROOT, check=True)
    if not (new / 'table.cpp').is_file() or not list(new.glob('code_*.cpp')):
        raise SystemExit(f'{name}: the recompiler wrote no code')
    # Files the new output does not have (fewer translation units) go; the build lists the directory.
    stale = [f for f in gen.glob('code_*.cpp') if not (new / f.name).exists()]
    changed = sync(new, gen)
    for f in stale:
        if f.exists():
            f.unlink()
            changed += 1
    shutil.rmtree(new, ignore_errors=True)
    module = nso.load(str(exefs / name), name)
    module_id = patches.build_id((exefs / name).read_bytes()[:0x60])
    problems = patches.apply(module.image, patches.patches_for(module_id, files))
    if problems:
        raise SystemExit('code patch: ' + '; '.join(problems))
    image = cpu / 'analysis' / f'{name}.bin'
    tmp = image.with_name(image.name + '.new')
    tmp.write_bytes(module.image)
    os.replace(tmp, image)
    return changed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cpu', type=Path, required=True, help='the program\'s cpu output (manifest.json, gen/, analysis/)')
    ap.add_argument('--exefs', type=Path, required=True)
    ap.add_argument('--mods', type=Path)
    ap.add_argument('--recorded', default='{}', help='JSON {module: fingerprint} the code was last made with')
    ap.add_argument('--module', action='append', help='make these again whatever was recorded')
    ap.add_argument('--check', action='store_true', help='only print the fingerprints, change nothing')
    a = ap.parse_args()
    manifest = json.loads((a.cpu / 'manifest.json').read_text(encoding='utf-8'))
    generated = manifest.get('generated', {})
    names = sorted(generated)
    prints, files = fingerprints(a.exefs, a.mods, names)
    if not a.check:
        recorded = json.loads(a.recorded)
        todo = a.module or [n for n in names if prints[n] != recorded.get(n, '')]
        for name in todo:
            command = generated[name].get('command', [])
            base = command[command.index('--base') + 1] if '--base' in command else None
            if not base:
                raise SystemExit(f'{name}: the manifest does not say where the module loads')
            print(f'{name}: making the code again{" with code patches" if prints[name] else " without code patches"}',
                  flush=True)
            changed = regenerate(a.cpu, a.exefs, name, files, base)
            print(f'{name}: {changed} generated files changed', flush=True)
    for name in names:
        print(f'patches {name} {prints[name] or "-"}')


if __name__ == '__main__':
    main()
