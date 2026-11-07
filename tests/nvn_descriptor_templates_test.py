#!/usr/bin/env python3
"""Build and run the descriptor update template mock against the Vulkan headers."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shlex.split(os.environ['CXX']) if os.environ.get('CXX') else [shutil.which('clang++') or shutil.which('g++')]
assert compiler[0], 'CXX, clang++ or g++ required'
vulkan = Path(os.environ.get('VULKAN_HEADERS', root / 'private/toolchains/Vulkan-Headers-1.4.357/include'))
with tempfile.TemporaryDirectory(prefix='bayo3-nvn-descriptor-templates-') as tmp:
    binary = str(Path(tmp) / 'test')
    subprocess.run([*compiler, '-std=c++20', '-O1', '-I' + str(vulkan),
                    str(root / 'tests/nvn_descriptor_templates_test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True, timeout=30)
