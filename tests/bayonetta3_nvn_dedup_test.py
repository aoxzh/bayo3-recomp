#!/usr/bin/env python3
"""Check production dedup reuse, invalidation and accounting without a GPU/game."""
import itertools
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

if sys.argv[1:] not in ([], ['--negative-control']):
    raise SystemExit(f'usage: {sys.argv[0]} [--negative-control]')
negative_control = bool(sys.argv[1:])

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
assert compiler, 'CXX, clang++ or g++ required'
vulkan = Path(os.environ.get('VULKAN_HEADERS', root / 'private/toolchains/Vulkan-Headers-1.4.357/include'))
base = {key: value for key, value in os.environ.items() if not key.startswith('NX_')}
values = (None, '0', '10', '01', '1')
with tempfile.TemporaryDirectory(prefix='bayo3-nvn-dedup-') as tmp:
    def compile_test(source, output):
        subprocess.run([compiler, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-pthread', '-DNX_BAYONETTA3', '-I' + str(root / 'runtime/include'),
                        '-I' + str(root / 'runtime/src'), '-I' + str(root / 'runtime/src/nvn'),
                        '-I' + str(vulkan), str(source), '-o', str(output)], check=True)

    binary = str(Path(tmp) / 'test')
    compile_test(root / 'tests/bayonetta3_nvn_dedup_test.cpp', binary)
    outputs = []
    for wide, reuse, stats in itertools.product((False, True), values, values):
        env = base.copy()
        if wide:
            env['NX_MICRO'] = 'dedup'
        if reuse is not None:
            env['NX_DEDUP_PAD_REUSE'] = reuse
        if stats is not None:
            env['NX_DEDUP_STATS'] = stats
        outputs.append(subprocess.check_output([binary], env=env, text=True, timeout=30))
    # The pre-existing disable switch is presence-based, including value "0".
    for disabled, reuse in itertools.product(('0', '1'), ('0', '1')):
        env = {**base, 'NX_NO_DEDUP': disabled, 'NX_DEDUP_PAD_REUSE': reuse, 'NX_DEDUP_STATS': '1'}
        outputs.append(subprocess.check_output([binary], env=env, text=True, timeout=30))
    assert len(set(outputs)) == 1, f'dedup modes changed visible bytes: {outputs!r}'
    print(outputs[0], end='')
    print(f'PASS {len(outputs)} dedup modes: exact switches, both front hashes, disabled cache')
    large = subprocess.check_output([binary, '--large'], env=base, text=True, timeout=30)
    assert large.strip() == 'PASS large dedup predecessor and ring wrap', large
    print(large, end='')

    if negative_control:
        original = (root / 'runtime/src/nvn/nvn_dedup.cpp').read_text()
        tests = (root / 'tests/bayonetta3_nvn_dedup_test.cpp').read_text()
        mutated_source = Path(tmp) / 'nvn_dedup_mutated.cpp'
        mutated_test = Path(tmp) / 'dedup_mutated_test.cpp'
        mutated_test.write_text(tests.replace(
            '#include "../runtime/src/nvn/nvn_draw_state.h"',
            f'#include "{root / "runtime/src/nvn/nvn_draw_state.h"}"').replace(
            '#include "../runtime/src/nvn/nvn_dedup.cpp"', f'#include "{mutated_source}"'))
        mutations = {
            'compare-current': ('if (compatible && !current) {', 'if (compatible) {'),
            'trust-old': ('unchanged = std::memcmp(p, e->upload.ptr, size) == 0;', 'unchanged = true;'),
            'partial-compare': ('unchanged = std::memcmp(p, e->upload.ptr, size) == 0;',
                                'unchanged = std::memcmp(p, e->upload.ptr, size - 1) == 0;'),
            'skip-current-invalidation': ('if (entry.size == variants.current_size) variants.current_size = 0;',
                                          '(void)entry;'),
            'skip-promotion': ('matched_variants->current_size = size;', '(void)matched_variants;'),
            'trust-old-map-hit': ('current = matched_variants->current_size == size;', 'current = true;'),
        }
        for name, (old, new) in mutations.items():
            assert original.count(old) == 1, f'mutation target changed: {name}'
            mutated_source.write_text(original.replace(old, new, 1))
            mutated_binary = Path(tmp) / f'test-{name}'
            compile_test(mutated_test, mutated_binary)
            result = subprocess.run([mutated_binary], env=base, text=True, capture_output=True, timeout=30)
            assert result.returncode != 0 and 'FAIL dedup:' in result.stderr, (name, result.stdout, result.stderr)
            print(f'PASS negative control: {name}')
