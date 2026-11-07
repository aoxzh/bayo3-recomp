#!/usr/bin/env python3
"""Compare the game-specific fast paths with the two emitted guest bodies.

Only the two functions and their bit-reversal callee are extracted; external
rendering functions are instrumented stubs and guest globals are synthetic.
No game binary is linked or run. Set B3_GEN_MAIN if the generated tree lives
elsewhere.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GEN = Path(os.environ.get("B3_GEN_MAIN", ROOT / "private/bayonetta3/gen/main"))


def extract(path, name):
    source = path.read_text()
    marker = f"void {name}(nx::CPU& NX_RESTRICT c) {{"
    if source.count(marker) != 1:
        raise RuntimeError(f"expected one {name} body in {path}")
    start = source.index(marker)
    end = source.index("\n}", start) + 2
    body = source[start:end]
    # The lead's guards may already be inserted in the emitted tree.  Compare
    # against the original body, including recursively invoked original nodes.
    body = re.sub(r"^\s*if\s*\(\s*nx::b3::try_(?:quad_walk|morton_split)\(c\)\s*\)\s*return\s*;[^\n]*\n", "\n", body, flags=re.M)
    return body


def main():
    reverse = extract(GEN / "code_235.cpp", "main_00B382B0")
    split = extract(GEN / "code_235.cpp", "main_00B382C0")
    quad = extract(GEN / "code_163.cpp", "main_007E1FE4")
    called = set(re.findall(r"\b(main_[0-9A-F]{8})\(c\)", quad + split))
    originals = {"main_007E1FE4", "main_00B382C0", "main_00B382B0"}
    stubs = sorted(called - originals)
    source = '#include <nx/cpu.h>\n#include <nx/neon_tbl.h>\n'
    source += 'extern uint64_t test_guest_base;\n#define NXB (test_guest_base)\n'
    source += ''.join(f'void {name}(nx::CPU&);\n' for name in sorted(called | originals))
    source += 'extern int test_draw_calls;\n'
    source += ''.join(f'void {name}(nx::CPU& c) {{ ++test_draw_calls; c.x[0] = 0; }}\n' for name in stubs)
    source += reverse + '\n' + split + '\n' + quad + '\n'

    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise RuntimeError("CXX, clang++ or g++ is required")
    with tempfile.TemporaryDirectory(prefix="b3-guest-fastpaths-") as temp:
        temp = Path(temp)
        generated = temp / "selected_original_bodies.cpp"
        generated.write_text(source)
        binary = temp / "guest-fastpaths-test"
        command = [compiler, "-std=c++20", "-O1", "-I" + str(ROOT / "runtime/include"),
                   str(ROOT / "tests/b3_guest_fastpaths_test.cpp"),
                   str(ROOT / "runtime/src/b3_guest_fastpaths.cpp"), str(generated), "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
