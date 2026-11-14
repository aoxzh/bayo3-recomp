#!/usr/bin/env python3
"""Validate local Bayonetta 3 1.2.0 inputs and publish inspect/full generation outputs.

Use the current Python environment with requirements-bayonetta3.txt installed. No downloads or patches.
Inspection is the default; --generate emits complete module tables (never --only). Runs on Linux, macOS and native
Windows: the lock is flock() or a Windows byte-range lock, and the RomFS/ExeFS links are symlinks or, where Windows
does not grant the symlink privilege, directory junctions.
"""
import argparse
from collections import Counter
from contextlib import contextmanager
import ctypes
try:
    import fcntl
except ImportError:  # Windows
    fcntl = None
    import msvcrt
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_GAME = Path(os.environ.get('B3_GAME_ROOT') or ROOT / 'private/game')
DEFAULT_PYTHON = Path(sys.executable)
CONFIG = ROOT / 'config/bayonetta3-1.2.0.json'
OWNER = 'Bayonetta3Recomp/tools/prepare_bayonetta3.py:v1'
BASES = {'main': 0x4000000000, 'subsdk0': 0x4008000000, 'sdk': 0x4010000000}
WINDOW_BASE, WINDOW_SIZE = 0x4000000000, 512 * 1024 * 1024
LEGACY_CONFIG_SHA256 = '19fd35045b073cb3f74f26757536fd35632c9ac59e92843f2971a06cd2a94294'


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')


@contextmanager
def output_lock(path):
    """Take a nonblocking exclusive lock until publication or migration is complete; a crash releases it."""
    with path.open('a') as lock:
        try:
            if fcntl is not None:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            else:
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError as exc:
            raise OSError(exc.errno, f'another preparation holds {path}') from exc
        try:
            yield
        finally:
            if fcntl is None:
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)


def is_link(path):
    """A symlink, or a Windows directory junction: recorded as a link, never followed or recursed into."""
    if path.is_symlink():
        return True
    try:
        st = os.lstat(path)
    except FileNotFoundError:
        return False
    return getattr(st, 'st_reparse_tag', 0) == getattr(stat, 'IO_REPARSE_TAG_MOUNT_POINT', -1)


def link_dir(link, target):
    """A directory link: a symlink where the host allows one, else (Windows without the symlink privilege, which
    needs Developer Mode or elevation) a junction, which needs neither."""
    try:
        link.symlink_to(target, target_is_directory=True)
    except OSError:
        if os.name != 'nt':
            raise
        import _winapi
        _winapi.CreateJunction(str(target), str(link))


def libc():
    """The C library, for renameat2; None on Windows, where ctypes.CDLL(None) does not exist."""
    return None if os.name == 'nt' else ctypes.CDLL(None, use_errno=True)


def load_config(path):
    cfg = json.loads(path.read_text(encoding='utf-8'))
    source_name = Path(cfg['provenance']['source'])
    if source_name.is_absolute() or '..' in source_name.parts:
        raise ValueError('Version configuration source must be an archive relative to config')
    source = path.parent / source_name
    if digest(source) != cfg['provenance']['source_sha256']:
        raise ValueError('Version configuration source SHA256 mismatch')
    reference = json.loads(source.read_text(encoding='utf-8'))
    for key in ('title_id', 'version', 'runtime_modules', 'romfs_marker'):
        if cfg[key] != reference[key]:
            raise ValueError(f'Version configuration differs from source: {key}')
    # Iterate the modules the configuration actually ships: a program may have fewer than the three
    # of the main game (B3's second program, 01004a4010fea001, is main+sdk with no subsdk0).
    for name in cfg['runtime_modules']:
        if name not in BASES:
            raise ValueError(f'Unknown module in runtime_modules: {name}')
        if name not in cfg['modules'] or name not in reference['modules']:
            raise ValueError(f'Version configuration has no record for module {name}')
        for key in ('sha256', 'build_id'):
            if cfg['modules'][name][key] != reference['modules'][name][key]:
                raise ValueError(f'Version configuration differs from source: {name}.{key}')
        if int(cfg['modules'][name]['base'], 0) != BASES[name]:
            raise ValueError(f'Unexpected base for {name}')
    if int(cfg['window_base'], 0) != WINDOW_BASE or cfg['window_size'] != WINDOW_SIZE:
        raise ValueError('Unexpected guest window')
    return cfg


