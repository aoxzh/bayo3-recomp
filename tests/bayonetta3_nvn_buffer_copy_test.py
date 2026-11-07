#!/usr/bin/env python3
"""Build the production copy HLE, recorder and GPU-written tracking without a game."""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
assert compiler, 'CXX, clang++ or g++ required'
vulkan = Path(os.environ.get('VULKAN_HEADERS', root / 'private/toolchains/Vulkan-Headers-1.4.357/include'))
with tempfile.TemporaryDirectory(prefix='bayo3-nvn-buffer-copy-') as tmp:
    binary = str(Path(tmp) / 'test')
    arch_flags = ['-march=x86-64-v3'] if platform.machine().lower() in ('x86_64', 'amd64') else []
    subprocess.run([compiler, '-std=c++20', '-O1', *arch_flags, '-ffunction-sections', '-fdata-sections',
                    '-Wl,--gc-sections', '-pthread', '-DNX_BAYONETTA3',
                    '-I' + str(root / 'runtime/include'), '-I' + str(root / 'runtime/src'),
                    '-I' + str(vulkan), str(root / 'tests/bayonetta3_nvn_buffer_copy_test.cpp'),
                    *[str(root / 'runtime/src/nvn' / p) for p in
                      ['nvn_cmd.cpp', 'nvn_buffer_copy.cpp', 'nvn_written.cpp']],
                    '-o', binary], check=True)
    os.environ['NX_EARLY_COMPLETE'] = '0'  # the whole-range paths under test: an early landing always merges
    # Dedicated processes isolate cached switches and the false-clean regression.
    for simd in ('0', '1'):
        for direct in ('0', '1'):
            subprocess.run([binary, 'merge-learning'],
                           env={**os.environ, 'NX_WRITE_BACK_SIMD': simd, 'NX_WRITE_BACK_DIRECT': direct},
                           check=True, timeout=30)
    # NX_REPLAY_CUTS is on by default now (barrier + cache): the plain run checks the cuts, and every arm below pins the
    # pre-cut behaviour or one cut on its own (the switch is read once per process, so one run per value).
    subprocess.run([binary], check=True)                                                    # default: barrier + cache
    subprocess.run([binary], env={**os.environ, 'NX_REPLAY_COUNTS': '1'}, check=True)       # counters + hazard probe
    for extra in ({'NX_REPLAY_CUTS': '0'},                                                   # pre-cut behaviour
                  {'NX_REPLAY_CUTS': 'barrier'}, {'NX_REPLAY_CUTS': 'cache'},
                  {'NX_REPLAY_CUTS': 'pipeline'}, {'NX_REPLAY_CUTS': '1'},
                  {'NX_REPLAY_CUTS': '1', 'NX_REPLAY_COUNTS': '1'}):
        subprocess.run([binary], env={**os.environ, **extra}, check=True)
    subprocess.run([binary, 'separate'], env={**os.environ, 'NX_COMPUTE_SEPARATE': '1', 'NX_REPLAY_CUTS': '0'},
                   check=True)
