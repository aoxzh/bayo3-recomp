#!/usr/bin/env python3
"""GPU-free battle extraction and failed-driver regressions.

Run directly: python3 tests/test_b3_battle.py
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

if __package__:
    from .test_cross_platform_harness import BASH, Skip
else:  # Direct invocation from CTest or the command line.
    from test_cross_platform_harness import BASH, Skip

ROOT = Path(__file__).resolve().parents[1]


def working_bash():
    try:
        return BASH()
    except Skip as exc:
        raise unittest.SkipTest(str(exc)) from exc


def window(fps, upload=65, worker=None):
    """One 60-present window line, optionally followed by the worker line that log_timing prints after it."""
    text = (f'nvn/vk: per frame over 60: {fps:.1f} fps, 1.0 submits, replay 20.0 ms, '
            f'submit (encode) 0.1 ms, fence wait 1.0 ms, other 0.1 ms, '
            f'present 0.1 ms, upload {upload:.1f} MB\n')
    if worker is not None:
        text += (f'nvn/vk: replay thread over 60: 3.0 jobs/frame, busy {worker[0]:.2f} ms/frame, '
                 f'idle {worker[1]:.2f} ms/frame (8% waiting for a job)\n')
    return text


class BattleExtraction(unittest.TestCase):
    def extract(self, rates, upload=65, pool=2400, drive=None, extra=''):
        with tempfile.TemporaryDirectory(prefix='b3-battle-test-') as tmp:
            logs = Path(tmp)
            lines = []
            for fps in rates:
                lines.append(window(fps, upload))
                if pool is not None:
                    lines.append(f'nvn: descriptor pool peak per submit: {pool} sets\n')
            (logs / 'fixture-game.log').write_text(''.join(lines) + extra)
            (logs / 'fixture-drive.log').write_text(drive or '[12:00:00] 60.ppm: BATTLE attack\n')
            env = dict(os.environ, LOGS=str(logs), BATTLE_MIN_WINDOWS='8')
            proc = subprocess.run([working_bash(), str(ROOT / 'scripts/b3_battle.sh'), '--extract', 'fixture'],
                                  env=env, text=True, capture_output=True, timeout=10)
            self.assertEqual(proc.stderr, '')
            return proc

    def test_fast_and_exactly_thirty_windows_remain_in_all_thresholds(self):
        proc = self.extract([29, 30, 31, 35, 40, 45, 50, 60])
        self.assertEqual(proc.returncode, 0, proc.stdout)
        self.assertIn('fps: median 37.5', proc.stdout)
        self.assertIn('battle: OK 8 heavy windows', proc.stdout)
        for threshold in (35, 50, 60):
            self.assertIn(f'upload > {threshold} MB): 8 windows', proc.stdout)

    def test_all_fast_windows_use_wide_fallback(self):
        proc = self.extract([45] * 8, upload=40)
        self.assertEqual(proc.returncode, 0, proc.stdout)
        self.assertIn('fps: median 45.0', proc.stdout)
        self.assertIn('strict (upload > 50 MB): none', proc.stdout)

    def test_light_scene_is_rejected_at_any_fps(self):
        for fps in (20, 60):
            with self.subTest(fps=fps):
                proc = self.extract([fps] * 8, upload=33)
                self.assertEqual(proc.returncode, 1)
                self.assertIn('FAIL 0 battle windows', proc.stdout)

    def test_low_and_missing_descriptor_evidence_are_rejected(self):
        for pool in (941, None):
            with self.subTest(pool=pool):
                proc = self.extract([40] * 8, pool=pool)
                self.assertEqual(proc.returncode, 1)
                self.assertIn('battle: FAIL', proc.stdout)
                self.assertNotIn('battle: OK', proc.stdout)

    def test_freeze_is_rejected_even_with_heavy_workload(self):
        drive = '[12:00:00] 60.ppm: dark-ui (mean 0.6, motion 0.1)\n' * 30
        proc = self.extract([40] * 8, drive=drive)
        self.assertEqual(proc.returncode, 1)
        self.assertIn('FAIL level-load freeze', proc.stdout)

    def test_short_cluster_is_rejected(self):
        proc = self.extract([40] * 7)
        self.assertEqual(proc.returncode, 1)
        self.assertIn('FAIL only 7 heavy windows', proc.stdout)

    def test_runtime_errors_invalidate_the_run(self):
        for marker in ('FATAL test error\n', 'CRASH: test error\n'):
            with self.subTest(marker=marker):
                proc = self.extract([40] * 8, extra=marker)
                self.assertEqual(proc.returncode, 1)
                self.assertIn('battle: FAIL fatal:', proc.stdout)


class CounterCadence(unittest.TestCase):
    """The worker and aux-counter lines belong to the window they are printed with, not the next one."""

    def run_extract(self, lines):
        with tempfile.TemporaryDirectory(prefix='b3-battle-cadence-') as tmp:
            logs = Path(tmp)
            (logs / 'fixture-game.log').write_text(''.join(lines))
            (logs / 'fixture-drive.log').write_text('[12:00:00] 60.ppm: BATTLE attack\n')
            env = dict(os.environ, LOGS=str(logs), BATTLE_MIN_WINDOWS='8')
            proc = subprocess.run([working_bash(), str(ROOT / 'scripts/b3_battle.sh'), '--extract', 'fixture'],
                                  env=env, text=True, capture_output=True, timeout=10)
            self.assertEqual(proc.stderr, '')
            return proc

    def test_worker_line_describes_the_window_before_it(self):
        # Only the first and last windows carry worker figures. A next-line association would give the
        # second..tenth windows the first value and lose the last one, so the medians differ.
        pool = 'nvn: descriptor pool peak per submit: 2400 sets\n'
        lines = [pool, window(30, worker=(100.0, 5.0))]
        for fps in (31, 32, 33, 34, 35, 36, 37, 38):
            lines += [pool, window(fps)]
        lines += [pool, window(39, worker=(0.0, 9.0))]
        proc = self.run_extract(lines)
        self.assertEqual(proc.returncode, 0, proc.stdout)
        self.assertIn('replay-thread load: busy 50.00 ms/frame, idle 7.00 ms/frame', proc.stdout)

    def test_aux_blocks_use_their_real_present_span(self):
        # Windows 1-5 each close a 60-present block with 600 draws and 60 sets; windows 6-10 close one
        # 300-present block with 3000 draws and 300 sets. Both are 10 draws and 60 sets per frame, so a
        # correct normalisation must report 10 -- the old fixed /300 reported the block totals as 2 and 10.
        pool = 'nvn: descriptor pool peak per submit: 2400 sets\n'
        lines, draws, sets = [pool], 0, 0
        for k, fps in enumerate((30, 31, 32, 33, 34), start=1):
            draws += 600
            sets = 60
            lines.append(f'nvn draws: {draws - 600} recorded, {draws} replayed:\n')
            lines.append(f"nvn: descriptor sets: 0 reused from the submit's cache, {sets} made\n")
            lines.append(window(fps))
        for fps in (35, 36, 37, 38):
            lines.append(window(fps))
        draws += 3000
        lines.append(f'nvn draws: {draws - 3000} recorded, {draws} replayed:\n')
        lines.append("nvn: descriptor sets: 0 reused from the submit's cache, 300 made\n")
        lines += [pool, window(39)]
        proc = self.run_extract(lines)
        self.assertEqual(proc.returncode, 0, proc.stdout)
        self.assertIn('cost per draw: replay 20.00 ms / 10 replayed draws per frame', proc.stdout)
        # 60 sets per window (60 presents) is 1 set per frame; the old per-window division said 60.
        self.assertIn('1 descriptor sets made per frame', proc.stdout)

    def test_write_back_wall_handles_legacy_and_normalized_windows(self):
        for normalized in (False, True):
            with self.subTest(normalized=normalized):
                pool = 'nvn: descriptor pool peak per submit: 2400 sets\n'
                lines, draws = [pool], 0
                for k in range(1, 11):
                    if k in (2, 10):
                        presents = 60 if k == 2 else 300
                        factor = 1 if normalized else presents / 60
                        marker = f', window={presents} presents' if normalized else ''
                        lines.append(f'nvn: write-back wall {6 * factor:.2f} ms/frame per batch '
                                     f'(20.00 ranges, {100 * factor:.1f} MB, NX_COPY_THREADS=6{marker})\n')
                    if k <= 5 or k == 10:
                        draws += 600 if k <= 5 else 3000
                        lines.append(f'nvn draws: {draws} recorded, {draws} replayed:\n')
                        lines.append("nvn: descriptor sets: 0 reused from the submit's cache, 60 made\n")
                    lines.extend((pool, window(40)))
                proc = self.run_extract(lines)
                self.assertEqual(proc.returncode, 0, proc.stdout)
                self.assertIn('write-back wall: 6.00 ms/frame (min 6.00 max 6.00, n=2), '
                              'ranges/batch 20.00, 100.0 MB/frame merged', proc.stdout)


class DescriptorThreshold(unittest.TestCase):
    """The "is this the fight" descriptor gate is calibrated per scene, not borrowed from another one."""

    def run_extract(self, pools, min_sets=None):
        with tempfile.TemporaryDirectory(prefix='b3-battle-sets-') as tmp:
            logs = Path(tmp)
            lines = []
            for pool in pools:
                lines.append(f'nvn: descriptor pool peak per submit: {pool} sets\n')
                lines.append(window(40))
            (logs / 'fixture-game.log').write_text(''.join(lines))
            (logs / 'fixture-drive.log').write_text('[12:00:00] 60.ppm: BATTLE attack\n')
            env = dict(os.environ, LOGS=str(logs), BATTLE_MIN_WINDOWS='8')
            env.pop('BATTLE_MIN_SETS', None)
            if min_sets is not None:
                env['BATTLE_MIN_SETS'] = str(min_sets)
            proc = subprocess.run([working_bash(), str(ROOT / 'scripts/b3_battle.sh'), '--extract', 'fixture'],
                                  env=env, text=True, capture_output=True, timeout=10)
            self.assertEqual(proc.stderr, '')
            return proc

    def test_scene_calibrated_threshold_accepts_its_own_fight(self):
        # The Chapter 2 fight peaks at 1,819-1,894 sets: below the Prologue's 2,000 but far above the same
        # run's movie/menu states (24-225). Its own calibrated threshold must accept it.
        proc = self.run_extract([1835] * 8, min_sets=1500)
        self.assertEqual(proc.returncode, 0, proc.stdout)
        self.assertIn('battle: OK 8 heavy windows', proc.stdout)
        self.assertIn('max 1835 sets (fight threshold 1500)', proc.stdout)

    def test_borrowed_threshold_still_rejects_a_light_scene(self):
        # Whatever the calibration, a run that never reaches it is not the fight.
        for min_sets in (None, 1500):
            with self.subTest(min_sets=min_sets):
                proc = self.run_extract([225] * 8, min_sets=min_sets)
                self.assertEqual(proc.returncode, 1)
                self.assertIn('battle: FAIL descriptor pool peak max 225', proc.stdout)
                self.assertNotIn('battle: OK', proc.stdout)


class DriverExit(unittest.TestCase):
    def test_exit_code_and_stderr_survive_the_real_harness(self):
        # A temporary checkout supplies a sleeping CPU-only game and a failing driver. The actual
        # experiment script and common contract execute unchanged, including wait/cleanup/summary.
        with tempfile.TemporaryDirectory(prefix='b3-driver-test-') as tmp:
            root = Path(tmp)
            for rel in ('scripts/lib', 'tools', 'data', 'logs'):
                (root / rel).mkdir(parents=True)
            for rel in ('scripts/b3_experiment.sh', 'scripts/lib/b3_common.sh'):
                shutil.copy2(ROOT / rel, root / rel)
            game = root / 'fake-game'
            game.write_text('#!/usr/bin/env bash\nexec sleep 30\n')
            game.chmod(0o755)
            (root / 'tools/b3_autodrive.py').write_text(
                "import sys\nsys.stderr.write('driver-test traceback sentinel\\n')\nsys.exit(7)\n")
            env = {k: v for k, v in os.environ.items() if not k.startswith(('NX_', 'B3_'))}
            env.update(EXE=str(game), DATA_ROOT=str(root / 'data'), LOGS=str(root / 'logs'),
                       POST='0', PERF='0', HEADED='0', TMPDIR_OVERRIDE=str(root / 'tmp'))
            proc = subprocess.run([working_bash(), str(root / 'scripts/b3_experiment.sh'), 'failure', '1'],
                                  cwd=root, env=env, text=True, capture_output=True, timeout=15)
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertIn('autodrive exited 7', proc.stderr)
            summary = (root / 'logs/failure-summary.txt').read_text()
            self.assertIn('driver exit: 7  stopped by harness: 0', summary)
            self.assertEqual((root / 'logs/failure-drive-stderr.log').read_text(),
                             'driver-test traceback sentinel\n')


if __name__ == '__main__':
    unittest.main()
