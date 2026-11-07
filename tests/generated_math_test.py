#!/usr/bin/env python3
"""Compare generated sqrt expressions with and without host math errno work.

This preserves the existing host arithmetic mapping; it is not an ARM FPCR/FPSR
conformance test. Host errno is intentionally outside the comparison.
"""
import ctypes
import os
from pathlib import Path
import platform
import random
import shlex
import shutil
import subprocess
import tempfile

SOURCE = r'''
#include <bit>
#include <cmath>
#include <cstdint>
extern "C" void run(const uint64_t* input, uint64_t* output, unsigned count, bool wide) {
    for (unsigned i = 0; i < count; ++i) {
        if (wide) {
            const double v = std::bit_cast<double>(input[i]);
            output[2 * i] = std::bit_cast<uint64_t>(std::sqrt(v));
            output[2 * i + 1] = std::bit_cast<uint64_t>(1.0 / std::sqrt(v));
        } else {
            const float v = std::bit_cast<float>(uint32_t(input[i]));
            output[2 * i] = std::bit_cast<uint32_t>(std::sqrt(v));
            output[2 * i + 1] = std::bit_cast<uint32_t>(1.0f / std::sqrt(v));
        }
    }
}
'''


def main():
    compiler = shlex.split(os.environ.get('CXX', '')) or [shutil.which('clang++') or shutil.which('g++') or 'c++']
    arch = ['-march=x86-64-v3'] if platform.machine().lower() in ('x86_64', 'amd64') else []
    rng = random.Random(0x53515254)
    with tempfile.TemporaryDirectory(prefix='bayo3-generated-math-') as directory:
        tmp = Path(directory)
        cpp = tmp / 'math.cpp'
        cpp.write_text(SOURCE)
        runners = []
        for name, flag in (('before', '-fmath-errno'), ('after', '-fno-math-errno')):
            library = tmp / (name + '.so')
            subprocess.run(compiler + ['-std=c++20', '-O2', '-ffp-contract=off', flag, '-fPIC', '-shared']
                           + arch + [str(cpp), '-o', str(library)], check=True)
            run = ctypes.CDLL(str(library)).run
            run.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64),
                            ctypes.c_uint, ctypes.c_bool]
            run.restype = None
            runners.append(run)
        for bits, edges in ((32, [0, 1, 0x007fffff, 0x00800000, 0x3f800000, 0x40000000,
                                  0x7f7fffff, 0x7f800000, 0x7f800001, 0x7fc12345]),
                            (64, [0, 1, 0x000fffffffffffff, 0x0010000000000000,
                                  0x3ff0000000000000, 0x4000000000000000,
                                  0x7fefffffffffffff, 0x7ff0000000000000,
                                  0x7ff0000000000001, 0x7ff8123456789abc])):
            values = [v | sign for v in edges for sign in (0, 1 << (bits - 1))]
            values += [rng.getrandbits(bits) for _ in range(4096)]
            inputs = (ctypes.c_uint64 * len(values))(*values)
            results = []
            for run in runners:
                output = (ctypes.c_uint64 * (2 * len(values)))()
                run(inputs, output, len(values), bits == 64)
                results.append(output)
            for i, (before, after) in enumerate(zip(*results)):
                if before != after:
                    raise AssertionError(f'{bits}-bit input={values[i // 2]:x} result={i % 2}: '
                                         f'before={before:x} after={after:x}')
            print(f'PASS generated math {bits}-bit: {2 * len(values)} bitwise sqrt/reciprocal-sqrt comparisons', flush=True)


if __name__ == '__main__':
    main()
