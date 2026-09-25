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


class CleanTests(unittest.TestCase):
    def test_holes_fragments_crop_fade(self):
        # 8x6: a solid block on the bottom rows with a hole inside, a 1-px
        # fragment floating above, and an opening in the top row that is not a hole
        T, G, R = (0, 0, 0, 0), (0, 255, 0, 255), (255, 0, 0, 255)
        rows = ["........",
                ".R......",
                "..GG....",
                ".GG.GG..",
                "GGGGGGGG",
                "GGGGGGGG"]
        px = [{".": T, "G": G, "R": R}[c] for row in rows for c in row]
        d = tempfile.mkdtemp()
        src, dst = os.path.join(d, "in.png"), os.path.join(d, "out.png")
        art.write_rgba_png(src, 8, 6, px)
        art.clean_png(src, dst, fill_holes=True, despeckle=3)
        w, h, out = load_png_rgba(dst)
        self.assertEqual(out[1 * 8 + 1][3], 0)              # fragment dropped
        self.assertEqual(out[3 * 8 + 3], G)                 # hole filled from a neighbour
        self.assertEqual(out[3 * 8 + 6][3], 0)              # open to the sky: kept
        art.clean_png(src, dst, crop=(2, 6), fade_bottom=(2, 0xFFF))
        w, h, out = load_png_rgba(dst)
        self.assertEqual((w, h), (8, 4))
        last = out[3 * 8:]
        self.assertTrue(any(p == (255, 255, 255, 255) for p in last))   # mist dithered in
        self.assertTrue(any(p == G for p in last))
        self.assertTrue(all(p[3] == 0 for p in out[1 * 8 + 6:1 * 8 + 8]))  # transparent stays


if __name__ == "__main__":
    unittest.main()


class RetroDiffusionOfflineTests(unittest.TestCase):
    """The parts of agk art-gen that don't need the network or a key."""

    def test_palette_png_is_one_pixel_per_colour(self):
        import base64
        from agk import rd
        png = base64.b64decode(rd.palette_png([0xF00, 0x0F0, 0x00F]))
        d = tempfile.mkdtemp()
        p = os.path.join(d, "pal.png")
        with open(p, "wb") as f:
            f.write(png)
        w, h, px = load_png_rgba(p)
        self.assertEqual((w, h), (3, 1))
        self.assertEqual([c[:3] for c in px], [(255, 0, 0), (0, 255, 0), (0, 0, 255)])

    def test_add_to_art_toml_once(self):
        import tomllib
        from agk import rd
        d = project({"art.toml": "[art]\ndepth = 3\n"})
        art_dir = os.path.join(d, "art")
        self.assertTrue(rd.add_to_art_toml(art_dir, "slime", "slime.png", "bob"))
        self.assertFalse(rd.add_to_art_toml(art_dir, "slime", "slime.png", "bob"))
        with open(os.path.join(art_dir, "art.toml"), "rb") as f:
            cfg = tomllib.load(f)
        self.assertEqual(cfg["slime"], {"source": "slime.png", "kind": "bob"})

    def test_key_from_credentials_file(self):
        from agk import rd
        d = tempfile.mkdtemp()
        cred = os.path.join(d, "credentials")
        with open(cred, "w") as f:
            f.write("OTHER=x\nRD_API_KEY=rdpk-test\n")
        old_env, old_path = os.environ.pop("RD_API_KEY", None), rd.CREDENTIALS
        try:
            rd.CREDENTIALS = cred
            self.assertEqual(rd.api_key(), "rdpk-test")
            rd.CREDENTIALS = os.path.join(d, "missing")
            with self.assertRaisesRegex(rd.GenError, "RD_API_KEY"):
                rd.api_key()
        finally:
            rd.CREDENTIALS = old_path
            if old_env is not None:
                os.environ["RD_API_KEY"] = old_env


class BitmapTests(unittest.TestCase):
    def test_wrap_x_and_auto_palette(self):
        text = ("colors\n  .  transparent\n  A 0xF00\n  B 0x00F\n  C 0x0F0\nframe\n"
                "AB..............\nC...............\n")
        d = project({"art.toml": '[band]\nsource = "b.txt"\nkind = "bitmap"\ndepth = 2\n'
                                 'palette = "auto"\nwrap_x = 4\n', "b.txt": text})
        (a,) = art.build(d)
        self.assertEqual((a.w, a.loop_w), (20, 16))          # 16 + 4 wrapped columns
        self.assertEqual(a.frames[0][0][16:18], [0xF00, 0x00F])  # start repeated at the right
        self.assertEqual(a.own_palette[0], 0x000)            # 0 = transparent slot
        self.assertEqual(sorted(a.own_palette.values())[1:], sorted([0xF00, 0x00F, 0x0F0]))
        h = open(os.path.join(d, "build", "art", "art.h")).read()
        self.assertIn("ART_BAND_LOOP_W 16", h)
        self.assertIn("g_pArtBandPalette[4]", h)
        # No mask words for a bitmap: 2 planes x 2 words per row
        self.assertEqual(len(a.planar()), 2 * 2 * 2)

    def test_auto_palette_reduces_to_depth(self):
        cols = ["0xF00", "0x0F0", "0x00F", "0xFF0", "0x0FF"]
        text = "colors\n" + "".join(f"  {chr(65 + i)} {c}\n" for i, c in enumerate(cols)) + "frame\nABCDE\n"
        d = project({"art.toml": '[b]\nsource = "b.txt"\nkind = "bitmap"\ndepth = 2\npalette = "auto"\n',
                     "b.txt": text})
        (a,) = art.build(d)
        self.assertEqual(len(a.own_palette), 4)               # 0 + 3 colours for 2 planes
        self.assertTrue(any("reduced to 3" in w for w in a.warnings))


