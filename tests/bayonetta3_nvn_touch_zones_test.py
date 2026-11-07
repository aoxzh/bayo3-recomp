#!/usr/bin/env python3
"""Check the touch control geometry (touch_zones.h) without a phone, a GPU or game data."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
assert compiler, 'CXX, clang++ or g++ required'
with tempfile.TemporaryDirectory(prefix='bayo3-nvn-touch-zones-') as tmp:
    binary = str(Path(tmp) / 'test')
    subprocess.run([compiler, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror',
                    str(root / 'tests/bayonetta3_nvn_touch_zones_test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)
