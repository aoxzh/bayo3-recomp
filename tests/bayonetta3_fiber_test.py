#!/usr/bin/env python3
"""Private integration test: existing generated B3 SDK code + production Fiber HLE.

No game bytes are checked into this test. Outputs stay under private/fiber-analysis.
Run with the Python used for B3 preparation, after generating the SDK module.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[1]
    parser.add_argument('--generated', type=Path, default=root/'private/bayonetta3/gen/sdk')
    parser.add_argument('--output', type=Path, default=root/'private/fiber-analysis/production-test')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'clang++'))
    parser.add_argument('--optimization', choices=['0','2'], default='2')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    wanted = {'00379220','00379290','003792F0','00379330','00379410','0037945C',
              '00379470','0037D284','0037D340','0037D3B8','0037D29C','0037D304',
              '0037D320','0037D33C','0037AA34','003CCDB0','003CCDB8'}
    functions = {}
    provenance = []
    for file in sorted(args.generated.glob('code_*.cpp')):
        text = file.read_text()
        found = []
        for name, body in re.findall(r'(?:^|\n)(void sdk_([0-9A-F]+)\(.*?\n\})', text, re.S):
            if body in wanted:
                functions[body] = name
                found.append(body)
        if found:
            provenance.append({'path':str(file), 'sha256':hashlib.sha256(file.read_bytes()).hexdigest(), 'functions':found})
    if set(functions) != wanted:
        raise RuntimeError(f'Missing generated functions: {wanted-set(functions)}')
    # Exercise the production setjmp emitter on a tiny synthetic AArch64 caller:
    # setjmp(buf); if zero { yield(); longjmp(buf,7); } return.
    import sys
    import struct
    from types import SimpleNamespace
    sys.path.insert(0, str(root))
    from recomp.emit import Emitter
    words = [0xa9bf7bfd,0x910003fd,0xd2824680,0x9400003d,0xb50000a0,
             0x9400007b,0xd2824680,0x528000e1,0x940000b8,0xa8c17bfd,0xd65f03c0]
    fn = SimpleNamespace(start=0,end=len(words)*4)
    analysis = SimpleNamespace(module=SimpleNamespace(image=struct.pack('<'+'I'*len(words),*words)),
        functions=[fn,SimpleNamespace(start=0x200),SimpleNamespace(start=0x300)],
        plt={0x100:'setjmp'},jump_tables={})
    sj = Emitter(analysis,prefix='sj').function(fn)
    (args.output/'generated_fiber_setjmp.inc').write_text(sj+'\n')
    emitted = '\n\n'.join(functions[k] for k in sorted(functions))+'\n'
    declarations = sorted(set(re.findall(r'sdk_[0-9A-F]+(?=\()', emitted)))
    (args.output/'generated_fiber_test.inc').write_text(emitted)
    (args.output/'generated_fiber_decls.inc').write_text(''.join(f'void {name}(nx::CPU&);\n' for name in declarations))
    kernel_path = root/'runtime/src/kernel.cpp'
    kernel = kernel_path.read_text()
    start = kernel.index('        case 0x04: {  // MapMemory')
    end = kernel.index('        case 0x06:', start)
    cases = kernel[start:end]
    # Rename only helper boundaries/constants so this compiles independently of
    # kernel global state. The switch case control/data flow remains production.
    for a,b in [('decommit(', 'fiber_test_decommit('), ('commit(', 'fiber_test_commit('),
                ('vm_set(', 'fiber_test_vm_set('), ('kStackBase','kTestStackBase'),
                ('kStackSize','kTestStackSize'), ('kMemStack','kTestMemStack'),
                ('kMemAlias','kTestMemAlias'), ('kMemFree','kTestMemFree')]:
        cases = re.sub(r'\b'+re.escape(a), b, cases)
    (args.output/'generated_fiber_map_cases.inc').write_text(
        'void fiber_test_memory_svc(CPU& c,unsigned imm) { unsigned rc=0; switch(imm) {\n'
        + cases + '\n default: std::abort(); } c.x[0]=rc; }\n')
    provenance.append({'path':str(kernel_path),'sha256':hashlib.sha256(kernel_path.read_bytes()).hexdigest(),
                       'projection':'MapMemory/UnmapMemory cases; helper boundaries/constants renamed'})
    (args.output/'provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
    binary = args.output/'fiber-test'
    cmd=[args.cxx,'-std=c++20','-O'+args.optimization,'-g','-pthread','-DNX_BAYONETTA3=1',
         '-I'+str(root/'runtime/include'),'-I'+str(args.output),
         str(root/'tests/bayonetta3_fiber_test.cpp'),'-o',str(binary)]
    (args.output/'compile-command.json').write_text(json.dumps(cmd,indent=2)+'\n')
    subprocess.run(cmd, check=True)
    for label, arguments, expected in [('normal',[],0),('cross-thread',['cross-thread'],90),('cross-resume',['cross-resume'],90)]:
        result=subprocess.run([str(binary),*arguments],text=True,capture_output=True,timeout=30)
        log=result.stdout+result.stderr
        (args.output/(label+'.log')).write_text(log)
        print(log,end='')
        if result.returncode != expected:
            raise RuntimeError(f'{label}: exit {result.returncode}, expected {expected}')
        if label.startswith('cross-') and 'cross-thread/context migration unsupported' not in log:
            raise RuntimeError('cross-thread mode failed for an unrelated reason')
    print('PASS production Fiber integration suite; no full game execution')


if __name__ == '__main__':
    main()
