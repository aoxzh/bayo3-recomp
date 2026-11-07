#!/usr/bin/env python3
"""Compile isolated ASTC partition differential tests or the synthetic CPU microbenchmark.

Usage: python3 tests/nvn_astc_partition_test.py [--negative-control | --bench | --bench-images | --images-only]
The tests never access game data or need Vulkan. Compiled files live in a temporary directory.
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'runtime/src/nvn/nvn_astc.cpp'
TEST = ROOT / 'tests/nvn_astc_partition_test.cpp'


def compile_test(binary, override=None, extra=()):
    compiler = shlex.split(os.environ.get('CXX', ''))
    if not compiler:
        compiler = [shutil.which('clang++') or shutil.which('g++') or '']
    if not compiler[0]:
        raise SystemExit('CXX, clang++ or g++ required')
    command = compiler + ['-std=c++20', '-O3', '-Wall', '-Wextra', '-Werror', '-pthread',
                          '-I' + str(SOURCE.parent)] + shlex.split(os.environ.get('CXXFLAGS', ''))
    if override is not None:
        command.append('-DASTC_SOURCE="' + str(override) + '"')
    subprocess.run(command + list(extra) + [str(TEST), '-o', str(binary)], check=True)


def negative_control(tmp, original, old, new, name, argument, expected):
    if original.count(old) != 1:
        raise SystemExit(f'negative control {name}: source marker missing or ambiguous')
    mutated = tmp / f'{name}.cpp'
    mutated.write_text(original.replace(old, new, 1))
    binary = tmp / name
    compile_test(binary, mutated)
    result = subprocess.run([str(binary), argument], capture_output=True, text=True,
                            env={**os.environ, 'NX_ASTC_POOL': '1'}, timeout=60)
    if result.returncode == 0 or expected not in result.stderr:
        raise SystemExit(f'negative control {name} was not detected: {result.stdout}\n{result.stderr}')
    print(f'PASS negative control {name}: {result.stderr.splitlines()[0]}', flush=True)


def worker_failure(tmp, original, exception, name):
    marker = 'threads_[k - 1] = std::thread([this, k] { worker(k); });'
    if original.count(marker) != 1:
        raise SystemExit(f'worker failure {name}: source marker missing or ambiguous')
    injected = tmp / f'{name}.cpp'
    injected.write_text(original.replace(marker, f'if (k == 2) throw {exception};\n                ' + marker, 1))
    binary = tmp / name
    compile_test(binary, injected, ['-DEXPECTED_ASTC_WORKERS=2'])
    subprocess.run([str(binary), '--workers-only'], check=True, timeout=60,
                   env={**os.environ, 'NX_ASTC_POOL': '1'})
    print(f'PASS worker creation failure {name}: partial team remains usable', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--negative-control', action='store_true', help='also verify broken block/worker variants and worker creation failures')
    modes.add_argument('--bench', action='store_true', help='run the synthetic block-only timing, not the correctness suite')
    modes.add_argument('--bench-images', action='store_true', help='compare old image scheduling with reusable workers')
    modes.add_argument('--images-only', action='store_true', help='only image and concurrency correctness checks')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='bayo3-astc-partition-') as directory:
        tmp = Path(directory)
        binary = tmp / 'test'
        compile_test(binary)
        mode = ['--bench'] if args.bench else ['--bench-images'] if args.bench_images else ['--images-only'] if args.images_only else []
        subprocess.run([str(binary)] + mode, check=True, timeout=180,
                       env={**os.environ, 'NX_ASTC_POOL': '1'})
        if not (args.bench or args.bench_images):
            subprocess.run([str(binary), '--images-only'], env={**os.environ, 'NX_ASTC_POOL': '0'},
                           check=True, timeout=60)
        if args.negative_control:
            source = SOURCE.read_text()
            negative_control(tmp, source, 'if (a >= b && a >= c && a >= d) return 0;',
                             'if (a > b && a >= c && a >= d) return 0;', 'bad_tiebreak',
                             '--blocks-only', 'FAIL block')
            negative_control(tmp, source,
                             'const int part = partitions > 1 ? partition_of(partition, s, t) : 0;',
                             'const int part = partitions > 1 ? (partition_of(partition, s, t) & 0) : 0;', 'ignored_partition',
                             '--blocks-only', 'FAIL block')
            negative_control(tmp, source, 'bad += bad_[k - 1];', 'bad += 0;', 'lost_invalid_count',
                             '--workers-only', 'image bytes, canary, or invalid count differed')
            negative_control(tmp, source, 'image->rows * k / active,', 'image->rows * k / active + 1,', 'lost_row',
                             '--workers-only', 'image bytes, canary, or invalid count differed')
            worker_failure(tmp, source, 'std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again))',
                           'thread_system_error')
            worker_failure(tmp, source, 'std::bad_alloc()', 'thread_bad_alloc')


if __name__ == '__main__':
    main()
