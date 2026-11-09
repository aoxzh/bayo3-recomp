"""Version-gated glue for the Bayonetta 3 1.2.0 runtime adapters.

The adapter implementations and their upstream notices live under runtime/ and
in docs/WITCH-TIME-NOTICES.md. Other modules and NSO revisions emit unchanged.
"""

MAIN_SHA256 = '499983f6adabc071488be7fe75f547c079c8c8dac95c3573e78a4277d7641800'
SCENE_CLOCK = 0x00C1F600
MODEL_LOD = 0x00BD563C
# Entries wrapped to call a runtime function after the original: (what the wrapper keeps of the entry registers, the
# call after the original, the header it needs). refresh_b3_hooks.py checks an existing wrapper against this text.
POST_HOOKS = {
    SCENE_CLOCK: (('const uint64_t clock_ = c.x[0];', 'const bool fixed_ = uint32_t(c.x[1]) == 0;'),
                  'nx::b3::scene_clock_after(clock_, fixed_);', 'nx/b3_scene_clock.h'),
    MODEL_LOD: (('const uint64_t mesh_ = c.x[0];', 'const uint64_t out_ = c.x[1];',
                 'const int32_t limit_ = int32_t(uint32_t(c.x[2]));'),
                'nx::b3::model_lod_after(mesh_, out_, limit_);', 'nx/b3_model_lod.h'),
}
FASTPATHS = {
    0x007E1FE4: 'try_quad_walk',
    0x00B382C0: 'try_morton_split',
}

