"""Targeted real-NSO instruction regression, including sticky FPSR.QC.

Run: python -m tests.difftest_bayonetta3 <main NSO> <subsdk0 NSO> <private work dir>
Only reads game modules. Generated runner stays in the explicitly supplied work directory.
This checks architectural values, not CSDB speculation guarantees or whole-function execution.
"""
import argparse
import ctypes
import pathlib
import random
import struct
import subprocess
from types import SimpleNamespace

import capstone
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as U

from recomp import emit, nso

MAIN = [0x126aa40, 0x126ab80, 0x126af50, 0x126afbc, 0x126b410, 0x126b550, 0x126b920, 0x126b98c]
SUB = {
    'csdb': [0xbefe0, 0xbf0f0, 0xc15fc, 0xc27fc, 0xc44d8, 0xc5fa0, 0xc6134, 0xc62c4,
             0xcb37c, 0xcb39c, 0xd1188, 0xd47c4, 0xd7b6c, 0xd8508, 0xda960, 0xdf7b8,
             0xdf870, 0xe0594, 0xe0680, 0xe076c, 0xe0858, 0xe0998, 0xe0a54, 0xe0b2c,
             0xe0be8, 0xe0ca4, 0xe0d60, 0xeff98, 0xf0208, 0xf0be4, 0xf5434],
    'bfc': [0x5caac, 0x5cbc8, 0x5cce4, 0x5ce10, 0x184510],
    'sqabs': [0x184550, 0x184554, 0x184558, 0x18455c],
    'sqrshl': [0x18465c, 0x184660, 0x184714, 0x184718],
    'sqshl': [0x184e18, 0x185038],
    'saddlv': [0x185128, 0x185248, 0x18524c, 0x185400, 0x185584],
}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('main_nso')
    ap.add_argument('subsdk0_nso')
    ap.add_argument('work')
    args = ap.parse_args()
    work = pathlib.Path(args.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    cs.detail = True
    cases = []
    for name, path, groups in [('main', args.main_nso, {'sqshl': MAIN}),
                                ('subsdk0', args.subsdk0_nso, SUB)]:
        m = nso.load(path)
        e = emit.Emitter(SimpleNamespace(module=m, functions=[]))
        if name == 'main':
            mod0 = m.u32(4)
            assert mod0 == 8 and m.image[mod0:mod0+4] == b'MOD0'
            i = next(cs.disasm(bytes(m.image[0x14:0x18]), 0x14))
            try:
                e.insn(SimpleNamespace(start=0x14, end=0x18), i, {})
            except emit.Unhandled:
                print('main+0x14 MOD0 bss_end: remains an explicit unsupported-instruction trap')
            else:
                raise AssertionError('MOD0 data was falsely accepted as a supported MOV')
        for mnemonic, addresses in groups.items():
            for addr in addresses:
                code = bytes(m.image[addr:addr+4])
                i = next(cs.disasm(code, addr))
                assert i.mnemonic == mnemonic, (name, hex(addr), i.mnemonic, mnemonic)
                stmt = e.insn(SimpleNamespace(start=addr, end=addr+4), i, {})
                cases.append((name, addr, code, i, stmt))
    src = ['#include <nx/cpu.h>', '#include <nx/neon_tbl.h>']
    for k, (_, _, _, _, stmt) in enumerate(cases):
        src.append(f'static void t{k}(nx::CPU& c) {{ {stmt} }}')
    src.append('static nx::Fn tests[] = {' + ','.join(f't{k}' for k in range(len(cases))) + '};')
    src.append('''extern "C" void run(unsigned id, uint64_t* x, uint8_t* v, uint32_t* flags) {
        nx::CPU c{}; std::memcpy(c.x,x,256); std::memcpy(c.v,v,512);
        nx::set_nzcv(c,flags[0]>>28); c.fpsr=flags[1]; tests[id](c);
        std::memcpy(x,c.x,256); std::memcpy(v,c.v,512);
        flags[0]=nx::get_nzcv(c); flags[1]=uint32_t(c.fpsr);
    }''')
    (work/'runner.cpp').write_text('\n'.join(src))
    root = pathlib.Path(__file__).resolve().parents[1]
    subprocess.run(['clang++', '-std=c++20', '-O2', '-march=x86-64-v3', '-ffp-contract=off',
                    '-fPIC', '-shared', '-I'+str(root/'runtime/include'), str(work/'runner.cpp'),
                    '-o', str(work/'runner.so')], check=True)
    lib = ctypes.CDLL(str(work/'runner.so'))
    lib.run.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64),
                       ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(ctypes.c_uint32)]
    lib.run.restype = None
    rng = random.Random(42)
    regs = [U.UC_ARM64_REG_X0+k for k in range(29)] + [U.UC_ARM64_REG_X29, U.UC_ARM64_REG_X30, U.UC_ARM64_REG_SP]
    values = [0, 1, -1, 2, -2, 0x7fffffff, -0x80000000, 0x40000000, -0x40000000, 3, -3]
    failures, runs = [], 0
    for tid, (name, addr, code, i, _) in enumerate(cases):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.ctl_set_cpu_model(U.UC_CPU_ARM64_MAX)
        uc.mem_map(0x1000, 0x1000); uc.mem_write(0x1000, code)
        shifting = i.mnemonic in ('sqshl', 'sqrshl')
        iterations = 256 * len(values) if shifting else 32
        for k in range(iterations):
            xs = [rng.getrandbits(64) for _ in range(32)]
            vs = bytearray(rng.randbytes(512))
            if shifting:
                n, b = (int(i.reg_name(o.reg)[1:]) for o in i.operands[1:3])
                lanes = 4 if i.operands[0].vas else 1
                for lane in range(lanes):
                    value = values[(k // 256 + lane) % len(values)]
                    struct.pack_into('<i', vs, n*16+lane*4, value)
                    # All signed low-byte shifts, with unrelated nonzero high bytes.
                    struct.pack_into('<I', vs, b*16+lane*4, (rng.getrandbits(24)<<8) | (k % 256))
            elif i.mnemonic == 'sqabs':
                n = int(i.reg_name(i.operands[1].reg)[1:])
                for lane in range(8):
                    value = [-32768, -32767, -1, 0, 1, 32767, 123, -123][(lane+k)%8] if k < 16 else lane-4
                    struct.pack_into('<h', vs, n*16+lane*2, value)
            elif i.mnemonic == 'saddlv':
                n = int(i.reg_name(i.operands[1].reg)[1:])
                for lane in range(4):
                    struct.pack_into('<i', vs, n*16+lane*4, values[k % len(values)] if k < 22 else values[(k+lane)%len(values)])
            for qc in (0, 1 << 27):
                nzcv = rng.randrange(16) << 28
                fpsr = qc | 0x11  # Preserve other sticky exception flags as well.
                for reg, value in zip(regs, xs): uc.reg_write(reg, value)
                for lane in range(32): uc.reg_write(U.UC_ARM64_REG_Q0+lane, int.from_bytes(vs[16*lane:16*lane+16], 'little'))
                uc.reg_write(U.UC_ARM64_REG_NZCV, nzcv)
                uc.reg_write(U.UC_ARM64_REG_FPCR, 0)
                uc.reg_write(U.UC_ARM64_REG_FPSR, fpsr)
                hx = (ctypes.c_uint64*32)(*xs)
                hv = (ctypes.c_uint8*512).from_buffer_copy(vs)
                hf = (ctypes.c_uint32*2)(nzcv, fpsr)
                lib.run(tid, hx, hv, hf)
                uc.emu_start(0x1000, 0x1004, count=1)
                ux = [uc.reg_read(reg) for reg in regs]
                uv = b''.join(uc.reg_read(U.UC_ARM64_REG_Q0+lane).to_bytes(16, 'little') for lane in range(32))
                uf = [uc.reg_read(U.UC_ARM64_REG_NZCV), uc.reg_read(U.UC_ARM64_REG_FPSR)]
                runs += 1
                if list(hx) != ux or bytes(hv) != uv or list(hf) != uf:
                    failures.append(f'{name}+{addr:#x} {i.mnemonic} {i.op_str}, case={k}, QC={qc:#x}: '
                                    f'GPR={list(hx)==ux}, SIMD={bytes(hv)==uv}, flags host={list(hf)} uc={uf}')
                    break
            if failures and failures[-1].startswith(f'{name}+{addr:#x} '): break
        print(f'{name}+{addr:#x} {i.mnemonic}: checked', flush=True)
    summary = f'{len(cases)} real instruction sites, {runs} runs, {len(failures)} failures (GPR/SIMD/NZCV/FPSR)'
    (work/'report.txt').write_text(summary+'\n'+'\n'.join(failures)+'\n')
    print(summary)
    for failure in failures: print(failure)
    raise SystemExit(bool(failures))


if __name__ == '__main__':
    main()
