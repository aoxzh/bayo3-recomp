#!/usr/bin/env python3
"""Symbolize a Bayonetta3Recomp crash / host address / Windows .map address.

Modes
-----
1. Windows .map (unchanged from the original tool):
       python3 tools/symbolize.py <game.map> <va hex> [generated dir]
2. Build a local symbol index next to the executable (macOS/Linux; local symbols such as the generated
   main_XXXXXXXX functions are absent from Linux .dynsym, so the runtime reads this sidecar at startup):
       python3 tools/symbolize.py --index <binary>            # writes <binary>.syms
3. Resolve one host address, or a guest module+offset:
       python3 tools/symbolize.py --host <binary> 0x104f04c9c [--gen DIR] [--nx-base HEX]
       python3 tools/symbolize.py --host <binary> sdk+0x58c3a0 [--gen DIR]
4. Resolve a crash report written by the runtime (crash-<pid>-<time>.txt, docs/CRASH-REPORTS.md):
       python3 tools/symbolize.py --crash <report.txt> [--binary EXE] [--gen DIR] [--data-root DIR]

Mode 4 maps the host PC -> generated function (main_/subsdk0_/sdk_ + 8 hex digits = the guest
module-relative basic-block address) -> guest VA = NX_BASE + module base + offset -> guest symbol from
gen/<module>/table.cpp. When the executable carries line tables for the generated code (-g or
-gline-tables-only; see docs/CRASH-REPORTS.md), atos (macOS) or llvm-symbolizer (Linux) maps the host PC to
the generated .cpp line, and the exact guest instruction is reconstructed from the block start + the source
instruction order (every generated line is one guest instruction) plus the disassembly comment. Without line
tables it degrades to the basic block + the first instruction word read from the analysis image.
"""
import argparse
import bisect
import os
import re
import shutil
import struct
import subprocess
import sys

# The three B3 modules keep the reference layout's spacing relative to NX_BASE (runtime/include/nx/runtime.h).
MODULE_BASE = {"main": 0x0, "subsdk0": 0x08000000, "sdk": 0x10000000}
NX_BASE_DEFAULT = 0x7BCFF00000 if sys.platform == "darwin" else 0x4000000000

try:
    import capstone  # optional: decode instruction words (Pillow/capstone are available in this environment)
except ImportError:  # pragma: no cover - degrade to the generated source's disassembly comment
    capstone = None


# ---- small helpers ---------------------------------------------------------------------------------------------

def run(cmd, timeout=180):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.stdout if p.returncode == 0 else None
    except (OSError, subprocess.SubprocessError):
        return None


def image_base(binary):
    """The preferred (link-time) base of the executable: Mach-O __TEXT vmaddr, ELF min PT_LOAD vaddr."""
    try:
        with open(binary, "rb") as f:
            head = f.read(32)
            if head[:4] == b"\x7fELF":
                if len(head) < 64 or head[4] != 2:
                    return 0
                endian = "<" if head[5] == 1 else ">"
                f.seek(0)
                data = f.read(64)
                e_phoff = struct.unpack_from(endian + "Q", data, 0x20)[0]
                e_phentsize = struct.unpack_from(endian + "H", data, 0x36)[0]
                e_phnum = struct.unpack_from(endian + "H", data, 0x38)[0]
                if not e_phoff or not e_phentsize or not e_phnum or e_phnum > 256:
                    return 0
                f.seek(e_phoff)
                ph = f.read(e_phentsize * e_phnum)
                base = None
                for i in range(e_phnum):
                    p = ph[i * e_phentsize:(i + 1) * e_phentsize]
                    if len(p) < 0x38 or struct.unpack_from(endian + "I", p, 0)[0] != 1:  # PT_LOAD
                        continue
                    vaddr = struct.unpack_from(endian + "Q", p, 0x10)[0]
                    base = vaddr if base is None else min(base, vaddr)
                return base or 0
            if head[:4] == b"\xcf\xfa\xed\xfe":  # Mach-O 64-bit, little endian
                ncmds = struct.unpack_from("<I", head, 16)[0]
                sizeofcmds = struct.unpack_from("<I", head, 20)[0]
                if ncmds > 4096 or sizeofcmds > (1 << 20):
                    return None
                f.seek(32)
                cmds = f.read(sizeofcmds)
                pos = 0
                for _ in range(ncmds):
                    if pos + 8 > len(cmds):
                        break
                    cmd, cmdsize = struct.unpack_from("<II", cmds, pos)
                    if cmdsize < 8:
                        break
                    if cmd == 0x19 and pos + 32 <= len(cmds):  # LC_SEGMENT_64
                        segname = cmds[pos + 8:pos + 24].split(b"\0", 1)[0].decode(errors="replace")
                        if segname == "__TEXT":
                            return struct.unpack_from("<Q", cmds, pos + 24)[0]
                    pos += cmdsize
    except (OSError, struct.error):
        pass
    return None