# Frame counters that count logic steps, made for 60 per second (runtime/src/b3_fps_counters.cpp turns their +/-1
# into the 60 Hz frames due when the display runs above 60). The sites come from Witch Time's
# tools/wtbuild/fps_counters.txt at 7e7af36250b6845eb8a5ebd20744a9858c8e9d28 (store addresses, what each counts);
# the op feeding each store was read again in this tree's generated code. Fields:
# (function, op address, expected disassembly at the op, store address that names the counter, kind, field base
# register, field offset). Kinds: 'dec' / 'inc' an integer -/+ #1 (the op's immediate becomes fps_ci), 'f1' a float
# +(-1.0) (the -1.0 operand is scaled by fps_cf), 'fs' a float + a per-frame amount (scaled by fps_fs).
FPS_SITES = (
    # first batch: per-frame float accumulations and countdowns
    (0x008132F8, 0x0081425C, 'fadd s0, s0, s1', 0x00814268, 'fs', 19, 0xC14),
    (0x00627290, 0x00627D10, 'fadd s0, s0, s1', 0x00627D18, 'fs', 19, 0x654),
    (0x00627290, 0x006274AC, 'fadd s0, s0, s1', 0x006274B8, 'fs', 19, 0x3F8),
    (0x007738C0, 0x00773974, 'fadd s0, s1, s0', 0x0077397C, 'fs', 19, 0x68),
    (0x00083744, 0x00084320, 'fadd s0, s0, s1', 0x00084328, 'fs', 21, 0xC60),
    (0x00C8A754, 0x00C8A7C4, 'fadd s0, s0, s1', 0x00C8A7CC, 'f1', 19, 0x940),
    (0x0064CEA8, 0x0064CFF0, 'fadd s3, s3, s4', 0x0064CFF8, 'f1', 19, 0x998),
    # the player's input update: the double-tap dodge window
    (0x00559EBC, 0x0055A414, 'subs w8, w8, #1', 0x0055A41C, 'dec', 19, 0x3BC),
    # the stick-motion tracker 0x55A7EC: 2 x 8 octant countdowns, the push windows and their timers
    (0x0055A7EC, 0x0055A808, 'subs w9, w8, #1', 0x0055A814, 'dec', 19, 0x178),
    (0x0055A7EC, 0x0055A81C, 'subs w9, w10, #1', 0x0055A828, 'dec', 19, 0x198),
    (0x0055A7EC, 0x0055A838, 'sub w9, w9, #1', 0x0055A83C, 'dec', 19, 0x17C),
    (0x0055A7EC, 0x0055A844, 'subs w11, w12, #1', 0x0055A850, 'dec', 19, 0x19C),
    (0x0055A7EC, 0x0055A858, 'subs w13, w11, #1', 0x0055A864, 'dec', 19, 0x180),
    (0x0055A7EC, 0x0055A86C, 'subs w14, w13, #1', 0x0055A878, 'dec', 19, 0x1A0),
    (0x0055A7EC, 0x0055A880, 'subs w15, w14, #1', 0x0055A88C, 'dec', 19, 0x184),
    (0x0055A7EC, 0x0055A894, 'subs w16, w15, #1', 0x0055A8A0, 'dec', 19, 0x1A4),
    (0x0055A7EC, 0x0055A8A8, 'subs w16, w17, #1', 0x0055A8B4, 'dec', 19, 0x188),
    (0x0055A7EC, 0x0055A8BC, 'subs w16, w18, #1', 0x0055A8C8, 'dec', 19, 0x1A8),
    (0x0055A7EC, 0x0055A8D0, 'subs w16, w1, #1', 0x0055A8DC, 'dec', 19, 0x18C),
    (0x0055A7EC, 0x0055A8E4, 'subs w16, w2, #1', 0x0055A8F0, 'dec', 19, 0x1AC),
    (0x0055A7EC, 0x0055A8F8, 'subs w16, w3, #1', 0x0055A904, 'dec', 19, 0x190),
    (0x0055A7EC, 0x0055A90C, 'subs w16, w4, #1', 0x0055A918, 'dec', 19, 0x1B0),
    (0x0055A7EC, 0x0055A920, 'subs w16, w5, #1', 0x0055A92C, 'dec', 19, 0x194),
    (0x0055A7EC, 0x0055A934, 'subs w16, w6, #1', 0x0055A940, 'dec', 19, 0x1B4),
    (0x0055A7EC, 0x0055AE90, 'sub w8, w8, #1', 0x0055AE94, 'dec', 19, 0x1E0),
    (0x0055A7EC, 0x0055AC44, 'subs w8, w8, #1', 0x0055AC4C, 'dec', 19, 0x1EC),
    (0x0055A7EC, 0x0055AE38, 'subs w8, w8, #1', 0x0055AC4C, 'dec', 19, 0x1EC),  # its second path (b.ge)
    (0x0055A7EC, 0x0055AC54, 'subs w8, w8, #1', 0x0055AC5C, 'dec', 19, 0x1E8),
    (0x0055A7EC, 0x0055AF04, 'sub w8, w8, #1', 0x0055AF08, 'dec', 19, 0x204),
    (0x0055A7EC, 0x0055AC64, 'subs w8, w8, #1', 0x0055AC6C, 'dec', 19, 0x210),
    (0x0055A7EC, 0x0055AC74, 'subs w8, w8, #1', 0x0055AC7C, 'dec', 19, 0x20C),
    (0x0055A7EC, 0x0055AC84, 'subs w8, w8, #1', 0x0055AC8C, 'dec', 19, 0x1F4),
    (0x0055A7EC, 0x0055ACBC, 'subs w8, w8, #1', 0x0055ACC4, 'dec', 19, 0x230),
    (0x0055A7EC, 0x0055AC94, 'subs w8, w8, #1', 0x0055AC9C, 'dec', 19, 0x1F8),
    (0x0055A7EC, 0x0055ACA4, 'subs w8, w8, #1', 0x0055ACAC, 'dec', 19, 0x400),
    (0x0055A7EC, 0x0055AFA4, 'fadd s0, s0, s1', 0x0055AFA8, 'f1', 19, 0x414),
    (0x0055A7EC, 0x0055B014, 'subs w8, w8, #1', 0x0055B01C, 'dec', 19, 0x418),
    (0x0055A7EC, 0x0055B03C, 'subs w8, w8, #1', 0x0055B01C, 'dec', 19, 0x418),  # its second path (b.ge)
    # the command decoder 0x55B058: press buffers, second-press windows, the demon tap/hold count, a lockout
    (0x0055B058, 0x0055B2C8, 'subs w9, w8, #1', 0x0055B2D4, 'dec', 24, -0x80),
    (0x0055B058, 0x0055B2DC, 'subs w9, w9, #1', 0x0055B2E4, 'dec', 24, 0),
    (0x0055B058, 0x0055B938, 'add w9, w9, #1', 0x0055B93C, 'inc', 8, 0),
    (0x0055B058, 0x0055B180, 'subs w8, w8, #1', 0x0055B18C, 'dec', 19, 0x40C),
    # Witch Time: the window after a hit lands, and the manager 0x7A231C's slow-down slots
    (0x004355BC, 0x0043574C, 'subs w8, w8, #1', 0x00435754, 'dec', 19, 0x2A78),
    (0x007A231C, 0x007A233C, 'subs w8, w8, #1', 0x007A2344, 'dec', 19, 0x38),
    (0x007A231C, 0x007A238C, 'subs w8, w8, #1', 0x007A2394, 'dec', 19, 0x44),
    (0x007A231C, 0x007A2444, 'subs w8, w8, #1', 0x007A2394, 'dec', 19, 0x44),  # its second path (b.ge)
    (0x007A231C, 0x007A25A4, 'subs w8, w8, #1', 0x007A25AC, 'dec', 19, 0x98),
    (0x007A231C, 0x007A25F4, 'subs w8, w8, #1', 0x007A25AC, 'dec', 19, 0x98),  # its second path (b.ge)
    (0x007A231C, 0x007A25B4, 'subs w8, w8, #1', 0x007A25BC, 'dec', 19, 0x50),
    (0x007A231C, 0x007A2608, 'subs w8, w8, #1', 0x007A25BC, 'dec', 19, 0x50),  # its second path (b.ge)
    (0x007A231C, 0x007A2594, 'subs w8, w8, #1', 0x007A259C, 'dec', 19, 0x90),
    (0x007A231C, 0x007A2584, 'subs w8, w8, #1', 0x007A258C, 'dec', 19, 0x9C),
)
FPS_FUNCTIONS = frozenset(site[0] for site in FPS_SITES)
ENTRIES = (*FASTPATHS, *POST_HOOKS, *sorted(FPS_FUNCTIONS))


