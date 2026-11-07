#!/usr/bin/env python3
"""Compare production scalar/batched plural recording, without a game or GPU."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
assert compiler, 'CXX, clang++ or g++ required'
vulkan = Path(os.environ.get('VULKAN_HEADERS', root / 'private/toolchains/Vulkan-Headers-1.4.357/include'))
with tempfile.TemporaryDirectory(prefix='bayo3-nvn-record-batch-') as tmp:
    binary = str(Path(tmp) / 'test')
    subprocess.run([compiler, '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections',
                    '-Wl,--gc-sections', '-pthread', '-DNX_BAYONETTA3',
                    '-I' + str(root / 'runtime/include'), '-I' + str(root / 'runtime/src'),
                    '-I' + str(vulkan), str(root / 'tests/bayonetta3_nvn_record_batch_test.cpp'),
                    str(root / 'runtime/src/nvn/nvn_draw_state.cpp'),
                    '-o', binary], check=True)
    base = {key: value for key, value in os.environ.items() if not key.startswith('NX_')}
    for deferred in (False, True):
        outputs = []
        for compact in (None, '0', '10', '01', '1'):
            for batch in (None, '0', '10', '01', '1'):
                env = base.copy()
                if batch is not None:
                    env['NX_RECORD_BATCH'] = batch
                if compact is not None:
                    env['NX_RECORD_COMPACT'] = compact
                if deferred:
                    env['NX_DEFERRED_BINDS'] = '1'
                outputs.append(subprocess.check_output([binary], env=env, text=True))
        assert len(set(outputs)) == 1, f'default/scalar/batched/compact outputs differ: {outputs!r}'
        print(outputs[0], end='')
    print('PASS 50 recording/replay modes: scalar, batched, compact, deferred off/on')
