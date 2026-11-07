#!/usr/bin/env python3
"""Bounded original SDK recorder emulation; no game or native GPU execution."""
from pathlib import Path
import hashlib
import struct
import sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X30, UC_ARM64_REG_SP, UC_ARM64_REG_PC
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from recomp.nso import load, apply_relative
path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'private/bayonetta3/extract/exefs/sdk'
assert hashlib.sha256(path.read_bytes()).hexdigest() == '2a89fa1c0e677710e89ed29b578162affef530a0c9b8c93cda15d50a57a20d28'
sdk = load(str(path)); apply_relative(sdk)
cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
table = sdk.u64(0xce8030)
functions = {sdk.cstr(0xb1d723 + sdk.u32(0xb21038 + i * 4)): sdk.u64(table + i * 8) for i in range(532)}
single = functions['CommandBufferDrawElementsIndirect']
multi = functions['CommandBufferMultiDrawElementsIndirectCount']
assert (single, multi) == (0x4c6ad0, 0x4c7170)
ops = list(cs.disasm_lite(sdk.image[single:single + 0x248], single))
assert ops[-1][2] == 'ret'
assert any(op == 'mov' and args == 'x21, x4' for _, _, op, args in ops)
assert any(op == 'mov' and args == 'x23, x3' for _, _, op, args in ops)
# x5 is not an input to the single-draw recorder (the trace shows stale ffffffff).
assert not any('x5' in args.split(', ') or 'w5' in args.split(', ') for _, _, _, args in ops)

def encode(entry, args):
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.mem_map(0, (len(sdk.image) + 4095) & -4096); u.mem_write(0, bytes(sdk.image))
    u.mem_map(0x2000000, 0x40000)
    cmd, out, ctrl, stop = 0x2001000, 0x2004000, 0x2014000, 0x203fff0
    u.mem_write(cmd, struct.pack('<QQ', out, out + 0x10000))
    u.mem_write(cmd + 0x20, struct.pack('<QQ', ctrl, ctrl + 0x10000))
    u.mem_write(cmd + 0x40, struct.pack('<Q', out))
    u.reg_write(UC_ARM64_REG_SP, 0x203f000); u.reg_write(UC_ARM64_REG_X30, stop)
    for i, value in enumerate([cmd] + args): u.reg_write(UC_ARM64_REG_X0 + i, value & 0xffffffffffffffff)
    u.emu_start(entry, stop, count=10000)
    assert u.reg_read(UC_ARM64_REG_PC) == stop, 'instruction budget exhausted'
    end, = struct.unpack('<Q', u.mem_read(cmd, 8))
    ctrl_end, = struct.unpack('<Q', u.mem_read(cmd + 0x20, 8))
    return bytes(u.mem_read(out, end - out)), bytes(u.mem_read(ctrl, ctrl_end - ctrl))

indices, arguments, count = 0x123456780, 0x234567890, 0x3456789a0
for mode, kind in [(4, 2), (5, 1), (14, 0)]:
    packet, control = encode(single, [mode, kind, indices, arguments, 0xffffffff])
    words = struct.unpack('<9I', packet)
    assert words[2] == 0x800005f6 | (kind << 16)
    assert (words[4] << 32 | words[5]) == indices and words[7] == mode
    # Control entry copies five words from the argument GPU address at submit.
    assert struct.unpack_from('<Q', control, 32)[0] == arguments
    assert struct.unpack_from('<I', control, 48)[0] == 5
    assert encode(single, [mode, kind, indices, arguments, 7]) == (packet, control)
for draws in [1, 3, 17]:
    for stride in [0, -1, 20, 32, 64]:
        packet, control = encode(multi, [4, 2, indices, arguments, count, draws, stride])
        words = struct.unpack('<12I', packet)
        actual_stride = stride if stride > 0 else 20
        assert (words[3] << 32 | words[4]) == indices
        assert words[7:11] == (0, draws, 4, (actual_stride - 20) // 4)
        assert struct.unpack_from('<Q', control, 32)[0] == count
        assert struct.unpack_from('<I', control, 48)[0] == 1
        assert struct.unpack_from('<Q', control, 56)[0] == arguments
        assert struct.unpack_from('<I', control, 72)[0] == ((draws - 1) * actual_stride + 20) // 4
# Main callsites independently corroborate x3 indices, x4 argument base,
# and count API x5 count address / w6 maximum / w7 stride=20.
main = load(str(ROOT / 'private/bayonetta3/extract/exefs/main'))
single_call = list(cs.disasm_lite(main.image[0xcf35f4:0xcf3614], 0xcf35f4))
assert any(op == 'add' and args == 'x4, x0, w19, uxtw' for _, _, op, args in single_call)
assert any(op == 'mov' and args == 'w1, w21' for _, _, op, args in single_call)
assert any(op == 'mov' and args == 'w2, w22' for _, _, op, args in single_call)
assert any(op == 'mov' and args == 'x3, x23' for _, _, op, args in single_call)
assert single_call[-1][2:] == ('blr', 'x8')
call = list(cs.disasm_lite(main.image[0xd1a0e8:0xd1a118], 0xd1a0e8))
assert any(op == 'mov' and args == 'w7, #0x14' for _, _, op, args in call)
assert any(op == 'mov' and args == 'w6, w19' for _, _, op, args in call)
assert call[-1][2:] == ('blr', 'x8')
print('PASS: retail SDK five-word draw ABI; count/max/stride encoding; both main callsites checked')
