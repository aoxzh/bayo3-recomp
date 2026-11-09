#!/usr/bin/env python3
"""Compile and run focused tests of the production scene-clock helper."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
compiler = os.environ.get("CXX")
command = shlex.split(compiler) if compiler else [shutil.which("clang++") or shutil.which("g++") or "c++"]

with tempfile.TemporaryDirectory(prefix="b3-scene-clock-") as temp:
    exe = Path(temp) / ("b3_scene_clock_test.exe" if os.name == "nt" else "b3_scene_clock_test")
    subprocess.run(
        command + [
            "-std=c++20", "-O1", "-pthread", "-I", str(ROOT / "runtime/include"),
            str(ROOT / "runtime/src/b3_scene_clock.cpp"), str(ROOT / "runtime/src/b3_fps_counters.cpp"),
            str(ROOT / "tests/b3_scene_clock_test.cpp"), "-o", str(exe),
        ],
        check=True,
    )
    for name, mode, value in [
        ("normal (unset)", "normal", None),
        ("disabled (0)", "disabled", "0"),
        ("enabled (empty)", "enabled", ""),
        ("enabled (1)", "enabled", "1"),
        ("enabled (0extra)", "enabled", "0extra"),
    ]:
        env = os.environ.copy()
        if value is None:
            env.pop("NX_SCENE_TIME", None)
        else:
            env["NX_SCENE_TIME"] = value
        subprocess.run([str(exe), mode], env=env, check=True)
        print("PASS", name, flush=True)
