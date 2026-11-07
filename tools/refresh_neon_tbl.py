#!/usr/bin/env python3
"""Refresh only recognized legacy TBL statements in an existing generated tree.

Unlike full regeneration this preserves local fixes to other instructions. The
current production emitter supplies the replacements. Default is a dry run;
--apply requires a fresh --backup directory and saves every changed file there.
"""
import argparse
from pathlib import Path
import re
import shutil
import struct
import sys
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import capstone
from recomp import emit

ASM = re.compile(r'tbl v(\d+)\.(8|16)b, \{([^}]+)\}, v(\d+)\.\2b')


def refresh(text, emitter, cs):
    result = []
    count = 0
    for line in text.splitlines(keepends=True):
        if ' // tbl ' not in line:
            result.append(line)
            continue
        code, asm = line.rstrip('\r\n').split(' // ', 1)
        match = ASM.fullmatch(asm)
        if not match:
            raise ValueError('Unrecognized TBL assembly: ' + asm)
        dest, lanes, table_text, index = match.groups()
        dest, lanes, index = int(dest), int(lanes), int(index)
        names = table_text.split(', ')
        if not 1 <= len(names) <= 4 or any(not re.fullmatch(r'v\d+\.16b', n) for n in names):
            raise ValueError('Unrecognized table list: ' + asm)
        tables = [int(n[1:-4]) for n in names]
        if any(not 0 <= n < 32 for n in [dest, index] + tables):
            raise ValueError('Out-of-range register: ' + asm)
        if tables != [(tables[0] + n) % 32 for n in range(len(tables))]:
            raise ValueError('Nonconsecutive tables: ' + asm)
        word = (0x0e000000 | ((lanes == 16) << 30) | (index << 16)
                | ((len(tables) - 1) << 13) | (tables[0] << 5) | dest)
        insn = next(cs.disasm(struct.pack('<I', word), 0x1000))
        replacement = emitter.insn(SimpleNamespace(start=0x1000, end=0x1004), insn, {})
        if code.strip() == replacement:
            result.append(line)
            continue
        copies = ' '.join(f'std::memcpy(t_ + {n * 16}, &c.v[{reg}], 16);' for n, reg in enumerate(tables))
        legacy = (f'{{ uint8_t t_[64]; {copies} nx::V128 I_ = c.v[{index}]; '
                  f'nx::V128 r = nx::vzero(); for (int k = 0; k < {lanes}; k++) '
                  f'r.u8[k] = I_.u8[k] < {len(tables) * 16} ? t_[I_.u8[k]] : 0; c.v[{dest}] = r; }}')
        if code.strip() != legacy:
            raise ValueError('Locally modified TBL body; refusing replacement: ' + asm)
        indent = code[:len(code) - len(code.lstrip())]
        newline = '\r\n' if line.endswith('\r\n') else '\n' if line.endswith('\n') else ''
        result.append(indent + replacement + ' // ' + asm + newline)
        count += 1
    updated = ''.join(result)
    if count and '#include <nx/neon_tbl.h>' not in updated:
        marker = '#include "funcs.h"'
        if updated.count(marker) != 1:
            raise ValueError('Missing or repeated funcs.h include')
        newline = '\r\n' if '\r\n' in updated else '\n'
        updated = updated.replace(marker, marker + newline + '#include <nx/neon_tbl.h>', 1)
    return updated, count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('generated', type=Path)
    parser.add_argument('--apply', action='store_true')
    parser.add_argument('--backup', type=Path)
    args = parser.parse_args()
    root = args.generated.resolve(strict=True)
    if not root.is_dir():
        parser.error('generated must be a directory')
    if args.apply and (args.backup is None or args.backup.exists() or args.backup.is_symlink()):
        parser.error('--apply requires a fresh --backup directory')
    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    cs.detail = True
    emitter = emit.Emitter(SimpleNamespace(module=None, functions=[]))
    changes = []
    for path in sorted(root.glob('*/code_*.cpp')):
        if path.is_symlink() or not path.resolve().is_relative_to(root):
            raise ValueError('Refusing generated symlink: ' + str(path))
        original = path.read_bytes()
        updated, count = refresh(original.decode('utf-8'), emitter, cs)
        if count:
            changes.append((path, original, updated.encode('utf-8'), count))
    print(f'{sum(c[3] for c in changes)} TBL statements in {len(changes)} files; root={root}', flush=True)
    if not args.apply or not changes:
        return
    backup = args.backup.absolute()
    backup.mkdir(parents=True, exist_ok=False)
    # Check every source before touching any of them, then retain the exact old bytes.
    for path, original, _, _ in changes:
        if path.read_bytes() != original:
            raise ValueError('Generated file changed while preparing: ' + str(path))
        saved = backup / path.relative_to(root)
        saved.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, saved)
    for path, original, updated, _ in changes:
        if path.read_bytes() != original:
            raise ValueError('Generated file changed before publication: ' + str(path))
        path.write_bytes(updated)
    print(f'Updated only recognized TBL statements; original files retained at {backup}', flush=True)


if __name__ == '__main__':
    main()
