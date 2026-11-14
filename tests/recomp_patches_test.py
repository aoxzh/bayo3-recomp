#!/usr/bin/env python3
"""recomp/patches.py: IPS, IPS32 and .pchtxt code patches, matched by build ID, at the console loader's offsets.
No game data: the patches and the image are synthesised."""
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from recomp import patches as P

BID = "0123456789ABCDEF0011223344556677" + "0" * 32  # 32 bytes, trailing zeros as most build IDs have

# The header's build ID field.
raw = bytearray(0x100)
raw[0x40:0x60] = bytes.fromhex(BID)
assert P.build_id(raw) == BID

# Build IDs: a file may name the ID by its first 16+ digits, with or without the trailing zeros.
assert P._same_id(BID, BID)
assert P._same_id(BID, BID.rstrip("0"))
assert P._same_id(BID, BID[:16])
assert not P._same_id(BID, BID[:15])          # too short to mean one module
assert not P._same_id(BID, "F" + BID[1:40])


def ips(records, rle=None):
    out = bytearray(b"PATCH")
    for off, data in records:
        out += off.to_bytes(3, "big") + len(data).to_bytes(2, "big") + data
    if rle:
        off, count, byte = rle
        out += off.to_bytes(3, "big") + b"\0\0" + count.to_bytes(2, "big") + bytes([byte])
    return bytes(out + b"EOF")


def ips32(records):
    out = bytearray(b"IPS32")
    for off, data in records:
        out += off.to_bytes(4, "big") + len(data).to_bytes(2, "big") + data
    return bytes(out + b"EEOF")


assert P.parse_ips(ips([(0x104, b"\x1f\x20\x03\xd5")], rle=(0x200, 3, 0xAA))) == [
    (0x104, b"\x1f\x20\x03\xd5"), (0x200, b"\xaa\xaa\xaa")]
assert P.parse_ips(ips32([(0x1000104, b"\xc0\x03\x5f\xd6")])) == [(0x1000104, b"\xc0\x03\x5f\xd6")]
try:
    P.parse_ips(b"NOTIPS")
    raise AssertionError("a file that is not IPS was read")
except ValueError:
    pass

PCH = f"""@nsobid-{BID.rstrip('0')}
# a comment
@flag offset_shift 0x100

// A patch that is off: not written.
@disabled
00000010 DEADBEEF

// Two patches that are on.
@enabled
00000004 1F2003D5
00000020 "1.2.X"
@little-endian
00000030 D503201F
@disabled

@nsobid-FFFFFFFFFFFFFFFF
@enabled
00000000 00000000
@stop
00000040 11111111
"""
parsed = P.parse_pchtxt(PCH)
assert len(parsed) == 2
assert parsed[0][1] == [(0x104, bytes.fromhex("1F2003D5")), (0x120, b"1.2.X\0"), (0x130, bytes.fromhex("1F2003D5"))], parsed[0]
# Lines before any @enabled are not written (as yuzu and Ryujinx read it).
assert P.parse_pchtxt(f"@nsobid-{BID}\n00000000 AABB\n") == [(BID, [])]

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    mods = tmp / "mods"
    (mods / "b-text" / "exefs").mkdir(parents=True)
    (mods / "a-code" / "atmosphere" / "exefs_patches" / "x").mkdir(parents=True)
    (mods / "-off" / "exefs").mkdir(parents=True)
    (mods / "c-romfs" / "romfs").mkdir(parents=True)
    (mods / "b-text" / "exefs" / "patch.pchtxt").write_text(PCH, encoding="utf-8")
    (mods / "a-code" / "atmosphere" / "exefs_patches" / "x" / f"{BID[:40]}.ips").write_bytes(
        ips([(0x108, b"\xc0\x03\x5f\xd6")]))
    (mods / "a-code" / "atmosphere" / "exefs_patches" / "x" / ("F" * 40 + ".ips")).write_bytes(ips([(0x100, b"\0")]))
    (mods / "-off" / "exefs" / f"{BID}.ips").write_bytes(ips([(0x100, b"\x99")]))
    (mods / "c-romfs" / "romfs" / "fake.ips").write_bytes(ips([(0x100, b"\x77")]))  # a game file, not a patch

    files = P.find_files(mods)
    names = [f.name for f in files]
    assert names == [f"{BID[:40]}.ips", "F" * 40 + ".ips", "patch.pchtxt"], names  # name order, off and romfs skipped
    (mods / "order.txt").write_text("b-text\na-code\n", encoding="utf-8")
    assert [f.name for f in P.find_files(mods)][0] == "patch.pchtxt"  # order.txt first

    pats = P.patches_for(BID, P.find_files(mods))
    assert [f.name for f, _ in pats] == ["patch.pchtxt", f"{BID[:40]}.ips"]  # the other build ID's file is skipped
    image = bytearray(0x200)
    assert P.apply(image, pats) == []
    assert image[4:8] == bytes.fromhex("1F2003D5")
    assert image[8:12] == b"\xc0\x03\x5f\xd6"
    assert image[0x20:0x26] == b"1.2.X\0"
    fp = P.fingerprint(BID, P.find_files(mods))
    assert fp and fp == P.fingerprint(BID, P.find_files(mods))
    assert P.fingerprint("AB" * 32, P.find_files(mods)) == ""   # nothing names that module

    # A write past the module is refused and reported, never clipped.
    bad = [(Path("x.ips"), [(0x1FE, b"\1\2\3\4")])]
    small = bytearray(0x200)
    problems = P.apply(small, bad)
    assert problems and small == bytearray(0x200), problems

print("recomp/patches.py: IPS, IPS32 and .pchtxt read at the loader's offsets, matched by build ID, in mod order")
