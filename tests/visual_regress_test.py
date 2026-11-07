#!/usr/bin/env python3
"""Synthetic tests for tools/visual_regress.py (no game, no network).

    python3 tests/visual_regress_test.py        # or: python3 tools/visual_regress.py --selftest

The same checks are reachable from the tool itself so the Lead can run them in 10 s; this file
keeps them in the test suite as well. It exercises the perceptual metrics, scene matching/order,
threshold math and the capture -> bless -> check roundtrip on synthetic images.
"""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import visual_regress as vr  # noqa: E402


class VisualRegressTest(unittest.TestCase):
    def test_thresholds_identical_and_noise(self):
        cfg = vr._selftest_cfg()
        im = vr._synth_scene("title")
        same = vr.compare_metrics(vr.analyze_rgb(im), vr.analyze_rgb(im), cfg)
        self.assertEqual(same["luma_mae"], 0)
        self.assertEqual(same["chroma_mae"], 0)
        self.assertGreater(same["ssim"], 0.999)
        noisy = vr.compare_metrics(vr.analyze_rgb(im), vr.analyze_rgb(vr._noise(im)), cfg)
        self.assertLess(noisy["luma_mae"], cfg["check"]["thresholds"]["luma_mae_max"])
        self.assertGreaterEqual(noisy["ssim"], cfg["check"]["thresholds"]["ssim_min"])

    def test_shift_and_black_tile_fail(self):
        cfg = vr._selftest_cfg()
        im = vr._synth_scene("arena")
        shifted = vr.compare_metrics(vr.analyze_rgb(im), vr.analyze_rgb(vr._shift(im)), cfg)
        self.assertGreater(shifted["tile_luma_mae"], 0)
        broken = im.copy()
        from PIL import ImageDraw
        ImageDraw.Draw(broken).rectangle([4 * 40, 2 * 36, 4 * 40 + 39, 2 * 36 + 35], fill=(0, 0, 0))
        tile = vr.compare_metrics(vr.analyze_rgb(im), vr.analyze_rgb(broken), cfg)
        self.assertGreater(tile["black_tile_count"], 0)

    def test_selftest_suite(self):
        self.assertTrue(vr.run_selftest(quiet=True))

    def test_selftest_subcommand_exit_code(self):
        """Regression F1: `visual_regress.py selftest` must exit 0 on PASS like `--selftest`."""
        self.assertEqual(vr.main(["selftest", "--quiet"]), 0)
        # a real selftest failure is a non-zero exit, not a traceback
        original = vr.run_selftest
        def boom(quiet=False):
            raise vr.Regression("synthetic failure")
        vr.run_selftest = boom
        try:
            self.assertEqual(vr.main(["selftest", "--quiet"]), 1)
        finally:
            vr.run_selftest = original

    def test_ps1_verify_command_line_contract(self):
        """Accept Windows-style paths in the visual verification command."""
        argv = ["verify", "--label", "windows", "--exe", r"private\build\bayonetta3.exe",
                "--seconds", "120", "--report-dir", r"private\regress\base",
                "--json", r"private\regress\base\visual-report.json", "--quiet",
                "--data-root", "private\run-bayonetta3"]
        args = vr.build_parser().parse_args(argv)
        self.assertEqual(args.command, "verify")
        self.assertEqual(args.label, "windows")
        self.assertEqual(args.seconds, 120.0)
        self.assertTrue(args.quiet)
        self.assertEqual(args.report_dir, r"private\regress\base")

    def test_report_has_fields_the_ps1_reads(self):
        """Report consumers need ran/ok/failures/check.scenes to remain stable."""
        cfg = vr._selftest_cfg()
        title = vr._synth_scene("title")
        store = vr._store_from_images({"title": title}, cfg=cfg)
        with __import__("tempfile").TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            vr._write_frames(tmp / "frames", [(10, title)])
            report = vr.check_frames(tmp / "frames", store, cfg, "selftest", quiet=True)
            vr._write_json(tmp / "visual-report.json", {"ran": True, "ok": report["ok"],
                                                        "failures": report["failures"],
                                                        "check": report})
            import json
            loaded = json.loads((tmp / "visual-report.json").read_text())
            for key in ("ran", "ok", "failures"):
                self.assertIn(key, loaded)
            self.assertIn("scenes", loaded["check"])
            self.assertEqual(loaded["check"]["scenes"][0]["scene"], "title")


if __name__ == "__main__":
    unittest.main(verbosity=2)
