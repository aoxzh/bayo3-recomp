#!/usr/bin/env python3
"""tools/b3_spec.py: the computer's minimum on real CPU and GPU names (no hardware read)."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import b3_spec  # noqa: E402

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print(f'FAIL: {what}')


def main():
    cpu = lambda name, threads=12: b3_spec.cpu_rule(name, threads)[0]  # noqa: E731
    cases = {
        'Intel(R) Core(TM) Ultra X7 358H': 'ok',
        'Intel(R) Core(TM) i5-11400F @ 2.60GHz': 'ok',
        '11th Gen Intel(R) Core(TM) i5-1135G7 @ 2.40GHz': 'ok',
        '12th Gen Intel(R) Core(TM) i3-12100F': 'ok',
        'Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz': 'weak',
        'Intel(R) Core(TM) i5-8250U CPU @ 1.60GHz': 'weak',
        'Intel(R) Core(TM) i3-10100 CPU @ 3.60GHz': 'weak',
        'Intel(R) Core(TM) i7-7700HQ CPU @ 2.80GHz': 'below',
        'Intel(R) Core(TM) i5-4590 CPU @ 3.30GHz': 'below',
        'Intel(R) Pentium(R) Gold 7505': 'below',
        'Intel(R) N100': 'below',
        'Intel(R) Celeron(R) N4020 CPU @ 1.10GHz': 'below',
        'AMD Ryzen 5 5600X 6-Core Processor': 'ok',
        'AMD Ryzen 7 5800H with Radeon Graphics': 'ok',
        'AMD Ryzen 7 4800H with Radeon Graphics': 'ok',
        'AMD Ryzen 7 3700X 8-Core Processor': 'ok',
        'AMD Ryzen 5 3600 6-Core Processor': 'weak',
        'AMD Ryzen 5 4500U with Radeon Graphics': 'weak',
        'AMD Ryzen 5 2600 Six-Core Processor': 'below',
        'AMD Ryzen 3 3200G with Radeon Vega Graphics': 'weak',
        'AMD Ryzen 7 PRO 7840U w/ Radeon 780M Graphics': 'ok',
        'AMD Ryzen Z1 Extreme': 'ok',
        'AMD Custom APU 0405': 'ok',
        'AMD FX(tm)-8350 Eight-Core Processor': 'below',
        'AMD Athlon Silver 3050U': 'below',
        'Some Future CPU': None,
    }
    for name, want in cases.items():
        got = cpu(name)
        check(got == want, f'CPU {name}: {got}, expected {want}')
    check(cpu('Some Future CPU', 2) == 'below', 'two threads are below the minimum')
    check(cpu('Some Future CPU', 6) == 'weak', 'six threads may struggle')

    gpu = lambda name, windows=True: b3_spec.gpu_rule(name, windows)[0]  # noqa: E731
    cases = {
        'NVIDIA GeForce RTX 3060 Laptop GPU': 'ok',
        'NVIDIA GeForce GTX 1660 SUPER': 'ok',
        'NVIDIA GeForce GTX 1060 6GB': 'ok',
        'NVIDIA GeForce GTX 970': 'ok',
        'NVIDIA GeForce GTX 1050 Ti': 'weak',
        'NVIDIA GeForce GTX 960': 'weak',
        'NVIDIA GeForce GTX 750 Ti': 'weak',
        'NVIDIA GeForce GTX 780': 'below',
        'NVIDIA GeForce GT 1030': 'below',
        'NVIDIA GeForce MX250': 'below',
        'NVIDIA GeForce MX450': 'weak',
        'AMD Radeon RX 6600': 'ok',
        'AMD Radeon RX 580 2048SP': 'ok',
        'Radeon RX 560 Series': 'weak',
        'AMD Radeon RX Vega 56': 'ok',
        'AMD Radeon(TM) 780M': 'ok',
        'AMD Radeon(TM) Graphics': 'weak',
        'AMD Radeon(TM) Vega 8 Graphics': 'weak',
        'AMD Radeon R9 200 Series': 'below',
        'AMD Radeon HD 7900 Series': 'below',
        'AMD Custom GPU 0405': 'ok',
        'Intel(R) Arc(TM) B390 GPU': 'ok',
        'Intel(R) Arc(TM) A750 Graphics': 'ok',
        'Intel(R) Iris(R) Xe Graphics': 'ok',
        'Intel(R) UHD Graphics 620': 'weak',
        'Intel(R) HD Graphics 630': 'weak',
        'Intel(R) HD Graphics 4600': 'below',
        'Microsoft Basic Display Adapter': 'below',
        'llvmpipe (LLVM 17.0.6, 256 bits)': 'below',
        'Some Future GPU': None,
    }
    for name, want in cases.items():
        got = gpu(name)
        check(got == want, f'GPU {name}: {got}, expected {want}')
    check(gpu('AMD Radeon R9 200 Series', windows=False) == 'weak', 'GCN 1-3 on Linux (RADV) is a warning only')

    rules = b3_spec.pc_rules
    check(rules(30, 'Intel(R) Core(TM) Ultra X7 358H', 16,
                ['GameViewer Virtual Display Adapter', 'Intel(R) Arc(TM) B390 GPU']) == ([], []),
          'this development PC passes, a virtual display adapter beside its GPU notwithstanding')
    check(rules(15.2, 'AMD Custom APU 0405', 8, ['AMD Custom GPU 0405']) == ([], []), 'the Steam Deck passes')
    below, weak = rules(7.8, 'Intel(R) Core(TM) i5-8250U CPU @ 1.60GHz', 8, ['Intel(R) UHD Graphics 620'])
    check(len(below) == 1 and 'memory' in below[0] and len(weak) == 2,
          f'an 8 GB laptop is below the minimum, its CPU and iGPU warnings: {below} {weak}')
    below, weak = rules(11.7, 'AMD Ryzen 5 5600X 6-Core Processor', 12, ['NVIDIA GeForce RTX 3060'])
    check(not below and weak == ['12 GB of memory (16 GB recommended)'], f'12 GB is a warning only: {weak}')
    below, _ = rules(31.9, 'AMD Ryzen 5 5600X 6-Core Processor', 12, ['NVIDIA GeForce GTX 780'])
    check(below == ['GPU: a NVIDIA GeForce GTX 780 (no Vulkan 1.3 driver)'], f'a Kepler GPU is refused: {below}')
    below, weak = rules(31.9, 'AMD Ryzen 5 5600X 6-Core Processor', 12,
                        ['AMD Radeon(TM) Graphics', 'NVIDIA GeForce RTX 3060 Laptop GPU'])
    check(not below and not weak, 'the discrete GPU counts, not the iGPU beside it')
    check(rules(0, '', 0, []) == ([], []), 'nothing read: nothing refused')

    if failures:
        print(f'b3_spec: {len(failures)} failure(s)')
        return 1
    print('b3_spec: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
