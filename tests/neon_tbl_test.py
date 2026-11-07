#!/usr/bin/env python3
"""Check emitted TBL instructions against unsigned byte-table semantics.

Needs Capstone, a C++20 compiler, and no game data. Exercises native SIMD and
portable code with every byte index, 8/16 lanes, 1..4 tables, aliases and v31 wrap.
"""
import ctypes
import os
from pathlib import Path
import platform
import random
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
try:
    import capstone
except ImportError:
    print('SKIP: Capstone is required for the production TBL emitter test')
    raise SystemExit(77)
from recomp import emit


def main():
    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    cs.detail = True
    emitter = emit.Emitter(SimpleNamespace(module=None, functions=[]))
    cases = []
    source = ['#include <nx/neon_tbl.h>']
    for lanes in (8, 16):
        for count in range(1, 5):
            for first in (3, 31):
                last = (first + count - 1) % 32
                for index in sorted({2, first, last}):
                    for dest in sorted({1, index, first, last}):
                        word = (0x0e000000 | ((lanes == 16) << 30) | (index << 16)
                                | ((count - 1) << 13) | (first << 5) | dest)
                        insn = next(cs.disasm(struct.pack('<I', word), 0x1000))
                        assert insn.mnemonic == 'tbl', insn.mnemonic
                        body = emitter.insn(SimpleNamespace(start=0x1000, end=0x1004), insn, {})
                        assert f'nx::neon_tbl<{lanes}>' in body, body
                        source.append(f'static void t{len(cases)}(nx::CPU& c) {{ {body} }}')
                        cases.append((lanes, count, first, index, dest))
    source.append('static nx::Fn tests[] = {' + ','.join(f't{k}' for k in range(len(cases))) + '};')
    source.append('''extern "C" void run(unsigned id, uint8_t* registers) {
        nx::CPU c{};
        std::memcpy(c.v, registers, sizeof(c.v));
        tests[id](c);
        std::memcpy(registers, c.v, sizeof(c.v));
    }''')
    compiler = shlex.split(os.environ.get('CXX', '')) or [shutil.which('clang++') or shutil.which('g++') or 'c++']
    native = ['-mssse3'] if platform.machine().lower() in ('x86_64', 'amd64') else []
    flags = shlex.split(os.environ.get('CXXFLAGS', ''))
    with tempfile.TemporaryDirectory(prefix='bayo3-neon-tbl-') as directory:
        tmp = Path(directory)
        cpp = tmp / 'tbl.cpp'
        cpp.write_text('\n'.join(source) + '\n')
        for name, extra in (('native', native), ('scalar', ['-DNX_TBL_SCALAR=1'])):
            library = tmp / (name + '.so')
            subprocess.run(compiler + ['-std=c++20', '-O2', '-ffp-contract=off', '-fPIC', '-shared',
                           '-I' + str(ROOT / 'runtime/include')] + flags + extra
                           + [str(cpp), '-o', str(library)], check=True)
            run = ctypes.CDLL(str(library)).run
            run.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint8)]
            run.restype = None
            rng = random.Random(0x54424c)
            checked = 0
            for case, (lanes, count, first, index, dest) in enumerate(cases):
                for base in range(0, 256, lanes):
                    before = bytearray(rng.randbytes(512))
                    before[index * 16:index * 16 + 16] = bytes((base + lane) % 256 for lane in range(16))
                    table = b''.join(before[((first + r) % 32) * 16:((first + r) % 32) * 16 + 16]
                                     for r in range(count))
                    expected = before.copy()
                    expected[dest * 16:dest * 16 + 16] = bytes(
                        table[before[index * 16 + lane]] if lane < lanes and before[index * 16 + lane] < len(table)
                        else 0 for lane in range(16))
                    actual = (ctypes.c_uint8 * 512).from_buffer_copy(before)
                    run(case, actual)
                    if bytes(actual) != expected:
                        raise AssertionError(f'{name}: case={cases[case]} index-base={base}')
                    checked += 1
            print(f'PASS TBL {name}: {len(cases)} emitted variants, {checked} complete-register comparisons', flush=True)


if __name__ == '__main__':
    main()
