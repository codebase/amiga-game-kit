"""expect-scroll: measuring how a region moves between frames (synthetic frames, no emulator)."""
import random
import unittest
from fractions import Fraction

from agk import motion, scenario

W, H = 80, 30


def frame(offset, noise=0, seed=0):
    """A textured layer scrolled left by `offset` game px, as region rows (hires RGB bytes),
    plus `noise` random 'stars' that don't scroll with it."""
    tex = random.Random(1)
    layer = [[tex.choice((0, 0, 0, 40, 90, 200)) for _ in range(W + 64)] for _ in range(H)]
    stars = random.Random(seed)
    rows = []
    for y in range(H):
        px = layer[y][offset:offset + W]
        for _ in range(noise):
            if stars.random() < 0.5:
                px[stars.randrange(W)] = 255
        rows.append(bytes(v for p in px for v in (p, p, p, p, p, p)))   # 2 hires px x RGB
    return rows


class MotionTests(unittest.TestCase):
    def test_smooth_quarter_pixel_scroll_passes(self):
        frames = [frame(n // 4, noise=3, seed=n) for n in range(40)]      # content moves left
        ok, steps, problems = motion.check(frames, Fraction(-1, 4), 0)
        self.assertTrue(ok, problems)
        self.assertEqual([s[0] for s in steps[:8]], [0, 0, 0, -1, 0, 0, 0, -1])

    def test_a_16px_jump_is_caught_and_measured(self):
        offs = [n // 4 for n in range(40)]
        offs[20] += 16                                                   # the skip: out and back
        frames = [frame(o, noise=3, seed=n) for n, o in enumerate(offs)]
        ok, steps, problems = motion.check(frames, Fraction(-1, 4), 0)
        self.assertFalse(ok)
        self.assertIn("frame 20: -17", problems[0])   # 4 -> 5 + 16
        self.assertIn("frame 21: +16", problems[0])

    def test_a_stall_is_caught_by_the_total(self):
        frames = [frame(0) for _ in range(40)]
        ok, _, problems = motion.check(frames, Fraction(-1, 4), 0)
        self.assertFalse(ok)
        self.assertTrue(any("moved +0 px in x over 39 frames, expected -9.75" in p for p in problems))

    def test_holding_still(self):
        frames = [frame(5, noise=2, seed=n) for n in range(10)]
        self.assertTrue(motion.check(frames, 0, 0)[0])

    def test_region_from_a_full_frame(self):
        from agk.image import Image, WIDTH, HEIGHT, SCREEN_X0, SCREEN_Y0
        rgb = bytearray(WIDTH * HEIGHT * 3)
        i = ((SCREEN_Y0 + 10) * WIDTH + SCREEN_X0 + 2 * 5) * 3
        rgb[i:i + 6] = bytes((1, 2, 3, 1, 2, 3))                        # game pixel (5, 10)
        rows = motion.region(Image(bytes(rgb)), 5, 10, 6, 11)
        self.assertEqual(rows[0], bytes((1, 2, 3, 1, 2, 3)) + bytes(6))
        self.assertEqual(len(rows), 2)


class ScenarioTests(unittest.TestCase):
    def test_expands_to_a_screenshot_per_frame(self):
        sc = scenario.parse("expect-scroll 0 30 319 250 4 -1/4\n")
        self.assertEqual(sc.motions, [(0, (0, 30, 319, 250), 4, Fraction(-1, 4), 0, 1)])
        self.assertEqual(sum(l.startswith("agk screenshot") for l in sc.lines), 5)
        self.assertEqual(sc.frames, 4)

    def test_errors(self):
        for line, msg in [("expect-scroll 0 0 10 10 8", "usage"),
                          ("expect-scroll 0 0 400 10 8 1", "inside 320x256"),
                          ("expect-scroll 0 0 10 10 1 1", "frames must be 2-500"),
                          ("expect-scroll 0 0 10 10 8 fast", "px per frame")]:
            with self.assertRaisesRegex(scenario.ScenarioError, msg):
                scenario.parse(line)


if __name__ == "__main__":
    unittest.main()
