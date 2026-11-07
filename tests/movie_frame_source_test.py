#!/usr/bin/env python3
# Compile the hook and exercise its bounded frame snapshot without a game.
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shlex.split(os.environ['CXX']) if os.environ.get('CXX') else [shutil.which('clang++') or shutil.which('g++')]
assert compiler[0], 'CXX, clang++ or g++ required'
with tempfile.TemporaryDirectory(prefix='b3-movie-frame-source-') as temp:
    binary = str(Path(temp) / 'frame-source-test')
    subprocess.run(compiler + ['-std=c++20', '-DNX_BAYONETTA3=1', '-I' + str(root / 'runtime/include'),
                               '-fsyntax-only', str(root / 'runtime/src/hle_movie_api.cpp')], check=True)
    subprocess.run(compiler + ['-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-pthread',
                               str(root / 'tests/movie_frame_source_test.cpp'), '-o', binary], check=True)
    reference = root / 'private/bayonetta3/analysis/main.bin'
    subprocess.run([binary] + ([str(reference)] if reference.is_file() else []), check=True)
