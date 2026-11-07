#!/usr/bin/env python3
"""Run production descriptor building, resolution and binding with fake Vulkan."""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shlex.split(os.environ['CXX']) if os.environ.get('CXX') else [shutil.which('clang++') or shutil.which('g++')]
assert compiler[0], 'C++ compiler required'
vulkan = Path(os.environ.get('VULKAN_HEADERS', root / 'private/toolchains/Vulkan-Headers-1.4.357/include'))
includes = ['-I' + str(root / p) for p in ('runtime/include', 'runtime/src')] + ['-I' + str(vulkan)]
with tempfile.TemporaryDirectory(prefix='bayo3-descriptor-resolve-') as directory:
    tmp = Path(directory)
    source = (root / 'runtime/src/nvn/nvn_sets.cpp').read_text() + (root / 'runtime/src/nvn/nvn_pipeline.cpp').read_text()
    names = sorted(set(re.findall(r'\bp_vk[A-Za-z0-9_]+', source)))
    stubs = tmp / 'vk_stubs.cpp'
    stubs.write_text('#include "vk_bridge.h"\n#include "vk_gen.h"\nnamespace nx::vk {\n'
                     + ''.join('PFN_vk{0} p_vk{0} = nullptr;\n'.format(n[4:]) for n in names) + '}\n')
    # Link the real layout cache. Rename the pipeline's image helpers so this test can replace GPU resource work
    # while exercising gather_textures/build_sets with the production layout metadata.
    pipeline = tmp / 'pipeline.o'
    subprocess.run([*compiler, '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections', '-pthread',
                    '-DNX_BAYONETTA3', '-Dsampled_ready=descriptor_test_unused_sampled_ready',
                    '-Dsampled_view=descriptor_test_unused_sampled_view', '-Dstorage_view=descriptor_test_unused_storage_view',
                    *includes, '-c', str(root / 'runtime/src/nvn/nvn_pipeline.cpp'), '-o', str(pipeline)], check=True)
    # Exercise the production dedup implementation as well; a thin test wrapper counts calls into it so a
    # same-block hit can prove it bypassed the generic lookup. The fake allocator below models ring resets.
    dedup = tmp / 'dedup.o'
    subprocess.run([*compiler, '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections', '-pthread',
                    '-DNX_BAYONETTA3', '-Ddedup_upload=descriptor_test_real_dedup_upload',
                    *includes, '-c', str(root / 'runtime/src/nvn/nvn_dedup.cpp'), '-o', str(dedup)], check=True)
    binary = tmp / 'test'
    subprocess.run([*compiler, '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections', '-pthread',
                    '-DNX_BAYONETTA3', *includes, str(root / 'tests/nvn_descriptor_resolve_test.cpp'), str(pipeline), str(dedup), str(stubs),
                    '-Wl,--gc-sections', '-o', str(binary)], check=True)
    for mode in ('default', 'template', 'no-cache', 'template-no-cache', 'no-dedup', 'stats',
                 'slow-uploads', 'slow-empty', 'slow-empty-template', 'slow-scratch', 'pad', 'ones'):
        env = os.environ.copy()
        for name in ('NX_SET_CACHE', 'NX_DESCRIPTOR_TEMPLATE', 'NX_DRAW_SLOW', 'NX_WRITE_BACK_ALL', 'NX_NO_DEDUP',
                     'NX_DEDUP_STATS', 'NX_PAD_BUFFERS', 'NX_UBO_UNBOUND_ONE'):
            env.pop(name, None)
        if mode in ('template', 'template-no-cache', 'slow-empty-template'): env['NX_DESCRIPTOR_TEMPLATE'] = '1'
        if mode in ('no-cache', 'template-no-cache'): env['NX_SET_CACHE'] = '0'
        if mode == 'no-dedup': env['NX_NO_DEDUP'] = '0'  # presence-based, including "0"
        if mode == 'stats': env['NX_DEDUP_STATS'] = '1'
        if mode == 'slow-uploads': env['NX_DRAW_SLOW'] = 'u'
        if mode in ('slow-empty', 'slow-empty-template'): env['NX_DRAW_SLOW'] = 'e'
        if mode == 'slow-scratch': env['NX_DRAW_SLOW'] = 's'
        if mode == 'pad': env['NX_PAD_BUFFERS'] = '1'  # exercise 4 KiB and 64 KiB ranges as well as 16-byte robust ranges
        if mode == 'ones': env['NX_UBO_UNBOUND_ONE'] = '1'
        subprocess.run([str(binary), mode], env=env, check=True, timeout=30)

    if '--negative-control' in sys.argv[1:]:
        # Recreate the prior fresh-allocation behavior in a temporary source copy. The real test must catch the
        # resulting allocation growth, demonstrating the regression is not masked by the resource mock.
        sets = (root / 'runtime/src/nvn/nvn_sets.cpp').read_text()
        old = '!p && !size && !draw_slow(kSlowUploads) && immutable_upload_reuse() ? zero_upload(cb, n)'
        assert sets.count(old) == 1
        sets_copy = tmp / 'nvn_sets_before_zero_reuse.cpp'
        sets_copy.write_text(sets.replace(old, 'false ? zero_upload(cb, n)', 1))
        test = (root / 'tests/nvn_descriptor_resolve_test.cpp').read_text()
        include = '#include "../runtime/src/nvn/nvn_sets.cpp"'
        assert test.count(include) == 1
        old_test = tmp / 'before_zero_reuse_test.cpp'
        old_test.write_text(test.replace(include, '#include "' + str(sets_copy) + '"', 1))
        old_binary = tmp / 'before_zero_reuse_test'
        subprocess.run([*compiler, '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections', '-pthread',
                        '-DNX_BAYONETTA3', *includes, '-I' + str(root / 'runtime/src/nvn'),
                        str(old_test), str(pipeline), str(dedup), str(stubs), '-Wl,--gc-sections', '-o', str(old_binary)], check=True)
        env = os.environ.copy()
        for name in ('NX_SET_CACHE', 'NX_DESCRIPTOR_TEMPLATE', 'NX_DRAW_SLOW', 'NX_WRITE_BACK_ALL', 'NX_NO_DEDUP',
                     'NX_DEDUP_STATS', 'NX_PAD_BUFFERS', 'NX_UBO_UNBOUND_ONE'):
            env.pop(name, None)
        if os.name == 'posix':
            import resource
            def no_core_dump():
                resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        else:
            no_core_dump = None
        result = subprocess.run([str(old_binary), 'default'], env=env, capture_output=True, timeout=30,
                                preexec_fn=no_core_dump)
        assert result.returncode != 0 and b'host_allocations == before_alloc' in result.stderr, result.stderr
        print('negative control: prior fresh-allocation behavior rejected as expected', flush=True)
