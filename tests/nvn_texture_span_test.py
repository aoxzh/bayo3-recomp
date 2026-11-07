#!/usr/bin/env python3
"""Run independent span-index and production NVN lifecycle differential tests.

--negative-control additionally builds deliberately broken temporary copies of
nvn_objects.cpp and checks that the production oracle detects each regression.
--sanitize enables AddressSanitizer and UndefinedBehaviorSanitizer.
No game, Vulkan device, generated code, or full project build is required.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
options = set(sys.argv[1:])
assert options <= {'--negative-control', '--sanitize'} and len(options) == len(sys.argv[1:]), __doc__
compiler = shlex.split(os.environ['CXX']) if os.environ.get('CXX') else [shutil.which('clang++') or shutil.which('g++')]
assert compiler[0], 'CXX, clang++ or g++ required'
vulkan = Path(os.environ.get('VULKAN_HEADERS', root / 'private/toolchains/Vulkan-Headers-1.4.357/include'))
assert (vulkan / 'vulkan/vulkan.h').is_file(), f'Vulkan headers not found: {vulkan}'
base = [*compiler, '-std=c++20', '-O1', '-g', '-pthread']
if '--sanitize' in options:
    base += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
flags = ['-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-DNX_BAYONETTA3',
         '-Wno-deprecated-declarations', '-I' + str(root / 'runtime/include'),
         '-I' + str(root / 'runtime/src'), '-I' + str(root / 'runtime/src/nvn'), '-I' + str(vulkan)]
unit = root / 'tests/nvn_texture_span_test.cpp'
integration = root / 'tests/nvn_texture_span_integration_test.cpp'
env = os.environ.copy()
for name in ('NX_TEXTURE_RACE_OLD', 'NX_KEEP_TEXTURES', 'NX_REINIT_WAITS_REPLAY', 'NX_TRACE_TEXTURES'):
    env.pop(name, None)

with tempfile.TemporaryDirectory(prefix='bayo3-nvn-texture-span-') as tmp:
    tmp = Path(tmp)
    unit_binary, binary = tmp / 'unit', tmp / 'integration'
    subprocess.run([*base, '-Wall', '-Wextra', '-Werror', str(unit), '-o', str(unit_binary)], check=True)
    subprocess.run([str(unit_binary)], env=env, check=True, timeout=60)
    subprocess.run([*base, *flags, str(integration), '-o', str(binary)], check=True)
    for mode in ({}, {'NX_TEXTURE_RACE_OLD': '1'}, {'NX_KEEP_TEXTURES': '1'},
                 {'NX_TEXTURE_RACE_OLD': '1', 'NX_KEEP_TEXTURES': '1'}):
        subprocess.run([str(binary)], env={**env, **mode}, check=True, timeout=30)

    if '--negative-control' in options:
        cases = [
            ('reinitialize', '        pool_textures().invalidate_guest(guest);', '', {}),
            ('keep', '        pool_textures().move_guest(guest, kept);', '        ;', {'NX_KEEP_TEXTURES': '1'}),
            ('snapshot', '    pool_textures().invalidate_guest(key);  // host keys can collide with a previously registered guest key',
             '', {}),
            ('legacy', '    if (texture_race_old()) {\n        pool_textures().for_each(pool, mark);',
             '    if (false) {\n        pool_textures().for_each(pool, mark);', {'NX_TEXTURE_RACE_OLD': '1'}),
            ('finalize', '        uint64_t kept = keep_for_slots(guest);\n        pool_textures().erase_guest(guest);',
             '        uint64_t kept = keep_for_slots(guest);', {}),
        ]
        original = (root / 'runtime/src/nvn/nvn_objects.cpp').read_text()
        for name, before, after, mode in cases:
            assert original.count(before) == 1, f'negative-control hunk changed: {name}'
            source = tmp / (name + '.cpp')
            source.write_text(original.replace(before, after))
            mutated = tmp / (name + '-control')
            subprocess.run([*base, *flags, f'-DB3_NVN_OBJECTS_SOURCE="{source}"', str(integration),
                            '-o', str(mutated)], check=True)
            result = subprocess.run([str(mutated)], env={**env, **mode}, capture_output=True, text=True, timeout=30)
            assert result.returncode == 1 and ('production flush differs from linear scan' in result.stderr or
                                               'partially initialized texture was skipped' in result.stderr), (
                                                   name, result.returncode, result.stdout, result.stderr)
            print(f'negative control ({name}): oracle detected the regression', flush=True)

    if '--negative-control' in options:
        header = (root / 'runtime/src/nvn/nvn_texture_span.h').read_text()
        before = '''            if (p->second.guests.empty()) {
                // Erasing the pool also releases its cached spans and matches.
                pools_.erase(p);
            } else {
                p->second.dirty = true;
            }'''
        assert header.count(before) == 1, 'negative-control pool release hunk changed'
        broken_header = tmp / 'nvn_texture_span.h'
        broken_header.write_text(header.replace(before, '            p->second.dirty = true;'))
        unit_source = tmp / 'pool-retention.cpp'
        original_include = '#include "../runtime/src/nvn/nvn_texture_span.h"'
        source = unit.read_text()
        assert source.count(original_include) == 1
        unit_source.write_text(source.replace(original_include, f'#include "{broken_header}"'))
        control = tmp / 'pool-retention-control'
        subprocess.run([*base, '-Wall', '-Wextra', '-Werror', str(unit_source), '-o', str(control)], check=True)
        result = subprocess.run([str(control)], env=env, cwd=tmp, capture_output=True, text=True, timeout=60)
        assert result.returncode != 0 and 'f.index.pool_count() == 1' in result.stderr, (
            result.returncode, result.stdout, result.stderr)
        print('negative control (pool retention): lifecycle test detected the regression', flush=True)
