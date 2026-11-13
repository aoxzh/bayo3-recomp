#!/usr/bin/env python3
"""Private B3 1.2.0 ROM -> Maxwell -> SMW-compatible SPIR-V, one worker.

PKZ/DAT parsing adapted from bayo3-android/tools/scan_rom.py (MIT; notices in
bayonetta3_shaders/). NVN layout knowledge is validated against original bytes.
No blade2 runtime is built, imported, or linked. Only its local offline Kraken
worker and Ryujinx shader compiler dependencies are used as explicit inputs.
"""
import argparse
import collections
import csv
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'private/shader-work'
TMP = ROOT / 'private/tmp/shaders'
# The archives holding each program's shaders, by the title ID of its --config. Program 0 (Bayonetta 3) keeps all
# of its runtime shaders in root_000.pkz. Program 1 (the Bayonetta Origins prologue the hub's picture book starts,
# whose RomFS the 1.2.0 update carries in full) keeps 1,321 there and the other 5,804 it draws with in shader_000.pkz.
ARCHIVES = {'01004a4010fea000': {'root_000.pkz': '87e7d717d1ced64d155d07e0b5197b2b24c08317b03c89d7ad49671699b2ce88'},
            '01004a4010fea001': {'root_000.pkz': '95de97468b0bd72174231595498a355f474f2eb51e3639a5dd934d2566d9f133',
                                 'shader_000.pkz': 'd3c116655e1471cf3f43abaca8267839cee930574a49b18d661ebb869f173abb'}}
EXE = '.exe' if os.name == 'nt' else ''
# SDK control 1.9 is exactly 0x7d1 bytes in five original B3 VSO/PSO
# resources. The loader fields +0x6f8..+0x708 are unchanged; keep raw bytes.
LAYOUTS = {9: 0x7d1, 12: 0x838, 14: 0x848, 15: 0x850, 16: 0x880}
STAGES = {1: 'vertex', 2: 'tess-control', 3: 'tess-evaluation', 4: 'geometry', 5: 'fragment'}
EXTENSIONS = {'.vso', '.pso', '.cso', '.hso', '.dso', '.gso'}
MANIFEST = ['id', 'stage', 'program_start', 'code_size', 'const_off', 'const_size',
            'control_size', 'control_sha256', 'code_sha256', 'source', 'code_sha1_16', 'code_file', 'ctrl_file']
RESULTS = ['hash', 'group', 'stage', 'stages_using', 'status', 'ms', 'spv_bytes', 'resources',
           'translator_messages', 'error', 'draw_time_queries']


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        while block := f.read(4 * 1024 * 1024):
            h.update(block)
    return h.hexdigest()


