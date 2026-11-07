#!/usr/bin/env python3
"""Bounded original B3 SDK tessellation parsing/packet audit; no guest launch.

Requires the existing Python environment with lz4, Capstone and Unicorn.
Original bytes and disassembly are emitted only to a private audit JSON.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from recomp.nso import load, apply_relative
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import (
    UC_ARM64_REG_SP, UC_ARM64_REG_PC, UC_ARM64_REG_X0, UC_ARM64_REG_X1,
    UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X8, UC_ARM64_REG_X12,
    UC_ARM64_REG_X21, UC_ARM64_REG_X24, UC_ARM64_REG_X30,
)

SDK_SHA256 = '2a89fa1c0e677710e89ed29b578162affef530a0c9b8c93cda15d50a57a20d28'
# Exact control/code identities, including the retained SXXG control variant.
TARGETS = {
    'de3b3b5f3334e0e1': (4,
        '6d536748a931bb273155919b3e800cef34584cb1683aaeb021efe71297e0a601',
        'c54dfdaa536661ffc24e967c7291736e983c09f47694d1f7a70bc52dbd2e6d73'),
    'fa9a66456776c577': (3,
        '0b9058a7100a6cff7eb9e9e2b5f2a472040d3b7c6211b11d8337c78242324002',
        'b0502a40f8a4b12310dd102cb49dcaa67358ab8593a0ce1cfeb9a332a37f9582'),
    '17a9eb715636f744': (3,
        '5c35421616074e20a516fa23ba121ac0646139da0686d6c5a9f1051be5d02498',
        'aa644af98a1434226a8c9afd68bd20ce2575f69119d30a5a2b2e816557a67db0'),
}
REGIONS = [(0x4afb90, 0x4afe20), (0x4b0780, 0x4b09f0),
           (0x4b0d90, 0x4b0dd0), (0x4b14b0, 0x4b1570),
           (0x4b1710, 0x4b1cf4), (0x4d21e0, 0x4d242c)]


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sdk', nargs='?', type=Path,
                        default=ROOT / 'private/bayonetta3/extract/exefs/sdk')
    parser.add_argument('--shaders', type=Path,
                        default=ROOT / 'private/shader-work/shaders_extracted/root_000')
    parser.add_argument('--audit', type=Path,
                        default=ROOT / 'private/shader-work/tessellation-sdk-audit.json')
    args = parser.parse_args()
    # The audit contains original machine bytes; never export it into public docs.
    audit_path = args.audit.resolve()
    assert audit_path.is_relative_to((ROOT / 'private').resolve()), 'audit must remain under private/'
    assert sha256(args.sdk.read_bytes()) == SDK_SHA256, 'requires original B3 1.2.0 SDK'
    sdk = load(str(args.sdk))
    apply_relative(sdk)
    table = sdk.u64(0xce8030)
    procs = {sdk.cstr(0xb1d723 + sdk.u32(0xb21038 + i * 4)): sdk.u64(table + i * 8)
             for i in range(532)}
    assert procs['ProgramSetShaders'] == 0x4aff70
    assert procs['CommandBufferSetPatchSize'] == 0x4c8390
    vtable = sdk.u64(0xce92d0) + 0x10
    assert vtable == 0xcbca48
    for slot, entry in {0x1a8: 0x4b0d90, 0x1b0: 0x4b0da0, 0x1b8: 0x4b0db0,
                        0x1c0: 0x4b0dc0, 0x1c8: 0x4b14b0, 0x1d0: 0x4b14c0,
                        0x1d8: 0x4b14d0, 0x1e0: 0x4b14e0}.items():
        assert sdk.u64(vtable + slot) == entry
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    audit = {
        'schema': 'bayonetta3-tessellation-sdk-audit-v1',
        'sdk': {'path': str(args.sdk.resolve()), 'sha256': SDK_SHA256,
                'address_space': 'decompressed module-relative; RELATIVE relocations applied',
                'program_set_shaders': '0x4aff70', 'vtable': hex(vtable)},
        'scope': 'bounded offline SDK functions and emitter slice; no guest main or GPU execution',
        'hardware_sources': [
            'https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/classes/3d/clb197.h',
            'https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/classes/3d/cla097sph.h'],
        'regions': [], 'shaders': {}, 'packets': [],
    }
    for start, end in REGIONS:
        raw = bytes(sdk.image[start:end])
        audit['regions'].append({'start': hex(start), 'end_exclusive': hex(end),
            'sha256': sha256(raw), 'hex': raw.hex(),
            'assembly': [f'{i.address:08x} {i.mnemonic} {i.op_str}'
                         for i in md.disasm(raw, start)]})

    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(0, (len(sdk.image) + 4095) & ~4095)
    uc.mem_write(0, bytes(sdk.image))
    uc.mem_map(0x2000000, 0x40000)
    stop, obj, control, meta, record = 0x2000000, 0x2001000, 0x2002000, 0x2004000, 0x2005000
    stages, out = 0x2006000, 0x2008000
    # Restrict execution to the measured leaf accessors, parser, record builder,
    # and the selected emitter slice. No allocator, OS service or guest main runs.
    allowed = [(0x4b0780, 0x4b09f0), (0x4b0a40, 0x4b1590),
               (0x4b1710, 0x4b1cf4), (0x4d21e0, 0x4d242c)]

    def guard(machine, address, size, _):
        assert any(a <= address and address + size <= b for a, b in allowed), hex(address)

    uc.hook_add(UC_HOOK_CODE, guard)

    def invoke(entry, values):
        uc.reg_write(UC_ARM64_REG_SP, 0x203f000)
        uc.reg_write(UC_ARM64_REG_X30, stop)
        for reg, value in zip([UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                               UC_ARM64_REG_X2, UC_ARM64_REG_X3], values):
            uc.reg_write(reg, value)
        uc.emu_start(entry, stop, count=20000)
        assert uc.reg_read(UC_ARM64_REG_PC) == stop, 'SDK instruction budget exceeded'

    records = {}
    for shader, (stage, ctrl_digest, code_digest) in TARGETS.items():
        ctrl = (args.shaders / f'ctrl_{shader}.bin').read_bytes()
        code = (args.shaders / f'code_{shader}.bin').read_bytes()
        assert len(ctrl) == 0x880 and sha256(ctrl) == ctrl_digest
        assert sha256(code) == code_digest
        assert struct.unpack_from('<3I', ctrl) == (0x98761234, 1, 16)
        factory = 0x4afbd8 + sdk.image[0xb21920 + u32(ctrl, 8) - 5] * 4
        assert factory == 0x4afdd8
        assert u32(ctrl, 0x714) == stage
        base = 0x738 if stage == 4 else 0x848
        fields = list(struct.unpack_from('<4I', ctrl, base))
        assert fields == [4, 0x202, 0, 0x901]
        flags_base = 0x868 if stage == 4 else 0x858
        flags = list(struct.unpack_from('<4I', ctrl, flags_base))
        assert flags == ([1, 0, 0, 0] if stage == 4 else [0, 0, 0, 0])
        entry = u32(ctrl, 0x708)
        assert entry == 0x30 and u32(ctrl, 0x700) == len(code)
        assert hashlib.sha1(code).hexdigest()[:16] == shader
        header = struct.unpack_from('<5I', code, entry)
        assert (header[0] >> 10) & 15 == (3 if stage == 4 else 2)
        output_vertices = (header[2] >> 24) & 255 if stage == 3 else None
        if stage == 3:
            assert header[2] == 0x03000000 and output_vertices == 3
            assert header[1] >> 24 == 6
            assert header[4] & 0xfff == 0  # Not the TCS OutputVertices field.
        uc.mem_write(obj, struct.pack('<15Q', vtable, *([control] * 14)))
        uc.mem_write(control, ctrl)
        uc.mem_write(meta, bytes(0x100))
        uc.mem_write(record, bytes(0x100))
        invoke(0x4b1710, [meta, obj])
        parsed = bytes(uc.mem_read(meta, 0x78))
        assert u32(parsed, 0x1c) == stage
        assert list(struct.unpack_from('<4I', parsed, 0x20)) == fields
        assert list(struct.unpack_from('<4I', parsed, 0x30)) == flags
        invoke(0x4b0780, [record, 0, 0, meta])
        records[shader] = bytes(uc.mem_read(record, 0x70))
        assert list(struct.unpack_from('<4I', records[shader], 0x34)) == fields
        assert list(struct.unpack_from('<4I', records[shader], 0x44)) == flags
        audit['shaders'][shader] = {'ctrl_sha256': ctrl_digest, 'code_sha256': code_digest,
            'stage': 'TES' if stage == 4 else 'TCS', 'control_fields_offset': hex(base),
            'control_fields': fields, 'presence_flags_offset': hex(flags_base), 'presence_flags': flags,
            'header_offset': hex(entry), 'header_hex': code[entry:entry + 0x50].hex(),
            'output_vertices': output_vertices, 'per_patch_attributes': header[1] >> 24,
            'parsed_metadata_hex': parsed.hex(), 'stage_record_hex': records[shader].hex()}

    for tcs in [None, 'fa9a66456776c577', '17a9eb715636f744']:
        uc.mem_write(stages, bytes(0x300))
        uc.mem_write(stages + 4 * 0x70, records['de3b3b5f3334e0e1'])
        if tcs:
            uc.mem_write(stages + 3 * 0x70, records[tcs])
        else:
            uc.mem_write(stages + 3 * 0x70 + 8, struct.pack('<I', 0xffffffff))
        uc.mem_write(out, bytes(32))
        for reg, value in [(UC_ARM64_REG_X21, stages), (UC_ARM64_REG_X24, out),
                           (UC_ARM64_REG_X8, 0x80000489), (UC_ARM64_REG_X12, stages + 0x184)]:
            uc.reg_write(reg, value)
        uc.emu_start(0x4d21e0, 0x4d242c, count=1000)
        assert uc.reg_read(UC_ARM64_REG_PC) == 0x4d242c, 'emitter instruction budget exceeded'
        assert uc.reg_read(UC_ARM64_REG_X24) == out + 4
        packet = bytes(uc.mem_read(out, 4))
        assert packet == struct.pack('<I', 0x820100c8), packet.hex()
        word = u32(packet, 0)
        method, value = (word & 0x1fff) * 4, (word >> 16) & 0x1fff
        assert method == 0x320 and value == 0x201
        assert (value & 3, (value >> 4) & 3, (value >> 8) & 3) == (1, 0, 2)
        audit['packets'].append({'tes': 'de3b3b5f3334e0e1', 'tcs': tcs,
            'hex': packet.hex(), 'word': hex(word), 'method': hex(method), 'value': hex(value)})

    audit['result'] = {'domain': 'triangles', 'spacing': 'equal', 'source_winding': 'CW',
        'point_mode': False, 'tcs_output_vertices': 3,
        'vulkan_convention': 'Ryujinx flips source TessCw: true to SPIR-V VertexOrderCcw',
        'input_patch_size': 'separate SET_PATCH draw state; not established by this probe',
        'status': 'PASS'}
    audit_path.parent.mkdir(parents=True, exist_ok=True)
    audit_path.write_text(json.dumps(audit, indent=2) + '\n')
    print('PASS: original SDK TES/TCS parsing, flags, all 3 packets = 0x820100c8; '
          'triangle/equal/CW; both TCS OutputVertices = 3')
    print(f'Private audit: {audit_path}')


if __name__ == '__main__':
    main()
