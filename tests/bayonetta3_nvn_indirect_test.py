#!/usr/bin/env python3
"""Compile production indirect record/replay and shared GPU tracking; no game run."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
assert compiler, 'C++ compiler required'
vulkan = Path(os.environ.get('VULKAN_HEADERS', root / 'private/toolchains/Vulkan-Headers-1.4.357/include'))
with tempfile.TemporaryDirectory(prefix='bayo3-nvn-indirect-') as tmp:
    binary = str(Path(tmp) / 'test')
    subprocess.run([compiler, '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections',
                    '-Wl,--gc-sections', '-pthread', '-DNX_BAYONETTA3',
                    '-I' + str(root / 'runtime/include'), '-I' + str(root / 'runtime/src'),
                    '-I' + str(vulkan), str(root / 'tests/bayonetta3_nvn_indirect_test.cpp'),
                    str(root / 'runtime/src/nvn/nvn_indirect.cpp'),
                    str(root / 'runtime/src/nvn/nvn_written.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)
    subprocess.run([binary, 'separate'], env={**os.environ, 'NX_COMPUTE_SEPARATE': '1'}, check=True)
