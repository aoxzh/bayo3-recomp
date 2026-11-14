#!/usr/bin/env python3
"""The recompiler's jump-table analysis: a flags-free branch between the cmp and its b.hi.

Bayonetta 3's main+3e6980 guards a 270-entry table with
    cmp w22, #0x10d / cbz w10, ... / b.hi default
and the scan for the guarding branch stopped at the cbz (it reads a register, not NZCV), so the
table fell back to the 256-entry limit: index 0x102 became a call into the middle of the function
(issue #2). No game data is needed here; the instructions and the table are synthesised.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from recomp.analysis import Analysis, Function, _bound, _jump_table

COUNT = 0x10D + 1   # cmp w22, #0x10d with b.hi admits the index 0..0x10d: 270 entries
TABLE = 0x3000      # .rodata, one byte per entry
ANCHOR = 0x2000     # adr x10, #0x2000; a target is ANCHOR + entry << 2
F_START, F_END = 0x1000, 0x2500


def table_byte(k):
    """The real table is 270 entries over few targets; entries 256..269 reach 14 targets no earlier
    entry does, so reading only 256 entries (the unbounded fallback) loses them."""
    return k % 34 if k < 256 else 34 + (k - 256)


class Image:
    """The parts of nso.Module that the analysis reads."""

    def __init__(self):
        self.text = (F_START, F_END - F_START)
        self.ro = (TABLE, 0x200)
        self.data = (0x4000, 0x100)
        self.image = bytearray(0x5000)
        for k in range(COUNT):
            self.image[TABLE + k] = table_byte(k)


def ins(text):
    """'cbz w10, #0x2400' -> ("cbz", "w10, #0x2400")."""
    parts = text.split(" ", 1)
    return (parts[0], parts[1] if len(parts) > 1 else "")


def analysis(guard="cbz w10, #0x2400", *, cmp_and_bound=None):
    insns = {
        0x1000: ("cmp", "w22, #0x10d"),
        0x1004: ins(guard),
        0x1008: ("b.hi", "#0x2400"),
        0x100C: ("adrp", "x9, #0x3000"),
        0x1010: ("adr", "x10, #0x2000"),
        0x1014: ("mov", "w8, w22"),
        0x1018: ("ldrb", "w11, [x9, x8]"),
        0x101C: ("add", "x10, x10, x11, lsl #2"),
        0x1020: ("br", "x10"),
    }
    if cmp_and_bound:
        insns[0x1000] = cmp_and_bound[0]
        insns[0x1008] = cmp_and_bound[1]
    # An instruction at every target the table can reach, so each one is a label of this function.
    for k in range(COUNT):
        insns[ANCHOR + 4 * table_byte(k)] = ("nop", "")
    return Analysis(Image(), insns)


def bound(*args, **kwargs):
    an = analysis(*args, **kwargs)
    return _bound(an, Function(F_START, F_END), 0x1018, 8)


def targets(*args, **kwargs):
    an = analysis(*args, **kwargs)
    return _jump_table(an, Function(F_START, F_END), 0x1020)


assert bound() == COUNT, bound()
assert bound("cbnz w10, #0x2400") == COUNT
assert bound("tbz w10, #1, #0x2400") == COUNT
assert bound("nop") == COUNT
assert bound(cmp_and_bound=(("cmp", "w22, #0x10d"), ("b.hs", "#0x2400"))) == COUNT - 1

# The same table: all 270 entries are read, including the targets only indices 256..269 reach.
tg = targets()
assert tg is not None, "the table was not resolved"
assert len(tg) == 48, len(tg)
assert ANCHOR + 4 * 34 in tg, "a target from past entry 256 is missing"
assert ANCHOR in tg

# A branch that reads the flags, or anything that writes them, still ends the scan: the cmp describes that
# use, not the table.
assert bound("b.eq #0x2400") is None
assert bound("b.lt #0x2400") is None
assert bound("adds w10, w10, #1") is None
# Without a bound the fallback stops at 256 entries, which is what lost the last 14 targets.
assert len(targets("b.eq #0x2400")) == 34, len(targets("b.eq #0x2400"))

# A cmp of another register is not this table's bound.
assert bound(cmp_and_bound=(("cmp", "w23, #0x10d"), ("b.hi", "#0x2400"))) is None
assert bound(cmp_and_bound=(("cmp", "w22, #0x10"), ("b.hi", "#0x2400"))) == 0x11

# Bayonetta 3's main+c7d40: the table register is written twice in the function, and the write that is last in
# address order (another switch's table) is not the one on this switch's path. Both tables are in .rodata and every
# entry of the wrong one is an instruction, so the wrong one used to be taken alone.
def two_tables():
    img = Image()
    right, wrong = TABLE + 0x100, TABLE + 0x180
    for k in range(8):
        img.image[right + k] = 40 + k   # the targets of the real table: ANCHOR + 4 * (40..47)
        img.image[wrong + k] = k        # another switch's: ANCHOR + 4 * (0..7)
    insns = {
        0x1000: ("adrp", "x26, #0x3000"),
        0x1004: ("add", "x26, x26, #0x100"),      # this switch's table, set early
        0x1008: ("b", "#0x1100"),
        0x1080: ("adrp", "x26, #0x3000"),
        0x1084: ("add", "x26, x26, #0x180"),      # the other switch's, later in address order
        0x1088: ("b", "#0x2400"),                  # ...on a path that does not reach this switch
        0x1100: ("cmp", "w22, #7"),
        0x1104: ("b.hi", "#0x2400"),
        0x1108: ("adr", "x9, #0x2000"),
        0x110C: ("mov", "w8, w22"),
        0x1110: ("ldrb", "w10, [x26, x8]"),
        0x1114: ("add", "x9, x9, x10, lsl #2"),
        0x1118: ("br", "x9"),
    }
    for v in range(48):
        insns[ANCHOR + 4 * v] = ("nop", "")
    return _jump_table(Analysis(img, insns), Function(F_START, F_END), 0x1118)


tg = two_tables()
assert tg is not None
assert all(ANCHOR + 4 * v in tg for v in range(40, 48)), "the real table's targets are missing"
assert not any(ANCHOR + 4 * v in tg for v in range(8)), "the other switch's table does not reach this load"

print("recomp/analysis.py: a flags-free branch between cmp and b.hi keeps the 270-entry bound; "
      "a table register written twice takes the definition that reaches the load")
