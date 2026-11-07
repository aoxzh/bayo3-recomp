#!/usr/bin/env python3
"""Offline original SDK command-encoding ABI checks, no game or driver execution."""
from pathlib import Path
import hashlib
import struct
import sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import *
root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root))
from recomp.nso import load, apply_relative
path = Path(sys.argv[1]) if len(sys.argv)>1 else root/'private/bayonetta3/extract/exefs/sdk'
assert hashlib.sha256(path.read_bytes()).hexdigest() == '2a89fa1c0e677710e89ed29b578162affef530a0c9b8c93cda15d50a57a20d28', 'requires original Bayonetta 3 1.2.0 SDK'
m = load(str(path)); apply_relative(m)
md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
# Resolve public proc names from SDK's own sorted names/default implementation tables.
functions = {}
table = m.u64(0xce8030)
for index in range(532):
    name = m.cstr(0xb1d723 + m.u32(0xb21038 + index*4))
    functions[name] = m.u64(table+index*8)

def machine():
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    u.mem_map(0,(len(m.image)+0xfff)&~0xfff);u.mem_write(0,bytes(m.image))
    u.mem_map(0x2000000,0x20000)
    u.reg_write(UC_ARM64_REG_SP,0x2018000);u.reg_write(UC_ARM64_REG_X30,0x2000000)
    u.mem_write(0x2001000,struct.pack('<QQ',0x2002000,0x2003000))
    u.mem_write(0x2001060,struct.pack('<Q',0x2004000))
    u.reg_write(UC_ARM64_REG_X0,0x2001000)
    return u

def encode(name,args=(),floating=None,float_bits=None):
    u=machine()
    for register,value in zip([UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3],args):u.reg_write(register,value&0xffffffffffffffff)
    if floating is not None:u.reg_write(UC_ARM64_REG_S0,struct.unpack('<I',struct.pack('<f',floating))[0])
    if float_bits is not None:
        for register,bits in zip([UC_ARM64_REG_S0,UC_ARM64_REG_S1],float_bits):u.reg_write(register,bits)
    address=functions['CommandBuffer'+name]
    assert list(md.disasm(bytes(m.image[address:address+4]),address))
    u.emu_start(address,0x2000000,count=1000)
    assert u.reg_read(UC_ARM64_REG_PC)==0x2000000
    end=struct.unpack('<Q',u.mem_read(0x2001000,8))[0]
    return bytes(u.mem_read(0x2002000,end-0x2002000))

assert encode('SetColorReductionEnable',(0,))==struct.pack('<I',0x80000367)
assert encode('SetColorReductionEnable',(1,))==struct.pack('<I',0x80010367)
assert encode('SetColorReductionEnable',(2,))==struct.pack('<I',0x80000367)
for kind,maximum in [(0,255),(3,63),(5,255)]:
    words=struct.unpack('<II',encode('SetColorReductionThresholds',(kind,500,-1)))
    assert words[1]==maximum<<16
    assert struct.unpack('<II',encode('SetColorReductionThresholds',(kind,0,0)))[1]==0
for count in [1,3,4,32]:assert encode('SetPatchSize',(count,))==struct.pack('<I',0x80000373|(count<<16))
assert encode('SetLineWidth',floating=2.5)==struct.pack('<Iff',0x200204ec,2.5,2.5)
# Original SetDepthBounds +4c81a0 uses low-byte-nonzero enable, then
# copies raw s0/s1 without validating, sorting or clamping either bound.
assert functions['CommandBufferSetDepthBounds']==0x4c81a0
bounds_cases=[(0x3f183e10,0x3fb28588), # actual out-of-unit-range bounds
              (0xbf800000,0x40000000), # -1,2
              (0x3f800000,0),          # reversed 1,0
              (0x7fc12345,0x7f800000), # NaN payload,+infinity
              (0xff800000,0x80000000), # -infinity,-zero
              (0,0x80000000),          # signed zeros
              (0x7f812345,0x3f000000)] # signaling NaN remains raw
for enabled in [0,1,2,0x100,0x101,0xff,0x102,0xffffffff]:
    for lower,upper in bounds_cases:
        expected=(struct.pack('<4I',0x8001066f,0x200203e7,lower,upper)
                  if enabled&0xff else struct.pack('<I',0x8000066f))
        assert encode('SetDepthBounds',(enabled,),float_bits=(lower,upper))==expected