def validate_identity(raw, expected, name):
    if len(raw) < 0x100 or raw[:4] != b'NSO0':
        raise ValueError(f'{name}: invalid NSO header')
    if hashlib.sha256(raw).hexdigest() != expected['sha256']:
        raise ValueError(f'{name}: SHA256 mismatch; expected original version 1.2.0 NSO')
    if raw[0x40:0x60].hex() != expected['build_id']:
        raise ValueError(f'{name}: build ID mismatch')


def validate_layout(modules, window_base=WINDOW_BASE, window_size=WINDOW_SIZE):
    ranges = []
    for name, record in modules.items():
        base = record['base']
        for kind, segment in record['segments'].items():
            offset, size = segment['offset'], segment['size']
            start, end = base + offset, base + offset + size
            if offset < 0 or size < 0 or start < window_base or end > window_base + window_size:
                raise ValueError(f'{name}.{kind}: outside 512 MiB guest window')
            if size:
                ranges.append((start, end, f'{name}.{kind}'))
    ranges.sort()
    for left, right in zip(ranges, ranges[1:]):
        if left[1] > right[0]:
            raise ValueError(f'Layout overlap: {left[2]} and {right[2]}')
    return [{'start': a, 'end': b, 'segment': n} for a, b, n in ranges]


def inspect_npdm(path, cfg):
    raw = path.read_bytes()
    if len(raw) < 0x80 or raw[:4] != b'META':
        raise ValueError('Invalid NPDM META header')
    resource_size = struct.unpack_from('<I', raw, 0x14)[0]
    if resource_size != cfg['npdm_system_resource_size']:
        raise ValueError(f'NPDM system resource size mismatch: {resource_size:#x}')
    if resource_size % 0x1000 or resource_size > WINDOW_SIZE:
        raise ValueError('Invalid NPDM system resource size')
    aci, aci_size, acid, acid_size = struct.unpack_from('<IIII', raw, 0x70)
    for off, size, minimum in ((aci, aci_size, 0x40), (acid, acid_size, 0x240)):
        if off < 0x80 or size < minimum or off + size > len(raw):
            raise ValueError('NPDM access-control extent outside file')
    if max(aci, acid) < min(aci + aci_size, acid + acid_size):
        raise ValueError('NPDM access-control extents overlap')
    if raw[aci:aci + 4] != b'ACI0' or raw[acid + 0x200:acid + 0x204] != b'ACID':
        raise ValueError('Invalid NPDM ACI0/ACID magic')
    title = struct.unpack_from('<Q', raw, aci + 0x10)[0]
    low, high = struct.unpack_from('<QQ', raw, acid + 0x210)
    if title != int(cfg['title_id'], 16) or not low <= title <= high:
        raise ValueError('NPDM title ID mismatch or outside ACID range')
    return {'source': str(path), 'sha256': hashlib.sha256(raw).hexdigest(),
            'size': len(raw), 'title_id': f'{title:016x}',
            'system_resource_size': resource_size, 'system_resource_size_offset': '0x14',
            'acid_title_range': [f'{low:016x}', f'{high:016x}'],
            'signature_verified': False}


