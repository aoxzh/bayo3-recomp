#!/usr/bin/env python3
"""Exercise Linux SDL/HID against virtual gamepads; never start the game."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
if not compiler:
    print('SKIP: C++ compiler unavailable')
    sys.exit(77)

prefix = os.environ.get('SDL3_PREFIX')
if prefix:
    p = Path(prefix)
    if not (p / 'include/SDL3/SDL.h').exists():
        print('SKIP: SDL3_PREFIX does not contain SDL3 headers')
        sys.exit(77)
    libdir = next((p / name for name in ('lib', 'lib64')
                   if any((p / name / lib).exists() for lib in ('libSDL3.so', 'libSDL3.a'))), None)
    if libdir is None:
        print('SKIP: SDL3_PREFIX does not contain an SDL3 development library')
        sys.exit(77)
    cflags = ['-I' + str(p / 'include')]
    libs = ['-L' + str(libdir), '-lSDL3', '-ldl', '-lm']
else:
    try:
        cflags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'sdl3'], text=True, stderr=subprocess.DEVNULL))
        libs = shlex.split(subprocess.check_output(['pkg-config', '--libs', 'sdl3'], text=True, stderr=subprocess.DEVNULL))
    except (FileNotFoundError, subprocess.CalledProcessError):
        print('SKIP: SDL3 development package unavailable (set SDL3_PREFIX or install sdl3.pc)')
        sys.exit(77)

vulkan = Path(os.environ.get('VULKAN_HEADERS', root / 'private/toolchains/Vulkan-Headers-1.4.357/include'))
base_env = os.environ.copy()
for key in ('NX_PAD_SWAP', 'NX_PAD_DEADZONE', 'NX_PAD_TRIGGER', 'NX_PAD_INDEX', 'NX_PAD_INVERT_X', 'NX_PAD_INVERT_Y', 'NX_INPUT'):
    base_env.pop(key, None)
scenarios = [
    ('default', {}),
    ('swap', {'NX_PAD_SWAP': '1', 'NX_PAD_INVERT_X': '1', 'NX_PAD_INVERT_Y': '1'}),
    ('trigger-zero', {'NX_PAD_TRIGGER': '0'}),
    ('trigger-max', {'NX_PAD_TRIGGER': '32767'}),
    ('selection', {'NX_PAD_INDEX': '1'}),
    ('rumble', {}),
]
with tempfile.TemporaryDirectory(prefix='bayo3-sdl-gamepad-') as tmp:
    binary = str(Path(tmp) / 'test')
    subprocess.run([compiler, '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections',
                    '-Wl,--gc-sections', '-pthread', '-DNX_BAYONETTA3',
                    '-I' + str(root / 'runtime/include'), '-I' + str(root / 'runtime/src'),
                    '-I' + str(vulkan), *cflags, str(root / 'tests/nvn_sdl_gamepad_test.cpp'),
                    str(root / 'runtime/src/hle_hid.cpp'), *libs, '-o', binary], check=True)
    for name, settings in scenarios:
        env = base_env | settings
        result = subprocess.run([binary, name], env=env, timeout=10)
        if result.returncode == 77:
            sys.exit(77)
        if result.returncode:
            sys.exit(result.returncode)
