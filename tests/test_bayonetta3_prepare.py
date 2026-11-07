"""Local validation and transactional publication tests; no game dump required."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from contextlib import nullcontext
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('prepare_bayonetta3', ROOT / 'tools/prepare_bayonetta3.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class ValidationTests(unittest.TestCase):
    def test_current_interpreter_is_the_default(self):
        self.assertEqual(prepare.DEFAULT_PYTHON, Path(os.sys.executable))

    def test_migration_requires_exact_previous_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = Path(tmp) / 'config.json'
            config.touch()
            cfg = {'provenance': {'original_source': 'redacted', 'source_sha256': 'expected'}}
            previous = {'inputs': {'config_sha256': prepare.LEGACY_CONFIG_SHA256},
                        'provenance': {'source': '/previous/config.json', 'source_sha256': 'wrong'}}
            args = argparse.Namespace(config=config, output=Path(tmp) / 'out', legacy_source=None)
            with patch.object(prepare, 'output_lock', return_value=nullcontext()),                     patch.object(prepare, 'load_config', return_value=cfg),                     patch.object(prepare, 'owned_manifest', return_value=previous):
                with self.assertRaisesRegex(ValueError, 'original provenance mismatch'):
                    prepare.migrate_config_source(args)
                args.legacy_source = '/previous/config.json'
                with self.assertRaisesRegex(ValueError, 'archive SHA256 mismatch'):
                    prepare.migrate_config_source(args)

    def test_archived_config_is_portable_and_hash_checked(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cfg = json.loads(prepare.CONFIG.read_text())
            cfg['provenance']['original_source'] = '/absent/old-project/config.json'
            config = root / 'bayonetta3.json'
            prepare.write_json(config, cfg)
            archive = root / cfg['provenance']['source']
            archive.write_bytes((prepare.CONFIG.parent / cfg['provenance']['source']).read_bytes())
            self.assertEqual(prepare.load_config(config), cfg)
            archive.write_bytes(archive.read_bytes() + b' ')
            with self.assertRaisesRegex(ValueError, 'source SHA256 mismatch'):
                prepare.load_config(config)

    def test_wrong_version_and_build_id_rejected_independently(self):
        raw = bytearray(0x100)
        raw[:4] = b'NSO0'
        raw[0x40:0x60] = bytes(range(32))
        expected = {'sha256': hashlib.sha256(raw).hexdigest(), 'build_id': raw[0x40:0x60].hex()}
        prepare.validate_identity(raw, expected, 'main')
        modified = raw.copy()
        modified[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            prepare.validate_identity(modified, expected, 'main')
        with self.assertRaisesRegex(ValueError, 'build ID mismatch'):
            prepare.validate_identity(raw, {**expected, 'build_id': '00' * 32}, 'main')

    def test_bss_overlap_and_window_overflow_are_rejected(self):
        base = prepare.WINDOW_BASE
        modules = {
            'main': {'base': base, 'segments': {'text': {'offset': 0, 'size': 0x1000},
                                               'bss': {'offset': 0x1000, 'size': 0x1000}}},
            'subsdk0': {'base': base + 0x2000, 'segments': {'text': {'offset': 0, 'size': 0x1000}}},
        }
        self.assertEqual(len(prepare.validate_layout(modules)), 3)  # Adjacent extents are valid.
        modules['main']['segments']['bss']['size'] += 1
        with self.assertRaisesRegex(ValueError, 'Layout overlap'):
            prepare.validate_layout(modules)
        modules['main']['segments']['bss']['size'] = prepare.WINDOW_SIZE
        with self.assertRaisesRegex(ValueError, 'outside 512 MiB'):
            prepare.validate_layout(modules)

    def test_npdm_wrong_title_and_resource_size(self):
        raw = bytearray(0x300)
        raw[:4] = b'META'
        struct.pack_into('<IIII', raw, 0x70, 0x80, 0x40, 0xc0, 0x240)
        raw[0x80:0x84] = b'ACI0'
        raw[0x2c0:0x2c4] = b'ACID'
        title = 0x01004a4010fea000
        struct.pack_into('<Q', raw, 0x90, title)
        struct.pack_into('<QQ', raw, 0x2d0, title, title)
        cfg = {'title_id': f'{title:016x}', 'npdm_system_resource_size': 0}
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'main.npdm'
            path.write_bytes(raw)
            self.assertEqual(prepare.inspect_npdm(path, cfg)['system_resource_size'], 0)
            struct.pack_into('<Q', raw, 0x90, title + 1)
            path.write_bytes(raw)
            with self.assertRaisesRegex(ValueError, 'title ID mismatch'):
                prepare.inspect_npdm(path, cfg)
            struct.pack_into('<Q', raw, 0x90, title)
            struct.pack_into('<I', raw, 0x14, 0x1000)
            path.write_bytes(raw)
            with self.assertRaisesRegex(ValueError, 'resource size mismatch'):
                prepare.inspect_npdm(path, cfg)


class PublicationTests(unittest.TestCase):
    def test_reuse_unchanged_generated_files_preserves_inodes_without_stale_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            old, new = Path(tmp) / 'old', Path(tmp) / 'new'
            old.mkdir()
            new.mkdir()
            for name in ('funcs.h', 'table.cpp', 'code_000.cpp'):
                (old / name).write_text('identical')
                (new / name).write_text('identical')
                os.utime(old / name, ns=(1000000000, 1000000000))
            (old / 'code_001.cpp').write_text('old behavior')
            (new / 'code_001.cpp').write_text('new behavior')
            (old / 'code_999.cpp').write_text('stale')
            (new / 'report.txt').write_text('new report')
            prepare.reuse_unchanged_files(old, new)
            for name in ('funcs.h', 'table.cpp', 'code_000.cpp'):
                self.assertEqual((old / name).stat().st_ino, (new / name).stat().st_ino)
                self.assertEqual((new / name).stat().st_mtime_ns, 1000000000)
            self.assertNotEqual((old / 'code_001.cpp').stat().st_ino, (new / 'code_001.cpp').stat().st_ino)
            self.assertEqual((old / 'code_001.cpp').read_text(), 'old behavior')
            self.assertEqual((new / 'code_001.cpp').read_text(), 'new behavior')
            self.assertFalse((new / 'code_999.cpp').exists())
            self.assertEqual((new / 'report.txt').read_text(), 'new report')

    def test_unowned_and_modified_files_are_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'valuable').write_text('user data')
            with self.assertRaisesRegex(ValueError, 'ownership manifest'):
                prepare.owned_manifest(root)
            manifest = {'owner': prepare.OWNER, 'status': 'complete', 'outputs': prepare.inventory(root)}
            prepare.write_json(root / 'manifest.json', manifest)
            self.assertEqual(prepare.owned_manifest(root), manifest)
            (root / 'valuable').write_text('changed user data')
            with self.assertRaisesRegex(ValueError, 'modified or unowned'):
                prepare.owned_manifest(root)
            self.assertEqual((root / 'valuable').read_text(), 'changed user data')

    def test_generation_failure_keeps_complete_output_and_inspection_is_repeatable(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            exefs, romfs = root / 'exefs', root / 'romfs'
            exefs.mkdir()
            romfs.mkdir()
            (romfs / 'marker').write_bytes(b'romfs')
            for name in (*prepare.BASES, 'main.npdm'):
                (exefs / name).write_bytes(name.encode())
            config = root / 'config.json'
            config.write_text('{}')
            cfg = {'version': '1.2.0', 'title_id': '01004a4010fea000', 'romfs_marker': 'marker',
                   'provenance': {}, 'runtime_modules': list(prepare.BASES),
                   'modules': {name: {} for name in prepare.BASES}}
            npdm = {'source': str(exefs / 'main.npdm'), 'sha256': prepare.digest(exefs / 'main.npdm'), 'title_id': cfg['title_id'],
                    'system_resource_size': 0}
            def inspect(path, name, expected):
                return ({'source': str(path), 'sha256': prepare.digest(path), 'base': prepare.BASES[name],
                         'segments': {'text': {'offset': 0, 'size': 16}},
                         'dt_init': {'offset': 0x30}, 'symbols': {'total': 0}, 'relocations': {'total': 0}}, b'raw image')
            args = argparse.Namespace(config=config, exefs=exefs, romfs=romfs, output=root / 'output',
                                      generate=False, module=None, python=Path(os.sys.executable))
            with patch.object(prepare, 'load_config', return_value=cfg), \
                    patch.object(prepare, 'inspect_npdm', return_value=npdm), \
                    patch.object(prepare, 'inspect_module', side_effect=inspect):
                first = prepare.prepare(args)
                before = (args.output / 'manifest.json').read_bytes()
                stamp = (args.output / 'manifest.json').stat().st_mtime_ns
                self.assertEqual(prepare.prepare(args), first)
                self.assertEqual((args.output / 'manifest.json').stat().st_mtime_ns, stamp)
                args.generate = True
                venv_python = root / 'venv/bin/python'
                venv_python.parent.mkdir(parents=True)
                try:
                    venv_python.symlink_to(os.sys.executable)
                    linked = True
                except OSError:  # Windows without the symlink privilege
                    venv_python, linked = Path(os.sys.executable), False
                args.python = venv_python
                def fail(source, destination, name, python):
                    self.assertEqual(python, venv_python.absolute())
                    if linked:
                        self.assertNotEqual(python, venv_python.resolve())
                    destination.mkdir(parents=True)
                    (destination / 'table.cpp').write_text('partial')
                    raise subprocess.CalledProcessError(1, ['mock-recomp'])
                with patch.object(prepare, 'generate_module', side_effect=fail):
                    with self.assertRaises(subprocess.CalledProcessError):
                        prepare.prepare(args)
                self.assertEqual((args.output / 'manifest.json').read_bytes(), before)
                self.assertEqual(prepare.owned_manifest(args.output), first)
                self.assertFalse(list(root.glob('.output.stage-*')))

                # Emulate a complete legacy generation, then migrate provenance only.
                cfg['provenance'] = {'source': 'archive.json', 'original_source': '/old/config.json',
                                     'source_sha256': 'archive-digest'}
                table = args.output / 'gen/main/table.cpp'
                table.parent.mkdir(parents=True)
                table.write_text('complete generated table')
                old = json.loads(before)
                old['inputs']['config_sha256'] = prepare.LEGACY_CONFIG_SHA256
                old['provenance'] = {'source': '/old/config.json', 'source_sha256': 'archive-digest'}
                old['generated'] = {'main': {'status': 'complete'}}
                old['outputs'] = prepare.inventory(args.output)
                prepare.write_json(args.output / 'manifest.json', old)
                inode, stamp = table.stat().st_ino, table.stat().st_mtime_ns
                migrated = prepare.migrate_config_source(args)
                self.assertEqual(migrated['outputs'], old['outputs'])
                self.assertEqual(migrated['generated'], old['generated'])
                self.assertEqual((table.stat().st_ino, table.stat().st_mtime_ns), (inode, stamp))
                self.assertEqual(migrated['inputs']['config_sha256'], prepare.digest(config))
                with self.assertRaisesRegex(ValueError, 'exact legacy configuration'):
                    prepare.migrate_config_source(args)
                # An unknown old config must never be silently accepted.
                old['inputs']['config_sha256'] = '0' * 64
                prepare.write_json(args.output / 'manifest.json', old)
                with self.assertRaisesRegex(ValueError, 'exact legacy configuration'):
                    prepare.migrate_config_source(args)

    def test_publish_keeps_a_complete_tree_on_every_platform(self):
        """Linux exchanges atomically; the macOS fallback moves the old tree aside.

        The invariant is the same either way: after a failed publish the previous complete tree is
        still at <final>, and after a successful one <final> is the new tree with no leftovers.
        """
        class FakeLibc:
            def __init__(self, has_renameat2):
                if has_renameat2:
                    self.renameat2 = lambda *args: -1  # simulate a refused exchange

        with tempfile.TemporaryDirectory() as tmp:
            final, stage = Path(tmp) / 'final', Path(tmp) / 'stage'
            final.mkdir()
            stage.mkdir()
            (final / 'data').write_text('previous complete')
            (stage / 'data').write_text('new complete')

            # Exchange path: the kernel refuses; the previous tree must stay exactly where it was.
            with patch.object(prepare, 'libc', return_value=FakeLibc(True)):
                with self.assertRaises(OSError):
                    prepare.publish(stage, final)
            self.assertEqual((final / 'data').read_text(), 'previous complete')
            self.assertEqual((stage / 'data').read_text(), 'new complete')

            # Fallback path (macOS has no renameat2): swap, then delete the old tree.
            with patch.object(prepare, 'libc', return_value=FakeLibc(False)):
                prepare.publish(stage, final)
            self.assertEqual((final / 'data').read_text(), 'new complete')
            self.assertFalse((Path(tmp) / 'final.publish-old').exists())
            self.assertFalse(stage.exists())

    def test_output_lock_is_exclusive(self):
        with tempfile.TemporaryDirectory() as tmp:
            lock = Path(tmp) / '.out.prepare.lock'
            with prepare.output_lock(lock):
                with self.assertRaisesRegex(OSError, 'another preparation'):
                    with prepare.output_lock(lock):
                        pass
            with prepare.output_lock(lock):  # released
                pass

    def test_links_are_recorded_and_never_followed(self):
        """The RomFS/ExeFS links (symlinks, or junctions on Windows) are inventoried as links, and replacing an
        output tree unlinks them without touching the game files behind them."""
        with tempfile.TemporaryDirectory() as tmp:
            game, out = Path(tmp) / 'RomFS', Path(tmp) / 'out'
            game.mkdir()
            (game / 'root_000.pkz').write_bytes(b'game data')
            (out / 'extract').mkdir(parents=True)
            prepare.link_dir(out / 'extract/romfs', game)
            self.assertTrue(prepare.is_link(out / 'extract/romfs'))
            listed = prepare.inventory(out)
            self.assertEqual(set(listed), {'extract', 'extract/romfs'})
            self.assertIn('symlink', listed['extract/romfs'])
            stage = Path(tmp) / 'stage'
            stage.mkdir()
            (stage / 'data').write_text('new')
            with patch.object(prepare, 'libc', return_value=None):
                prepare.publish(stage, out)
            self.assertEqual((out / 'data').read_text(), 'new')
            self.assertEqual((game / 'root_000.pkz').read_bytes(), b'game data')

if __name__ == '__main__':
    unittest.main()