print('SDK depth bounds: raw s0/s1, low-byte-nonzero enable, no clamp (56 cases) passed')
# Original SetAlphaRef (sdk+4c76a0) copies s0, including out-of-range,
# infinities, signed zero and NaN payloads. x1 is not the float argument.
for bits in [0,0x80000000,0x3f000000,0xbf800000,0x40000000,0x7f800000,0xff800000,0x7fc12345]:
    value=struct.unpack('<f',struct.pack('<I',bits))[0]
    assert encode('SetAlphaRef',(0xdeadbeef,),floating=value)==struct.pack('<II',0x200104c4,bits)
u=machine()
u.emu_start(functions['ColorStateSetDefaults'],0x2000000,count=1000)
assert bytes(u.mem_read(0x2001000,4))==struct.pack('<I',0x08030000)
u.emu_start(functions['ColorStateGetAlphaTest'],0x2000000,count=1000)
assert u.reg_read(UC_ARM64_REG_W0)==8  # ALWAYS: no separate enable bit
for compare in range(1,9):
    u=machine();u.reg_write(UC_ARM64_REG_W1,compare)
    u.emu_start(functions['ColorStateSetAlphaTest'],0x2000000,count=1000)
    u.emu_start(functions['ColorStateGetAlphaTest'],0x2000000,count=1000)
    assert u.reg_read(UC_ARM64_REG_W0)==compare
# FenceSync writes control memory rather than pushbuffer methods. Give the
# SDK an empty pushbuffer plus a control arena; recording cannot modify Sync.
for condition,flags in [(0,1),(0,0),(1,1)]:
    u=machine()
    u.mem_write(0x2001040,struct.pack('<Q',0x2002000))
    u.mem_write(0x2001020,struct.pack('<QQ',0x2005000,0x2006000))
    u.reg_write(UC_ARM64_REG_X1,0x2007000)
    u.reg_write(UC_ARM64_REG_W2,condition);u.reg_write(UC_ARM64_REG_W3,flags)
    u.emu_start(functions['CommandBufferFenceSync'],0x2000000,count=1000)
    assert u.reg_read(UC_ARM64_REG_PC)==0x2000000
    assert bytes(u.mem_read(0x2005000,16))==struct.pack('<IIQ',5|(condition<<8),flags,0x2007000)
    assert bytes(u.mem_read(0x2007000,64))==bytes(64)
assert encode('ClearBuffer',(0x1234567890,0,0xabcdef01))==b''
clear=encode('ClearBuffer',(0x1234567890,0x80,0xabcdef01))
assert len(clear)==0x48 and struct.unpack_from('<I',clear,8)[0]==0xabcdef01
assert struct.unpack_from('<I',clear,0x2c)[0]==0x20
expected_cache={1:0x800103d8,2:0x800003d8,3:0x800003e0,4:0x800103e0,5:None,6:0x8001044d}
for action in range(1,7):
    words=encode('SetTiledCacheAction',(action,))
    expected=expected_cache[action]
    assert words==(b'' if expected is None else struct.pack('<I',expected))
    print('SDK tiled action',action,words.hex())
u=machine();u.reg_write(UC_ARM64_REG_X0,0);u.reg_write(UC_ARM64_REG_X1,0x4f);u.reg_write(UC_ARM64_REG_X2,0x2001100)
u.emu_start(functions['DeviceGetInteger'],0x2000000,count=1000)
assert struct.unpack('<I',u.mem_read(0x2001100,4))[0]==32
scratch=list(md.disasm(bytes(m.image[functions['CommandBufferSetShaderScratchMemory']:functions['CommandBufferSetShaderScratchMemory']+0x7c]),functions['CommandBufferSetShaderScratchMemory']))
assert any(i.mnemonic=='mov' and i.op_str=='x19, x2' for i in scratch)
assert any(i.mnemonic=='mov' and i.op_str=='x20, x1' for i in scratch)
assert any(i.mnemonic=='mov' and i.op_str=='x21, x3' for i in scratch)
print('SDK ABI: clear/line/cache/color/patch encoding and max patch=32 passed; scratch register contract checked')
