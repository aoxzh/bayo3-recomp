#!/usr/bin/env python3
"""Compile and run the isolated playback accounting model, without an audio device."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
if not cxx:
    print('SKIP: C++ compiler unavailable')
    sys.exit(77)
with tempfile.TemporaryDirectory(prefix='b3-audio-playback-') as tmp:
    binary = str(Path(tmp) / 'playback')
    subprocess.run([cxx, '-std=c++20', '-O1', '-Wall', '-Wextra', '-pthread',
                    str(root / 'tests/audio_playback_test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True, timeout=15)
