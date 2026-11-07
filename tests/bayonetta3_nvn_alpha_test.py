#!/usr/bin/env python3
"""Validate the production alpha wrapper, including real translated fragment CFGs."""
from pathlib import Path
import argparse
import math
import os
import shutil
import struct
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]

def run(*args):
    subprocess.run([str(a) for a in args],check=True,stdout=subprocess.DEVNULL)

def words(path):
    raw=path.read_bytes()
    return struct.unpack('<%dI'%(len(raw)//4),raw)

def instructions(w):
    at=5
    while at<len(w):
        n=w[at]>>16
        assert n and at+n<=len(w)
        yield w[at]&65535,w[at:at+n]
        at+=n

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--cache',type=Path)
    args=parser.parse_args()
    compiler=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
    with tempfile.TemporaryDirectory(prefix='b3-alpha-') as temp:
        tmp=Path(temp);exe=tmp/'patch'
        run(compiler,'-std=c++20','-O1','-I'+str(ROOT/'runtime/src'),ROOT/'tests/bayonetta3_nvn_alpha_test.cpp',ROOT/'runtime/src/nvn/nvn_spirv_alpha.cpp','-o',exe)
        # Multiple returns in a loop and a selection, plus an existing discard.
        asm='''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %color
OpExecutionMode %main OriginUpperLeft
OpDecorate %color Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%vec4 = OpTypeVector %float 4
%ptr = OpTypePointer Output %vec4
%bool = OpTypeBool
%true = OpConstantTrue %bool
%zero = OpConstant %float 0
%value = OpConstantComposite %vec4 %zero %zero %zero %zero
%color = OpVariable %ptr Output
%main = OpFunction %void None %fn
%entry = OpLabel
OpStore %color %value
OpBranch %loop
%loop = OpLabel
OpLoopMerge %end %cont None
OpBranchConditional %true %body %end
%body = OpLabel
OpSelectionMerge %sel None
OpBranchConditional %true %ret %discard
%ret = OpLabel
OpReturn
%discard = OpLabel
OpKill
%sel = OpLabel
OpBranch %cont
%cont = OpLabel
OpBranch %loop
%end = OpLabel
OpReturn
OpFunctionEnd
'''
        fixture=tmp/'fixture.spvasm';fixture.write_text(asm)
        original=tmp/'original.spv';patched=tmp/'patched.spv'
        run('spirv-as','--target-env','vulkan1.1',fixture,'-o',original)
        run('spirv-val','--target-env','vulkan1.1',original)
        expected={2:184,3:180,4:188,5:186,6:182,7:190}
        for compare in range(1,9):
            for reference in ['0.00392156862745098','nan','-0','inf']:
                run(exe,original,patched,compare,reference)
                run('spirv-val','--target-env','vulkan1.1',patched)
                out=words(patched)
                if compare==8:assert out==words(original)
                else:
                    ops=[op for op,_ in instructions(out)]
                    assert ops.count(57)==1 # original entry called once
                    if compare in expected:assert expected[compare] in ops
                    assert ops.count(252)==2 # original kill plus alpha failure
        # Execute the injected wrapper's data/control instructions in a tiny
        # scalar oracle; verify pass/kill at below/equal/above and unordered
        # values, rather than merely asserting that a compare opcode exists.
        for compare in range(1,8):
            ref=struct.unpack('<f',struct.pack('<f',1/255))[0]
            run(exe,original,patched,compare,str(ref))
            code=words(patched);all_inst=list(instructions(code))
            constant={w[2]:struct.unpack('<f',struct.pack('<I',w[3]))[0]
                      for op,w in all_inst if op==43 and len(w)==4}
            start=max(i for i,(op,_) in enumerate(all_inst) if op==54)
            body=all_inst[start:]
            labels={w[1]:i for i,(op,w) in enumerate(body) if op==248}
            for alpha in [-math.inf,0.0,ref,0.5,math.inf,math.nan]:
                vals=dict(constant);ip=0;killed=False
                while ip<len(body):
                    op,w=body[ip];ip+=1
                    if op==61:vals[w[2]]=(0,0,0,alpha)
                    elif op==81:vals[w[2]]=vals[w[3]][w[4]]
                    elif op in expected.values():
                        a,b=vals[w[3]],vals[w[4]]
                        vals[w[2]]=not(math.isnan(a) or math.isnan(b)) and {
                            184:lambda:a<b,180:lambda:a==b,188:lambda:a<=b,
                            186:lambda:a>b,182:lambda:a!=b,190:lambda:a>=b}[op]()
                    elif op==250:ip=labels[w[2] if vals[w[1]] else w[3]]
                    elif op==252:killed=True;break
                    elif op==253:break
                expected_pass=False if compare==1 else (not math.isnan(alpha) and {
                    2:lambda:alpha<ref,3:lambda:alpha==ref,4:lambda:alpha<=ref,
                    5:lambda:alpha>ref,6:lambda:alpha!=ref,7:lambda:alpha>=ref}[compare]())
                assert killed!=expected_pass,(compare,alpha)
        assert subprocess.run([str(exe),str(original),str(patched),'0','0']).returncode==4
        assert subprocess.run([str(exe),str(original),str(patched),'9','0']).returncode==4
        # Reject early depth/stencil commits instead of silently discarding too late.
        fixture.write_text(asm.replace('OpExecutionMode %main OriginUpperLeft','OpExecutionMode %main OriginUpperLeft\nOpExecutionMode %main EarlyFragmentTests'))
        early=tmp/'early.spv'
        run('spirv-as','--target-env','vulkan1.1',fixture,'-o',early)
        assert subprocess.run([str(exe),str(early),str(patched),'5','0']).returncode==4
        # No color0: mirror translator OmapTargets alpha guard; NEVER still kills.
        fixture.write_text(asm.replace(' "main" %color',' "main"').replace('OpDecorate %color Location 0','').replace('%color = OpVariable %ptr Output','').replace('OpStore %color %value',''))
        run('spirv-as','--target-env','vulkan1.1',fixture,'-o',original)
        for compare in [1,5,8]:
            run(exe,original,patched,compare,'0.0039215686')
            run('spirv-val','--target-env','vulkan1.1',patched)
            if compare!=1:assert words(original)==words(patched)
        count=0
        if args.cache:
            for path in sorted(args.cache.rglob('*.spv')):
                if not any(op==15 and w[1]==4 for op,w in instructions(words(path))):continue
                for compare in [1,5,6]:
                    run(exe,path,patched,compare,'0.00392156862745098')
                    run('spirv-val','--target-env','vulkan1.2',patched)
                count+=1
        print(f'Alpha production wrapper: all compares, special refs, structured returns/discard, depth-only passed; {count} real fragment modules validated x3.')
if __name__=='__main__':main()
