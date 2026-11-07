#!/usr/bin/env python3
"""Exercise per-present draw accounting without the game or Vulkan."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
assert compiler, "CXX, clang++ or g++ required"
with tempfile.TemporaryDirectory(prefix="nvn-draw-frame-stats-") as tmp:
    binary = str(Path(tmp) / "test")
    subprocess.run([compiler, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
                    str(root / "tests/nvn_draw_frame_stats_test.cpp"), "-o", binary], check=True)
    subprocess.run([binary], check=True)
