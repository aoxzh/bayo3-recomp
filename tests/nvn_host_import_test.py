#!/usr/bin/env python3
"""Exercise production host memory import using deterministic Vulkan callbacks.

--negative-control additionally compiles temporary copies with individual safeguards
removed and verifies that the assertions catch each regression. The checkout is not
modified by the negative-control runs.
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--negative-control', action='store_true')
args = parser.parse_args()

root = Path(__file__).resolve().parents[1]
cxx = shlex.split(os.environ['CXX']) if os.environ.get('CXX') else [shutil.which('clang++') or shutil.which('g++')]
assert cxx[0], 'C++ compiler required'
vulkan = Path(os.environ.get('VULKAN_HEADERS', root / 'private/toolchains/Vulkan-Headers-1.4.357/include'))
includes = ['-I' + str(root / p) for p in ('runtime/include', 'runtime/src', 'runtime/src/nvn')] + ['-I' + str(vulkan)]
common = [*cxx, '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections', '-pthread', '-DNX_BAYONETTA3', *includes]
test_path = root / 'tests/nvn_host_import_test.cpp'
vk_path = root / 'runtime/src/nvn/nvn_vk.cpp'

# Each mutation removes one individual guard. Match each source fragment exactly so
# a future refactor cannot silently turn the negative controls into no-ops.
mutations = [
    ('noncoherent selection',
     '            if ((f & want) == want) {  // the direct path does not flush or invalidate imported memory\n',
     '            if (type == UINT32_MAX) type = k;\n'
     '            if ((f & want) == want) {  // the direct path does not flush or invalidate imported memory\n',
     'incompatible memory did not fall back'),
    ('external buffer declaration', '        bi.pNext = &external;', '        bi.pNext = nullptr;',
     'buffer was not created for HOST_ALLOCATION import'),
    ('capability query',
     '    return (external.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) &&\n'
     '           (external.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT) &&\n'
     '           !(external.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT);',
     '    return true;', 'non-importable external buffer did not fall back'),
    ('previous range overlap',
     '            if (previous->second.cap > UINTPTR_MAX - p || p + previous->second.cap > base) return reject(8);',
     '            if (false) return reject(8);', 'contained overlap was imported twice'),
    ('next range overlap', '        if (it != g_guest_imports.end() && it->first < end) return reject(8);',
     '        if (false) return reject(8);', 'overlap with next import was not rejected'),
    ('host-page alignment', '    return checked_host_import_alignment(g_host_align, host_page_size());',
     '    return g_host_align;', 'driver alignment ignored the physical page size'),
    ('guest page containment',
     '    if (cap > guest_cap || base != first_guest_page || ((end - 1) & ~uintptr_t(page - 1)) > last_guest_page)\n'
     '        return reject(2);', '    if (cap > guest_cap) return reject(2);',
     'import included trailing guest-unowned host pages'),
    ('budget overflow',
     '        if (g_guest_import_bytes > kGuestImportMax || cap > kGuestImportMax - g_guest_import_bytes) return reject(4);',
     '        if (g_guest_import_bytes + cap > kGuestImportMax) return reject(4);', 'import budget arithmetic wrapped'),
    ('host allocation size alignment', '        ai.allocationSize = cap;', '        ai.allocationSize = req.size;',
     'import allocation size is not host-pointer aligned'),
    ('oversized buffer memory requirements',
     '        if (req.size > cap) {  // the import must not cover guest pages the allocation does not own',
     '        if (false) {  // the import must not cover guest pages the allocation does not own',
     'allocation is smaller than buffer memory requirements'),
    ('rounded address overflow',
     '    if (!rounded(bytes, cap) || !rounded(guest_bytes, guest_cap) || cap > UINTPTR_MAX - base) return reject(2);',
     '    if (!rounded(bytes, cap) || !rounded(guest_bytes, guest_cap)) return reject(2);',
     'unexpected host pointer query'),
]

with tempfile.TemporaryDirectory(prefix='bayo3-host-import-') as directory:
    tmp = Path(directory)
    binary = tmp / 'test'
    subprocess.run([*common, str(test_path), '-Wl,--gc-sections', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
    if args.negative_control:
        source = vk_path.read_text()
        copy_source = tmp / 'runtime/src/nvn/nvn_vk.cpp'
        copy_test = tmp / 'tests/nvn_host_import_test.cpp'
        copy_source.parent.mkdir(parents=True)
        copy_test.parent.mkdir(parents=True)
        copy_test.write_text(test_path.read_text())
        for name, old, new, expected in mutations:
            assert source.count(old) == 1, f'{name}: source fragment changed; update negative control'
            copy_source.write_text(source.replace(old, new))
            subprocess.run([*common, str(copy_test), '-Wl,--gc-sections', '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            if result.returncode == 0 or expected not in result.stderr + result.stdout:
                raise AssertionError(f'{name}: mutation was not caught as expected: {result.stdout}{result.stderr}')
            print(f'negative control caught: {name}')
