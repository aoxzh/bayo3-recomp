#!/usr/bin/env python3
"""Exercise compute retention and write-back against an independent oracle, without a GPU."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
options = set(sys.argv[1:])
assert options <= {'--sanitize'} and len(options) == len(sys.argv[1:]), __doc__
compiler = shlex.split(os.environ['CXX']) if os.environ.get('CXX') else [shutil.which('clang++') or shutil.which('g++')]
assert compiler[0], 'C++ compiler required'
vulkan = Path(os.environ.get('VULKAN_HEADERS', root / 'private/toolchains/Vulkan-Headers-1.4.357/include'))
assert (vulkan / 'vulkan/vulkan.h').is_file(), f'Vulkan headers not found: {vulkan}'
flags = ['-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections', '-pthread', '-Wl,--gc-sections',
         '-I' + str(root / 'runtime/include'), '-I' + str(root / 'runtime/src'), '-I' + str(vulkan)]
if '--sanitize' in options:
    flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
env = {k: v for k, v in os.environ.items() if not k.startswith('NX_')}
env['NX_EARLY_COMPLETE'] = '0'  # the whole-range paths under test: an early landing always merges
modes = ('default', 'sync', 'chunk', 'kept', 'fence', 'overlap', 'carry', 'carry-exits', 'fill', 'fill-merge', 'fill-overlap',
         'failure-recorded', 'mapping', 'clean-no-read', 'clean-no-read-simd',
         'kernel-io', 'kernel-io-kept', 'kernel-io-no-snapshot', 'kernel-io-kept-no-snapshot',
         'differential', 'differential-staging', 'differential-simd', 'differential-simd-staging', 'parallel', 'parallel-simd',
         'snapshot-cycle', 'snapshot-cycle-plain', 'snapshot-owner', 'snapshot-owner-plain', 'snapshot-owner-parallel',
         'snapshot-selection', 'snapshot-saturation', 'snapshot-overlap',
         'no-coherent', 'allocate-failure', 'bind-failure', 'map-failure', 'budget', 'log',
         'memory-types', 'memory-types-fallback-log', 'direct-import', 'direct-import-zero',
         'direct-import-host', 'direct-import-read', 'direct-import-host-zero', 'direct-import-read-zero')
with tempfile.TemporaryDirectory(prefix='bayo3-nvn-compute-keep-') as tmp:
    binary = str(Path(tmp) / 'test')
    subprocess.run([*compiler, *flags, str(root / 'tests/nvn_compute_keep_test.cpp'), '-o', binary], check=True)
    for mode in modes:
        subprocess.run([binary, mode], env=env, check=True, timeout=30)
