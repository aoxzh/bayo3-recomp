#!/usr/bin/env python3
"""Audit the original B3 SDK's NVN device ABI, without booting the game.
Requires existing project venv (lz4, capstone, unicorn).
Usage: python tests/bayonetta3_nvn_device_sdk.py /path/to/ExeFS/sdk
Only pure SDK builders/getters/query code is emulated. DeviceInitialize is never run.
"""
import hashlib
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from recomp.nso import load, apply_relative
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X30, UC_ARM64_REG_PC


def main():
    path = Path(sys.argv[1])
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    assert digest == '2a89fa1c0e677710e89ed29b578162affef530a0c9b8c93cda15d50a57a20d28', digest
    sdk = load(str(path))
    apply_relative(sdk)
    # Addresses below are assertions bound to this original NSO digest. They are
    # analysis coordinates only; production resolves through SDK bootstrap.
    exports = {s.name: s.value for s in sdk.symbols if not s.imported}
    assert exports['nvnBootstrapLoader'] == 0x4e9410
    assert sdk.u64(0xcbaa78) == 0x4a6de0
    assert sdk.u64(0xce8060) == 0xcd3c70
    assert sdk.u64(0xcd3c70) == 0x4ac810
    table = sdk.u64(0xce8030)
    assert table == 0xcd2bc0
    names = [sdk.cstr(0xb1d723 + sdk.u32(0xb21038 + i * 4)) for i in range(0x214)]
    assert names == sorted(names) and len(set(names)) == len(names)
    procs = {'nvn' + name: sdk.u64(table + i * 8) for i, name in enumerate(names)}
    source = (ROOT / 'runtime/src/nvn/nvn_device.cpp').read_text()
    registrations = re.findall(r'NX_HLE\("([^"]+)"', source)
    for name in registrations:
        assert name in procs and sdk.in_text(procs[name]), name
        print('RESOLVED', name, f'sdk+{procs[name]:x}')
    assert procs['nvnDeviceInitialize'] == 0x4a6e60
    assert sdk.u64(sdk.u64(0xce8068)) == 0x4abd40
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)

    def instructions(address, size):
        return [(op, args) for _, _, op, args in cs.disasm_lite(sdk.image[address:address + size], address)]

    assert instructions(procs['nvnDeviceBuilderGetFlags'], 8) == [('ldr', 'w0, [x0, #4]'), ('ret', '')]
    assert instructions(procs['nvnDeviceBuilderSetDefaults'], 12) == [('stp', 'xzr, xzr, [x0]'), ('str', 'xzr, [x0, #0x10]'), ('ret', '')]
    assert instructions(procs['nvnDeviceBuilderSetFlags'], 8) == [('str', 'w1, [x0, #4]'), ('ret', '')]
    for name, op, operand in [
        ('GetWindowOriginMode', 'ldr', 'w0, [x0]'), ('SetWindowOriginMode', 'str', 'w1, [x0]'),
        ('GetDepthMode', 'ldr', 'w0, [x0, #4]'), ('SetDepthMode', 'str', 'w1, [x0, #4]'),
    ]:
        assert instructions(procs['nvnDevice' + name], 8) == [(op, operand), ('ret', '')]
    for name in ['nvnDeviceSetDebugLabel', 'nvnDeviceInstallDebugCallback']:
        assert instructions(procs[name], 4) == [('ret', '')]  # retail SDK; host adds diagnostic semantics
    assert instructions(procs['nvnDeviceIsExternalDebuggerAttached'], 8) == [('mov', 'w0, wzr'), ('ret', '')]
    assert ('bl', '#0x4abd90') in instructions(procs['nvnDeviceGetInteger'], 48)

    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.mem_map(0, (len(sdk.image) + 4095) & -4096)
    u.mem_write(0, bytes(sdk.image))
    u.mem_map(0x2000000, 0x20000)
    device, builder, output, stop = 0x2001000, 0x2004000, 0x2005000, 0x201fff0

    def call(fn, *args):
        u.reg_write(UC_ARM64_REG_SP, 0x201f000)
        u.reg_write(UC_ARM64_REG_X30, stop)
        for reg, arg in zip([UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2], args):
            u.reg_write(reg, arg)
        u.emu_start(fn, stop, count=1000)
        assert u.reg_read(UC_ARM64_REG_PC) == stop, 'pure SDK function exceeded instruction budget'
        return u.reg_read(UC_ARM64_REG_X0)

    u.mem_write(builder - 16, b'\xa5' * 56)
    call(procs['nvnDeviceBuilderSetDefaults'], builder)
    assert u.mem_read(builder, 24) == bytes(24)
    assert u.mem_read(builder - 16, 16) == b'\xa5' * 16
    assert u.mem_read(builder + 24, 16) == b'\xa5' * 16
    for flags in [0, 0x88, 0x100, 0xffffffff]:
        call(procs['nvnDeviceBuilderSetFlags'], builder, flags)
        assert call(procs['nvnDeviceBuilderGetFlags'], builder) == flags

    def query(pname):
        u.mem_write(output - 4, struct.pack('<3i', 123, -1, 456))
        call(procs['nvnDeviceGetInteger'], device, pname, output)
        before, value, after = struct.unpack('<3i', u.mem_read(output - 4, 12))
        assert (before, after) == (123, 456)
        return value

    # Every static source-table constant is checked against actual SDK execution.
    # The source's feature/limit overrides are tested separately against weak-host
    # Vulkan properties in the production-handler test.
    body = source.split('const std::map<uint32_t, int32_t> kDeviceInfo = {', 1)[1].split('};', 1)[0]
    constants = {int(key): (12 if val == 'kApiMinor' else int(val, 0))
                 for key, val in re.findall(r'\{(\d+),\s*(kApiMinor|0x[\da-fA-F]+|\d+)\}', body)}
    assert constants[0] == 55 and constants[1] == 12
    for pname, value in constants.items():
        assert query(pname) == value, (pname, value, query(pname))
    # Dynamic fields have a layout different from SDK16. Inject distinctive values
    # and verify the formulas; these are NOT claims about a real Tegra's capabilities.
    fields = {0xb4: 7, 0xac: 13, 0x20: 17, 0x3c: 19, 0x44: 23, 0xcc: 29,
              0x28: 31, 0x40: 37, 0x2c: 41, 0x30: 43, 0x34: 47, 0xbc: 53}
    for offset, value in fields.items():
        u.mem_write(device + offset, struct.pack('<I', value))
    expected = {23: 7, 24: 7 * 13, 43: 17 << 10, 45: 19, 55: 13, 56: 23, 58: 29,
                61: 31, 62: 37, 63: 41, 64: 43, 65: 47, 98: 53}
    for pname, value in expected.items():
        assert query(pname) == value, (pname, value, query(pname))
    assert query(59) == 1
    u.mem_write(device + 0x1b35, b'\x01')
    assert query(59) == 0
    assert [query(i) for i in range(99, 105)] == [256, 16, 65535, 65535, 256, 16383]
    print(f'PASS: {len(registrations)} production device APIs resolve; {len(constants)} SDK constants; builder and dynamic query ABI verified')
    print('Scope: offline pure SDK execution only. No DeviceInitialize, guest main, Vulkan, or whole-game execution.')


if __name__ == '__main__':
    main()
