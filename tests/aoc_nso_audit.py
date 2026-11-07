#!/usr/bin/env python3
"""Bounded original B3 SDK AOC wire probe; does not launch the game.
Run with a Python environment containing the required dependencies: python tests/aoc_nso_audit.py /path/to/sdk.
Only synthetic TLS/transport and reply-reader boundaries are substituted.
"""
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from recomp.nso import load
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X30, UC_ARM64_REG_SP, UC_ARM64_REG_PC


def main():
    path = Path(sys.argv[1])
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    config = json.loads((ROOT / 'config/bayonetta3-1.2.0.json').read_text())
    assert digest == config['modules']['sdk']['sha256'], digest
    m = load(str(path))
    exports = {s.name: s for s in m.symbols if not s.imported}
    api = exports['_ZN2nn3aoc28CheckAddOnContentMountStatusEv']
    proxy = next(s for s in exports.values() if '_nn_sf_sync_CheckAddOnContentMountStatusEm' in s.name)
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    def code(s):
        return list(cs.disasm_lite(m.image[s.value:s.value+s.size], s.value))
    assert ('mov', 'x1, xzr') in [(n, o) for _, _, n, o in code(api)]
    assert ('mov', 'w2, #0x32') in [(n, o) for _, _, n, o in code(proxy)]
    # Resolve the proxy's actual PLT and symbol relocation rather than selecting
    # a similarly named template instantiation.
    plt = next(int(o[1:], 16) for _, _, n, o in code(proxy) if n == 'bl')
    stub = list(cs.disasm_lite(m.image[plt:plt+16], plt))
    slot = int(stub[0][3].split('#')[1], 16) + int(stub[1][3].split('#')[1].split(']')[0], 16)
    reloc = next(r for r in m.relocs if r.offset == slot)
    helper = m.symbols[reloc.sym]
    assert 'InRawArgumentInfoILm8ELm8ELl0E' in helper.name
    assert 'ELm8ELm0ELb1E' in helper.name  # input8, output0, sendPID
    hooks = {}
    for _, _, n, operand in code(helper):
        if n != 'bl':
            continue
        target = int(operand[1:], 16)
        stub = list(cs.disasm_lite(m.image[target:target+16], target))
        slot = int(stub[0][3].split('#')[1], 16) + int(stub[1][3].split('#')[1].split(']')[0], 16)
        r = next(r for r in m.relocs if r.offset == slot)
        hooks[target] = m.symbols[r.sym].name
    results = []
    for response in (0, 0x190A6):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(0, (len(m.image) + 4095) & ~4095)
        uc.mem_write(0, bytes(m.image))
        region = 0x2000000
        uc.mem_map(region, 0x10000)
        tls, obj, stop = region, region + 0x1000, region + 0x2000
        stack = region + 0xF000
        uc.mem_write(obj + 4, struct.pack('<I', 42))
        uc.reg_write(UC_ARM64_REG_X0, obj)
        uc.reg_write(UC_ARM64_REG_X1, 0)
        uc.reg_write(UC_ARM64_REG_X2, 50)
        uc.reg_write(UC_ARM64_REG_SP, stack)
        uc.reg_write(UC_ARM64_REG_X30, stop)
        captures = []
        def hook(u, address, size, _):
            if address == stop:
                u.emu_stop()
                return
            name = hooks.get(address)
            if name is None:
                return
            if 'GetMessageBufferOnTls' in name:
                u.reg_write(UC_ARM64_REG_X0, tls)
            elif 'GetInlineContext' in name:
                u.reg_write(UC_ARM64_REG_X0, 0)
            elif 'SendSyncRequest' in name:
                assert u.reg_read(UC_ARM64_REG_X0) == 42
                assert u.reg_read(UC_ARM64_REG_X1) == tls
                assert u.reg_read(UC_ARM64_REG_X2) == 0x100
                words = struct.unpack('<16I', u.mem_read(tls, 64))
                assert words[:3] == (6, 0x8000000A, 1), words
                assert words[8] == 0x49434653 and words[10] == 50, words
                assert words[12:14] == (0, 0), words
                captures.append({'type': 6, 'command': 50, 'send_pid': True,
                                 'raw_input_bytes': 8, 'placeholder': 0, 'raw_output_bytes': 0})
                u.mem_write(tls, struct.pack('<4I', 0x4F434653, 0, response, 0))
                u.reg_write(UC_ARM64_REG_X0, 0)
            elif 'HipcMessageReader10Initialize' in name:
                # Substitute reply-reader metadata only; Result is then loaded
                # by original SDK instructions from our synthetic SFCO reply.
                output = u.reg_read(UC_ARM64_REG_X0)
                u.mem_write(output, bytes(0x60))
                u.mem_write(output, struct.pack('<Q', tls))
            elif 'HandleHipcErrorOnClient' in name:
                pass
            else:
                raise AssertionError(name)
            u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_X30))
        uc.hook_add(UC_HOOK_CODE, hook)
        uc.emu_start(helper.value, stop + 4, count=500)
        assert uc.reg_read(UC_ARM64_REG_PC) == stop
        assert uc.reg_read(UC_ARM64_REG_X0) == response
        assert len(captures) == 1
        results.append({'request': captures[0], 'synthetic_result': response, 'returned_result': response})
    print(json.dumps({'sdk_sha256': digest, 'export': hex(api.value), 'proxy': hex(proxy.value),
                      'helper': hex(helper.value), 'probes': results,
                      'limits': 'Synthetic transport/TLS/reply reader; validates original client ABI, not Horizon server internals.'}, indent=2))

if __name__ == '__main__':
    main()
