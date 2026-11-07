#!/usr/bin/env python3
"""Test production movie timing and child cleanup with a synthetic decoder, without a game."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
assert compiler, 'C++ compiler required'
with tempfile.TemporaryDirectory(prefix='bayo3-movie-timing-') as tmp:
    binary = Path(tmp) / 'movie_test'
    subprocess.run([compiler, '-std=c++20', '-O1', '-pthread',
                    '-I' + str(root / 'runtime/include'), '-I' + str(root / 'runtime/src'),
                    str(root / 'tests/movie_timing_test.cpp'), '-o', str(binary)], check=True)
    (Path(tmp) / 'ffmpeg').symlink_to(binary)
    subprocess.run([str(binary), tmp], check=True, timeout=20)
    android_binary = Path(tmp) / 'movie_android_test'
    subprocess.run([compiler, '-std=c++20', '-O1', '-pthread', '-D__ANDROID__',
                    '-I' + str(root / 'runtime/include'), '-I' + str(root / 'runtime/src'),
                    str(root / 'tests/movie_timing_test.cpp'), '-o', str(android_binary)], check=True)
    subprocess.run([str(android_binary)], check=True, timeout=10)
