#!/usr/bin/env python3
"""Exercise real synchronous submission and write-back with fake Vulkan."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
assert cxx, 'C++ compiler required'
vulkan = Path(os.environ.get('VULKAN_HEADERS', root / 'private/toolchains/Vulkan-Headers-1.4.357/include'))
includes = ['-I' + str(root / p) for p in ('runtime/include', 'runtime/src')] + ['-I' + str(vulkan)]
common = [cxx, '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections', '-pthread', '-DNX_BAYONETTA3', *includes]
with tempfile.TemporaryDirectory(prefix='bayo3-sync-submit-') as directory:
    tmp = Path(directory)
    # Initialization is linked but deliberately skipped. Define the generated Vulkan
    # function pointers it references; only the seven used in this test are set.
    vkcpp = (root / 'runtime/src/nvn/nvn_vk.cpp').read_text()
    names = sorted(set(re.findall(r'\bp_vk[A-Za-z0-9_]+', vkcpp)))
    stubs = tmp / 'vk_stubs.cpp'
    stubs.write_text('#include "vk_bridge.h"\n#include "vk_gen.h"\nnamespace nx::vk {\n'
                     + ''.join('PFN_vk{0} p_vk{0} = nullptr;\n'.format(n[4:]) for n in names)
                     + 'const Entry g_entries[] = {{nullptr, nullptr}};\nconst size_t g_entry_count = 0;\n}\n')
    sources = [
        (root / 'tests/nvn_sync_submit_test.cpp', []),
        (root / 'runtime/src/nvn/nvn_draw.cpp', ['-Ddraw_begin_submit=unused_real_draw_begin_submit',
                                                '-Ddraw_end_rendering=unused_real_draw_end_rendering']),
        (root / 'runtime/src/nvn/nvn_written.cpp', ['-Dwritten_take=unused_real_written_take',
                                                          '-Dwritten_carry_seed=unused_real_written_carry_seed']),
        (stubs, []),
    ]
    objects = []
    for index, (source, extra) in enumerate(sources):
        obj = tmp / f'{index}.o'
        subprocess.run([*common, *extra, '-c', str(source), '-o', str(obj)], check=True)
        objects.append(str(obj))
    binary = tmp / 'test'
    subprocess.run([cxx, '-pthread', '-Wl,--gc-sections', *objects, '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