def nm_symbols(binary):
    """(image base, [(image-relative offset, name)]) from `nm -n`; local symbols included."""
    out = run(["nm", "-n", binary])
    if out is None:
        return None, None
    base = image_base(binary)
    if base is None:
        base = 0
    syms = []
    for line in out.splitlines():
        m = re.match(r"^\s*([0-9a-fA-F]{8,16})\s+[A-Za-z?]\s+(.+?)\s*$", line)
        if not m:
            continue
        syms.append((int(m.group(1), 16) - base, m.group(2)))
    syms.sort()
    return base, syms


def symbol_at(syms, offset):
    """The last symbol starting at or before `offset`."""
    if not syms:
        return None
    keys = [s[0] for s in syms]
    i = bisect.bisect_right(keys, offset) - 1
    if i < 0:
        return None
    return syms[i]


GEN_RE = re.compile(r"(main|subsdk0|sdk)_([0-9A-Fa-f]{8})(?![0-9A-Fa-f])")


def generated_name(name):
    """('main'|'subsdk0'|'sdk', module-relative block offset) for a generated function name, else None."""
    if not name:
        return None
    m = GEN_RE.search(name)
    if not m:
        return None
    return m.group(1), int(m.group(2), 16)


def describe_guest(guest_va, module, module_off, gen_dir):
    """The runtime's describe() equivalent: guest module+offset, plus the nearest preceding function symbol."""
    text = "%s+0x%x" % (module, module_off)
    if gen_dir:
        syms = guest_symbols(gen_dir, module)
        best = symbol_at(syms, module_off) if syms else None
        if best:
            text += " (%s+0x%x)" % (best[1], module_off - best[0])
    return text


_SYMBOL_CACHE = {}

SYMBOL_LINE = re.compile(
    r'^\s*\{"((?:[^"\\]|\\.)*)",\s*(0x[0-9a-fA-F]+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\},')


def guest_symbols(gen_dir, module):
    """The generated module's ELF symbol table (gen/<module>/table.cpp): [(value, name)] of defined functions."""
    key = (gen_dir, module)
    if key in _SYMBOL_CACHE:
        return _SYMBOL_CACHE[key]
    path = os.path.join(gen_dir, module, "table.cpp")
    syms = []
    try:
        with open(path, "r", errors="replace") as f:
            inside = False
            for line in f:
                if not inside:
                    if "static const nx::Symbol symbols[]" in line:
                        inside = True
                    continue
                if line.startswith("};"):
                    break
                m = SYMBOL_LINE.match(line)
                if not m:
                    continue
                name, value = m.group(1), int(m.group(2), 16)
                type_, imported = int(m.group(4)), int(m.group(6))
                if type_ == 2 and not imported:  # STT_FUNC, defined
                    syms.append((value, name))
    except OSError:
        syms = []
    syms.sort()
    _SYMBOL_CACHE[key] = syms
    return syms


