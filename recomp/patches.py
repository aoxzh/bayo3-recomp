"""Code patches for the Switch program, the formats Atmosphere, yuzu and Ryujinx load: IPS / IPS32 files named by the
module's build ID, and IPSwitch text patches (.pchtxt) that name it with @nsobid-.

The game's code is recompiled from the NSO, so a patch is applied to the module's image before the recompiler reads
it (the instructions it changes become the code that is compiled) and to the image the runtime loads (the data it
changes is what the game sees). Offsets are those of the console's loader: an IPS offset counts the NSO's 0x100-byte
header, so the image offset is the IPS offset - 0x100; a .pchtxt line's offset gets its file's `@flag offset_shift`
added first (most patches set it to 0x100 and so write image offsets).

    patches_for(build_id, files) -> [(file, [(image offset, bytes)])]   the files that apply to this module
    apply(image, patches)        -> list of problems (writes outside the image are refused, not clipped)
"""
import hashlib
import re
from pathlib import Path

NSO_HEADER = 0x100
SUFFIXES = (".ips", ".pchtxt")


def build_id(raw):
    """The module's build ID (NSO header 0x40, 32 bytes) as upper-case hex."""
    return bytes(raw[0x40:0x60]).hex().upper()


def _same_id(module_id, named):
    """A patch names a build ID by its first bytes (files often drop the trailing zeros, some keep 16 of 64 digits)."""
    a, b = module_id.upper().rstrip("0"), named.upper().strip().rstrip("0")
    return len(b) >= 16 and a.startswith(b)


def parse_ips(data):
    """[(file offset, bytes)] of an IPS ("PATCH" .. "EOF", 24-bit offsets) or IPS32 ("IPS32" .. "EEOF") file."""
    if data[:5] == b"IPS32":
        width, end, pos = 4, b"EEOF", 5
    elif data[:5] == b"PATCH":
        width, end, pos = 3, b"EOF", 5
    else:
        raise ValueError("not an IPS file")
    out = []
    while True:
        if data[pos:pos + len(end)] == end and pos + len(end) == len(data):
            return out
        if pos + width + 2 > len(data):
            if data[pos:pos + len(end)] == end:
                return out
            raise ValueError("truncated IPS record")
        offset = int.from_bytes(data[pos:pos + width], "big")
        size = int.from_bytes(data[pos + width:pos + width + 2], "big")
        pos += width + 2
        if size:
            chunk = bytes(data[pos:pos + size])
            if len(chunk) != size:
                raise ValueError("truncated IPS record")
            pos += size
        else:  # run-length: a count, then the byte to repeat
            if pos + 3 > len(data):
                raise ValueError("truncated IPS run")
            count = int.from_bytes(data[pos:pos + 2], "big")
            chunk = bytes(data[pos + 2:pos + 3]) * count
            pos += 3
        out.append((offset, chunk))


def _unescape(s):
    return s.encode("utf-8").decode("unicode_escape").encode("latin-1")


def parse_pchtxt(text):
    """[(build ID, [(file offset, bytes)])] of an IPSwitch text patch.

    `@nsobid-<id>` starts a module's patches; `@flag offset_shift <n>` is added to the offsets after it; lines are
    written only between `@enabled` and the next `@disabled` (lines outside any such block are not, as yuzu and
    Ryujinx read it); `@stop` ends the file. A line is `<hex offset> <hex bytes>` (bytes in the order written;
    `@little-endian` reverses each value) or `<hex offset> "<string>"` (written with a terminating NUL)."""
    out, current, shift, enabled, little = [], None, 0, False, False
    for raw in text.splitlines():
        line = raw.split("//")[0].strip()
        if not line or line.startswith("#"):
            continue
        low = line.lower()
        if low.startswith("@nsobid-"):
            current = (line[8:].strip(), [])
            out.append(current)
            shift, enabled = 0, False
        elif low == "@stop":
            break
        elif low == "@enabled":
            enabled = True
        elif low == "@disabled":
            enabled = False
        elif low == "@little-endian":
            little = True
        elif low == "@big-endian":
            little = False
        elif low.startswith("@flag"):
            parts = line.split()
            if len(parts) >= 3 and parts[1].lower() == "offset_shift":
                shift = int(parts[2], 0)
        elif line.startswith("@"):
            continue  # @flag print_values, @flag debug_info and the like
        elif current is not None and enabled:
            m = re.match(r'^([0-9A-Fa-f]+)\s+(?:"(.*)"|([0-9A-Fa-f\s]+))\s*$', line)
            if not m:
                raise ValueError(f"unreadable patch line: {raw.strip()}")
            offset = int(m.group(1), 16) + shift
            if m.group(2) is not None:
                value = _unescape(m.group(2)) + b"\0"
            else:
                digits = re.sub(r"\s+", "", m.group(3))
                if len(digits) % 2:
                    raise ValueError(f"odd number of hex digits: {raw.strip()}")
                value = bytes.fromhex(digits)
                if little:
                    value = value[::-1]
            current[1].append((offset, value))
    return out


def patches_for(module_id, files):
    """The patches of `files` (paths, applied in this order) that name this build ID: [(file, [(image offset, bytes)])].
    An unreadable file raises ValueError naming it."""
    out = []
    for f in files:
        f = Path(f)
        try:
            if f.suffix.lower() == ".ips":
                if not _same_id(module_id, f.stem):
                    continue
                records = parse_ips(f.read_bytes())
            else:
                records = []
                for named, recs in parse_pchtxt(f.read_text(encoding="utf-8", errors="replace")):
                    if _same_id(module_id, named):
                        records.extend(recs)
                if not records:
                    continue
        except (ValueError, OSError) as e:
            raise ValueError(f"{f}: {e}") from None
        out.append((f, [(off - NSO_HEADER, data) for off, data in records]))
    return out


def apply(image, patches):
    """Write the patches into the image (bytearray), in order. Returns the problems (a write before the image or past
    its end is skipped and reported)."""
    problems = []
    for f, records in patches:
        for off, data in records:
            if off < 0 or off + len(data) > len(image):
                problems.append(f"{Path(f).name}: {off + NSO_HEADER:#x} (+{len(data)}) is outside the module")
                continue
            image[off:off + len(data)] = data
    return problems


def find_files(mods_root):
    """The code patch files of the switched-on mods under mods_root, in the order the mods apply (mods/order.txt,
    then name order; a folder starting with '-' or '.' is off), each mod's files in path order."""
    root = Path(mods_root)
    if not root.is_dir():
        return []
    try:
        order = [l.strip().lstrip("-") for l in (root / "order.txt").read_text(encoding="utf-8").splitlines()]
        order = [n for n in order if n]
    except OSError:
        order = []
    mods = [d for d in root.iterdir() if d.is_dir() and not d.name.startswith(("-", "."))]
    mods.sort(key=lambda d: (order.index(d.name) if d.name in order else len(order), d.name))
    files = []
    for d in mods:
        files.extend(sorted((p for p in d.rglob("*") if p.is_file() and p.suffix.lower() in SUFFIXES
                             and "romfs" not in [x.lower() for x in p.relative_to(d).parts]),
                            key=lambda p: p.relative_to(d).as_posix().lower()))
    return files


def fingerprint(module_id, files):
    """A hash of what these files would write into this module ('' when nothing applies)."""
    pats = patches_for(module_id, files)
    if not pats:
        return ""
    h = hashlib.sha256()
    for _, records in pats:
        for off, data in records:
            h.update(off.to_bytes(8, "little", signed=True) + len(data).to_bytes(4, "little") + data)
    return h.hexdigest()
