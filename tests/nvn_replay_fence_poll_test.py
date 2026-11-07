#!/usr/bin/env python3
"""Compile and exercise the production replay fence and retirement paths without a GPU."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
assert compiler, 'C++ compiler required'
vulkan = Path(os.environ.get('VULKAN_HEADERS', root / 'private/toolchains/Vulkan-Headers-1.4.357/include'))
with tempfile.TemporaryDirectory(prefix='bayo3-nvn-replay-fence-') as tmp:
    binary = str(Path(tmp) / 'test')
    subprocess.run([compiler, '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections',
                    '-Wl,--gc-sections', '-pthread', '-DNX_BAYONETTA3',
                    '-I' + str(root / 'runtime/include'), '-I' + str(root / 'runtime/src'),
                    '-I' + str(vulkan), str(root / 'tests/nvn_replay_fence_poll_test.cpp'),
                    '-o', binary], check=True)
    subprocess.run([binary], check=True, timeout=10)
    subprocess.run([binary, 'off'], check=True, timeout=10)
    subprocess.run([binary, 'pool'], check=True, timeout=10)
    subprocess.run([binary, 'carry'], check=True, timeout=10)