def instruction_word(data_root, module, module_off):
    """The guest instruction word at a module-relative offset, from data/analysis/<module>.bin."""
    if not data_root:
        return None
    path = os.path.join(data_root, "analysis", module + ".bin")
    try:
        with open(path, "rb") as f:
            f.seek(module_off)
            b = f.read(4)
    except OSError:
        return None
    if len(b) < 4:
        return None
    return struct.unpack("<I", b)[0]


def decode_word(word, address=0):
    """AArch64 disassembly of an instruction word via capstone, or None.

    `address` is the NX_BASE-relative address of the word, so PC-relative operands (branches, adr, literal loads)
    are printed as the guest addresses the generated source comment uses.
    """
    if capstone is None or word is None:
        return None
    try:
        md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        for ins in md.disasm(struct.pack("<I", word), address):
            return (ins.mnemonic + " " + ins.op_str).strip()
    except (capstone.CsError, ValueError):
        return None
    return None


# ---- line tables -----------------------------------------------------------------------------------------------

def atos_line(image, address, load_base=None):
    """macOS: generated .cpp line for an address, when `image` (binary or dSYM DWARF) has line tables.

    Without `load_base` the address is a link-time address in the image's own address space (the dSYM DWARF
    file takes those); with `load_base` the address is a runtime PC (atos slides it by the load address).
    """
    if not shutil.which("atos"):
        return None
    cmd = ["atos", "-o", image, "-arch", "arm64"]
    if load_base is not None:
        cmd += ["-l", hex(load_base)]
    cmd.append(hex(address))
    out = run(cmd)
    if not out:
        return None
    m = re.search(r"\(([^()\s]+):(\d+)\)\s*$", out.strip().splitlines()[0])
    if not m:
        return None
    return m.group(1), int(m.group(2))


def dsym_dwarf(binary, explicit=None):
    """The DWARF file inside a dSYM bundle (explicit --dsym, or <binary>.dSYM next to the executable).

    The generated modules are linked from static archives; resolving sdk/subsdk0/interior PCs from the dSYM is
    both faster and more reliable than atos on the bare binary (sanitizer-eng's NX_GEN_LINE_TABLES proof).
    """
    if not binary:
        return None

    def dwarf_in(bundle):
        return os.path.join(bundle, "Contents", "Resources", "DWARF", os.path.basename(binary))

    if explicit:
        path = dwarf_in(explicit) if os.path.isdir(explicit) else explicit
        return path if os.path.isfile(path) else None
    sibling = binary + ".dSYM"
    path = dwarf_in(sibling) if os.path.isdir(sibling) else None
    return path if path and os.path.isfile(path) else None


def macho_uuid(path):
    """The LC_UUID of a Mach-O file (the executable, or the DWARF file inside a dSYM), or None."""
    try:
        with open(path, "rb") as f:
            head = f.read(32)
            if len(head) < 32 or head[:4] != b"\xcf\xfa\xed\xfe":  # Mach-O 64-bit, little endian
                return None
            ncmds = struct.unpack_from("<I", head, 16)[0]
            sizeofcmds = struct.unpack_from("<I", head, 20)[0]
            if ncmds > 4096 or sizeofcmds > (1 << 20):
                return None
            f.seek(32)
            cmds = f.read(sizeofcmds)
    except (OSError, struct.error):
        return None
    pos = 0
    for _ in range(ncmds):
        if pos + 8 > len(cmds):
            break
        cmd, cmdsize = struct.unpack_from("<II", cmds, pos)
        if cmdsize < 8:
            break
        if cmd == 0x1B and pos + 24 <= len(cmds):  # LC_UUID
            return cmds[pos + 8:pos + 24].hex()
        pos += cmdsize
    return None


