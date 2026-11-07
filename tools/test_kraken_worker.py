#!/usr/bin/env python3
"""Exercise the decoder worker protocol with synthetic, non-game blocks."""

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

import bayonetta3_shaders


def run(worker, data):
    try:
        return subprocess.run([str(worker)], input=data, capture_output=True, timeout=15)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(f"worker could not complete: {exc}") from exc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", type=Path, required=True, help="path to the compiled worker executable")
    args = parser.parse_args()
    if not args.worker.is_file():
        parser.error("--worker must point to a compiled worker")
    worker = args.worker.resolve()

    # These two block encodings come from the public ooz format implementation;
    # the data and expected output below are synthetic and contain no game data.
    raw = b"synthetic\x00\x0a\x0d\x0a\x1a\xff"
    blocks = ((b"\x4c\x06" + raw, raw), (b"\x0c\x06\x07\xff\xff\x0a", b"\x0a" * 100))
    request = b"".join(struct.pack("<QQ", len(block), len(expected)) + block for block, expected in blocks)
    expected_response = b"".join(struct.pack("<q", len(expected)) + expected for _, expected in blocks)
    checks = ((request, 0, expected_response),
              (b"X", 2, b""),
              (struct.pack("<QQ", 512 * 1024 * 1024 + 1, 1), 3, b""),
              (struct.pack("<QQ", 4, 2) + b"xx", 4, b""))
    try:
        for data, expected_code, expected_out in checks:
            result = run(worker, data)
            if result.returncode != expected_code or result.stdout != expected_out:
                raise RuntimeError(f"unexpected response: exit {result.returncode}, stdout {result.stdout.hex()}")
        # Exercise the same persistent client used by shader extraction, with
        # its working directory confined to this disposable temporary folder.
        with tempfile.TemporaryDirectory(prefix="b3-worker-test-") as tmp:
            bayonetta3_shaders.TMP = Path(tmp)
            client = bayonetta3_shaders.Decoder(worker)
            try:
                for block, expected in blocks:
                    if client.decode(block, len(expected)) != expected:
                        raise RuntimeError("shader client received an incorrect decoded block")
            finally:
                client.close()
    except (OSError, RuntimeError, ValueError) as exc:
        parser.exit(1, f"{exc}\n")
    print("Worker and shader client passed synthetic raw/fill, repeated request and malformed-frame checks.")


if __name__ == "__main__":
    main()