def inspect_module(path, name, expected):
    sys.path.insert(0, str(ROOT)) if str(ROOT) not in sys.path else None
    from recomp import nso
    raw = path.read_bytes()
    validate_identity(raw, expected, name)
    flags = struct.unpack_from('<I', raw, 0x0c)[0]
    header_segments = {}
    for i, kind in enumerate(('text', 'ro', 'data')):
        file_off, offset, size = struct.unpack_from('<III', raw, 0x10 + 16 * i)
        stored = struct.unpack_from('<I', raw, 0x60 + 4 * i)[0]
        if file_off < 0x100 or file_off + stored > len(raw) or offset + size > WINDOW_SIZE:
            raise ValueError(f'{name}.{kind}: invalid NSO extent')
        if not flags & (1 << i) and stored != size:
            raise ValueError(f'{name}.{kind}: uncompressed segment size mismatch')
        header_segments[kind] = {'offset': offset, 'size': size}
    module = nso.load(str(path), name)
    for i, kind in enumerate(('text', 'ro', 'data')):
        offset, size = getattr(module, kind)
        if header_segments[kind] != {'offset': offset, 'size': size}:
            raise ValueError(f'{name}.{kind}: decompressed size mismatch')
        if flags & (1 << (i + 3)):
            actual = hashlib.sha256(module.image[offset:offset + size]).digest()
            if actual != raw[0xa0 + 32 * i:0xc0 + 32 * i]:
                raise ValueError(f'{name}.{kind}: segment SHA256 mismatch')
    segments = {kind: {'offset': getattr(module, kind)[0], 'size': getattr(module, kind)[1]}
                for kind in ('text', 'ro', 'data', 'bss')}
    base = BASES[name]
    init = module.dynamic.get(nso.DT_INIT)
    if init is not None and not module.in_text(init):
        raise ValueError(f'{name}: DT_INIT outside text')
    if any(r.sym >= len(module.symbols) for r in module.relocs):
        raise ValueError(f'{name}: invalid relocation symbol index')
    reloc_types = Counter(str(r.type) for r in module.relocs)
    entries = [{'name': s.name, 'offset': s.value, 'address': base + s.value}
               for s in module.symbols if not s.imported and s.type == nso.STT_FUNC
               and (s.name in ('main', '_start', '_init', '_fini', 'nnMain') or 'nnMain' in s.name)]
    record = {'source': str(path), 'sha256': expected['sha256'], 'build_id': expected['build_id'],
              'source_size': len(raw), 'base': base, 'segments': segments,
              'nso_header_bss_size': struct.unpack_from('<I', raw, 0x3c)[0],
              'image_size': len(module.image), 'image_sha256': hashlib.sha256(module.image).hexdigest(),
              'image_semantics': 'Original decompressed segments and zero padding; no relocations or patches; BSS described separately.',
              'dt_init': {'present': init is not None, 'offset': init,
                          'address': base + init if init is not None else None,
                          'bytes': module.image[init:init + 16].hex() if init is not None else None,
                          'authority': 'observed dynamic tag; execution not tested'},
              'entries': {'nso_text_entry_offset': module.text[0], 'named_function_symbols': entries,
                          'init_array': module.init_array, 'fini_array': module.fini_array},
              'symbols': {'total': len(module.symbols),
                          'imports': sum(s.imported for s in module.symbols),
                          'defined_functions': sum(not s.imported and s.type == nso.STT_FUNC for s in module.symbols)},
              'relocations': {'total': len(module.relocs), 'by_type': dict(sorted(reloc_types.items())),
                              'applied_to_analysis_image': False}}
    return record, module.image


def inventory(root):
    result = {}

    def walk(directory):
        for path in sorted(directory.iterdir()):
            rel = path.relative_to(root).as_posix()
            if rel == 'manifest.json':
                continue
            if is_link(path):
                result[rel] = {'symlink': os.readlink(path)}
            elif path.is_dir():
                result[rel] = {'directory': True}
                walk(path)
            elif path.is_file():
                result[rel] = {'sha256': digest(path), 'size': path.stat().st_size}
            else:
                raise ValueError(f'Unsupported output object: {path}')

    walk(root)
    return result


def owned_manifest(root):
    if not os.path.lexists(root):
        return None
    if is_link(root) or not root.is_dir() or (root / 'manifest.json').is_symlink():
        raise ValueError(f'Refusing unowned output: {root}')
    try:
        manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
    except (OSError, ValueError) as exc:
        raise ValueError(f'Refusing output without valid ownership manifest: {root}') from exc
    if manifest.get('owner') != OWNER or manifest.get('status') != 'complete':
        raise ValueError(f'Refusing unowned/incomplete output: {root}')
    if manifest.get('outputs') != inventory(root):
        raise ValueError(f'Refusing modified or unowned output files: {root}')
    return manifest