def select_dsym(binary, explicit=None):
    """(dSYM DWARF path, stale warning or None).

    The dSYM is a snapshot of the debug map: after a relink it describes the previous build's line tables and
    atos can resolve against it silently wrong. Its LC_UUID must match the executable's; a mismatch is reported
    and the dSYM is not used (regenerate with `dsymutil <binary> -o <binary>.dSYM`).
    """
    if not binary:
        return None, None
    path = dsym_dwarf(binary, explicit)
    if not path:
        return None, None
    binary_uuid, dwarf_uuid = macho_uuid(binary), macho_uuid(path)
    if binary_uuid and dwarf_uuid and binary_uuid != dwarf_uuid:
        return None, ("stale dSYM %s: its UUID does not match %s (regenerate with: dsymutil %s -o %s.dSYM); "
                      "the dSYM was not used" % (path, binary, binary, binary))
    return path, None


def llvm_symbolizer_path():
    for name in ("llvm-symbolizer", "llvm-symbolizer-19", "llvm-symbolizer-18", "llvm-symbolizer-17"):
        found = shutil.which(name)
        if found:
            return found
    return None


def llvm_line(binary, address):
    """Linux: generated .cpp line for a link-time address, when llvm-symbolizer and line tables exist."""
    tool = llvm_symbolizer_path()
    if not tool:
        return None
    out = run([tool, "--obj=" + binary, "--functions=short", hex(address)])
    if not out:
        return None
    for line in out.splitlines():
        m = re.match(r"^\s*(\S+\.(?:cpp|cc|cxx|c|hpp|h)):(\d+)", line)
        if m:
            return m.group(1), int(m.group(2))
    return None


def resolve_source_line(binary, report, offset, dwarf=None):
    """file:line for an image-relative offset.

    macOS: atos against a UUID-checked dSYM DWARF (link-time address; the generated modules are static archives,
    so this is the fast/reliable path) when one is available, else atos against the executable with the report's
    runtime load base. Linux: llvm-symbolizer at the link-time address.

    Optimized line tables have gaps: a single instruction can be attributed to line 0 (observed inside a
    generated block, surrounded by instructions of the same source line). Probe the neighbouring addresses of the
    same line region, preferring the preceding ones, and report which probe was used.
    """
    load_base = report.get("image_base")
    pc = report.get("pc")
    if sys.platform == "darwin":
        link_base = image_base(binary)
        if dwarf and link_base is not None:
            start = link_base + offset
            lookup = lambda at: atos_line(dwarf, at)
        elif load_base is not None and pc is not None:
            start = pc
            lookup = lambda at: atos_line(binary, at, load_base)
        else:
            return None
    else:
        base = image_base(binary)
        if base is None:
            return None
        start = base + offset
        lookup = lambda at: llvm_line(binary, at)
    for delta in (0, -4, 4, -8, 8, -12, 12, -16, 16, -20, 20):
        located = lookup(start + delta)
        if located and located[1] > 0:
            return located[0], located[1], delta
    return None


# ---- crash report parsing --------------------------------------------------------------------------------------

REPORT_KEYS = {
    "nx base": "nx_base",
    "host image base": "image_base",
    "host pc": "pc",
    "host pc offset (from the image base)": "offset",
}


