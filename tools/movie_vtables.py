#!/usr/bin/env python3
"""Print the movie:: vtables of the recompiled Bayonetta 3 modules, resolved from the generated tables.

Why this exists: the FFmpeg port of `movie::` replaces the object vtables, so it needs the exact slot map of the
classes the game really uses (docs/archive/LINUX-VERIFICATION-2026-10-22.md §12). The vtables are static data with
relocations, so the map can be read offline — no build, no run, no probe. This script is that read, made repeatable.

How it works:
  1. `_ZTVN5movie...` symbols in gen/<module>/table.cpp give each vtable's module-relative address and size
     (size/8 = slot count).
  2. `relocs[]` has one entry per vtable slot at `vtable + i*8`; its `sym` field indexes `symbols[]`, which carries
     the method's mangled name.

Usage:
  tools/movie_vtables.py                       # every movie:: vtable, slot by slot
  tools/movie_vtables.py --class NnCodec       # one class
  tools/movie_vtables.py --json                # machine-readable, for the runtime or a test
  tools/movie_vtables.py --gen <gen dir>       # a different generated tree (default: private/bayonetta3/gen)

Exit codes: 0 ok, 1 the generated tree is missing or unparsable.
"""
import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

# gen/<module>/table.cpp entry shapes, as emitted by recomp/.
SYMBOL_RE = re.compile(r'\{"((?:[^"\\]|\\.)*)", (0x[0-9a-fA-F]+), (\d+), (\d+), (\d+), (\d+)\}')
RELOC_RE = re.compile(r'\{(0x[0-9a-fA-F]+), (\d+), (\d+), (-?\d+)ll\}')
MODULE_RE = re.compile(r'ModuleInfo\s+nx_module_(\w+)\s*=\s*\{(.*?)\n\};', re.S)


def _length_prefixed_names(body):
    """Read Itanium length-prefixed components: '5movie7NnCodec' -> ['movie', 'NnCodec']."""
    parts, i = [], 0
    while i < len(body) and body[i].isdigit():
        j = i
        while j < len(body) and body[j].isdigit():
            j += 1
        n = int(body[i:j])
        parts.append(body[j:j + n])
        i = j + n
    return parts


def demangle_vtable(sym):
    """'_ZTVN5movie7NnCodecE' -> 'movie::NnCodec' (enough for a report; not a full demangler)."""
    if not sym.startswith("_ZTV"):
        return sym
    body = sym[4:]
    # A nested name is introduced by N and terminated by E; the leading N must not be read as a component.
    if body.startswith("N"):
        body = body[1:]
    if body.endswith("E"):
        body = body[:-1]
    return "::".join(_length_prefixed_names(body))


def demangle_method(sym):
    """A short readable name: keep the class, drop the return type and the argument list's mangling detail."""
    if sym == "__cxa_pure_virtual":
        return "__cxa_pure_virtual (pure virtual)"
    if not sym.startswith("_ZN") and not sym.startswith("_ZNK"):
        return sym
    # _ZN5movie7NnCodec5StartEv  -> movie::NnCodec::Start
    # _ZNK5movie7NnCodec15GetOutputFormatEPNS_9MediaDataE -> movie::NnCodec::GetOutputFormat  (const)
    const = sym.startswith("_ZNK")
    body = sym[4:] if const else sym[3:]  # drop _ZN / _ZNK
    # The component list ends where the argument list begins: the first 'E' that is not inside a nested name.
    depth, end = 0, len(body)
    for i, c in enumerate(body):
        if c == "N":
            depth += 1
        elif c == "E":
            if depth == 0:
                end = i
                break
            depth -= 1
    parts = _length_prefixed_names(body[:end])
    return ("::".join(parts) + (" (const)" if const else "")) if parts else sym


def parse_module(table_cpp):
    """(symbols, reloc_map) of one module's table.cpp."""
    text = table_cpp.read_text(encoding="utf-8", errors="replace")
    symbols = [(name, int(val, 16), int(size), int(typ), int(bind), int(imp))
               for name, val, size, typ, bind, imp in SYMBOL_RE.findall(text)]
    reloc_map = {}
    for off, typ, sym, addend in RELOC_RE.findall(text):
        reloc_map[int(off, 16)] = (int(typ), int(sym), int(addend))
    return symbols, reloc_map


def vtable_slots(base, size, symbols, reloc_map):
    """The slots of one vtable, with each slot's symbol name (None for the vtable header words)."""
    slots = []
    for i in range(size // 8):
        entry = reloc_map.get(base + i * 8)
        if not entry:
            slots.append({"slot": i, "symbol": None, "method": None, "target": None})
            continue
        _, sym_index, addend = entry
        if sym_index < len(symbols):
            name = symbols[sym_index][0]
            slots.append({"slot": i, "symbol": name, "method": demangle_method(name),
                          "target": hex(symbols[sym_index][1] + addend)})
        else:
            slots.append({"slot": i, "symbol": None, "method": None, "target": None})
    return slots


def collect(gen_dir, prefix="_ZTVN5movie"):
    """Every vtable whose symbol starts with `prefix` (default movie::) in every module."""
    out = []
    for table in sorted(gen_dir.glob("*/table.cpp")):
        module = table.parent.name
        symbols, reloc_map = parse_module(table)
        for name, value, size, typ, bind, imp in symbols:
            if not name.startswith(prefix):
                continue
            slots = vtable_slots(value, size, symbols, reloc_map)
            out.append({
                "module": module,
                "symbol": name,
                "class": demangle_vtable(name),
                "offset": hex(value),
                "size": size,
                "slot_count": size // 8,
                "slots": slots,
            })
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gen", default=str(REPO / "private" / "bayonetta3" / "gen"),
                    help="generated module tree (default: private/bayonetta3/gen)")
    ap.add_argument("--class", dest="cls", default=None,
                    help="only vtables whose demangled class contains this text (e.g. NnCodec)")
    ap.add_argument("--prefix", default="_ZTVN5movie",
                    help="mangled vtable-symbol prefix (default _ZTVN5movie; e.g. _ZTVN7android lists the "
                         "Android media stack's vtables)")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args(argv)

    gen_dir = Path(args.gen)
    if not gen_dir.is_dir():
        print(f"movie_vtables: no generated tree at {gen_dir} (run tools/prepare_bayonetta3.py --generate)",
              file=sys.stderr)
        return 1
    tables = collect(gen_dir, args.prefix)
    if not tables:
        print(f"movie_vtables: no {args.prefix}* symbols under {gen_dir}", file=sys.stderr)
        return 1
    if args.cls:
        tables = [t for t in tables if args.cls.lower() in t["class"].lower()]
        if not tables:
            print(f"movie_vtables: no vtable matches {args.cls!r}", file=sys.stderr)
            return 1

    if args.json:
        print(json.dumps(tables, indent=2))
        return 0

    for t in tables:
        print(f"\n{t['class']}  ({t['module']}+{t['offset']}, {t['size']} bytes = {t['slot_count']} slots)")
        for s in t["slots"]:
            if s["symbol"] is None:
                print(f"  {s['slot']:2d}  (no relocation: vtable header or unused)")
            else:
                print(f"  {s['slot']:2d}  {s['method']}")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
