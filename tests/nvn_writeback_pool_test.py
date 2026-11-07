#!/usr/bin/env python3
"""Test production writeback job claiming and merge without a game or GPU."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
options = set(sys.argv[1:])
assert options <= {"--tsan"} and len(options) == len(sys.argv[1:]), __doc__
compiler = shlex.split(os.environ["CXX"]) if os.environ.get("CXX") else [shutil.which("clang++") or shutil.which("g++")]
assert compiler[0], "C++ compiler required"
vulkan = Path(os.environ.get("VULKAN_HEADERS", root / "private/toolchains/Vulkan-Headers-1.4.357/include"))
assert (vulkan / "vulkan/vulkan.h").is_file(), f"Vulkan headers not found: {vulkan}"
flags = ["-std=c++20", "-O1", "-ffunction-sections", "-fdata-sections", "-pthread", "-Wl,--gc-sections",
         "-I" + str(root / "runtime/include"), "-I" + str(root / "runtime/src"), "-I" + str(vulkan)]
if "--tsan" in options:
    flags += ["-fsanitize=thread", "-fno-omit-frame-pointer"]
env = {key: value for key, value in os.environ.items() if not key.startswith("NX_")}
with tempfile.TemporaryDirectory(prefix="bayo3-nvn-writeback-pool-") as temp:
    binary = str(Path(temp) / "test")
    subprocess.run([*compiler, *flags, str(root / "tests/nvn_writeback_pool_test.cpp"), "-o", binary], check=True)
    for helpers in (0, 1, 3, 6, 12):
        env["NX_COPY_THREADS"] = str(helpers)
        subprocess.run([binary], env=env, check=True, timeout=60)
