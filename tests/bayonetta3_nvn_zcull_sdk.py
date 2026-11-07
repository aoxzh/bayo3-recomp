#!/usr/bin/env python3
"""Original B3 NVN ZCull ABI audit. Offline; no guest game or GPU execution.
Usage: existing project venv python tests/bayonetta3_nvn_zcull_sdk.py /path/to/ExeFS
"""
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from recomp.nso import load, apply_relative
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X30, UC_ARM64_REG_PC


def main():
    exefs = Path(sys.argv[1])
    config = json.loads((ROOT / 'config/bayonetta3-1.2.0.json').read_text())
    for name in ('main', 'sdk'):
        assert hashlib.sha256((exefs / name).read_bytes()).hexdigest() == config['modules'][name]['sha256']
    sdk = load(str(exefs / 'sdk'))
    apply_relative(sdk)
    names = [sdk.cstr(0xb1d723 + sdk.u32(0xb21038 + i * 4)) for i in range(0x214)]
    procs = {'nvn' + name: sdk.u64(sdk.u64(0xce8030) + i * 8) for i, name in enumerate(names)}
    getter = procs['nvnTextureGetZCullStorageSize']
    save = procs['nvnCommandBufferSaveZCullData']
    restore = procs['nvnCommandBufferRestoreZCullData']
    assert (getter, save, restore) == (0x4b7cc0, 0x4c9920, 0x4c9a90)
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    game = load(str(exefs / 'main'))
    for off in (0xcf23f0, 0xcf2450):
        ins = list(cs.disasm_lite(game.image[off:off+0x60], off))
        call = next(i for i, (_, _, op, _) in enumerate(ins) if op == 'blr')
        tail = [(op, args) for _, _, op, args in ins[call+1:]]
        assert ('mov', 'x2, x0') in tail  # exact getter result, no minimum metadata allocation
        assert ('ldr', 'x1, [x19, #0x7b0]') in tail
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(0, (len(sdk.image)+4095)&~4095)
    uc.mem_write(0, bytes(sdk.image))
    uc.mem_map(0x2000000, 0x20000)
    cmd, device, texture, buffer, control, stop = 0x2000000, 0x2001000, 0x2004000, 0x2006000, 0x2008000, 0x201FFF0
    metadata_size = 0
    def hook(u, address, size, _):
        if address == 0x4b7010:
            # Substitute device-specific metadata-size calculation; inspect the
            # original getter's texture-field unpacking and return width.
            out = u.reg_read(UC_ARM64_REG_X1)
            u.mem_write(out+0x18, struct.pack('<I', metadata_size))
            u.reg_write(UC_ARM64_REG_X0, 1)
            u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_X30))
        elif address == 0x4c9b00:
            assert u.reg_read(UC_ARM64_REG_X2) == 0x18
            u.mem_write(u.reg_read(UC_ARM64_REG_X1), struct.pack('<Q', control))
            u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_X30))
    uc.hook_add(UC_HOOK_CODE, hook)
    def call(fn, *args):
        uc.reg_write(UC_ARM64_REG_SP, 0x201E000)
        uc.reg_write(UC_ARM64_REG_X30, stop)
        for reg, arg in zip((UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2), args):
            uc.reg_write(reg, arg)
        uc.emu_start(fn, stop, count=1000)
        assert uc.reg_read(UC_ARM64_REG_PC) == stop
        return uc.reg_read(UC_ARM64_REG_X0)
    uc.mem_write(texture+8, struct.pack('<Q', device))
    for metadata_size in (0, 256, 0x87654321):
        assert call(getter, texture) == metadata_size
    storage = 0x1234567887654321
    for requested in (0, 256):
        uc.mem_write(cmd, bytes(0x100))
        uc.mem_write(cmd, struct.pack('<QQ', buffer, buffer+0x100))
        uc.mem_write(cmd+0x60, struct.pack('<Q', device))
        uc.mem_write(device+0x1a1c, bytes(4))
        uc.mem_write(buffer, bytes(0x100))
        call(save, cmd, storage, requested)
        # Original save records start GPU address and end=start+byte_size.
        words = struct.unpack('<7I', uc.mem_read(buffer, 28))
        assert words[1:3] == (storage >> 32, storage & 0xFFFFFFFF)
        assert words[3:5] == ((storage+requested) >> 32, (storage+requested) & 0xFFFFFFFF)
        assert struct.unpack('<Q', uc.mem_read(cmd,8))[0] == buffer+28
        uc.mem_write(control, bytes(24))
        call(restore, cmd, storage, requested)
        assert uc.mem_read(control,1) == b'\x1f'
        assert struct.unpack('<Q',uc.mem_read(control+8,8))[0] == storage
        assert struct.unpack('<I',uc.mem_read(control+16,4))[0] == requested
    print(json.dumps({'sdk_sha256': config['modules']['sdk']['sha256'],
                      'getter': hex(getter), 'save': hex(save), 'restore': hex(restore),
                      'abi': '(command_buffer, GPU_storage_address, byte_size)',
                      'game_passes_getter_result_as_size': True,
                      'getter_sizes': [0,256,0x87654321], 'recorded_sizes': [0,256],
                      'scope': 'Original SDK client code with synthetic allocation/metadata-query hooks; Vulkan size0 is host design.'}, indent=2))

if __name__ == '__main__':
    main()