def parse_report(path):
    """The host fields symbolize.py needs from a crash report; also returns the raw lines.

    String fields (host executable, report file) are kept as strings: `--crash` without `--binary` uses the
    executable the report names. Numeric fields are parsed, and an unparsable value is ignored.
    """
    report = {}
    raw = []
    try:
        with open(path, "r", errors="replace") as f:
            for line in f:
                raw.append(line.rstrip("\n"))
                if line.startswith("host executable:"):
                    value = line.split(":", 1)[1].strip()
                    if value:
                        report["executable"] = os.path.normpath(os.path.expanduser(value))
                    continue
                if line.startswith("report file:"):
                    value = line.split(":", 1)[1].strip()
                    if value:
                        report["report_file"] = os.path.normpath(os.path.expanduser(value))
                    continue
                for prefix, key in REPORT_KEYS.items():
                    if line.startswith(prefix + ":"):
                        value = line.split(":", 1)[1].strip().split()[0]
                        try:
                            report[key] = int(value, 16) if value.lower().startswith("0x") else int(value)
                        except ValueError:
                            pass
                if "generated=" in line and "generated_name" not in report:
                    m = re.search(r"generated=(\S+)", line)
                    if m and generated_name(m.group(1)):
                        report["generated_name"] = m.group(1)
                if "host_block_offset=" in line and "block_offset" not in report:
                    m = re.search(r"host_block_offset=(0x[0-9a-fA-F]+|\d+)", line)
                    if m:
                        report["block_offset"] = int(m.group(1), 0)
                if line.startswith("CRASH:"):
                    report["crash"] = line
    except OSError as e:
        sys.exit("symbolize: cannot read report %s: %s" % (path, e))
    if "offset" not in report and "pc" in report and "image_base" in report:
        report["offset"] = report["pc"] - report["image_base"]
    return report, raw


# ---- exact guest instruction -----------------------------------------------------------------------------------

def find_generated_source(gen_dir, module, basename):
    """The generated .cpp for a line-table entry: the crash's own module first (code_NNN.cpp exists per module)."""
    for candidate in (module, "main", "subsdk0", "sdk"):
        path = os.path.join(gen_dir, candidate, basename)
        if os.path.isfile(path):
            return path
    return None


def instruction_comment(line):
    """The disassembly comment of a generated source line (the last // segment)."""
    if "//" not in line:
        return None
    return line.rsplit("//", 1)[1].strip()


