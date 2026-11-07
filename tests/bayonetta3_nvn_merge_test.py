#!/usr/bin/env python3
"""Compile and check write-back kernels without Vulkan or a game.

The x86-64 run exercises both the portable fallback and the project's required
x86-64-v3 target. Other architectures exercise the portable fallback only.
The optional microbenchmark in the C++ source is never run by this test.
"""
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
assert compiler, 'CXX, clang++ or g++ required'
x86 = platform.machine().lower() in ('x86_64', 'amd64')
modes = [('scalar', ['-mno-avx2'] if x86 else [])]
if x86:
    modes.append(('avx2', ['-march=x86-64-v3']))
with tempfile.TemporaryDirectory(prefix='bayo3-nvn-merge-') as tmp:
    for mode, flags in modes:
        binary = str(Path(tmp) / mode)
        subprocess.run([*shlex.split(compiler), '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                        *flags, str(root / 'tests/bayonetta3_nvn_merge_test.cpp'), '-o', binary], check=True)
        subprocess.run([binary], check=True, timeout=30)
