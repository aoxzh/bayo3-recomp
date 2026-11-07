#!/usr/bin/env python3
"""Check production image retirement without a game or Vulkan device.

--negative-control additionally restores each former leak in a temporary source
copy and requires the corresponding regression to fail at its lifetime check.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
assert compiler, 'CXX, clang++ or g++ required'
vulkan = Path(os.environ.get('VULKAN_HEADERS', root / 'private/toolchains/Vulkan-Headers-1.4.357/include'))
assert sys.argv[1:] in ([], ['--negative-control']), 'usage: [--negative-control]'

with tempfile.TemporaryDirectory(prefix='bayo3-nvn-image-retire-') as tmp:
    tmp = Path(tmp)
    common = [*shlex.split(compiler), '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections',
              '-Wl,--gc-sections', '-pthread', '-DNX_BAYONETTA3', '-Wno-deprecated-declarations',
              '-I' + str(root / 'runtime/include'), '-I' + str(root / 'runtime/src'),
              '-I' + str(root / 'runtime/src/nvn'), '-I' + str(vulkan)]
    source = str(root / 'tests/bayonetta3_nvn_image_retire_test.cpp')
    binary = tmp / 'fixed'
    subprocess.run([*common, source, '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)

    if sys.argv[1:]:
        cases = [
            ('nvn_frames.cpp', 'B3_NVN_FRAMES_SOURCE', 'duplicate',
             '''        if (img.use_count() > 1) {
            // Drop this completed reference now: another entry in `dead` may
            // own the last reference and must be able to free the Vulkan image.
            img.reset();
            continue;
        }''',
             '        if (img.use_count() > 1) continue;',
             'duplicate completed references must free Vulkan handles exactly once'),
            ('nvn_objects.cpp', 'B3_NVN_OBJECTS_SOURCE', 'snapshot',
             '        if (snap) retire_image(std::move(snap->image));',
             '        if (snap) snap->image.reset();',
             'snapshot expiry must retain image in deferred grave'),
        ]
        for name, macro, mode, fixed, old, error in cases:
            current = (root / 'runtime/src/nvn' / name).read_text()
            assert current.count(fixed) == 1, f'negative-control hunk changed: {name}'
            mutated = tmp / name
            mutated.write_text(current.replace(fixed, old))
            binary = tmp / mode
            subprocess.run([*common, f'-D{macro}="{mutated}"', source, '-o', str(binary)], check=True)
            result = subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=30)
            assert result.returncode == 1 and error in result.stderr, (mode, result.returncode, result.stdout, result.stderr)
            print(f'negative control ({mode}): reproduced {error}', flush=True)
