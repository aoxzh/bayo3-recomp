#!/usr/bin/env python3
"""Emulate authenticated B3 SDK volume size/mip queries and compare host layout.
No game or GPU execution. Requires existing Capstone/Unicorn environment.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_X6, UC_ARM64_REG_X7, UC_ARM64_REG_X30, UC_ARM64_REG_PC
root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root))
from recomp.nso import load, apply_relative
path = Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'private/bayonetta3/extract/exefs/sdk'
digest = hashlib.sha256(path.read_bytes()).hexdigest()
assert digest == '2a89fa1c0e677710e89ed29b578162affef530a0c9b8c93cda15d50a57a20d28'
module = load(str(path)); apply_relative(module)
u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
u.mem_map(0, (len(module.image) + 4095) & ~4095); u.mem_write(0, bytes(module.image))
u.mem_map(0x2000000, 0x20000)
stack, stop, descriptor, exponents, output, flag = 0x2018000, 0x2000000, 0x2001000, 0x2001100, 0x2001200, 0x2001300
registers = [UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_X6, UC_ARM64_REG_X7]
def call(address, arguments):
    u.reg_write(UC_ARM64_REG_SP, stack); u.reg_write(UC_ARM64_REG_X30, stop)
    for register, value in zip(registers, arguments): u.reg_write(register, value)
    u.emu_start(address, stop, count=100000)
    assert u.reg_read(UC_ARM64_REG_PC) == stop
    return u.reg_read(UC_ARM64_REG_X0)
# Original descriptor constructor: flags4, target2 (3D), four bytes/texel,
# samples0, block1x1, levels8; stack width/height/depth128, border1.
for offset, value in [(0,128),(8,128),(16,128),(24,1)]: u.mem_write(stack + offset, struct.pack('<I', value))
call(0x499e40, [descriptor,4,2,4,0,1,1,8])
u.mem_write(exponents, bytes([0,0,4,0]))  # width,height,depth logarithms
size = call(0x49a330, [descriptor, exponents])
expected_offsets = [0,0x800000,0x900000,0x920000,0x924000,0x925000,0x925800,0x925c00]
expected_sizes = [0x800000,0x100000,0x20000,0x4000,0x1000,0x800,0x400,0x200]
assert size == 0x925e00
mips = []
for level in range(8):
    call(0x49a440, [descriptor,output,flag,exponents,level])
    data = bytes(u.mem_read(output,40))
    width_log, height_log, depth_log, blocks_x, blocks_y, blocks_z = struct.unpack_from('<6I',data)
    offset, mip_size = struct.unpack_from('<QQ',data,24)
    assert offset == expected_offsets[level] and mip_size == expected_sizes[level]
    assert width_log == height_log == 0 and depth_log == min(4,7-level)
    mips.append(dict(level=level,width=max(1,128>>level),height=max(1,128>>level),depth=max(1,128>>level),
                     offset=offset,size=mip_size,block_width_gobs=1<<width_log,
                     block_height_gobs=1<<height_log,block_depth_gobs=1<<depth_log,
                     block_counts=[blocks_x,blocks_y,blocks_z]))
audit = dict(sdk_sha256=digest,sdk_path=str(path),authority='original-sdk-offline-emulation',
             procedures=dict(descriptor='0x499e40',total='0x49a330',mip='0x49a440',size_math='0x4e8d60',offset_math='0x4e9190'),
             texture=dict(target=2,format=0x25,width=128,height=128,depth=128,levels=8,flags=4,packaged_layout=0x40),
             total_size=size,mips=mips)
# Actual second packaged texture: 0x403 means Hlog3/Dlog0 plus
# horizontal GOB-count alignment log4, NOT a width-interleaving exponent.
for offset, value in [(0,512),(8,1024),(16,1),(24,1)]: u.mem_write(stack + offset, struct.pack('<I', value))
call(0x499e40, [descriptor,4,1,16,0,4,4,1])
astc_checks=[]
for alignment_log in [0,4]:
    u.mem_write(exponents, bytes([alignment_log,3,0,0]))
    total=call(0x49a330,[descriptor,exponents])
    call(0x49a440,[descriptor,output,flag,exponents,0])
    result=struct.unpack('<6IQQ',bytes(u.mem_read(output,40)))
    alignment=struct.unpack('<I',bytes(u.mem_read(flag,4)))[0]
    assert total==0x80000 and result==(0,3,0,32,4,1,0,0x80000)
    assert alignment==1<<alignment_log
    astc_checks.append(dict(packaged_layout=(alignment_log<<8)|3,total_size=total,
                            mip_metadata=list(result),horizontal_gob_alignment=alignment))
audit['astc_512x1024_layout403']=astc_checks
pitch_cases=[]
for width,height,levels in [(128,1024,1),(256,1024,1),(512,1024,4),(128,1024,4),(300,1024,1),(300,128,1),(300,255,1)]:
    for alignment_log in [0,4]:
        for offset,value in [(0,width),(8,height),(16,1),(24,1)]: u.mem_write(stack+offset,struct.pack('<I',value))
        call(0x499e40,[descriptor,4,1,16,0,4,4,levels])
        u.mem_write(exponents,bytes([alignment_log,3,0,0]))
        total=call(0x49a330,[descriptor,exponents]); records=[]; cumulative=0
        for level in range(levels):
            call(0x49a440,[descriptor,output,flag,exponents,level])
            result=struct.unpack('<6IQQ',bytes(u.mem_read(output,40)))
            alignment=struct.unpack('<I',bytes(u.mem_read(flag,4)))[0]
            cols=(max(1,width>>level)+3)//4; rows=(max(1,height>>level)+3)//4
            expected_alignment=(1<<alignment_log) if cols>=4*(1<<alignment_log) and rows>=8*8 else 1
            row_gobs=(cols*16+63)//64
            row_gobs=(row_gobs+expected_alignment-1)//expected_alignment*expected_alignment
            h_gobs=min(8,1<<max(0,((rows+7)//8-1).bit_length()))
            expected_size=row_gobs*((rows+8*h_gobs-1)//(8*h_gobs))*512*h_gobs
            assert alignment==expected_alignment and result[0]==0 and result[3]==row_gobs
            assert result[6]==cumulative and result[7]==expected_size
            cumulative+=expected_size
            records.append(dict(level=level,offset=result[6],size=result[7],row_gobs=result[3],horizontal_alignment=alignment))
        assert cumulative==total
        pitch_cases.append(dict(width=width,height=height,levels=levels,packaged_layout=(alignment_log<<8)|3,total=total,mips=records))
audit['pitch_alignment_cases']=pitch_cases
audit_path = root / 'private/nvn-callsite-analysis/volume-layout-audit.json'
audit_path.write_text(json.dumps(audit,indent=2)+'\n')
compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
assert compiler
# Compare against production volume-layout metadata. Expected numbers originate
# in the SDK queries above, not a duplicated host sizing implementation.
checks = '\n'.join(f'check(t.levels[{m["level"]}].tiled_offset=={m["offset"]}ull && t.levels[{m["level"]}].depth=={m["depth"]}u && t.levels[{m["level"]}].block_height_gobs=={m["block_height_gobs"]}u && t.levels[{m["level"]}].block_depth_gobs=={m["block_depth_gobs"]}u);' for m in mips)
for case in pitch_cases:
    alignment=1<<((case['packaged_layout']>>8)&15)
    checks+='\n{auto p=nx::nvn::block_linear_layout('+','.join(map(str,[case['width'],case['height'],1,case['levels'],4,4,16,8,alignment]))+');'
    checks+=f'check(p.tiled_size=={case["total"]}ull && p.levels.size()=={case["levels"]}u);'
    for mip in case['mips']:
        level=mip['level']
        checks+=f'check(p.levels[{level}].tiled_offset=={mip["offset"]}ull && p.levels[{level}].tiled_pitch_gobs=={mip["row_gobs"]}u);'
        checks+=f'check(nx::nvn::block_linear_size(p.levels[{level}].width_blocks,p.levels[{level}].height_blocks,1,16,p.levels[{level}].block_height_gobs,1,p.levels[{level}].tiled_pitch_gobs)=={mip["size"]}ull);'
    checks+='}'
with tempfile.TemporaryDirectory(prefix='b3-volume-sdk-') as temporary:
    directory=Path(temporary)
    source=directory/'test.cpp'; executable=directory/'test'
    source.write_text('#include "nvn/nvn_tiling.h"\n#include <cstdlib>\nvoid check(bool v){if(!v)std::abort();}\nint main(){auto t=nx::nvn::block_linear_volume_layout(128,128,128,8,1,1,4,1,16);check(t.tiled_size=='+str(size)+'ull && t.levels.size()==8);\n'+checks+'\n}\n')
    subprocess.run([compiler,'-std=c++20','-O1','-I'+str(root/'runtime/src'),str(source),str(root/'runtime/src/nvn/nvn_tiling.cpp'),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True)
print('PASS original SDK vs production: volume128^3 eight mips total0x925e00; all14 padded-pitch cases, offsets and sizes')
print(audit_path)
