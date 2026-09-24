"""Unit tests for the art pipeline (agk art)."""
import os
import subprocess
import shutil
import tempfile
import unittest

from agk import art
from agk.image import Image, load_png_rgba


def project(files):
    d = tempfile.mkdtemp()
    os.makedirs(os.path.join(d, "art"))
    for name, text in files.items():
        with open(os.path.join(d, "art", name), "w") as f:
            f.write(text)
    return d


SPRITE = """colors
  .  transparent
  A  0xFFF
  B  0xF00
frame
.AB.
AABB
"""


class TextArtTests(unittest.TestCase):
    def test_sprite_planes_and_colours(self):
        d = project({"art.toml": '[s]\nsource = "s.txt"\nkind = "sprite"\n', "s.txt": SPRITE})
        (a,) = art.build(d)
        self.assertEqual(a.sprite_colors, [0xFFF, 0xF00, 0x000])
        # row 0: .AB. -> A=1 (plane 0) at x=1, B=2 (plane 1) at x=2
        self.assertEqual(a.planar()[:2], [0x4000, 0x2000])
        c = open(os.path.join(d, "build", "art", "art.c")).read()
        # colour index 1..3 -> slots 17..19 on channel 0 (regression: was 18..20)
        self.assertIn("pPalette[17] = 0xFFF;", c)
        self.assertIn("pPalette[19] = 0x000;", c)
        self.assertNotIn("pPalette[20]", c)
        self.assertTrue(os.path.exists(os.path.join(d, "build", "art", "preview", "s.png")))

    def test_errors_are_specific(self):
        cases = [
            ("colors\n  . transparent\nframe\n.X\n", "'X' at column 2"),
            ("colors\n  . transparent\nframe\n..\nframe\n...\n", "frame 2"),
            ("frame\n..\n", "start with a 'colors' section"),
        ]
        for text, msg in cases:
            with self.subTest(msg=msg):
                d = project({"art.toml": '[s]\nsource = "s.txt"\n', "s.txt": text})
                with self.assertRaisesRegex(art.ArtError, msg):
                    art.build(d)

    def test_sprite_too_wide(self):
        d = project({"art.toml": '[s]\nsource = "s.txt"\n',
                     "s.txt": "colors\n  A 0xFFF\nframe\n" + "A" * 17 + "\n"})
        with self.assertRaisesRegex(art.ArtError, "16 px"):
            art.build(d)

    def test_too_many_sprite_colours_are_reduced_with_warning(self):
        d = project({"art.toml": '[s]\nsource = "s.txt"\n',
                     "s.txt": "colors\n  A 0xFFF\n  B 0xF00\n  C 0x0F0\n  D 0x00F\nframe\nAAAABBBCCD\n"})
        (a,) = art.build(d)
        self.assertEqual(len(a.colors_used()), 3)
        self.assertIn("reduced", a.warnings[0])

    def test_channel_pair_must_share_colours(self):
        other = SPRITE.replace("0xF00", "0x0F0")
        d = project({"art.toml": '[a]\nsource = "a.txt"\nchannel = 0\n[b]\nsource = "b.txt"\nchannel = 1\n',
                     "a.txt": SPRITE, "b.txt": other})
        with self.assertRaisesRegex(art.ArtError, "share colours 17-19"):
            art.build(d)

    def test_bob_uses_palette_and_mask(self):
        d = project({"palette.txt": "0 0x000\n1 0xF00\n2 0x0F0\n3 0x00F\n",
                     "art.toml": '[e]\nsource = "e.txt"\nkind = "bob"\n',
                     "e.txt": "colors\n  . transparent\n  R 0xF00\n  G 0x0E0\nframe\nRG.\n"})
        (a,) = art.build(d)
        self.assertEqual(a.depth, 2)
        # 0x0E0 isn't in the palette -> nearest (0x0F0, index 2) with a warning
        self.assertTrue(any("0x0E0" in w for w in a.warnings))
        # row: plane0 (R=1) x=0, plane1 (G=2) x=1, mask x=0..1
        self.assertEqual(a.planar(), [0x8000, 0x4000, 0xC000])


class PngTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("magick"), "needs ImageMagick to make test PNGs")
    def test_rgba_and_indexed_pngs(self):
        d = tempfile.mkdtemp()
        for fmt in ("PNG32", "PNG8"):
            p = os.path.join(d, f"{fmt}.png")
            subprocess.run(["magick", "-size", "4x2", "xc:none", "-fill", "#FF0000",
                            "-draw", "point 1,0", "-fill", "#00FF00", "-draw", "point 2,1",
                            f"{fmt}:{p}"], check=True)
            w, h, px = load_png_rgba(p)
            self.assertEqual((w, h), (4, 2))
            self.assertLess(px[0][3], 128, fmt)            # transparent
            self.assertEqual(px[1][:3], (255, 0, 0), fmt)
            self.assertEqual(px[6][:3], (0, 255, 0), fmt)

    def test_rgb_png_roundtrip_through_art(self):
        d = project({"art.toml": '[p]\nsource = "p.png"\nframe_width = 2\n'})
        Image(bytes((255, 255, 255, 255, 0, 0, 0, 0, 0, 255, 255, 255)), 4, 1).save_png(
            os.path.join(d, "art", "p.png"))
        (a,) = art.build(d)
        self.assertEqual(len(a.frames), 2)                 # 4 px wide / 2 per frame
        self.assertEqual(a.frames[0][0], [0xFFF, 0xF00])


if __name__ == "__main__":
    unittest.main()
