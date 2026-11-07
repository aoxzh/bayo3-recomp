#!/usr/bin/env python3
"""Audit original B3 buffer-copy command encoding, without executing guest main.
Run with the required Python dependencies and a locally supplied ExeFS/sdk path.
This executes only a bounded SDK recorder into private emulated command memory.
"""
import hashlib
from pathlib import Path
import struct
import sys
import re

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from recomp.nso import load, apply_relative
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import (UC_ARM64_REG_SP, UC_ARM64_REG_X0,
    UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4,
    UC_ARM64_REG_X30, UC_ARM64_REG_PC)

path = Path(sys.argv[1])
assert hashlib.sha256(path.read_bytes()).hexdigest() == '2a89fa1c0e677710e89ed29b578162affef530a0c9b8c93cda15d50a57a20d28'
sdk = load(str(path))
apply_relative(sdk)
names = [sdk.cstr(0xb1d723 + sdk.u32(0xb21038 + i * 4)) for i in range(0x214)]
entry = sdk.u64(sdk.u64(0xce8030) + 8 * names.index('CommandBufferCopyBufferToBuffer'))
assert entry == 0x4a5b70
cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
ops = list(cs.disasm_lite(sdk.image[entry:0x4a5cb8], entry))
assert ops[-1][2] == 'ret'
assert all(not re.search(r'\b[wx]4\b', args) for _, _, _, args in ops)
assert any(op == 'mov' and args == 'w24, #0x3fffff' for _, _, op, args in ops)
u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
u.mem_map(0, (len(sdk.image) + 4095) & -4096)
u.mem_write(0, bytes(sdk.image))
u.mem_map(0x2000000, 0x20000)
cmd, out, stop = 0x2001000, 0x2004000, 0x201fff0

def record(src, dst, size, flags):
    u.mem_write(cmd, struct.pack('<QQ', out, out + 0x4000))
    u.mem_write(out, bytes(0x4000))
    u.reg_write(UC_ARM64_REG_SP, 0x201f000)
    u.reg_write(UC_ARM64_REG_X30, stop)
    for reg, value in zip([UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                           UC_ARM64_REG_X3, UC_ARM64_REG_X4], [cmd, src, dst, size, flags]):
        u.reg_write(reg, value)
    u.emu_start(entry, stop, count=10000)
    assert u.reg_read(UC_ARM64_REG_PC) == stop, 'recorder exceeded bounded instruction budget'
    end, = struct.unpack('<Q', u.mem_read(cmd, 8))
    return bytes(u.mem_read(out, end - out))

for src, dst, size in [(0x18c635700, 0x18c588de0, 32), (0x100000003, 0x200000007, 33),
                       (0x100000000, 0x200000000, 0x400004)]:
    packet = record(src, dst, size, 0)
    chunks = (size + 0x3ffffe) // 0x3fffff
    assert len(packet) == 8 + 36 * chunks
    left = size
    for i in range(chunks):
        words = struct.unpack_from('<9I', packet, 4 + 36 * i)
        count = min(left, 0x3fffff)
        assert words[1] << 32 | words[2] == src
        assert words[3] << 32 | words[4] == dst
        assert words[6] == count
        src += count; dst += count; left -= count
    # Observed retail SDK behavior is not permission to ignore flags in HLE:
    # production deliberately rejects nonzero values until their contract is supported.
    for flags in [1, 2, 0x20, 0x40, 0x80000000, 0xffffffff]:
        assert record(src - size, dst - size, size, flags) == packet
print('PASS: original SDK copy ABI, byte granularity, chunk encoding and unused flags verified')
