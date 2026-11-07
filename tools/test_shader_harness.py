#!/usr/bin/env python3
"""Translate and validate a synthetic compute shader without game data."""

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


def run(command, label):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(f"{label} could not complete: {exc}") from exc
    if result.returncode:
        details = (result.stderr + "\n" + result.stdout).strip()
        raise RuntimeError(f"{label} failed (exit {result.returncode}):\n{details}")
    return result.stdout.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", type=Path, required=True, help="path to the built Ryujinx.Graphics.Shader.dll")
    parser.add_argument("--dotnet", default="dotnet", help="dotnet executable (default: dotnet on PATH)")
    parser.add_argument("--spirv-val", default="spirv-val", help="spirv-val executable (default: spirv-val on PATH)")
    args = parser.parse_args()
    if not args.harness.is_file():
        parser.error("--harness must point to the built harness DLL")

    # NVN-style compute header, one scheduling word and an unconditional EXIT.
    # This is an artificial no-op program, not code extracted from a game.
    code = bytearray(0x200)
    struct.pack_into("<I", code, 0, 0x12345679)
    struct.pack_into("<Q", code, 0x108, 0xE30000000007000F)
    control = bytearray(0x880)
    for offset, value in ((0x6F8, 16), (0x704, len(code)), (0x708, 0x100),
                          (0x748, 1), (0x74C, 1), (0x750, 1)):
        struct.pack_into("<I", control, offset, value)

    try:
        with tempfile.TemporaryDirectory(prefix="b3-synthetic-shader-") as tmp:
            root = Path(tmp)
            code_file, control_file, output_file = (root / name for name in ("code.bin", "control.bin", "output.spv"))
            code_file.write_bytes(code)
            control_file.write_bytes(control)
            run([args.dotnet, str(args.harness.resolve()), str(code_file), str(control_file),
                 "-o", str(output_file)], "Shader translation")
            if not output_file.is_file():
                raise RuntimeError("Shader translation did not produce an output file")
            run([args.spirv_val, "--target-env", "vulkan1.2", str(output_file)], "SPIR-V validation")
    except RuntimeError as exc:
        parser.exit(1, f"{exc}\n")
    print("Synthetic compute shader translated and passed Vulkan 1.2 SPIR-V validation.")


if __name__ == "__main__":
    main()