class AttachedSpriteTests(unittest.TestCase):
    def test_attached_bits_and_mirror(self):
        # 17 px wide -> 2 columns; colour index 5 = 0b0101 -> even part bit 0, odd part bit 0
        cols = ["0x100", "0x200", "0x300", "0x400", "0x500"]
        text = ("colors\n  .  transparent\n" + "".join(f"  {chr(65 + i)} {c}\n" for i, c in enumerate(cols))
                + "frame\nE...............A\n")
        d = project({"art.toml": '[h]\nsource = "h.txt"\nkind = "sprite"\nattached = true\nmirror = true\n',
                     "h.txt": text})
        (a,) = art.build(d)
        self.assertEqual((a.columns, len(a.frames), a.mirror_offset), (2, 2, 1))
        self.assertEqual(a.index[0x500], 1)          # E first seen -> index 1
        data = a.planar()                             # frame, part, row: 2 words
        self.assertEqual(data[0:2], [0x8000, 0])      # part 0 (col 0 even): index 1 -> bit 0
        self.assertEqual(data[2:4], [0, 0])           # part 1 (col 0 odd): bits 2-3 of 1 = 0
        self.assertEqual(data[4:6], [0, 0x8000])      # part 2 (col 1 even): A = index 2 (0b10)
        # mirrored frame: A now at x=0, E at x=16
        self.assertEqual(data[8:10], [0, 0x8000])
        h = open(os.path.join(d, "build", "art", "art.h")).read()
        self.assertIn("ART_H_PARTS 4", h)
        self.assertIn("ART_H_MIRROR 1", h)
        c = open(os.path.join(d, "build", "art", "art.c")).read()
        self.assertIn("pPalette[31]", c)             # 15 colours -> slots 17-31

    def test_attached_needs_even_channel(self):
        d = project({"art.toml": '[h]\nsource = "h.txt"\nattached = true\nchannel = 1\n', "h.txt": SPRITE})
        with self.assertRaisesRegex(art.ArtError, "even channel"):
            art.build(d)


class ExportTests(unittest.TestCase):
    def test_png_to_text_and_back(self):
        d = project({})
        png = os.path.join(d, "art", "x.png")
        Image(bytes((255, 0, 0, 0, 0, 255, 255, 0, 0, 0, 0, 255)), 2, 2).save_png(png)
        n, warnings = art.export_text(png, os.path.join(d, "art", "x.txt"))
        self.assertEqual((n, warnings), (2, []))
        with open(os.path.join(d, "art", "art.toml"), "w") as f:
            f.write('[x]\nsource = "x.txt"\n')
        (a,) = art.build(d)
        self.assertEqual(a.frames[0], [[0xF00, 0x00F], [0xF00, 0x00F]])


class SharedAttachedPaletteTests(unittest.TestCase):
    def test_attached_sprites_share_one_palette(self):
        a = "colors\n  A 0xF00\n  B 0x0F0\nframe\nAB\n"
        b = "colors\n  B 0x0F0\n  C 0x00F\nframe\nBC\n"
        d = project({"art.toml": '[a]\nsource = "a.txt"\nattached = true\n[b]\nsource = "b.txt"\n'
                                 'attached = true\nchannel = 4\n', "a.txt": a, "b.txt": b})
        x, y = art.build(d)
        self.assertEqual(x.sprite_colors[:3], [0xF00, 0x0F0, 0x00F])
        self.assertEqual(x.sprite_colors, y.sprite_colors)
        self.assertEqual(y.index[0x0F0], 2)             # same index as in [a]

    def test_too_many_shared_colours(self):
        a = "colors\n" + "".join(f"  {chr(65 + i)} 0x{i:03X}\n" for i in range(10)) + "frame\nABCDEFGHIJ\n"
        b = "colors\n" + "".join(f"  {chr(65 + i)} 0x{0x100 + i:03X}\n" for i in range(10)) + "frame\nABCDEFGHIJ\n"
        d = project({"art.toml": '[a]\nsource = "a.txt"\nattached = true\n[b]\nsource = "b.txt"\n'
                                 'attached = true\nchannel = 4\n', "a.txt": a, "b.txt": b})
        with self.assertRaisesRegex(art.ArtError, "max 15"):
            art.build(d)