def signature(start):
    return f'void main_{start:08X}(nx::CPU& NX_RESTRICT c) {{'


def _field(base, off):
    if off > 0:
        return f'c.x[{base}] + {off:#x}ull'
    if off < 0:
        return f'c.x[{base}] - {-off:#x}ull'
    return f'c.x[{base}]'


def _counter_op(line, disasm, store, kind, base, off):
    """The op line with its 1 (or 1.0, or per-frame amount) replaced by the runtime's frames-due call."""
    field = _field(base, off)
    if kind in ('dec', 'inc'):
        src = int(disasm.split(', ')[1][1:])
        old = 'uint32_t(0x1ull)'
        new = (f'nx::b3::fps_ci({store:#x}ull, {field}, uint32_t(c.x[{src}]), 1, '
               f'{-1 if kind == "dec" else 1})')
    else:
        _, a, b = (r.rstrip(',') for r in disasm.split(' ', 1)[1].split(', '))
        val, other = int(a[1:]), int(b[1:])
        old = f'+ (c.v[{other}].f32[0])'
        scale = (f'float(nx::b3::fps_cf({store:#x}ull, {field}, c.v[{val}].f32[0], -1))' if kind == 'f1' else
                 f'nx::b3::fps_fs({store:#x}ull, {field})')
        new = f'+ (c.v[{other}].f32[0] * {scale})'
    if line.count(old) != 1:
        raise ValueError(f'Unexpected op form for the counter at {store:#x}: {line.strip()}')
    return line.replace(old, new, 1)


def _counters(text, start):
    """Patch every FPS_SITES op of one function; repeat-safe (a patched op is left as it is)."""
    lines = text.split('\n')
    at, address = {}, start
    for k, line in enumerate(lines[1:], 1):
        if line.startswith('}'):
            break
        if line.startswith('L_') or not line.startswith('    '):
            continue
        at[address] = k
        address += 4
    for fn, op, disasm, store, kind, base, off in FPS_SITES:
        if fn != start:
            continue
        k = at.get(op)
        if k is None or not lines[k].endswith('// ' + disasm):
            raise ValueError(f'Counter op {op:#x} is not "{disasm}" in main_{start:08X}')
        if 'nx::b3::fps_' not in lines[k]:
            lines[k] = _counter_op(lines[k], disasm, store, kind, base, off)
    return '\n'.join(lines)


def apply(text, name, start, source_sha256):
    """Wrap one emitted function, keeping its original body as the fallback."""
    if name != 'main' or source_sha256 != MAIN_SHA256 or start not in ENTRIES:
        return text
    opening = signature(start)
    if not text.startswith(opening + '\n') or not text.rstrip().endswith('\n}'):
        raise ValueError(f'Unexpected generated function shape at {start:#x}')
    if start in FPS_FUNCTIONS:
        return _counters(text, start)
    if start in FASTPATHS:
        statement = f'    if (nx::b3::{FASTPATHS[start]}(c)) return;\n'
        # Also makes a targeted refresh safe to repeat.
        if text.startswith(opening + '\n' + statement):
            return text
        return text.replace(opening + '\n', opening + '\n' + statement, 1)
    original = f'main_{start:08X}_original'
    renamed = text.replace(opening, f'static void {original}(nx::CPU& NX_RESTRICT c) {{', 1)
    keep, call, _ = POST_HOOKS[start]
    wrapper = '\n'.join([opening, *('    ' + k for k in keep), f'    {original}(c);', '    ' + call, '}'])
    return renamed + '\n\n' + wrapper


def includes(body):
    """Only affected translation units need the adapter headers."""
    headers = []
    if 'nx::b3::try_quad_walk(' in body or 'nx::b3::try_morton_split(' in body:
        headers.append('#include <nx/b3_guest_fastpaths.h>\n')
    for _, call, header in POST_HOOKS.values():
        if call.split('(')[0] + '(' in body:
            headers.append(f'#include <{header}>\n')
    if 'nx::b3::fps_' in body:
        headers.append('#include <nx/b3_fps_counters.h>\n')
    return ''.join(headers)
