#!/usr/bin/env python3
"""Original B3 NSO hardware Opus wire ABI audit; offline only."""
from pathlib import Path
import hashlib
import json
import struct
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from recomp.nso import load
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

exefs = Path(sys.argv[1])
config = json.loads((ROOT / 'config/bayonetta3-1.2.0.json').read_text())
modules = {}
for name in ('main','subsdk0','sdk'):
    p = exefs / name
    digest = hashlib.sha256(p.read_bytes()).hexdigest()
    assert digest == config['modules'][name]['sha256']
    print(name, digest)
    modules[name] = load(str(p))
    for s in modules[name].symbols:
        if s.imported and 'Opus' in s.name:
            print('IMPORT', s.name)
m = modules['sdk']; cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
def disassemble(address, size):
    instructions = list(cs.disasm_lite(m.image[address:address+size], address))
    for a, _, op, args in instructions: print(f'{a:x}: {op} {args}')
    return [(op,args) for _,_,op,args in instructions]
def exported(fragment):
    symbols = [s for s in m.symbols if not s.imported and fragment in s.name]
    assert len(symbols) == 1, (fragment, len(symbols))
    s = symbols[0]; print(s.name, hex(s.value)); return disassemble(s.value,s.size)
assert ('mov','w3, #5') in exported('31_nn_sf_sync_GetWorkBufferSizeEx')
assert ('mov','w5, #4') in exported('37_nn_sf_sync_OpenHardwareOpusDecoderEx')
assert ('mov','w12, #8') in exported('29_nn_sf_sync_DecodeInterleavedENS')
assert ('and','w8, w8, #0xfffff000') in exported('36GetHardwareOpusDecoderWorkBufferSizeEiii')
assert ('mov','w9, #0x226f') in exported('33DecodeOpusInterleavedWithHardwareEP')
assert ('mov','w9, #0x3e8') in exported('36GetHardwareOpusDecoderProcessingTimeEP')
rel = {r.offset:r for r in m.relocs}
for plt, expected in [(0x5b3090, 'InRawArgumentInfoILm16ELm4ELl0EEE'),
                      (0x5b8d60, 'OutRawArgumentInfoILm4ELm4ELl0EEE'),
                      (0x5b8dd0, 'OutRawArgumentInfoILm4ELm4ELl0EEE'),
                      (0x5b88f0, 'GetHeaderFromPacket')]:
    w,l = struct.unpack_from('<II',m.image,plt)
    imm = ((w>>5&0x7ffff)<<2)|(w>>29&3)
    if imm & (1<<20): imm -= 1<<21
    got = (plt & ~4095)+(imm<<12)+((l>>10&4095)*8)
    r = rel[got]; s = m.symbols[r.sym]
    assert expected in s.name
    print('PLT',hex(plt),'target',hex(s.value),s.name)
    instructions = disassemble(s.value,s.size)
    if plt==0x5b88f0:
        assert instructions==[('ldp','w8, w9, [x1]'),('rev','w8, w8'),('rev','w9, w9'),('stp','w8, w9, [x0]'),('ret','')]
print('PASS original hashes; hwopus manager 5/4 and decoder 8; 16-byte parameters, 4-byte size, 20-byte open args, 8-byte big-endian header, output {i32,i32,i64}, microsecond timing.')
