#!/usr/bin/env python3
"""Audit original B3 SDK format table/getters, then test production format_block."""
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root))
from recomp.nso import load, apply_relative

path = Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'private/bayonetta3/extract/exefs/sdk'
assert hashlib.sha256(path.read_bytes()).hexdigest() == '2a89fa1c0e677710e89ed29b578162affef530a0c9b8c93cda15d50a57a20d28'
module = load(str(path))
apply_relative(module)
assert any(r.offset == 0xce8020 and r.type == 1027 and r.addend == 0xcd0d90 for r in module.relocs)
assert module.u64(0xce8020) == 0xcd0d90
cs = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
observed = [(i.mnemonic, i.op_str) for i in cs.disasm(bytes(module.image[0x4b6c3c:0x4b6c80]), 0x4b6c3c)]
for instruction in [('ldr', 'x23, [x23, #0x20]'), ('mov', 'w9, #0x30'), ('ldrb', 'w0, [x8, #0x10]'), ('ldrb', 'w9, [x8, #8]'), ('ldrb', 'w8, [x8, #9]')]:
    assert instruction in observed, instruction
base = 0xcd0d90
table = bytes(module.image[base:base + 0xd0 * 0x30])
assert len(table) == 0xd0 * 0x30
assert tuple(table[0x42 * 0x30 + i] for i in [8, 9, 0x10]) == (4, 4, 8)
dimensions = [(4,4),(5,4),(5,5),(6,5),(6,6),(8,5),(8,6),(8,8),(10,5),(10,6),(10,8),(10,10),(12,10),(12,12)]
for half in [0x79, 0x87]:
    for index, (width, height) in enumerate(dimensions):
        record = (half + index) * 0x30
        assert (table[record + 8], table[record + 9], table[record + 0x10]) == (width, height, 16)
        assert bool(table[record + 0x12] & 1) == (half == 0x87), 'sRGB table bit'
# Scalar builder getters use direct offsets; no inherited structure decoding.
proc_table = module.u64(0xce8030)
functions = {module.cstr(0xb1d723 + module.u32(0xb21038 + i * 4)): module.u64(proc_table + i * 8) for i in range(532)}
for name, offset in [('Target',0x24),('Width',0x28),('Height',0x2c),('Depth',0x30),('Levels',0x34),('Format',0x38),('Samples',0x3c)]:
    address = functions['TextureBuilderGet' + name]
    instructions = list(cs.disasm(bytes(module.image[address:address + 8]), address))
    assert instructions[0].mnemonic == 'ldr' and instructions[0].op_str == f'w0, [x0, #{offset:#x}]'
    assert instructions[1].mnemonic == 'ret'
# Packaged layout output nibbles map to GOB block height/depth/width exponents.
packaged = [(i.mnemonic, i.op_str) for i in cs.disasm(bytes(module.image[0x4b6e38:0x4b6e68]), 0x4b6e38)]
for instruction in [('ubfx', 'w8, w8, #4, #4'), ('strb', 'w8, [x19, #0x29]'),
                    ('strb', 'w8, [x19, #0x2a]'), ('lsr', 'w8, w8, #0xc'), ('strb', 'w8, [x19, #0x28]')]:
    assert instruction in packaged, instruction
axes = [(i.mnemonic, i.op_str) for i in cs.disasm(bytes(module.image[0x4e8d88:0x4e8e10]), 0x4e8d88)]
assert ('ldp', 'w9, w10, [x0]') in axes
assert ('ldr', 'w12, [x0, #8]') in axes
assert ('lsl', 'w19, w2, w10') in axes  # 8 rows per GOB times 2^heightExponent
compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
assert compiler
vulkan = Path(os.environ.get('VULKAN_HEADERS', root / 'private/toolchains/Vulkan-Headers-1.4.357/include'))
with tempfile.TemporaryDirectory(prefix='bayo3-nvn-format-') as temporary:
    directory = Path(temporary)
    fixture = directory / 'original-format-table.bin'
    fixture.write_bytes(table)
    executable = directory / 'test'
    subprocess.run([compiler, '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-DNX_BAYONETTA3', '-I' + str(root / 'runtime/include'), '-I' + str(root / 'runtime/src'),
                    '-I' + str(vulkan), str(root / 'tests/bayonetta3_nvn_format_test.cpp'), '-o', str(executable)], check=True)
    subprocess.run([str(executable), str(fixture)], check=True)
print('PASS authenticated original SDK audit: table relocation, field instructions, ASTC dimensions/sRGB, builder getter offsets')
