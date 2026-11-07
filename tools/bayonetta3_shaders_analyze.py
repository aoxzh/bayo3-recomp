#!/usr/bin/env python3
"""Inspect original B3 unsupported/legacy programs; emit source-bound disassembly.

No translation semantics are substituted. Run after an extraction and harness
build, optionally against a new version via --work. Artifacts remain private.
"""
import argparse
import collections
import csv
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location('b3', Path(__file__).with_name('bayonetta3_shaders.py'))
b3 = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(b3)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work', type=Path, default=b3.WORK/'control9-v2')
    p.add_argument('--dotnet', type=Path, default=b3.shutil.which('dotnet'), help='dotnet executable (default: PATH)')
    p.add_argument('--harness', type=Path, help='Optional separately built disassembly harness')
    a = p.parse_args()
    dotnet = b3.shutil.which(str(a.dotnet)) if a.dotnet else None
    if not dotnet:
        p.error('dotnet not found; pass --dotnet or add it to PATH')
    a.dotnet = Path(dotnet)
    work = a.work.resolve()
    if not work.is_relative_to((b3.ROOT/'private').resolve()):
        p.error('work must be private')
    extraction = json.loads((work/'extraction.json').read_text())
    statuses = {r['hash']: r for r in csv.DictReader((work/'results-all.tsv').open(), delimiter='\t')}
    selected = []
    for row in extraction['rows']:
        ctrl = work/'shaders_extracted/root_000'/row['ctrl_file']
        minor = struct.unpack_from('<I', ctrl.read_bytes(), 8)[0]
        status = statuses[row['code_sha1_16']]['status']
        if minor == 9 or status not in ('ok', 'ok-retry'):
            selected.append((row, minor, status))
    out = work/'analysis'
    out.mkdir(exist_ok=True)
    dll = a.harness or work/'build/bin/Release/net10.0/Ryujinx.Graphics.Shader.dll'
    records = []
    for row, minor, status in selected:
        key = row['code_sha1_16']
        src = work/'shaders_extracted/root_000'
        path = out/(key+'.disasm.tsv')
        with path.open('w') as f:
            subprocess.run([str(a.dotnet), str(dll), '--disassemble', str(src/row['code_file']), str(src/row['ctrl_file'])], stdout=f, check=True)
        ins = list(csv.DictReader(path.open(), delimiter='\t'))
        pairs, active = [], None
        for i, op in enumerate(ins):
            if op['opcode'] == 'Sam':
                if active is not None:
                    raise ValueError('nested SAM needs explicit investigation')
                active = i
            if op['opcode'] == 'Ram':
                if active is None:
                    raise ValueError('unpaired RAM needs explicit investigation')
                block = ins[active:i+1]
                pairs.append(dict(sam=ins[active]['code_offset'], ram=op['code_offset'],
                                  raw_sam=ins[active]['raw_u64'], raw_ram=op['raw_u64'],
                                  opcode_counts=dict(collections.Counter(v['opcode'] for v in block)),
                                  textures=[v for v in block if v['opcode'].startswith('Tex')],
                                  context=ins[max(0, active-2):min(len(ins),i+4)]))
                active = None
        if active is not None:
            raise ValueError('unpaired SAM needs explicit investigation')
        records.append(dict(hash=key, source=row['source'], stage=row['stage'], minor=minor,
           status=status, code_sha256=row['code_sha256'], control_sha256=row['control_sha256'],
           control_size=row['control_size'], program_start=row['program_start'],
           const_off=row['const_off'], const_size=row['const_size'], code_size=row['code_size'],
           disassembly=path.name, disassembly_sha256=b3.sha(path),
           opcode_counts=dict(collections.Counter(v['opcode'] for v in ins)), sam_ram_pairs=pairs,
           partial_quad_moves=[v for v in ins if v['opcode'] in ('MovR','Mov32i') and
                               'QuadMask=' in v['operands'] and 'QuadMask=15' not in v['operands']],
           interpretation='Original declared program linear decode; scheduling words excluded. No dynamic reachability claim.'))
    b3.js(out/'investigation.json', dict(schema=1, records=records,
        decoder_harness_sha256=b3.sha(dll), extraction_sha256=b3.sha(work/'extraction.json'),
        conclusions=dict(legacy='Five original control 1.9 records are actual VSO/PSO programs with unchanged loader fields.',
          sam_ram='Real opcode pairs in Vanish shaders; Ryujinx logs and emits no IR for SAM/RAM. Semantics unresolved; not publishable.',
          tessellation='Vanish and HMFlower names identify shader families only; scene use requires runtime stage submissions.'),
        independent_references=[
          'https://raw.githubusercontent.com/envytools/envytools/master/envydis/gm107.c',
          'https://raw.githubusercontent.com/eden-emulator/mirror/master/src/shader_recompiler/frontend/maxwell/translate/impl/not_implemented.cpp',
          'https://forums.developer.nvidia.com/t/what-dose-the-cuda-sass-instruction-sam-and-ram-mean/288761'],
        limitations=['No hardware execution semantics for SAM/RAM established.',
                     'Linear decode includes dead padding; source offsets are code-block coordinates.',
                     'Vulkan validation does not establish visual equivalence.']))
    print(f'Analyzed {len(records)} programs; {sum(len(r["sam_ram_pairs"]) for r in records)} SAM/RAM pairs; {out}')


if __name__ == '__main__':
    main()
