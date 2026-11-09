#!/usr/bin/env python3
"""Check exact-version generation, preserved fallback bodies and post-call ABI."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from recomp import b3_hooks
from tools.refresh_b3_hooks import refresh


def body(entry, statements):
    return b3_hooks.signature(entry) + '\n' + statements + '\n}'


clock = body(b3_hooks.SCENE_CLOCK, '''    if (c.x[4]) throw 7;
    c.x[0] = 99;
    c.x[1] = 1;
    ++c.x[3];
    return;''')
quad = body(0x007E1FE4, '    ++c.x[10];\n    return;')
morton = body(0x00B382C0, '    ++c.x[11];\n    return;')
original = '#include "funcs.h"\n// LOCAL EDIT MUST SURVIVE\n' + '\n\n'.join([quad, morton, clock]) + '\n'
updated, found = refresh(original, b3_hooks.MAIN_SHA256)
assert found == {b3_hooks.SCENE_CLOCK, 0x007E1FE4, 0x00B382C0}
assert '// LOCAL EDIT MUST SURVIVE\n' in updated
assert '    ++c.x[10];\n    return;' in updated
assert '    ++c.x[11];\n    return;' in updated
assert clock.split('\n', 1)[1] in updated
assert refresh(updated, b3_hooks.MAIN_SHA256) == (updated, found)
assert updated.count('#include <nx/b3_scene_clock.h>') == 1
assert updated.count('#include <nx/b3_guest_fastpaths.h>') == 1
for name, digest, entry in [('sdk', b3_hooks.MAIN_SHA256, b3_hooks.SCENE_CLOCK),
                            ('main', '0' * 64, b3_hooks.SCENE_CLOCK),
                            ('main', b3_hooks.MAIN_SHA256, 0x1234)]:
    assert b3_hooks.apply(clock, name, entry, digest) == clock
assert b3_hooks.includes('void unchanged() {}') == ''

# Frame counters: the op at its address is rewritten, once; anything else at that address is refused.
hit = 0x004355BC
filler = '\n'.join('    c.x[0] = 0; // nop' for _ in range((0x0043574C - hit) // 4))
subs = '    c.x[8] = uint32_t(nx::subs32(c, uint32_t(c.x[8]), uint32_t(0x1ull))); // subs w8, w8, #1'
counter = body(hit, filler + '\nL_43574C:;\n' + subs + '\n    return; // ret')
patched = b3_hooks.apply(counter, 'main', hit, b3_hooks.MAIN_SHA256)
assert patched.count('nx::b3::fps_ci(0x435754ull, c.x[19] + 0x2a78ull, uint32_t(c.x[8]), 1, -1)') == 1
assert b3_hooks.apply(patched, 'main', hit, b3_hooks.MAIN_SHA256) == patched
assert b3_hooks.includes(patched) == '#include <nx/b3_fps_counters.h>\n'
try:
    b3_hooks.apply(counter.replace('// subs w8, w8, #1', '// subs w8, w9, #1'), 'main', hit, b3_hooks.MAIN_SHA256)
except ValueError:
    pass
else:
    raise AssertionError('a different instruction at a counter op was accepted')
fadd = '    { float r_ = float((c.v[0].f32[0]) + (c.v[1].f32[0])); c.v[0].f32[0] = r_; } // fadd s0, s0, s1'
assert '+ (c.v[1].f32[0] * float(nx::b3::fps_cf(0xc8a7ccull, c.x[19] + 0x940ull, c.v[0].f32[0], -1)))' in b3_hooks._counter_op(fadd, 'fadd s0, s0, s1', 0xC8A7CC, 'f1', 19, 0x940)
assert '+ (c.v[0].f32[0] * nx::b3::fps_fs(0x77397cull, c.x[19] + 0x68ull))' in b3_hooks._counter_op(
    fadd.replace('(c.v[0].f32[0]) + (c.v[1]', '(c.v[1].f32[0]) + (c.v[0]'), 'fadd s0, s1, s0', 0x77397C, 'fs', 19, 0x68)
assert 'c.x[24] - 0x80ull' == b3_hooks._field(24, -0x80)
for malformed in [original + '\n' + quad, original.replace('return;\n}', 'return;\nBROKEN}', 1)]:
    try:
        refresh(malformed, b3_hooks.MAIN_SHA256)
    except ValueError:
        pass
    else:
        raise AssertionError('malformed function was accepted')
try:
    refresh(original, 'f' * 64)
except ValueError:
    pass
else:
    raise AssertionError('wrong source version was accepted')

compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
assert compiler, 'a C++ compiler is required'
prelude = r'''
#include <cassert>
#include <cstdint>
#define NX_RESTRICT
namespace nx { struct CPU { uint64_t x[32]{}; }; }
static bool use_fast = false;
static unsigned after_calls = 0;
static uint64_t captured_clock = 0;
static bool captured_fixed = false;
namespace nx::b3 {
bool try_quad_walk(CPU& c) { if (!use_fast) return false; c.x[0] = 41; return true; }
bool try_morton_split(CPU& c) { if (!use_fast) return false; c.x[0] = 42; return true; }
void scene_clock_after(uint64_t clock, bool fixed) {
    ++after_calls; captured_clock = clock; captured_fixed = fixed;
}
}
'''
checks = r'''
int main() {
    nx::CPU c;
    c.x[0] = 0x1234; c.x[1] = uint64_t(1) << 32;
    main_00C1F600(c);
    assert(after_calls == 1 && captured_clock == 0x1234 && captured_fixed);
    assert(c.x[0] == 99 && c.x[1] == 1 && c.x[3] == 1);
    c.x[0] = 0x5678; c.x[1] = 2;
    main_00C1F600(c);
    assert(after_calls == 2 && captured_clock == 0x5678 && !captured_fixed);
    c.x[4] = 1;
    try { main_00C1F600(c); assert(false); } catch (int value) { assert(value == 7); }
    assert(after_calls == 2);
    main_007E1FE4(c); main_00B382C0(c);
    assert(c.x[10] == 1 && c.x[11] == 1);
    use_fast = true;
    main_007E1FE4(c); assert(c.x[0] == 41 && c.x[10] == 1);
    main_00B382C0(c); assert(c.x[0] == 42 && c.x[11] == 1);
}
'''
with tempfile.TemporaryDirectory(prefix='b3-generation-hooks-') as tmp:
    temp = Path(tmp)
    generated = temp / 'generated'
    (generated / 'main').mkdir(parents=True)
    cpp = generated / 'main' / 'code_000.cpp'
    cpp.write_text(original)
    wrong_nso = temp / 'wrong.nso'
    wrong_nso.write_bytes(b'not-the-supported-game')
    backup = temp / 'backup'
    result = subprocess.run([sys.executable, str(ROOT / 'tools/refresh_b3_hooks.py'), str(generated),
                             '--nso', str(wrong_nso), '--apply', '--backup', str(backup)],
                            capture_output=True, text=True)
    assert result.returncode == 2 and 'NSO SHA256' in result.stderr, result
    assert cpp.read_text() == original and not backup.exists()
    compiled = prelude + '\n'.join(line for line in updated.splitlines()
                                   if not line.startswith('#include')) + checks
    source = temp / 'test.cpp'
    source.write_text(compiled)
    binary = temp / 'test'
    subprocess.run([compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print('PASS B3 hook version gate, idempotent refresh, local edits, fallback and post-call ABI')