def exact_instruction(binary, report, gen_dir, data_root, dsym=None):
    """Reconstruct the guest instruction for the reported host PC.

    Returns a dict with block_guest, instruction_guest, word, text, source, note. Without line tables only the
    block and its first instruction are known (note says so).
    """
    name = report.get("generated_name")
    gen = generated_name(name) if name else None
    if not gen:
        return None
    module, block_off = gen
    nx_base = report.get("nx_base", NX_BASE_DEFAULT)
    result = {
        "module": module,
        "block_off": block_off,
        "block_va": nx_base + MODULE_BASE[module] + block_off,
        "instruction_va": None,
        "word": None,
        "text": None,
        "source": None,
        "note": "",
    }
    dwarf, stale_note = (None, None)
    if sys.platform == "darwin":
        dwarf, stale_note = select_dsym(binary, dsym)

    def with_stale(note):
        return (note + "; " + stale_note) if note and stale_note else (stale_note or note)

    # The fallback is always available: the first instruction of the basic block.
    result["word"] = instruction_word(data_root, module, block_off)
    result["text"] = decode_word(result["word"], MODULE_BASE[module] + block_off)
    offset = report.get("offset")
    if offset is None or not binary:
        result["note"] = with_stale("no host pc or no executable available: line-table resolution skipped")
        return result
    located = resolve_source_line(binary, report, offset, dwarf)
    if not located:
        result["note"] = with_stale("no line tables for the generated code in %s (a plain Release build has "
                                    "none): exact instruction unavailable; the basic block and its first "
                                    "instruction are reported instead" % binary)
        return result
    basename, line_no = located[0], located[1]
    probe_delta = located[2] if len(located) > 2 else 0
    source = find_generated_source(gen_dir, module, os.path.basename(basename)) if gen_dir else None
    if source is None:
        result["note"] = with_stale("line tables point at %s:%d, but the generated tree (--gen) has no %s" % (
            basename, line_no, os.path.basename(basename)))
        return result
    try:
        with open(source, "r", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        result["note"] = with_stale("cannot read generated source %s" % source)
        return result
    if line_no < 1 or line_no > len(lines):
        result["note"] = with_stale("line %d outside %s" % (line_no, source))
        return result
    # Find the generated function the line belongs to (the nearest preceding "void <name>(nx::CPU&" line). The
    # generated source uses the C++ identifier (sdk_005996D0), not the mangled symbol (_Z12sdk_005996D0RN...).
    source_name = GEN_RE.search(name).group(0) if GEN_RE.search(name) else name
    start = None
    for i in range(line_no - 1, max(-1, line_no - 200000), -1):
        if re.match(r"^void\s+%s\(nx::CPU&" % re.escape(source_name), lines[i]):
            start = i
            break
    if start is None:
        result["note"] = with_stale("line %d (%s) is not inside generated function %s" %
                                    (line_no, basename, source_name))
        return result
    # Every generated line with a disassembly comment is one guest instruction, in block order.
    target = line_no - 1
    effective = target
    if "//" not in lines[effective]:
        # The line table can point at the function declaration (a crash at the block's entry PC) or at a
        # goto label: the exact instruction is then the next generated instruction line. At a function-entry PC
        # that is the block's first instruction (guest_off = block_off, count = 0).
        forward = target
        while forward + 1 < len(lines) and "//" not in lines[forward] and not lines[forward].lstrip().startswith("}"):
            forward += 1
        if "//" in lines[forward]:
            effective = forward
        else:
            while effective > start and "//" not in lines[effective]:
                effective -= 1
            if "//" not in lines[effective]:
                result["note"] = with_stale("no instruction line at %s:%d" % (basename, line_no))
                return result
    count = sum(1 for i in range(start + 1, effective) if "//" in lines[i])
    guest_off = block_off + 4 * count
    result["instruction_va"] = nx_base + MODULE_BASE[module] + guest_off
    result["instruction_off"] = guest_off
    result["word"] = instruction_word(data_root, module, guest_off)
    result["text"] = decode_word(result["word"], MODULE_BASE[module] + guest_off) or instruction_comment(lines[effective])
    result["source"] = "%s:%d" % (source, effective + 1)
    literal = re.search(r"NXB \+ (0x[0-9a-fA-F]+)ull", lines[effective])
    result["literal"] = int(literal.group(1), 16) if literal else None
    result["comment"] = instruction_comment(lines[effective])
    notes = []
    if probe_delta:
        notes.append("line table gap at the crashing instruction; used the line at pc%+d" % probe_delta)
    if effective != target:
        notes.append("line %d has no instruction; used the nearest instruction line %d" % (line_no, effective + 1))
    if stale_note:
        notes.append(stale_note)
    if notes:
        result["note"] = "; ".join(notes)
    return result


# ---- output ----------------------------------------------------------------------------------------------------

def print_resolution(report, binary, gen_dir, data_root, dsym=None):
    nx_base = report.get("nx_base", NX_BASE_DEFAULT)
    name = report.get("generated_name")
    if not name:
        sym_name = report.get("symbol")
        gen = generated_name(sym_name) if sym_name else None
        if not gen:
            print("generated function: none (the host PC is not inside a generated main_/subsdk0_/sdk_ function)")
            return 1
        report["generated_name"] = sym_name
        name = sym_name
    module, block_off = generated_name(name)
    block_va = nx_base + MODULE_BASE[module] + block_off
    print("host pc            : 0x%x (image base 0x%x, offset 0x%x)" %
          (report.get("pc", 0), report.get("image_base", 0), report.get("offset", 0)))
    print("generated function : %s (host block +0x%x)" % (name, report.get("block_offset", 0)))
    print("guest block        : %s+0x%x = NX_BASE+0x%x" % (module, block_off, MODULE_BASE[module] + block_off))
    print("guest symbol       : %s" % describe_guest(block_va, module, block_off, gen_dir))
    exact = exact_instruction(binary, report, gen_dir, data_root, dsym)
    if exact and exact.get("instruction_va") is not None:
        word = exact.get("word")
        print("exact instruction  : %s+0x%x = NX_BASE+0x%x  word %s  %s" % (
            module, exact["instruction_off"], MODULE_BASE[module] + exact["instruction_off"],
            "0x%08x" % word if word is not None else "?", exact.get("text") or "?"))
        if exact.get("literal") is not None:
            print("  address literal  : NXB + 0x%x (the guest address this instruction refers to)" % exact["literal"])
        if exact.get("source"):
            print("  source           : %s (line tables)" % exact["source"])
        if exact.get("note"):
            print("  note             : %s" % exact["note"])
    else:
        print("exact instruction  : unavailable")
        if exact:
            if exact.get("note"):
                print("  note             : %s" % exact["note"])
            if exact.get("word") is not None:
                print("  fallback block   : %s+0x%x = NX_BASE+0x%x" % (module, block_off, MODULE_BASE[module] + block_off))
                print("  first instruction: %s+0x%x word 0x%08x %s" % (
                    module, block_off, exact["word"], exact.get("text") or ""))
        extra = ""
        if sys.platform == "darwin" and binary:
            extra = "; on macOS also create the dSYM: dsymutil %s -o %s.dSYM" % (binary, binary)
        print("  how to get exact : build the generated modules with line tables (-g / -gline-tables-only, "
              "docs/CRASH-REPORTS.md) and rerun this tool on that binary%s" % extra)
    return 0


def host_address(report, binary, value, gen_dir, data_root, dsym=None):
    """--host: either a guest module+offset or a host address."""
    m = re.match(r"^(main|subsdk0|sdk)\+(0x[0-9a-fA-F]+|\d+)$", value)
    if m:
        module, off = m.group(1), int(m.group(2), 0)
        nx_base = report.get("nx_base", NX_BASE_DEFAULT)
        print("guest address      : %s+0x%x = NX_BASE+0x%x" % (module, off, MODULE_BASE[module] + off))
        print("guest symbol       : %s" % describe_guest(nx_base + MODULE_BASE[module] + off, module, off, gen_dir))
        word = instruction_word(data_root, module, off)
        if word is not None:
            print("instruction word   : 0x%08x %s" % (word, decode_word(word, MODULE_BASE[module] + off) or ""))
        return 0
    try:
        address = int(value, 0)
    except ValueError:
        sys.exit("symbolize: cannot parse address %r (use 0x... or main+0x...)" % value)
    base, syms = nm_symbols(binary)
    if syms is None:
        sys.exit("symbolize: nm failed on %s" % binary)
    if report.get("image_base") is not None:
        offset = address - report["image_base"] if address >= report["image_base"] else address
    elif base is not None and address >= base and address - base < (1 << 32):
        offset = address - base
    else:
        offset = address
    report["offset"] = offset
    if report.get("pc") is None:
        # No runtime load address was given: atos/llvm-symbolizer take link-time addresses, which are exact for an
        # unslid image (the usual case for a --host query).
        report["pc"] = (base or 0) + offset
    report["image_base"] = report.get("image_base", base)
    found = symbol_at(syms, offset)
    if not found:
        print("no symbol at offset 0x%x" % offset)
        return 1
    sym_off, name = found
    print("host address       : 0x%x = %s+0x%x (%s+0x%x)" % (
        address, os.path.basename(binary), offset, name, offset - sym_off))
    gen = generated_name(name)
    if not gen:
        return 0
    report["generated_name"] = name
    report["block_offset"] = offset - sym_off
    if "nx_base" not in report:
        report["nx_base"] = NX_BASE_DEFAULT
    return print_resolution(report, binary, gen_dir, data_root, dsym)


# ---- the original Windows .map mode ----------------------------------------------------------------------------

def legacy_map(map_path, va, gen_dir=None):
    va = int(va, 16)
    syms = []
    with open(map_path, errors="replace") as f:
        for line in f:
            m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s", line)
            if m:
                syms.append((int(m.group(2), 16), m.group(1)))
    syms.sort()
    k = bisect.bisect_right([s[0] for s in syms], va) - 1
    if k < 0:
        sys.exit("symbolize: address %x is below the first symbol" % va)
    addr, name = syms[k]
    print(f"{name} + {va - addr:#x}")
    fn = re.search(r"(main|subsdk0|sdk)_([0-9A-F]{8})", name)
    if fn:
        print(f"guest function: {fn.group(1)}+{int(fn.group(2), 16):#x}")
    return 0


# ---- main ------------------------------------------------------------------------------------------------------

def main(argv):
    # The original invocation has no options; keep it working byte for byte.
    if len(argv) >= 3 and not argv[1].startswith("-"):
        return legacy_map(argv[1], argv[2], argv[3] if len(argv) > 3 else None)

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--index", metavar="BINARY", help="write <BINARY>.syms (nm -n local symbol index) and exit")
    ap.add_argument("--host", nargs=2, metavar=("BINARY", "ADDRESS"), help="resolve a host address or guest module+offset")
    ap.add_argument("--crash", metavar="REPORT", help="resolve a crash-<pid>-<time>.txt crash report")
    ap.add_argument("--binary", metavar="EXE", help="the executable the report came from (default: the report's)")
    ap.add_argument("--gen", metavar="DIR", help="generated tree root (default: $NX_GEN)")
    ap.add_argument("--data-root", metavar="DIR", help="data root with analysis/<module>.bin (default: $NX_DATA_ROOT)")
    ap.add_argument("--nx-base", metavar="HEX", help="NX_BASE override (default: the report's, else the platform's)")
    ap.add_argument("--image-base", metavar="HEX", help="runtime load address of the executable (for --host)")
    ap.add_argument("--dsym", metavar="PATH", help="dSYM bundle or DWARF file for atos (default: <binary>.dSYM)")
    args = ap.parse_args(argv[1:])
    gen_dir = args.gen or os.environ.get("NX_GEN")
    data_root = args.data_root or os.environ.get("NX_DATA_ROOT")

    if args.index:
        base, syms = nm_symbols(args.index)
        if syms is None:
            sys.exit("symbolize: nm failed on %s" % args.index)
        path = args.index + ".syms"
        with open(path, "w") as f:
            f.write("# syms %s base=0x%x entries=%d\n" % (args.index, base or 0, len(syms)))
            for off, name in syms:
                f.write("%x %s\n" % (off, name))
        print("wrote %s (%d symbols, image base 0x%x)" % (path, len(syms), base or 0))
        return 0

    if args.crash:
        report, _ = parse_report(args.crash)
        binary = args.binary or report.get("executable")
        if binary and not os.path.isfile(binary):
            print("symbolize: host executable from the report does not exist: %s" % binary)
            print("symbolize: resolving the guest view from the report text only; pass --binary PATH to add "
                  "line tables")
            binary = None
        if not binary and not report.get("generated_name") and report.get("pc") is None:
            sys.exit("symbolize: the report has no host pc and no executable; pass --binary PATH")
        if args.nx_base:
            report["nx_base"] = int(args.nx_base, 0)
        if args.image_base:
            report["image_base"] = int(args.image_base, 0)
        if report.get("pc") is None and report.get("offset") is None:
            sys.exit("symbolize: %s has no host pc / host pc offset line" % args.crash)
        if "offset" not in report and report.get("pc") is not None:
            report["offset"] = report["pc"] - report.get("image_base", 0)
        print("crash report       : %s" % args.crash)
        if report.get("crash"):
            print("signal             : %s" % report["crash"].split(":", 1)[1].strip())
        return print_resolution(report, binary, gen_dir, data_root, args.dsym)

    if args.host:
        binary, value = args.host
        if not os.path.isfile(binary):
            sys.exit("symbolize: executable not found: %s" % binary)
        report = {}
        if args.nx_base:
            report["nx_base"] = int(args.nx_base, 0)
        if args.image_base:
            report["image_base"] = int(args.image_base, 0)
        return host_address(report, binary, value, gen_dir, data_root, args.dsym)

    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