def js(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def table(path, fields, rows):
    with Path(path).open('w', newline='') as f:
        w = csv.DictWriter(f, fields, delimiter='\t', lineterminator='\n', extrasaction='ignore')
        w.writeheader()
        w.writerows(rows)


def extent(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError('extent outside input')
    return memoryview(data)[offset:offset + size]


def cstring(data, offset):
    if not 0 <= offset < len(data):
        raise ValueError('string outside table')
    end = data.find(b'\0', offset)
    if end < 0:
        raise ValueError('unterminated string')
    return data[offset:end].decode('utf-8')


def pkz_directory(path):
    size = path.stat().st_size
    with path.open('rb') as f:
        magic, version, declared, count, off, strings, alignment = struct.unpack('<4sIQ4I', f.read(32))
        if magic != b'pkzl' or version != 0x20000 or declared != size:
            raise ValueError('unknown PKZ identity/size/version')
        if count > 1000000 or strings > 64*1024*1024 or off < 32 or off + count*32 + strings > size:
            raise ValueError('invalid PKZ directory')
        f.seek(off)
        entries, names = f.read(count*32), f.read(strings)
    rows = []
    for i in range(count):
        no, co, raw, offset, stored = struct.unpack_from('<IIQQQ', entries, i*32)
        name, compression = cstring(names, no), cstring(names, co)
        if name.startswith('/') or '..' in PurePosixPath(name).parts or '\\' in name:
            raise ValueError('unsafe PKZ name')
        if offset < off + count*32 + strings or offset + stored > size or raw > 512*1024*1024:
            raise ValueError('invalid/oversized member')
        if compression == 'None' and raw != stored:
            raise ValueError('uncompressed size mismatch')
        rows.append(dict(name=name, compression=compression, size=raw, offset=offset, stored=stored))
    spans = sorted((r['offset'], r['offset']+r['stored']) for r in rows if r['stored'])
    if any(a[1] > b[0] for a, b in zip(spans, spans[1:])):
        raise ValueError('overlapping members')
    return rows


def dat_leaves(data, prefix, base=0, depth=0):
    if depth > 12:
        raise ValueError('DAT recursion limit')
    if data[:4] != b'DAT\0':
        yield prefix, base, len(data)
        return
    _, count, offsets, extensions, names, sizes, hashes = struct.unpack_from('<4s6I', data)
    if count > 1000000:
        raise ValueError('DAT count limit')
    extent(data, offsets, count*4)
    extent(data, sizes, count*4)
    width = struct.unpack_from('<I', extent(data, names, 4))[0]
    if not 1 <= width <= 4096:
        raise ValueError('DAT name width')
    extent(data, names+4, count*width)
    for i in range(count):
        off = struct.unpack_from('<I', data, offsets+i*4)[0]
        size = struct.unpack_from('<I', data, sizes+i*4)[0]
        name = bytes(extent(data, names+4+i*width, width)).split(b'\0')[0].decode('utf-8')
        yield from dat_leaves(extent(data, off, size), prefix+'!'+name, base+off, depth+1)


class Decoder:
    def __init__(self, path):
        self.p = subprocess.Popen([str(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, cwd=TMP)

    def decode(self, data, size):
        self.p.stdin.write(struct.pack('<QQ', len(data), size))
        self.p.stdin.write(data)
        self.p.stdin.flush()
        header = self.p.stdout.read(8)
        if len(header) != 8 or struct.unpack('<q', header)[0] != size:
            raise ValueError('decoder failure')
        out = self.p.stdout.read(size)
        if len(out) != size:
            raise ValueError('short decoder output')
        return out

    def close(self):
        self.p.stdin.close()
        try:
            self.p.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
        self.p.stdout.close()


def shader_record(data):
    magic, version, ctl_size, ctl_off, code_size, code_off = struct.unpack_from('<6I', data)
    ctrl, code = bytes(extent(data, ctl_off, ctl_size)), bytes(extent(data, code_off, code_size))
    if len(ctrl) < 0x24:
        raise ValueError('short control')
    cmagic, major, minor = struct.unpack_from('<3I', ctrl)
    declared = struct.unpack_from('<I', ctrl, 0x20)[0]
    if cmagic != 0x98761234 or major != 1 or LAYOUTS.get(minor) != declared or len(ctrl) != declared:
        raise ValueError(f'unknown control layout {cmagic:#x}/{major}/{minor}, size={len(ctrl):#x}')
    program_size, const_size, const_off, total, start = struct.unpack_from('<5I', ctrl, 0x6f8)
    if not 0 < total <= 16*1024*1024 or total != len(code) or start >= total or const_off+const_size > total:
        raise ValueError('invalid code/constants/program extent')
    if code[:4] == bytes.fromhex('78563412') and start == 0x30:
        stage = STAGES.get((struct.unpack_from('<I', code, start)[0] >> 10) & 15)
        if not stage:
            raise ValueError('unknown SPH stage')
    elif (code[:4] == bytes.fromhex('79563412') and start == 0x100) or start == 0:
        if not all(struct.unpack_from('<3I', ctrl, 0x748)):
            raise ValueError('invalid compute local size')
        stage = 'compute'
    else:
        raise ValueError('unknown NVN code header/start')
    key = hashlib.sha1(code).hexdigest()[:16]
    return dict(stage=stage, program_start=hex(start), code_size=hex(total), const_off=hex(const_off),
                const_size=hex(const_size), control_size=len(ctrl), control_sha256=hashlib.sha256(ctrl).hexdigest(),
                code_sha256=hashlib.sha256(code).hexdigest(), code_sha1_16=key,
                code_file=f'code_{key}.bin', ctrl_file=f'ctrl_{key}.bin'), ctrl, code


def extract(a):
    game = a.game.resolve()
    profile = json.loads(a.config.read_text(encoding='utf-8'))
    archives = ARCHIVES.get(profile['title_id'])
    if not archives:
        raise ValueError('no known shader archives for title '+profile['title_id'])
    identities = {}
    for name, expected in profile['modules'].items():
        p = game/'ExeFS'/name
        actual = sha(p)
        with p.open('rb') as f:
            h = f.read(0x60)
        if actual != expected['sha256'] or h[:4] != b'NSO0' or h[0x40:0x60].hex() != expected['build_id']:
            raise ValueError('B3 1.2.0 version mismatch: '+name)
        identities[name] = dict(sha256=actual, build_id=h[0x40:0x60].hex())
    for archive_name, archive_sha in archives.items():
        if sha(game/'RomFS'/archive_name) != archive_sha:
            raise ValueError('unrecognized '+archive_name+' SHA-256')
    # One directory for every archive: translate reads shaders_extracted/root_000, and the runtime looks shaders up
    # by code hash alone, so a hash that appears in two archives is merged like any other duplicate.
    destination = WORK/'shaders_extracted/root_000'
    if destination.exists():
        raise ValueError('extraction destination exists; retain it or choose a new --work')
    destination.mkdir(parents=True)
    decoder = Decoder(a.decoder.resolve())
    rows, rejected, duplicates = [], [], []
    unique = {}
    try:
        for archive_name in archives:
            archive = game/'RomFS'/archive_name
            with archive.open('rb') as f:
                for member in pkz_directory(archive):
                    f.seek(member['offset'])
                    packed = f.read(member['stored'])
                    if member['compression'] == 'None':
                        data = packed
                    elif member['compression'] == 'OodleKraken':
                        data = decoder.decode(packed, member['size'])
                    else:
                        raise ValueError('unsupported compression: '+member['compression'])
                    for name, offset, size in dat_leaves(data, archive_name+'!'+member['name']):
                        if PurePosixPath(name).suffix.lower() not in EXTENSIONS:
                            continue
                        blob = extent(data, offset, size)
                        try:
                            row, ctrl, code = shader_record(blob)
                        except (ValueError, struct.error) as e:
                            rejected.append(dict(source=name, status='unsupported-layout', error=str(e),
                                                 sha256=hashlib.sha256(blob).hexdigest(), bytes=size,
                                                 header=bytes(blob[:64]).hex()))
                            continue
                        row.update(id=str(len(rows)), source=name)
                        key = row['code_sha1_16']
                        if key in unique:
                            old = unique[key]
                            old_ctrl = (destination/old['ctrl_file']).read_bytes()
                            # NVN compiler IDs at +0x7d0 can differ for identical code.
                            # Only merge when every field consumed by runtime/translator agrees.
                            if row['code_sha256'] != old['code_sha256'] or ctrl[0x6f8:0x70c] != old_ctrl[0x6f8:0x70c] or ctrl[0x748:0x758] != old_ctrl[0x748:0x758]:
                                raise ValueError('runtime hash collision or incompatible control for '+key)
                            row['ctrl_file'] = 'ctrl_variant_'+row['control_sha256'][:16]+'.bin'
                            (destination/row['ctrl_file']).write_bytes(ctrl)
                            duplicates.append(row)
                            continue
                        unique[key] = row
                        (destination/row['code_file']).write_bytes(code)
                        (destination/row['ctrl_file']).write_bytes(ctrl)
                        rows.append(row)
                    print(f"extracted {archive_name}!{member['name']}: {len(rows)} unique, {len(rejected)} rejected", flush=True)
    finally:
        decoder.close()
    table(destination/'manifest.tsv', MANIFEST, rows)
    js(WORK/'extraction.json', dict(schema=1, game=str(game), version=profile['version'], title_id=profile['title_id'],
       modules=identities, archives=[dict(path=str(game/'RomFS'/n), sha256=h) for n, h in archives.items()], decoder=dict(path=str(a.decoder.resolve()), sha256=sha(a.decoder)),
       rows=rows, duplicates=duplicates, rejected=rejected,
       stages=dict(collections.Counter(r['stage'] for r in rows)),
       scope='All shader leaves inside '+', '.join(archives)+'. Other PKZ archives and embedded ExeFS shaders not scanned.'))
    print(f'{len(rows)} unique original code blocks, {len(duplicates)} duplicate occurrences, {len(rejected)} rejected layouts')


def env(a):
    e = dict(os.environ, TMPDIR=str(TMP), DOTNET_CLI_HOME=str(TMP/'dotnet-home'),
             DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1', DOTNET_PROCESSOR_COUNT='1',
             DOTNET_GENERATE_ASPNET_CERTIFICATE='false')
    if a.dotnet:
        e['DOTNET_ROOT'] = str(a.dotnet.resolve().parent)
    return e


def harness_cmd(a):
    """How to start the translator: a prebuilt one (--harness, a release's toolset: no .NET SDK needed), else the one
    `build` made, run by dotnet."""
    if a.harness:
        return [str(a.harness)]
    return [str(a.dotnet), str(WORK/'build/bin/Release/net10.0/Ryujinx.Graphics.Shader.dll')]


def build(a):
    if a.harness:
        # A prebuilt translator (tools/build_toolset.py publishes the same project self-contained): nothing to build.
        js(WORK/'build-provenance.json', dict(prebuilt=str(a.harness.resolve()), harness_sha256=sha(a.harness),
           note='Prebuilt ShaderHarness (tools/build_toolset.py): the same sources, published self-contained.'))
        print('Using the prebuilt harness; provenance recorded', flush=True)
        return
    project = ROOT/'tools/bayonetta3_shaders/ShaderHarness.csproj'
    cmd = [str(a.dotnet), 'build', str(project), '-c', 'Release', '-m:1',
           '-p:RyuShader='+str(a.ryu_shader.resolve()), '-p:RyuLib='+str(a.ryu_lib.resolve()),
           '-p:BaseIntermediateOutputPath='+str(WORK/'build/obj')+'/',
           '-p:BaseOutputPath='+str(WORK/'build/bin')+'/']
    with (WORK/'build.log').open('w') as log:
        p = subprocess.run(cmd, env=env(a), stdout=log, stderr=subprocess.STDOUT)
    if p.returncode:
        raise ValueError('harness build failed; see '+str(WORK/'build.log'))
    inputs = sorted(a.ryu_shader.rglob('*.cs'))
    inputs = [p for p in inputs if not {'obj', 'bin'} & set(p.relative_to(a.ryu_shader).parts)]
    inputs += list((ROOT/'tools/bayonetta3_shaders').glob('*.cs'))
    inputs += list((ROOT/'tools/bayonetta3_shaders').glob('*.csproj'))
    inputs += [a.ryu_lib/'Ryujinx.Common.dll', a.ryu_lib/'Spv.Generator.dll']
    js(WORK/'build-provenance.json', dict(command=cmd, dotnet_sdk=subprocess.check_output([str(a.dotnet), '--version'], env=env(a), text=True).strip(),
       inputs=[dict(path=str(p.resolve()), sha256=sha(p)) for p in inputs],
       harness_sha256=sha(WORK/'build/bin/Release/net10.0/Ryujinx.Graphics.Shader.dll'),
       note='Only shader translator source and Ryujinx.Common/Spv.Generator libraries; no blade2 runtime.'))
    print('Built offline harness; provenance recorded', flush=True)


def translate(a):
    extraction = json.loads((WORK/'extraction.json').read_text())
    source = WORK/'shaders_extracted/root_000'
    out = WORK/'spirv'
    raw = WORK/'translation-raw'
    out.mkdir(exist_ok=True)
    raw.mkdir(exist_ok=True)
    if any(out.glob('*.spv')):
        raise ValueError('SPIR-V destination not empty; use a new --work for a new translation')
    rows = extraction['rows']
    for row in rows:
        if sha(source/row['code_file']) != row['code_sha256'] or sha(source/row['ctrl_file']) != row['control_sha256']:
            raise ValueError('extracted code/control hash mismatch: '+row['code_sha1_16'])
    if a.limit:
        rows = rows[:a.limit]
    all_results = []
    # Batches bound the lifetime and memory of the .NET compiler. Timeout kills
    # the whole process; partial outputs are never published without results/val.
    # Batches are independent processes with their own output folders, so --parallel runs several at once; their
    # results are merged in batch order, so the published set does not depend on which finished first.
    def run_batch(number, start):
        batch = rows[start:start+a.batch_size]
        with tempfile.TemporaryDirectory(dir=TMP, prefix='batch-') as temp:
            manifest = Path(temp)/'manifest.tsv'
            table(manifest, MANIFEST, [dict(r, code_file=str(source/r['code_file']), ctrl_file=str(source/r['ctrl_file'])) for r in batch])
            dest = raw/f'{number:04d}'
            dest.mkdir(exist_ok=False)
            cmd = harness_cmd(a) + ['--batch', str(temp), '--out', str(dest), '--jobs', '1', '--debug-mode',
                   '--spirv-val', str(a.spirv_val), '--target-env', 'vulkan1.2']
            with (dest/'process.log').open('w') as log:
                try:
                    p = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, env=env(a), timeout=a.batch_timeout)
                    code = p.returncode
                except subprocess.TimeoutExpired:
                    code = -1
            if code == 0 and (dest/'results.tsv').exists():
                results = list(csv.DictReader((dest/'results.tsv').open(), delimiter='\t'))
            else:
                results = [dict(hash=r['code_sha1_16'], group='root_000', stage=r['stage'], status='process-failure',
                                error=f'batch {number} exit {code}; see translation-raw/{number:04d}/process.log') for r in batch]
        return dest, results

    import concurrent.futures
    starts = list(range(0, len(rows), a.batch_size))
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.parallel)) as pool:
        futures = [pool.submit(run_batch, number, start) for number, start in enumerate(starts)]
        done = 0
        for future in concurrent.futures.as_completed(futures):
            done += 1
            print(f'translated batch {done}/{len(starts)}', flush=True)
        for number, future in enumerate(futures):
            dest, results = future.result()
            for r in results:
                r['group'] = 'root_000'
                if r['status'] in ('ok', 'ok-retry') and r.get('translator_messages', '').strip():
                    r['status'] = 'translator-warning'
                    r['error'] = 'Not published: translator reported incomplete or questionable translation'
                if r['status'] in ('ok', 'ok-retry'):
                    module = dest/(r['hash']+'.spv')
                    shutil.copyfile(module, out/module.name)
                all_results.append(r)
    table(WORK/'results-all.tsv', RESULTS, all_results)
    print(f"translated {len(all_results)}/{len(rows)}: {dict(collections.Counter(r['status'] for r in all_results))}", flush=True)
    good = [r for r in all_results if r['status'] in ('ok', 'ok-retry')]
    table(out/'results.tsv', RESULTS, good)
    js(WORK/'translation-summary.json', dict(total=len(all_results), accepted=len(good),
       statuses=dict(collections.Counter(r['status'] for r in all_results)),
       stages=dict(collections.Counter(r['stage'] for r in good)),
       unsupported_layouts=extraction['rejected'],
       assumptions='Static SMW accessor defaults: 2D normalized RGBA8 textures, float attributes/outputs, triangle topology. Draw-time queries recorded per shader. Not gameplay proof.',
       modules=[dict(hash=r['hash'], sha256=sha(out/(r['hash']+'.spv'))) for r in good]))


def audit(a):
    """Fail closed on translator diagnostics, including warning-producing valid SPIR-V."""
    rows = list(csv.DictReader((WORK/'results-all.tsv').open(), delimiter='\t'))
    quarantine = WORK/'quarantined'
    quarantine.mkdir(exist_ok=True)
    for row in rows:
        if row['status'] in ('ok', 'ok-retry') and row.get('translator_messages', '').strip():
            row['status'] = 'translator-warning'
            row['error'] = 'Not published: translator reported incomplete or questionable translation'
        module = WORK/'spirv'/(row['hash']+'.spv')
        if row['status'] not in ('ok', 'ok-retry') and module.exists():
            module.rename(quarantine/module.name)
    good = [r for r in rows if r['status'] in ('ok', 'ok-retry')]
    table(WORK/'results-all.tsv', RESULTS, rows)
    table(WORK/'spirv/results.tsv', RESULTS, good)
    summary = json.loads((WORK/'translation-summary.json').read_text())
    summary.update(accepted=len(good), statuses=dict(collections.Counter(r['status'] for r in rows)),
                   stages=dict(collections.Counter(r['stage'] for r in good)),
                   modules=[dict(hash=r['hash'], sha256=sha(WORK/'spirv'/(r['hash']+'.spv'))) for r in good])
    js(WORK/'translation-summary.json', summary)
    print('Audited results: '+str(summary['statuses']), flush=True)


def verify(a):
    audit(a)
    compiler = a.cxx or shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        raise ValueError('C++20 compiler required for runtime contract verification')
    cmd = [compiler, *a.cxx_flag, '-std=c++20', '-O2', '-DNX_SHADERS_STANDALONE',
           '-I'+str(ROOT/'runtime/src/nvn'), str(ROOT/'tools/bayonetta3_shaders/verify.cpp'),
           str(ROOT/'runtime/src/nvn/nvn_shaders.cpp'), str(ROOT/'runtime/src/nvn/nvn_spirv_reflect.cpp'),
           '-o', str(WORK/('verify-shaders'+EXE))]
    subprocess.run(cmd, env=env(a), check=True)
    with (WORK/'verification.log').open('w') as log:
        p = subprocess.run([str(WORK/('verify-shaders'+EXE)), str(WORK)], stdout=log, stderr=subprocess.STDOUT, env=env(a))
    if p.returncode:
        raise ValueError('runtime contract verification failed; see verification.log')
    results = list(csv.DictReader((WORK/'spirv/results.tsv').open(), delimiter='\t'))
    for index, r in enumerate(results, 1):
        subprocess.run([str(a.spirv_val), '--target-env', 'vulkan1.2', str(WORK/'spirv'/(r['hash']+'.spv'))], check=True, env=env(a))
        if index % 500 == 0:
            print(f'validated {index}/{len(results)} Vulkan modules', flush=True)
    js(WORK/'verification.json', dict(status='pass', runtime_sources=[dict(path=str(p), sha256=sha(p)) for p in
        [ROOT/'runtime/src/nvn/nvn_shaders.cpp', ROOT/'runtime/src/nvn/nvn_spirv_reflect.cpp', ROOT/'runtime/src/nvn/nvn_shaders.h']],
        modules=len(results), results_sha256=sha(WORK/'spirv/results.tsv'),
        verifier_sha256=sha(WORK/('verify-shaders'+EXE)), report=(WORK/'verification.log').read_text(),
        validator=subprocess.check_output([str(a.spirv_val), '--version'], text=True).strip(),
        scope='Static Vulkan validation and actual runtime shader hash/reflection/resource contract; not rendered gameplay.'))
    print((WORK/'verification.log').read_text(), flush=True)


def publish(a):
    destination = a.publish_dir.resolve()
    if not destination.is_relative_to((ROOT/'private').resolve()) or destination.is_relative_to((ROOT/'private/bayonetta3').resolve()):
        raise ValueError('publish directory must be private and outside managed bayonetta3')
    if destination.exists():
        raise ValueError('publish destination exists; refusing overwrite')
    v = json.loads((WORK/'verification.json').read_text())
    summary = json.loads((WORK/'translation-summary.json').read_text())
    if v['status'] != 'pass' or v['results_sha256'] != sha(WORK/'spirv/results.tsv') or v['modules'] != len(summary['modules']):
        raise ValueError('verification stale or incomplete')
    for m in summary['modules']:
        if sha(WORK/'spirv'/(m['hash']+'.spv')) != m['sha256']:
            raise ValueError('module changed after translation: '+m['hash'])
    with tempfile.TemporaryDirectory(dir=TMP, prefix='publish-') as temp:
        stage = Path(temp)/'bundle'
        stage.mkdir()
        shutil.copytree(WORK/'spirv', stage/'spirv')
        for name in ['extraction.json', 'build-provenance.json', 'translation-summary.json', 'results-all.tsv', 'verification.json', 'verification.log']:
            shutil.copyfile(WORK/name, stage/name)
        if (WORK/'analysis').exists():
            shutil.copytree(WORK/'analysis', stage/'analysis')
        if (WORK/'version-comparison.json').exists():
            shutil.copyfile(WORK/'version-comparison.json', stage/'version-comparison.json')
        with (stage/'SHA256SUMS').open('w') as f:
            for p in sorted(stage.rglob('*')):
                if p.is_file() and p.name != 'SHA256SUMS':
                    f.write(sha(p)+'  '+p.relative_to(stage).as_posix()+'\n')
        destination.parent.mkdir(parents=True, exist_ok=True)
        os.rename(stage, destination)
    print('Published verified shader bundle: '+str(destination), flush=True)


def main():
    global WORK
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['extract', 'build', 'translate', 'verify', 'publish', 'all'])
    p.add_argument('--publish-dir', type=Path, default=ROOT/'private/bayonetta3-shaders')
    p.add_argument('--work', type=Path, default=WORK)
    p.add_argument('--game', type=Path, default=Path(os.environ.get('B3_GAME_ROOT') or ROOT/'private/game'),
                   help='game root (default: B3_GAME_ROOT or private/game)')
    p.add_argument('--config', type=Path, default=ROOT/'config/bayonetta3-1.2.0.json',
                   help='version configuration of the program to extract (config/bayonetta3-1.2.0-prog1.json for program 1)')
    p.add_argument('--cxx', help='C++20 compiler for verify (default: clang++ or g++ on PATH)')
    p.add_argument('--cxx-flag', action='append', default=[],
                   help="an extra flag for that compiler, repeatable (the release toolset's clang: its --sysroot and lld)")
    p.add_argument('--decoder', type=Path, help='path to a compatible Kraken decoder worker (required for extract)')
    p.add_argument('--dotnet', type=Path, default=shutil.which('dotnet'), help='dotnet executable (default: PATH)')
    p.add_argument('--ryu-shader', type=Path, help='Ryujinx.Graphics.Shader source directory (required for build)')
    p.add_argument('--ryu-lib', type=Path, help='directory containing Ryujinx.Common.dll and Spv.Generator.dll (required for build)')
    p.add_argument('--spirv-val', type=Path, default=shutil.which('spirv-val'), help='spirv-val executable (default: PATH)')
    p.add_argument('--harness', type=Path, help='a prebuilt translator executable (a release toolset): build and '
                   'translate then need no .NET SDK')
    p.add_argument('--parallel', type=int, default=max(1, min(8, (os.cpu_count() or 2) // 2)),
                   help='translator batches run at once (default: half the cores, at most 8)')
    p.add_argument('--batch-size', type=int, default=100)
    p.add_argument('--batch-timeout', type=int, default=300)
    p.add_argument('--limit', type=int, default=0)
    a = p.parse_args()
    actions = ['extract', 'build', 'translate', 'verify'] if a.action == 'all' else [a.action]
    if 'extract' in actions and (not a.decoder or not a.decoder.is_file()):
        p.error('extract requires --decoder pointing to a compatible Kraken worker')
    if 'build' in actions and not a.harness:
        if not a.ryu_shader or not a.ryu_shader.is_dir():
            p.error('build requires --ryu-shader pointing to the Ryujinx shader source directory')
        if not a.ryu_lib or any(not (a.ryu_lib / name).is_file()
                                for name in ('Ryujinx.Common.dll', 'Spv.Generator.dll')):
            p.error('build requires --ryu-lib containing Ryujinx.Common.dll and Spv.Generator.dll')
    if a.harness and not a.harness.is_file():
        p.error(f'--harness {a.harness} is not a file')
    for name, needed in (('dotnet', {'build', 'translate'}), ('spirv_val', {'translate', 'verify'})):
        if name == 'dotnet' and a.harness:
            continue
        if needed.intersection(actions):
            value = getattr(a, name)
            executable = shutil.which(str(value)) if value else None
            if not executable:
                p.error(f'{name.replace("_", "-")} not found; pass --{name.replace("_", "-")} or add it to PATH')
            setattr(a, name, Path(executable))
    WORK = a.work.resolve()
    private = (ROOT/'private').resolve()
    if not WORK.is_relative_to(private) or WORK.is_relative_to(private/'bayonetta3'):
        p.error('--work must be private and outside managed private/bayonetta3')
    if a.batch_size < 1 or a.batch_timeout < 1:
        p.error('batch limits must be positive')
    WORK.mkdir(parents=True, exist_ok=True)
    TMP.mkdir(parents=True, exist_ok=True)
    os.environ['TMPDIR'] = str(TMP)
    try:
        for action in (['extract', 'build', 'translate', 'verify'] if a.action == 'all' else [a.action]):
            globals()[action](a)
    except (ValueError, OSError, subprocess.SubprocessError) as e:
        print('ERROR:', e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
