#!/usr/bin/env python3
"""Offline original-NSO AudioOut import/ABI audit; never executes the game.
Run with a Python environment containing the required dependencies: python tests/audio_nso_audit.py /path/to/ExeFS.
"""
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from recomp.nso import load
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM


def main():
    exefs = Path(sys.argv[1])
    config = json.loads((ROOT / "config/bayonetta3-1.2.0.json").read_text())
    source = (ROOT / "runtime/src/hle_audio.cpp").read_text()
    registrations = set(re.findall(r'NX_HLE\("([^"]+)"', source))
    all_imports = set()
    renderer_imports = set()
    modules = {}
    for name in ("main", "subsdk0", "sdk"):
        path = exefs / name
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        assert digest == config["modules"][name]["sha256"], (name, digest)
        print(name, digest)
        module = modules[name] = load(str(path))
        for symbol in module.symbols:
            if symbol.imported and symbol.name.startswith("_ZN2nn5audio"):
                print("IMPORT", symbol.name)
                if "AudioOut" in symbol.name or "8AudioOut" in symbol.name:
                    all_imports.add(symbol.name)
                    assert symbol.name in registrations, symbol.name
                if "Renderer" in symbol.name:
                    renderer_imports.add(symbol.name)
    assert not renderer_imports, renderer_imports
    sdk = modules["sdk"]
    exports = {s.name: s for s in sdk.symbols if not s.imported}
    for name in sorted(all_imports):
        assert name in exports, name
        print("COVERED", name, hex(exports[name].value))
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    def instructions(name):
        symbol = exports[name]
        result = list(cs.disasm_lite(sdk.image[symbol.value:symbol.value + symbol.size], symbol.value))
        print("EXPORT", name, hex(symbol.value))
        for address, _, mnemonic, operands in result:
            print(f" {address:x}: {mnemonic} {operands}")
        return [(mnemonic, operands) for _, _, mnemonic, operands in result]
    prefix = "_ZN2nn5audio"
    assert instructions(prefix + "21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm") == [
        ("stp", "x1, x2, [x0, #8]"), ("str", "x3, [x0, #0x18]"), ("ret", "")]
    for suffix, operand in [
        ("28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", "x0, [x0, #8]"),
        ("25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", "x0, [x0, #0x18]"),
        ("27GetAudioOutBufferBufferSizeEPKNS0_14AudioOutBufferE", "x0, [x0, #0x10]"),
        ("21GetAudioOutSampleRateEPKNS0_8AudioOutE", "w0, [x0]"),
        ("23GetAudioOutChannelCountEPKNS0_8AudioOutE", "w0, [x0, #4]"),
        ("23GetAudioOutSampleFormatEPKNS0_8AudioOutE", "w0, [x0, #8]"),
    ]:
        assert instructions(prefix + suffix) == [("ldr", operand), ("ret", "")]
    init = instructions(prefix + "27InitializeAudioOutParameterEPNS0_17AudioOutParameterE")
    assert init == [("mov", "x8, #-0x3502000000000000"), ("str", "x8, [x0]"), ("ret", "")]
    # These coordinates are assertions against the exact hash above, never runtime hooks.
    assert struct.unpack_from("<5Q", sdk.image, 0xa99750) == (1, 2, 3, 4, 4)
    for suffix in ["19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE",
                   "19RegisterBufferEventEPNS0_8AudioOutEPNS_2os11SystemEventE",
                   "13StartAudioOutEPNS0_8AudioOutE", "12StopAudioOutEPNS0_8AudioOutE",
                   "13CloseAudioOutEPNS0_8AudioOutE"]:
        instructions(prefix + suffix)
    print(f"PASS: {len(all_imports)} distinct AudioOut imports covered; no AudioRenderer imports in main/subsdk0/sdk.")
    print("Static audit only: absence of direct imports does not rule out dynamic renderer use.")


if __name__ == "__main__":
    main()
