"""Fast unit tests for the agk harness (no emulator needed).

    PYTHONPATH=harness python3 -m unittest discover -s harness/tests
"""
import os
import tempfile
import unittest

from agk import scenario
from agk.image import Image, diff


class ScenarioTests(unittest.TestCase):
    def test_press_holds_then_releases(self):
        sc = scenario.parse("press right 20")
        self.assertEqual(sc.lines, ["joystick2 pull right", "wait 20 frames", "joystick2 release x"])
        self.assertEqual(sc.frames, 20)

    def test_combo_and_default_duration(self):
        sc = scenario.parse("press up+fire")
        self.assertEqual(sc.lines, ["joystick2 pull up", "joystick2 press 1", "wait 1 frames",
                                    "joystick2 release y", "joystick2 unpress 1"])

    def test_hold_is_released_at_end(self):
        sc = scenario.parse("hold left\nwait 3")
        self.assertEqual(sc.lines[-1], "joystick2 release x")

    def test_release_all(self):
        sc = scenario.parse("hold left\nhold fire\nrelease\nwait 1")
        self.assertIn("joystick2 unpress 1", sc.lines)
        self.assertEqual(sc.lines[-1], "wait 1 frames")

    def test_screenshot_and_expects(self):
        sc = scenario.parse('screenshot a\nexpect-serial "x=1"\nexpect-no-serial "ERR"')
        self.assertEqual(sc.screenshots, ["a"])
        self.assertEqual(sc.lines, ["agk screenshot {out}/a.raw"])
        self.assertEqual([(p, m) for p, m, _ in sc.expects], [("x=1", True), ("ERR", False)])

    def test_wait_serial_is_hex_encoded(self):
        # '=' would otherwise be parsed as a key=value argument by RetroShell
        sc = scenario.parse('wait-serial "score=1" 100')
        self.assertEqual(sc.lines, ["waitserial hex:73636f72653d31 100", scenario.REALIGN])

    def test_expect_color(self):
        sc = scenario.parse("screenshot a\nexpect-color a 10 20 0xFA0")
        self.assertEqual(sc.colors, [("a", 10, 20, (15, 10, 0), 2)])
        for bad, msg in [("screenshot a\nexpect-color b 1 1 0x000", "unknown screenshot"),
                         ("screenshot a\nexpect-color a 320 1 0x000", "inside the 320x256"),
                         ("screenshot a\nexpect-color a 1 1 0x1000", "12-bit")]:
            with self.subTest(bad=bad):
                with self.assertRaisesRegex(scenario.ScenarioError, msg):
                    scenario.parse(bad)

    def test_origins_map_generated_lines_to_source(self):
        sc = scenario.parse("press right 2\n\n# c\nhold left\nscreenshot s\nrelease")
        self.assertEqual(len(sc.origins), len(sc.lines))
        self.assertEqual(sc.origins, [1, 1, 1, 4, 5, 6])

    def test_dump_mem_bounds(self):
        with self.assertRaisesRegex(scenario.ScenarioError, "24-bit"):
            scenario.parse("dump-mem big 0 0x7FFFFFFF")

    def test_regs_and_dump(self):
        sc = scenario.parse("regs r cpu copper\ndump-mem chip 0 0x80000")
        self.assertEqual(sc.lines, ["debugger", "r cpu", "r copper", "commander",
                                    "mem save bin {out}/chip.bin 0 524288"])

    def test_comments_and_blank_lines(self):
        sc = scenario.parse("# hi\n\nwait 2   # trailing\n")
        self.assertEqual(sc.lines, ["wait 2 frames"])

    def test_errors_name_the_line(self):
        for text, msg in [("jump 3", "unknown command"), ("press sideways", "unknown input"),
                          ("wait x", "must be a number"), ("screenshot a\nscreenshot a", "used twice"),
                          ("screenshot ../x", "may only use"), ('expect-serial "("', "bad regex"),
                          ("regs r gpu", "unknown component"), ("key 0x80", "0x00-0x7F")]:
            with self.subTest(text=text):
                with self.assertRaisesRegex(scenario.ScenarioError, msg):
                    scenario.parse(text)


