#!/usr/bin/env python3
"""Run HID sentinels and optionally replay exact B3 SDK state-copy instructions.

Use a Python environment with capstone, lz4 and unicorn installed. No full game,
Horizon IPC, SDK initialization, or external applet is executed.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def native_test(sanitize):
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise RuntimeError("CXX/clang++/g++ is required")
    with tempfile.TemporaryDirectory(prefix="bayo3-hid-") as tmp:
        binary = str(Path(tmp) / "hid-test")
        command = [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                   "-DNX_BAYONETTA3", "-I" + str(ROOT / "runtime/include"),
                   str(ROOT / "tests/bayonetta3_hid_test.cpp"),
                   str(ROOT / "runtime/src/hle_hid.cpp"), "-o", binary]
        if sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([binary], check=True)


def sdk_test(exefs):
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
    from recomp.nso import load, plt_imports
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_WRITE
    from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                                    UC_ARM64_REG_X3, UC_ARM64_REG_X4,
                                    UC_ARM64_REG_X13, UC_ARM64_REG_X14,
                                    UC_ARM64_REG_X16)

    sdk = load(str(exefs / "sdk"))
    main = load(str(exefs / "main"))
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    md.detail = True
    print("sdk sha256", hashlib.sha256((exefs / "sdk").read_bytes()).hexdigest())
    print("main sha256", hashlib.sha256((exefs / "main").read_bytes()).hexdigest())

    # Exact copy blocks in the original SDK, stopped before unrelated ring logic.
    # Each case executes the decoded NSO bytes, with synthetic shared-memory input
    # and guarded destination, and verifies the byte-for-byte result and all stores.
    cases = [
        ("FullKey", 0x240a14, 0x240a2c, 0x28, UC_ARM64_REG_X16, 0x50, UC_ARM64_REG_X14, 0),
        ("Handheld", 0x240bf4, 0x240c14, 0x28, UC_ARM64_REG_X16, 0x3a0, UC_ARM64_REG_X14, 0),
        ("JoyDual", 0x240de4, 0x240e04, 0x28, UC_ARM64_REG_X16, 0x6f0, UC_ARM64_REG_X14, 0),
        ("JoyLeft", 0x240fd4, 0x240ff4, 0x28, UC_ARM64_REG_X16, 0xa40, UC_ARM64_REG_X14, 0),
        ("JoyRight", 0x2411c4, 0x2411e4, 0x28, UC_ARM64_REG_X16, 0xd90, UC_ARM64_REG_X14, 0),
        ("Mouse", 0x23ea48, 0x23ea60, 0x28, UC_ARM64_REG_X13, 0x28, UC_ARM64_REG_X14, 0),
        ("SixAxis", 0x242600, 0x242630, 0x60, None, 0x28, UC_ARM64_REG_X16, 8),
        ("Touch1", 0x245f08, 0x245f38, 0x38, UC_ARM64_REG_X13, 0x28, UC_ARM64_REG_X14, 0),
        ("Touch16", 0x249aac, 0x249c88, 0x290, UC_ARM64_REG_X3, 0x28, UC_ARM64_REG_X4, 0),
    ]
    from unicorn.arm64_const import UC_ARM64_REG_X18, UC_ARM64_REG_X15, UC_ARM64_REG_X12
    for name, start, end, size, source_reg, source_offset, dest_reg, dest_bias in cases:
        instructions = list(md.disasm(bytes(sdk.image[start:end]), start))
        assert instructions and instructions[-1].address + 4 == end
        # No calls/branches can escape these bounded copy blocks.
        assert all(i.mnemonic in ("ldr", "ldp", "str", "stp", "add", "cmp", "csinc", "csel") for i in instructions)
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(0x200000, 0x80000)
        uc.mem_write(start, bytes(sdk.image[start:end]))
        src, dst = 0x400000, 0x500100
        uc.mem_map(src, 0x2000)
        uc.mem_map(0x500000, 0x2000)
        uc.mem_write(dst - 16, b"\xa5" * (size + 32))
        payload = bytearray((i * 37 + 11) % 256 for i in range(size))
        if name.startswith("Touch"):
            struct.pack_into("I", payload, 8, 0)  # real empty touch sample
        uc.mem_write(src + source_offset, bytes(payload))
        uc.reg_write(source_reg or UC_ARM64_REG_X18, src)
        uc.reg_write(dest_reg, dst + dest_bias)
        if name == "Touch16":
            # This block computes x4=x16+x15 before writing the result.
            uc.reg_write(UC_ARM64_REG_X16, dst)
            uc.reg_write(UC_ARM64_REG_X15, 0)
            uc.reg_write(UC_ARM64_REG_X12, 16)
        stores = []
        uc.hook_add(UC_HOOK_MEM_WRITE, lambda _u, _a, address, length, _v, _d: stores.append((address, length)))
        uc.emu_start(start, end, count=len(instructions))
        assert all(dst <= address and address + length <= dst + size for address, length in stores), name
        actual = bytes(uc.mem_read(dst - 16, size + 32))
        assert actual[:16] == actual[-16:] == b"\xa5" * 16, name
        if name.startswith("Touch"):
            # The SDK copies fields, leaving header padding untouched. HLE zeros it.
            payload[12:16] = b"\xa5" * 4
        assert actual[16:-16] == bytes(payload), name
        assert max(address + length - dst for address, length in stores) == size, name
        print(f"SDK replay {name}: {size:#x} bytes; exact stores and sentinels passed")

    # Check public state/configuration imports are actually covered by registration.
    source = (ROOT / "runtime/src/hle_hid.cpp").read_text()
    imports = [s.name for s in main.symbols if s.imported and "2nn3hid" in s.name]
    for name in imports:
        if any(token in name for token in ("GetNpadState", "GetMouseState", "GetTouchScreenState", "GetSixAxisSensor", "InitializeTouchScreen", "ShowControllerSupport", "SetSupportedNpad", "SetNpadJoyHold", "SetNpadHandheld")):
            assert f'NX_HLE("{name}"' in source, name
    assert not any("GetSupportedNpadIdType" in s.name for s in sdk.symbols), "SDK ABI changed: inspect ID getter"

    # The game's SDK queues vibration samples internally and calls its own
    # defined detail function through the PLT. The loader also redirects
    # function relocations for SDK-defined symbols to matching HLE entries.
    vibration_batch = ("_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandle"
                       "EPKNS0_14VibrationValueEi")
    slots = [r for r in sdk.relocs if r.sym and sdk.symbols[r.sym].name == vibration_batch]
    assert len(slots) == 1 and (slots[0].offset, slots[0].type) == (0xcdb068, 1026), slots
    assert sdk.symbols[slots[0].sym].type == 2 and not sdk.symbols[slots[0].sym].imported
    assert f'NX_HLE("{vibration_batch}"' in source, vibration_batch
    flush_call = list(md.disasm(bytes(sdk.image[0x24aaf8:0x24aafc]), 0x24aaf8))
    assert len(flush_call) == 1 and flush_call[0].mnemonic == "bl"
    assert flush_call[0].operands[0].imm == 0x5bfb30
    print("SDK vibration batch: flush PLT, defined symbol relocation, and HLE registration verified")

    # Resolve main's PLT from its actual relocations and enumerate direct BL sites.
    got = plt_imports(main)
    targets = {}
    for p in range(main.text[0] + 12, sum(main.text), 4):
        if main.u32(p) != 0xd61f0220:  # br x17
            continue
        ins = list(md.disasm(bytes(main.image[p - 12:p + 4]), p - 12))
        if len(ins) == 4 and ins[0].mnemonic == "adrp" and ins[1].mnemonic == "ldr":
            slot = ins[0].operands[1].imm + ins[1].operands[1].mem.disp
            name = got.get(slot, "")
            if "SixAxisSensor" in name:
                targets[p - 12] = name
    calls = []
    for p in range(main.text[0], sum(main.text), 4):
        word = main.u32(p)
        if word >> 26 != 0x25:  # bl imm26
            continue
        imm = word & 0x3ffffff
        if imm & (1 << 25):
            imm -= 1 << 26
        name = targets.get(p + imm * 4)
        if name:
            calls.append((p, name))
    reads = [p for p, n in calls if "21GetSixAxisSensorState" in n]
    assert len(reads) == 10, reads
    print("main direct SixAxis reads:", ", ".join(hex(p) for p in reads))
    print("All native HID and shipped-SDK ABI checks passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exefs", type=Path, help="Original B3 ExeFS containing main and sdk")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    native_test(args.sanitize)
    if args.exefs:
        sdk_test(args.exefs)
