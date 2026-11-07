#!/usr/bin/env python3
"""Compile the real Linux SDL callback against a controlled mock stream."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', type=Path, default=root / 'private/build-linux')
build = parser.parse_args().build_dir.resolve()
if not shutil.which('ninja') or not (build / 'build.ninja').is_file():
    print('SKIP: configure a Ninja game build or supply --build-dir')
    sys.exit(77)
rows = subprocess.check_output(['ninja', '-C', str(build), '-t', 'targets', 'all'], text=True).splitlines()
target = next((row.split(':', 1)[0] for row in rows
               if row.startswith('CMakeFiles/') and '/runtime/src/platform_linux.cpp.o:' in row), None)
if target is None:
    print('SKIP: configured build has no Linux game target')
    sys.exit(77)
command = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', target], text=True).splitlines()[-1]
words = shlex.split(command)
flags = [word for word in words[1:] if word.startswith(('-I', '-D', '-std=', '-march='))]
with tempfile.TemporaryDirectory(prefix='b3-audio-sdl-') as tmp:
    binary = str(Path(tmp) / 'sdl-playback')
    subprocess.run([words[0], *flags, '-O1', '-ffunction-sections', '-fdata-sections',
                    str(root / 'tests/audio_sdl_playback_test.cpp'), '-Wl,--gc-sections', '-pthread',
                    '-o', binary], check=True)
    env = {key: value for key, value in os.environ.items() if not key.startswith('NX_')}
    subprocess.run([binary], env=env, check=True, timeout=15)
