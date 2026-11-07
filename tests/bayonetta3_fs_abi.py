"""Static checks on the supplied SDK NSO; never executes the game.
Run with the old project's venv (lz4 + capstone installed):
  .venv/bin/python /path/to/Bayonetta3Recomp/tests/bayonetta3_fs_abi.py /path/to/ExeFS
All addresses below are SDK module-relative, not runtime addresses.
"""
import hashlib
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from recomp.nso import load
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

exefs = Path(sys.argv[1])
sdk = load(str(exefs / "sdk"))
main = load(str(exefs / "main"))
cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
cs.detail = True
symbols = {s.name: s for s in sdk.symbols if not s.imported}
relocs = {r.offset: sdk.symbols[r.sym] for r in sdk.relocs if r.sym}

def require(condition, message):
    if not condition:
        raise RuntimeError(message)

def code(start, size):
    return list(cs.disasm(sdk.image[start:start + size], start))

def at(address, mnemonic, operands):
    ins = code(address, 4)[0]
    require((ins.mnemonic, ins.op_str) == (mnemonic, operands), f"SDK changed at {address:#x}: {ins.mnemonic} {ins.op_str}")

def export(name, address):
    require(symbols[name].value == address, f"SDK export changed: {name}")

def plt(address):
    ins = code(address, 16)
    slot = ins[0].operands[1].imm + ins[1].operands[1].mem.disp
    return relocs[slot].name

export("_ZN2nn2fs14CommitSaveDataEPKc", 0x1e42e0)
export("_ZN2nn2fs6CommitEPKc", 0x1e4130)
at(0x1e42f0, "bl", "#0x1e4150")
at(0x1e4140, "bl", "#0x1e4150")
at(0x1e42f4, "and", "x0, x0, #0xffffffff")
export("_ZN2nn2fs6detail10MountTable4FindEPPNS1_18FileSystemAccessorEPKc", 0x1e04f8)
at(0x1e0524, "mov", "w19, #0xf202")
at(0x1e0528, "movk", "w19, #0x35, lsl #16")
at(0x1e0560, "mov", "w19, #0x5e02")
at(0x1e0564, "movk", "w19, #0x2f, lsl #16")
export("_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE", 0x2393d0)
at(0x2393e0, "mov", "x19, x1")
at(0x2393e4, "mov", "x20, x0")
at(0x2393f4, "mov", "x1, x20")
at(0x2393f8, "mov", "x2, x19")
at(0x239408, "ldurb", "w0, [x29, #-4]")
export("_ZN2nn2fs25GetCurrentThreadFsContextEv", 0x1d5e70)
export("_ZN2nn2fs25SetCurrentThreadFsContextEPKNS0_9FsContextE", 0x1d5ef0)
require(plt(0x5bd4f0) == "_ZN2nn2os11GetTlsValueENS0_7TlsSlotE", "FsContext Get TLS target")
require(plt(0x5bd500) == "_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm", "FsContext Set TLS target")
at(0x1d5e80, "bl", "#0x5bd4f0")
at(0x1d5e90, "csel", "x0, x8, x0, eq")  # null TLS returns the SDK default context
at(0x1d5efc, "b", "#0x5bd500")
for start, op in [(0x379590, "ldr"), (0x3795b0, "str")]:
    at(start, "mrs", "x8, tpidrro_el0")
    at(start + 4, "ldr", "x8, [x8, #0x1f8]")
    at(start + 8, "add", "x8, x8, w0, uxtw #3")
    at(start + 12, op, ("x0" if op == "ldr" else "x1") + ", [x8, #0x80]")
# SDK installs its ThreadType in the runtime-provided per-thread TLS page.
at(0x3779a8, "mrs", "x8, tpidrro_el0")
at(0x3779ac, "str", "x0, [x8, #0x1f8]")  # thread entry
at(0x377f14, "mrs", "x8, tpidrro_el0")
at(0x377f18, "str", "x20, [x8, #0x1f8]")  # main thread manager constructor
at(0x1debf8, "ldr", "w8, [x20]")
at(0x1dec04, "and", "w8, w8, #1")  # WriteOption is a referenced 32-bit flag

imports = {s.name for s in main.symbols if s.imported and s.name.startswith("_ZN2nn2fs")}
source = (Path(__file__).resolve().parents[1] / "runtime/src/hle_fs.cpp").read_text()
hle = set(re.findall(r'NX_HLE\("([^"]+)"', source))
expected_sdk = {"_ZN2nn2fs25GetCurrentThreadFsContextEv", "_ZN2nn2fs25SetCurrentThreadFsContextEPKNS0_9FsContextE"}
require(len(imports) == 24 and len(imports & hle) == 22 and imports - hle == expected_sdk, "main FS coverage changed")
print("SDK SHA256", hashlib.sha256((exefs / "sdk").read_bytes()).hexdigest())
print("bayonetta3_fs_abi: ABI, mount result, TLS initialization and 22 HLE / 2 SDK imports checked")