class SyncTests(unittest.TestCase):
    def test_frames_mode_realigns_after_wait_serial(self):
        from agk.runner import _sync_lines
        lines = scenario.parse('press right 3\nwait-serial "x"').lines
        self.assertEqual(_sync_lines(lines, "frames"),
                         ["joystick2 pull right", "wait 3 frames", "joystick2 release x",
                          "waitserial hex:78 500", "wait 1 frames"])

    def test_ticks_mode_counts_game_frames(self):
        from agk.runner import _sync_lines
        lines = scenario.parse('press right 3\nwait-serial "x"\nwait 2').lines
        self.assertEqual(_sync_lines(lines, "ticks"),
                         ["joystick2 pull right", "wait 3 ticks", "joystick2 release x",
                          "waitserial hex:78 500", "wait 2 ticks"])


class GoldenTests(unittest.TestCase):
    """--update: first profile of a run defines the shared golden, later
    profiles get an override only where they differ."""

    def _shot(self, d, name, rgb):
        p = os.path.join(d, name + ".png")
        Image(rgb, 2, 1).save_png(p)
        return {"screenshots": {"s": {"png": p}}, "ok": True, "failures": [], "outdir": d}

    def test_update_rules(self):
        from agk.cli import _check_goldens
        with tempfile.TemporaryDirectory() as d:
            gdir = os.path.join(d, "golden")
            red, blue = b"\xff\0\0" * 2, b"\0\0\xff" * 2
            refreshed = set()
            _check_goldens(self._shot(d, "a", red), gdir, "p1", True, refreshed)
            _check_goldens(self._shot(d, "b", red), gdir, "p2", True, refreshed)
            _check_goldens(self._shot(d, "c", blue), gdir, "p3", True, refreshed)
            self.assertTrue(os.path.exists(os.path.join(gdir, "s.png")))
            self.assertFalse(os.path.exists(os.path.join(gdir, "p2")))
            self.assertTrue(os.path.exists(os.path.join(gdir, "p3", "s.png")))
            # A new run where everything changed rewrites the shared golden, not overrides
            refreshed = set()
            for prof in ("p1", "p2"):
                _check_goldens(self._shot(d, prof, blue), gdir, prof, True, refreshed)
            self.assertEqual(Image.load_png(os.path.join(gdir, "s.png")).rgb, blue)
            self.assertFalse(os.path.exists(os.path.join(gdir, "p1")))
            # Checking (no update): p3's override is used, p1 matches shared
            res = self._shot(d, "x", blue)
            _check_goldens(res, gdir, "p3", False, set())
            self.assertTrue(res["ok"])
            res = self._shot(d, "y", red)
            _check_goldens(res, gdir, "p1", False, set())
            self.assertFalse(res["ok"])
            self.assertEqual(res["screenshots"]["s"]["golden"]["pixels"], 2)


class ImageTests(unittest.TestCase):
    def test_png_roundtrip(self):
        rgb = bytes((x * 7 + y * 13) % 256 for y in range(5) for x in range(12))
        img = Image(rgb, 4, 5)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "t.png")
            img.save_png(p)
            self.assertEqual(Image.load_png(p).rgb, rgb)

    def test_diff_bbox(self):
        a = Image(bytes(4 * 3 * 3), 4, 3)
        b_rgb = bytearray(a.rgb)
        b_rgb[(1 * 4 + 2) * 3] = 255          # pixel (2,1)
        b_rgb[(2 * 4 + 3) * 3 + 1] = 9        # pixel (3,2)
        count, bbox, dimg = diff(a, Image(bytes(b_rgb), 4, 3))
        self.assertEqual((count, bbox), (2, (2, 1, 3, 2)))
        self.assertEqual(dimg.pixel(2, 1), (255, 0, 0))

    def test_canonical12(self):
        # vAmiga shows OCS colour nibble n as n*16; canonical form is n*17
        img = Image(bytes((0xF0, 0xA0, 0x00, 0x10, 0x10, 0x30)), 2, 1).canonical12()
        self.assertEqual(img.rgb, bytes((255, 170, 0, 17, 17, 51)))

    def test_diff_identical(self):
        a = Image(bytes(2 * 2 * 3), 2, 2)
        self.assertEqual(diff(a, a)[:2], (0, None))


if __name__ == "__main__":
    unittest.main()
