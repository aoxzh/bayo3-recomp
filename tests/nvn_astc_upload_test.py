#!/usr/bin/env python3
"""Test production ASTC uploads with a mocked Vulkan allocator and real decoder."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'private/build-linux'
TARGET = 'CMakeFiles/bayonetta3.dir/runtime/src/nvn/nvn_upload.cpp.o'


def main():
    command = subprocess.check_output(['ninja', '-C', str(BUILD), '-t', 'commands', TARGET], text=True).splitlines()[-1]
    words = shlex.split(command)
    flags = [word for word in words[1:] if word.startswith(('-I', '-D', '-std=', '-march='))]
    with tempfile.TemporaryDirectory(prefix='b3-nvn-astc-upload-') as temp:
        exe = str(Path(temp) / 'test')
        asan = os.environ.get('ASTC_UPLOAD_ASAN') == '1'
        # The bundled clang may omit its private runtime; use the system libasan when requested.
        sanitizer = ['-fsanitize=address', '-fno-omit-frame-pointer', '-fno-sanitize-link-runtime'] if asan else []
        subprocess.run([words[0], *flags, '-O1', '-ffunction-sections', '-fdata-sections', *sanitizer,
                        str(ROOT / 'tests/nvn_astc_upload_test.cpp'),
                        str(ROOT / 'runtime/src/nvn/nvn_astc.cpp'),
                        '-Wl,--gc-sections', '-pthread', '-o', exe,
                        *([os.environ.get('ASTC_UPLOAD_ASAN_LIB', '/usr/lib/libasan.so.8')] if asan else [])], check=True)
        for mode, cache, direct, pool in [('direct', '1', '1', '1'),
                                          ('disabled', '1', '0', '0'),
                                          ('no-cache', '0', '1', '1'),
                                          ('behind', '0', '1', '1')]:
            env = {k: v for k, v in os.environ.items() if not k.startswith('NX_')}
            env.update(NX_ASTC_CACHE_MB=cache, NX_ASTC_DIRECT=direct, NX_ASTC_POOL=pool)
            if mode == 'behind':
                env['NX_ASTC_BEHIND'] = '1'
            if asan:
                # The production ASTC worker pool intentionally lives until process exit.
                env['ASAN_OPTIONS'] = 'detect_leaks=0'
            subprocess.run([exe, mode], env=env, check=True)


if __name__ == '__main__':
    main()
