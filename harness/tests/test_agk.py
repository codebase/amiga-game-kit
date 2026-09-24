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

    def test_wait_serial_realigns(self):
        sc = scenario.parse('wait-serial "level 2" 100')
        self.assertEqual(sc.lines, ['waitserial "level 2" 100', "wait 1 frames"])

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

    def test_diff_identical(self):
        a = Image(bytes(2 * 2 * 3), 2, 2)
        self.assertEqual(diff(a, a)[:2], (0, None))


if __name__ == "__main__":
    unittest.main()
