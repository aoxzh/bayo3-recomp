#!/usr/bin/env python3
"""Exercise production Program HLE and deferred selected-stage completeness."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
assert compiler
vulkan = Path(os.environ.get('VULKAN_HEADERS', root / 'private/toolchains/Vulkan-Headers-1.4.357/include'))
with tempfile.TemporaryDirectory(prefix='b3-nvn-program-') as tmp:
    binary = str(Path(tmp) / 'test')
    subprocess.run([compiler, '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections',
                    '-Wl,--gc-sections', '-pthread', '-DNX_BAYONETTA3',
                    '-I' + str(root/'runtime/include'), '-I' + str(root/'runtime/src'), '-I' + str(vulkan),
                    str(root/'tests/bayonetta3_nvn_program_test.cpp'),
                    *[str(root/'runtime/src/nvn'/name) for name in
                      ['nvn_program.cpp', 'nvn_spirv_reflect.cpp']], '-o', binary], check=True)
    subprocess.run([binary], check=True)
    subprocess.run([binary, 'strict'], check=True)
