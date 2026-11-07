#!/usr/bin/env python3
"""Compile the real pipe reader and test it with synthetic pipes and FFmpeg."""
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
ffmpeg = os.environ.get('FFMPEG') or shutil.which('ffmpeg')
with tempfile.TemporaryDirectory(prefix='b3-movie-pipe-') as tmp:
    binary = str(Path(tmp) / 'movie-pipe-test')
    subprocess.run([cxx, '-std=c++20', '-O1', '-Wall', '-Wextra', '-pthread',
                    str(root / 'tests/movie_decoder_pipe_test.cpp'), '-o', binary], check=True)
    command = [binary]
    if ffmpeg:
        command.append(ffmpeg)
    else:
        print('SKIP: real FFmpeg case (binary unavailable)', flush=True)
    subprocess.run(command, check=True, timeout=30)
