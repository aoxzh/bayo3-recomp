#!/usr/bin/env python3
"""Validate precision annotations and optionally every position-stage module in a bundle."""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ASM = '''OpCapability Shader
%glsl = OpExtInstImport "GLSL.std.450"
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %position
OpDecorate %position BuiltIn Position
OpDecorate %sum NoContraction
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%ptr = OpTypePointer Output %v4
%int = OpTypeInt 32 1
%one = OpConstant %float 1
%two = OpConstant %float 2
%i1 = OpConstant %int 1
%i2 = OpConstant %int 2
%position = OpVariable %ptr Output
%main = OpFunction %void None %fn
%entry = OpLabel
%sum = OpFAdd %float %one %two
%mul = OpFMul %float %sum %two
%div = OpFDiv %float %mul %two
%rem = OpFRem %float %div %two
%mod = OpFMod %float %rem %two
%fma = OpExtInst %float %glsl Fma %one %two %sum
%idiv = OpSDiv %int %i2 %i1
%v = OpCompositeConstruct %v4 %sum %div %fma %one
OpStore %position %v
OpReturn
OpFunctionEnd
'''

def words(path):
    data = path.read_bytes()
    return struct.unpack(f'<{len(data)//4}I', data)

def instructions(code):
    i = 5
    while i < len(code):
        wc, op = code[i] >> 16, code[i] & 65535
        assert wc and i + wc <= len(code)
        yield op, code[i:i + wc]
        i += wc

def run(*args):
    subprocess.run([str(a) for a in args], check=True)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bundle', type=Path)
    args = parser.parse_args()
    cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
    with tempfile.TemporaryDirectory(prefix='b3-precision-') as temp:
        d = Path(temp)
        exe = d / 'precision'
        run(cxx, '-std=c++17', '-O2', '-I', ROOT / 'runtime/src/nvn',
            ROOT / 'tests/bayonetta3_nvn_precision_test.cpp', '-o', exe)
        run(exe)
        asm, original, patched = d / 'fixture.spvasm', d / 'fixture.spv', d / 'patched.spv'
        asm.write_text(ASM)
        run('spirv-as', '--target-env', 'vulkan1.1', asm, '-o', original)
        run('spirv-val', '--target-env', 'vulkan1.1', original)
        run(exe, original, patched)
        run('spirv-val', '--target-env', 'vulkan1.1', patched)
        ops = list(instructions(words(patched)))
        decorated = [w[1] for op, w in ops if op == 71 and w[2] == 42]
        expected = {w[2] for op, w in ops if op in (129, 133, 136, 140, 141) or op == 12}
        integers = {w[2] for op, w in ops if op == 135}
        assert set(decorated) == expected and len(decorated) == len(expected)
        assert not integers.intersection(decorated)
        # An opcode numbered 50 in an unrelated instruction set is not GLSL Fma. The replacement has to keep
        # the same length: a literal string is stored inline, so a longer one shifts every later word and the
        # file stops being a whole number of words, and the harness rejects it before it ever reaches the patch.
        foreign = bytearray(original.read_bytes())
        pos = foreign.index(b'GLSL.std.450')
        foreign[pos:pos + 12] = b'OtherSet.450'
        other = d / 'other.spv'
        other.write_bytes(foreign)
        run(exe, other, patched)
        fma_id = next(w[2] for op, w in instructions(words(original)) if op == 12)
        assert not any(op == 71 and w[1] == fma_id and w[2] == 42 for op, w in instructions(words(patched)))
        count = 0
        if args.bundle:
            for path in sorted(args.bundle.glob('*.spv')):
                code = words(path)
                if not any(op == 15 and w[1] in (0, 1, 2) for op, w in instructions(code)):
                    continue
                run(exe, path, patched)
                run('spirv-val', '--target-env', 'vulkan1.2', patched)
                count += 1
        print(f'precision tests passed; {count} real position-stage modules validated')

if __name__ == '__main__':
    main()
