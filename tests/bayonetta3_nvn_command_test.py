#!/usr/bin/env python3
"""Build production NVN recorder/replay/raster/write tracking and test without a game."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
assert compiler, 'CXX, clang++ or g++ required'
vulkan = Path(os.environ.get('VULKAN_HEADERS', root / 'private/toolchains/Vulkan-Headers-1.4.357/include'))
sources = ['nvn_cmd.cpp', 'nvn_cmd_replay.cpp', 'nvn_replay.cpp', 'nvn_raster.cpp', 'nvn_written.cpp', 'nvn_buffer_copy.cpp', 'nvn_zcull.cpp']
with tempfile.TemporaryDirectory(prefix='bayo3-nvn-command-') as tmp:
    binary = str(Path(tmp) / 'test')
    subprocess.run([compiler, '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections',
                    '-Wl,--gc-sections', '-pthread', '-DNX_BAYONETTA3',
                    '-I' + str(root / 'runtime/include'), '-I' + str(root / 'runtime/src'),
                    '-I' + str(vulkan), str(root / 'tests/bayonetta3_nvn_command_test.cpp'),
                    *[str(root / 'runtime/src/nvn' / p) for p in sources], '-o', binary], check=True)
    for mode in ('default', 'old-slope', 'no-slack', 'more-units'):
        subprocess.run([binary, mode], check=True)