def publish(stage, final):
    """Publish `stage` as `final`, keeping a complete tree until the swap succeeds.

    Linux uses renameat2(RENAME_EXCHANGE): the swap is atomic and a failure leaves the previous tree
    in place. macOS and Windows have neither renameat2 nor RENAME_EXCHANGE, so the fallback moves the
    old tree aside, renames the new one in and only then deletes the old one. That is not atomic, but
    the caller holds the output lock, and a failure still leaves exactly one complete tree behind.
    The links in the old tree are unlinked, never followed (shutil.rmtree treats junctions as links).
    """
    if not os.path.lexists(final):
        stage.rename(final)
        return
    rename = getattr(libc(), 'renameat2', None)
    if rename is not None:
        rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
        rename.restype = ctypes.c_int
        if rename(-100, os.fsencode(stage), -100, os.fsencode(final), 2):
            err = ctypes.get_errno()
            raise OSError(err, os.strerror(err), str(final))
        # The old owned tree is now at stage and removed by TemporaryDirectory.
        return
    backup = final.parent / (final.name + '.publish-old')
    if os.path.lexists(backup):
        shutil.rmtree(backup)
    final.rename(backup)
    try:
        stage.rename(final)
    except OSError:
        backup.rename(final)  # put the previous complete tree back before propagating
        raise
    shutil.rmtree(backup)


def generate_module(source, destination, name, python, patch_files=()):
    extra = ['--patch', *map(str, patch_files)] if patch_files else []
    subprocess.run([str(python), '-m', 'recomp', str(source), str(destination),
                    '--name', name, '--base', hex(BASES[name]), *extra], cwd=ROOT, check=True)
    required = ('funcs.h', 'table.cpp', 'report.txt')
    if any(not (destination / f).is_file() or not (destination / f).stat().st_size for f in required):
        raise ValueError(f'{name}: incomplete generated output')
    if not list(destination.glob('code_*.cpp')):
        raise ValueError(f'{name}: no generated translation units')


def reuse_unchanged_files(previous, generated):
    """Run only after emission: preserve identical files without writing into old inodes."""
    if not previous.is_dir():
        return
    for new in sorted(generated.rglob('*')):
        old = previous / new.relative_to(generated)
        if (new.is_file() and not new.is_symlink() and old.is_file() and not old.is_symlink()
                and new.stat().st_size == old.stat().st_size and digest(new) == digest(old)):
            new.unlink()
            os.link(old, new)


def migrate_config_source(args):
    """Explicit provenance-only migration; never replaces directories or generated files."""
    cfg = load_config(args.config.resolve(strict=True))
    final = args.output.absolute()
    with output_lock(final.parent / f'.{final.name}.prepare.lock'):
        previous = owned_manifest(final)
        if not previous or previous['inputs']['config_sha256'] != LEGACY_CONFIG_SHA256:
            raise ValueError('Source migration requires the exact legacy configuration SHA256')
        legacy_source = getattr(args, 'legacy_source', None) or cfg['provenance']['original_source']
        if previous['provenance']['source'] != legacy_source:
            raise ValueError('Source migration original provenance mismatch; supply the exact previous source with --legacy-source')
        if previous['provenance']['source_sha256'] != cfg['provenance']['source_sha256']:
            raise ValueError('Source migration archive SHA256 mismatch')
        if (previous['version'], previous['title_id']) != (cfg['version'], cfg['title_id']):
            raise ValueError('Source migration version/title mismatch')
        npdm = inspect_npdm(Path(previous['npdm']['source']), cfg)
        modules = {}
        for name in BASES:
            modules[name], _ = inspect_module(Path(previous['modules'][name]['source']), name, cfg['modules'][name])
        layout = validate_layout(modules)
        if (modules != previous['modules'] or npdm != previous['npdm'] or layout != previous['layout']
                or previous['window'] != {'base': WINDOW_BASE, 'size': WINDOW_SIZE}):
            raise ValueError('Source migration input/layout observations changed')
        if (previous['inputs']['modules'] != {n: m['sha256'] for n, m in modules.items()}
                or previous['inputs']['npdm'] != npdm['sha256']):
            raise ValueError('Source migration manifest input hashes disagree')
        romfs = Path(previous['inputs']['romfs_source'])
        if not romfs.is_dir() or not (romfs / cfg['romfs_marker']).is_file():
            raise ValueError('Source migration RomFS missing')
        migrated = json.loads(json.dumps(previous))
        migrated['inputs']['config_sha256'] = digest(args.config)
        migrated['provenance'] = cfg['provenance']
        migrated['source_migration'] = {
            'kind': 'verified-provenance-only', 'from_config_sha256': LEGACY_CONFIG_SHA256,
            'to_config_sha256': migrated['inputs']['config_sha256'],
            'archive_sha256': cfg['provenance']['source_sha256'],
            'generated_outputs_preserved': True}
        # Only the manifest changes. Existing compiler readers keep the same gen paths/inodes.
        with tempfile.TemporaryDirectory(prefix='.bayonetta3-migrate-', dir=final.parent) as tmp:
            pending = Path(tmp) / 'manifest.json'
            write_json(pending, migrated)
            if owned_manifest(final) != previous:
                raise ValueError('Output changed during source migration')
            os.replace(pending, final / 'manifest.json')
    print(f'Configuration source migrated; all generated files preserved: {final / "manifest.json"}')
    return migrated


