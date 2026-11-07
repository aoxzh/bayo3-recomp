#!/usr/bin/env python3
"""GPU-free saved-frame and input regressions for tutorial/death recognition."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'tests/fixtures/b3_autodrive'
spec = importlib.util.spec_from_file_location('b3_autodrive', ROOT / 'tools/b3_autodrive.py')
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


@unittest.skipUnless((FIXTURES / 'manifest.json').is_file(), 'game-derived image fixtures are not included')
class PromptRecognition(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='b3-prompts-')
        cls.directory = Path(cls.temp.name)
        cls.manifest = json.loads((FIXTURES / 'manifest.json').read_text())
        cls.frames = {}
        # Each compact atlas contains original crops, in manifest order. Rebuild only those
        # regions at their original coordinates; unrelated gameplay pixels are unnecessary.
        for case in cls.manifest['cases']:
            image = Image.new('RGB', tuple(cls.manifest['source_dimensions']))
            with Image.open(FIXTURES / case['fixture']) as atlas:
                y = 0
                for left, top, right, bottom in cls.manifest['regions']:
                    image.paste(atlas.crop((0, y, right - left, y + bottom - top)), (left, top))
                    y += bottom - top
            path = cls.directory / case['fixture']
            image.save(path)
            cls.frames[Path(case['fixture']).stem] = path

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_saved_tutorial_combat_cutscene_and_death_regions(self):
        for case in self.manifest['cases']:
            with self.subTest(case=case['fixture']):
                path = self.frames[Path(case['fixture']).stem]
                self.assertEqual(driver.visible_prompt(path), case['expected'])

    def test_prompt_classification_precedes_generic_scene_rules(self):
        for name, expected in [('clouds-close', 'tutorial-close'), ('death', 'death')]:
            with self.subTest(name=name):
                self.assertEqual(driver.classify(self.frames[name])[0], expected)

    def test_prompt_scales_with_frame_resolution(self):
        for name, expected in [('clouds-close-later', 'tutorial-close'), ('death', 'death')]:
            for scale in (0.75, 1.5):
                with self.subTest(name=name, scale=scale):
                    with Image.open(self.frames[name]) as original:
                        image = original.resize((round(1600 * scale), round(900 * scale)))
                    path = self.directory / 'scaled.png'
                    image.save(path)
                    self.assertEqual(driver.visible_prompt(path), expected)

    def test_close_footer_requires_both_tutorial_borders(self):
        for removed in ((0, 120, 1600, 140), (0, 760, 1600, 780)):
            with self.subTest(removed=removed):
                with Image.open(self.frames['clouds-close']) as original:
                    image = original.copy()
                image.paste((0, 0, 0), removed)
                path = self.directory / 'without-border.png'
                image.save(path)
                self.assertIsNone(driver.visible_prompt(path))

    def drive_one_frame(self, frame):
        class Clock:
            now = 0.0

            def time(self):
                return self.now

            def sleep(self, seconds):
                self.now += seconds

        clock = Clock()
        with tempfile.TemporaryDirectory(prefix='b3-drive-prompt-') as root:
            args = ['b3_autodrive.py', root, '--timeout', '1', '--every', '2',
                    '--settle', '0.1', '--a-only-until', '0']
            output = io.StringIO()
            with mock.patch.object(driver.sys, 'argv', args), \
                 mock.patch.object(driver.time, 'time', clock.time), \
                 mock.patch.object(driver.time, 'sleep', clock.sleep), \
                 mock.patch.object(driver, 'latest_frame', side_effect=[None, (61, frame)]), \
                 mock.patch.object(driver, 'write_live') as write_live, \
                 contextlib.redirect_stdout(output):
                self.assertEqual(driver.main(), 0)
            buttons = [call.args[1] for call in write_live.call_args_list]
            return buttons, output.getvalue()

    def test_tutorial_is_closed_on_first_observation(self):
        buttons, output = self.drive_one_frame(self.frames['clouds-close'])
        self.assertEqual(buttons, ['B', '', ''])
        self.assertIn('TUTORIAL: visible B Close prompt', output)
        self.assertNotIn('static scene', output)
        self.assertNotIn('BATTLE:', output)

    def test_death_is_acknowledged_and_never_fought(self):
        buttons, output = self.drive_one_frame(self.frames['death'])
        self.assertEqual(buttons, ['A', '', ''])
        self.assertIn('DEATH: The Witch Hunts Are Over', output)
        self.assertNotIn('BATTLE:', output)
    def test_combat_hud_scales_and_precedes_generic_scene_rules(self):
        for name in ('boss-combat', 'combat-opening', 'combat-low-health', 'combat-many-enemies'):
            for scale in (0.75, 1.0, 1.5):
                with self.subTest(name=name, scale=scale):
                    with Image.open(self.frames[name]) as original:
                        image = original.resize((round(1600 * scale), round(900 * scale)))
                    path = self.directory / 'combat-scaled.png'
                    image.save(path)
                    self.assertEqual(driver.classify(path)[0], 'combat')

    def test_combat_requires_both_health_and_text_regions(self):
        for name, text_regions in (
                ('boss-combat', [(1360, 85, 1540, 145)]),
                ('combat-opening', [(50, 345, 230, 400)]),
                ('combat-low-health', [(1360, 85, 1540, 145), (50, 345, 230, 400)])):
            for removed in ([(50, 10, 380, 85)], text_regions):
                with self.subTest(name=name, removed=removed):
                    with Image.open(self.frames[name]) as original:
                        image = original.copy()
                    for box in removed:
                        image.paste((0, 0, 0), box)
                    path = self.directory / 'partial-hud.png'
                    image.save(path)
                    self.assertIsNone(driver.visible_prompt(path))

    def drive_movie_frames(self, names, counts, timeout):
        class Clock:
            now = 1000.0

            def time(self):
                return self.now

            def sleep(self, seconds):
                self.now += seconds

        clock = Clock()
        frames = [None] + [(60 * (i + 1), self.frames[name]) for i, name in enumerate(names)]
        with tempfile.TemporaryDirectory(prefix='b3-drive-movie-') as root:
            args = ['b3_autodrive.py', root, '--timeout', str(timeout), '--every', '2',
                    '--settle', '0.1', '--a-only-until', '0', '--game-log', 'unused.log',
                    '--movie-frames', '0.1', '--movie-skip-after', '1']
            output = io.StringIO()
            with mock.patch.object(driver.sys, 'argv', args), \
                 mock.patch.object(driver.time, 'time', clock.time), \
                 mock.patch.object(driver.time, 'sleep', clock.sleep), \
                 mock.patch.object(driver, 'latest_frame', side_effect=frames), \
                 mock.patch.object(driver, 'movie_line_count', side_effect=counts), \
                 mock.patch.object(driver, 'movie_fill_count', side_effect=counts), \
                 mock.patch.object(driver, 'last_fill_file', return_value='ev0025'), \
                 mock.patch.object(driver, 'keep_movie_frame', return_value='kept'), \
                 mock.patch.object(driver, 'classify', wraps=driver.classify) as classify, \
                 mock.patch.object(driver, 'fight', side_effect=lambda live, seconds: clock.sleep(seconds)) as fight, \
                 mock.patch.object(driver, 'write_live') as write_live, \
                 contextlib.redirect_stdout(output):
                self.assertEqual(driver.main(), 0)
                self.assertEqual(classify.call_count, len(names))
            buttons = [call.args[1] for call in write_live.call_args_list]
            return buttons, fight.call_args_list, output.getvalue()

    def test_first_combat_frame_overrides_movie_hold(self):
        buttons, fights, output = self.drive_movie_frames(['combat-opening'], [12], 1)
        self.assertEqual(len(fights), 1)
        self.assertEqual(fights[0].args[1], 5)
        self.assertEqual([button for button in buttons if button], [])
        self.assertIn('visible combat overrides movie hold', output)
        self.assertIn('BATTLE: visible health and combat HUD', output)
        self.assertEqual(buttons[-1], '')

    def test_movie_prompts_override_hold_without_fighting(self):
        for name, kind, button in (('clouds-close', 'tutorial-close', 'B'), ('death', 'death', 'A')):
            with self.subTest(kind=kind):
                buttons, fights, output = self.drive_movie_frames([name], [12], 1)
                self.assertEqual(fights, [])
                self.assertEqual([pressed for pressed in buttons if pressed], [button])
                self.assertIn(f'visible {kind} overrides movie hold', output)
                self.assertEqual(buttons[-1], '')

    def test_falling_then_saturated_movie_count_does_not_hide_combat(self):
        buttons, fights, output = self.drive_movie_frames(
            ['cutscene', 'control-cutscene', 'boss-combat'], [12, 11, 11], 6)
        self.assertEqual(len(fights), 1)
        self.assertEqual([button for button in buttons if button], [])
        self.assertIn('visible combat overrides movie hold', output)

    def test_hud_precedes_due_movie_skip(self):
        buttons, fights, output = self.drive_movie_frames(
            ['cutscene', 'control-cutscene', 'boss-combat'], [10, 11, 12], 6)
        self.assertEqual(len(fights), 1)
        self.assertEqual([button for button in buttons if button], [])
        self.assertNotIn('PLUS, DOWN, A to skip it', output)

    def test_true_cutscene_and_menu_stay_suppressed_under_stale_movie_counts(self):
        buttons, fights, output = self.drive_movie_frames(
            ['cutscene', 'chapter-select', 'enemy-intro'], [12, 11, 11], 6)
        self.assertEqual(fights, [])
        self.assertEqual([button for button in buttons if button], [])
        self.assertNotIn('overrides movie hold', output)
        self.assertEqual(buttons[-1], '')


class SyntheticRegressions(unittest.TestCase):
    """Driver checks that do not use the excluded game-derived images."""

    def test_uniform_flash_and_black_frame_are_not_prompts(self):
        with tempfile.TemporaryDirectory(prefix='b3-synthetic-') as tmp:
            for value in (0, 255):
                path = Path(tmp) / 'blank.png'
                Image.new('RGB', (1600, 900), (value, value, value)).save(path)
                self.assertIsNone(driver.visible_prompt(path))

    def test_fight_cycle_uses_real_dodge_and_stops_at_five_seconds(self):
        now = 0.0

        def sleep(seconds):
            nonlocal now
            now += seconds

        with mock.patch.object(driver.time, 'time', lambda: now), \
             mock.patch.object(driver.time, 'sleep', sleep), \
             mock.patch.object(driver, 'write_live') as write_live:
            driver.fight('unused', driver.FIGHT_BURST_SECONDS)
        buttons = [call.args[1] for call in write_live.call_args_list]
        first_cycle = [button for button in buttons if button][:20]
        self.assertEqual(len(first_cycle), 20)
        self.assertEqual(first_cycle.count('ZR'), 4)
        self.assertEqual(first_cycle.count('B'), 1)
        self.assertEqual([button for button in first_cycle if button not in ('ZR', 'B')],
                         ['X', 'X', 'A', 'X', 'A', 'Y', 'UP', 'X', 'X', 'A', 'X', 'A', 'RIGHT', 'X', 'Y'])
        self.assertAlmostEqual(now, 5.0)
        self.assertEqual(buttons[-1], '')

    def test_interrupted_fight_releases_held_button(self):
        with mock.patch.object(driver.time, 'sleep', side_effect=KeyboardInterrupt), \
             mock.patch.object(driver, 'write_live') as write_live:
            with self.assertRaises(KeyboardInterrupt):
                driver.fight('unused', 5)
        self.assertTrue(write_live.call_args_list[0].args[1])
        self.assertEqual(write_live.call_args_list[-1].args[1], '')


if __name__ == '__main__':
    unittest.main()
