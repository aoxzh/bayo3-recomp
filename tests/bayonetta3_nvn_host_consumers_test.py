#!/usr/bin/env python3
"""Build production texture uploads/write tracking and test without a GPU or game."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
assert compiler, 'CXX, clang++ or g++ required'
vulkan = Path(os.environ.get('VULKAN_HEADERS', root / 'private/toolchains/Vulkan-Headers-1.4.357/include'))
with tempfile.TemporaryDirectory(prefix='bayo3-nvn-host-consumers-') as tmp:
    binary = str(Path(tmp) / 'test')
    subprocess.run([compiler, '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections',
                    '-Wl,--gc-sections', '-pthread', '-DNX_BAYONETTA3',
                    '-I' + str(root / 'runtime/include'), '-I' + str(root / 'runtime/src'),
                    '-I' + str(vulkan), str(root / 'tests/bayonetta3_nvn_host_consumers_test.cpp'),
                    *[str(root / 'runtime/src/nvn' / p) for p in
                      ['nvn_upload.cpp', 'nvn_written.cpp', 'nvn_tiling.cpp']],
                    '-o', binary], check=True)
    subprocess.run([binary], check=True)