def prepare(args):
    cfg = load_config(args.config.resolve(strict=True))
    exefs = args.exefs.resolve(strict=True)
    romfs = args.romfs.resolve(strict=True)
    if not romfs.is_dir() or not (romfs / cfg['romfs_marker']).is_file():
        raise ValueError('RomFS directory/marker missing')
    npdm = inspect_npdm(exefs / 'main.npdm', cfg)
    modules, images = {}, {}
    for name in cfg['runtime_modules']:
        modules[name], images[name] = inspect_module(exefs / name, name, cfg['modules'][name])
    layout = validate_layout(modules)
    # Code patches: the same ones go into the image the runtime loads (analysis/<name>.bin) and into the code generated
    # from it, so a module's data and its compiled code always agree. Recorded per module as `code_patches`.
    sys.path.insert(0, str(ROOT)) if str(ROOT) not in sys.path else None
    from recomp import patches as code_patches
    patch_files = code_patches.find_files(args.mods) if getattr(args, 'mods', None) else []
    patched = {}
    for name in images:
        module_id = code_patches.build_id((exefs / name).read_bytes()[:0x60])
        pats = code_patches.patches_for(module_id, patch_files)
        problems = code_patches.apply(images[name], pats)
        if problems:
            raise ValueError('code patch: ' + '; '.join(problems))
        patched[name] = code_patches.fingerprint(module_id, patch_files)
        for f, records in pats:
            print(f'{name}: code patch {f.name} ({len(records)} writes)')
    # POSIX keys, so a manifest written on Windows names the recompiler files as one written elsewhere does.
    recompiler = {p.relative_to(ROOT).as_posix(): digest(p) for p in sorted((ROOT / 'recomp').glob('*.py'))}
    inputs = {'config_sha256': digest(args.config), 'npdm': npdm['sha256'],
              'modules': {n: m['sha256'] for n, m in modules.items()}, 'romfs_source': str(romfs)}
    final = args.output.absolute()
    final.parent.mkdir(parents=True, exist_ok=True)
    if is_link(final):
        raise ValueError('Output root must not be a symlink')
    # An advisory lock serializes this producer; a crash releases it automatically.
    with output_lock(final.parent / f'.{final.name}.prepare.lock'):
        previous = owned_manifest(final)
        if previous and previous['inputs'] != inputs:
            raise ValueError('Existing output has different inputs; choose a fresh --output')
        with tempfile.TemporaryDirectory(prefix=f'.{final.name}.stage-', dir=final.parent) as tmp:
            stage = Path(tmp)
            (stage / 'analysis').mkdir()
            for name, image in images.items():
                (stage / 'analysis' / f'{name}.bin').write_bytes(image)
            (stage / 'extract').mkdir()
            link_dir(stage / 'extract/romfs', romfs)
            link_dir(stage / 'extract/exefs', exefs)
            generated = dict(previous.get('generated', {})) if previous else {}
            if previous and (final / 'gen').exists():
                shutil.copytree(final / 'gen', stage / 'gen', copy_function=os.link)
            if args.generate:
                for name in args.module or cfg['runtime_modules']:
                    out = stage / 'gen' / name
                    if out.exists():
                        shutil.rmtree(out)
                    # Preserve the venv entrypoint symlink: resolving it loses pyvenv.cfg discovery.
                    generate_module(exefs / name, out, name, args.python.absolute(), patch_files)
                    if previous:
                        reuse_unchanged_files(final / 'gen' / name, out)
                    generated[name] = {'status': 'complete', 'source_sha256': modules[name]['sha256'],
                                       'recompiler_sha256': recompiler, 'code_patches': patched[name],
                                       'command': [str(args.python.absolute()), '-m', 'recomp', str(exefs / name),
                                                   str(final / 'gen' / name), '--name', name, '--base', hex(BASES[name])]}
            # A module's image and its generated code must carry the same code patches.
            stale = [n for n, g in generated.items() if n in patched and g.get('code_patches', '') != patched[n]]
            if stale:
                raise ValueError(f'the code patches changed for {", ".join(stale)}: generate it again '
                                 f'(--generate {" ".join("--module " + n for n in stale)})')
            # Detect changing inputs before publication; failed work never changes final.
            if any(digest(exefs / n) != m['sha256'] for n, m in modules.items()) or digest(exefs / 'main.npdm') != npdm['sha256']:
                raise ValueError('Inputs changed during preparation')
            if any(digest(ROOT / p) != h for p, h in recompiler.items()):
                raise ValueError('Recompiler changed during preparation; retry with stable sources')
            manifest = {'owner': OWNER, 'schema_version': 1, 'status': 'complete',
                        'version': cfg['version'], 'title_id': cfg['title_id'],
                        'provenance': cfg['provenance'], 'inputs': inputs,
                        'window': {'base': WINDOW_BASE, 'size': WINDOW_SIZE},
                        'npdm': npdm, 'modules': modules, 'layout': layout, 'generated': generated,
                        'romfs': {'source': str(romfs), 'marker': cfg['romfs_marker'],
                                  'content_hashed': False, 'storage': 'local symlink'},
                        'generation_complete': set(generated) == set(BASES),
                        'outputs': inventory(stage)}
            write_json(stage / 'manifest.json', manifest)
            # Refuse concurrent external edits even if they do not use our lock.
            if owned_manifest(final) != previous:
                raise ValueError('Output changed during preparation')
            if previous != manifest:
                publish(stage, final)
    for name, m in modules.items():
        print(f"{name}: SHA256/build ID OK; base={m['base']:#x}; DT_INIT={m['dt_init']['offset']:#x}; "
              f"symbols={m['symbols']['total']}; relocations={m['relocations']['total']}")
    print(f"NPDM: title={npdm['title_id']}; system_resource_size={npdm['system_resource_size']:#x}")
    print(f'Complete manifest: {final / "manifest.json"}')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--inspect-only', action='store_true', help='Validate and publish unrelocated analysis images (default)')
    mode.add_argument('--generate', action='store_true', help='Generate complete C++ modules after validation')
    mode.add_argument('--migrate-config-source', action='store_true',
                      help='Verify and migrate the known legacy manifest to the archived config; preserve gen')
    parser.add_argument('--module', action='append', choices=tuple(BASES), help='Module to generate; repeatable; default all')
    parser.add_argument('--exefs', type=Path, default=DEFAULT_GAME / 'ExeFS', help='Default: $B3_GAME_ROOT/ExeFS or private/game/ExeFS')
    parser.add_argument('--romfs', type=Path, default=DEFAULT_GAME / 'RomFS', help='Default: $B3_GAME_ROOT/RomFS or private/game/RomFS')
    parser.add_argument('--config', type=Path, default=CONFIG)
    parser.add_argument('--output', type=Path, default=ROOT / 'private/bayonetta3')
    parser.add_argument('--python', type=Path, default=DEFAULT_PYTHON, help='Python for generation (default: current interpreter)')
    parser.add_argument('--mods', type=Path, help="The data root's mods folder: the code patches (.ips, .pchtxt) of "
                        'its switched-on mods are applied to the images and the generated code (recomp/patches.py)')
    parser.add_argument('--legacy-source', help='Exact original source path from the old manifest, for --migrate-config-source')
    args = parser.parse_args()
    if args.module and not args.generate:
        parser.error('--module requires --generate; inspection always validates the complete layout')
    if args.legacy_source and not args.migrate_config_source:
        parser.error('--legacy-source requires --migrate-config-source')
    try:
        if args.migrate_config_source:
            migrate_config_source(args)
        else:
            prepare(args)
    except (OSError, ValueError, KeyError, struct.error, subprocess.CalledProcessError) as exc:
        parser.exit(1, f'Preparation failed: {exc}\n')


if __name__ == '__main__':
    main()
