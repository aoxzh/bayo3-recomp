"""Version-gated glue for the Bayonetta 3 1.2.0 runtime adapters.

The adapter implementations and their upstream notices live under runtime/ and
in docs/WITCH-TIME-NOTICES.md. Other modules and NSO revisions emit unchanged.
"""

MAIN_SHA256 = '499983f6adabc071488be7fe75f547c079c8c8dac95c3573e78a4277d7641800'
SCENE_CLOCK = 0x00C1F600
FASTPATHS = {
    0x007E1FE4: 'try_quad_walk',
    0x00B382C0: 'try_morton_split',
}
ENTRIES = (*FASTPATHS, SCENE_CLOCK)


def signature(start):
    return f'void main_{start:08X}(nx::CPU& NX_RESTRICT c) {{'


def apply(text, name, start, source_sha256):
    """Wrap one emitted function, keeping its original body as the fallback."""
    if name != 'main' or source_sha256 != MAIN_SHA256 or start not in ENTRIES:
        return text
    opening = signature(start)
    if not text.startswith(opening + '\n') or not text.rstrip().endswith('\n}'):
        raise ValueError(f'Unexpected generated function shape at {start:#x}')
    if start in FASTPATHS:
        statement = f'    if (nx::b3::{FASTPATHS[start]}(c)) return;\n'
        # Also makes a targeted refresh safe to repeat.
        if text.startswith(opening + '\n' + statement):
            return text
        return text.replace(opening + '\n', opening + '\n' + statement, 1)
    original = f'main_{start:08X}_original'
    renamed = text.replace(opening, f'static void {original}(nx::CPU& NX_RESTRICT c) {{', 1)
    wrapper = '\n'.join([
        opening,
        '    const uint64_t clock_ = c.x[0];',
        '    const bool fixed_ = uint32_t(c.x[1]) == 0;',
        f'    {original}(c);',
        '    nx::b3::scene_clock_after(clock_, fixed_);',
        '}',
    ])
    return renamed + '\n\n' + wrapper


def includes(body):
    """Only affected translation units need the adapter headers."""
    headers = []
    if 'nx::b3::try_quad_walk(' in body or 'nx::b3::try_morton_split(' in body:
        headers.append('#include <nx/b3_guest_fastpaths.h>\n')
    if 'nx::b3::scene_clock_after(' in body:
        headers.append('#include <nx/b3_scene_clock.h>\n')
    return ''.join(headers)
