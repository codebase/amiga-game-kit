"""AGA is opt-in; verify low RGB bits, high palette indices, and eight planes."""
import tempfile
import unittest
from pathlib import Path
from agk import art
from agk.image import Image

class AgaArtTests(unittest.TestCase):
    def project(self, config, palette):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        p = Path(tmp.name)
        (p/'art').mkdir()
        (p/'art/art.toml').write_text(config)
        (p/'art/palette.txt').write_text(palette)
        return p

    def test_rgb24_survives_png_mapping_preview_and_c(self):
        p = self.project('[art]\nchipset="aga"\ndepth=8\n[probe]\nsource="probe.png"\nkind="bob"\n',
                         '0 0x000000\n128 0x123456\n255 0xABCDEF\n')
        Image(bytes.fromhex('123456abcdef'),2,1).save_png(p/'art/probe.png')
        (a,) = art.build(p)
        self.assertEqual(a.frames, [[[0x123456,0xABCDEF]]])
        self.assertEqual(a.planar(), [0x4000]*7+[0xC000,0xC000])
        self.assertEqual(a.preview(zoom=1,palette={128:0x123456,255:0xABCDEF}).pixel(0,0), (18,52,86))
        self.assertIn('artPaletteApply(ULONG *pPalette)', (p/'build/art/art.h').read_text())
        self.assertIn('pPalette[255] = 0xABCDEF;', (p/'build/art/art.c').read_text())

    def test_nearest_color_uses_all_channels_and_low_bits(self):
        p = self.project('[art]\nchipset="aga"\ndepth=8\n[a]\nsource="a.txt"\nkind="bitmap"\n',
                         '1 0x113456\n200 0xED3456\n')
        (p/'art/a.txt').write_text('colors\nA 0x123456\nframe\nA\n')
        (a,) = art.build(p)
        self.assertEqual(a.index[0x123456], 1)

    def test_ocs_defaults_still_reject_aga_palette(self):
        p = self.project('[a]\nsource="a.txt"\n', '255 0x123456\n')
        with self.assertRaisesRegex(art.ArtError,'0-31'):
            art.build(p)

    def test_invalid_palette_depth_and_chipset(self):
        for cfg,pal in [('[art]\nchipset="ecs"\n',''),
                        ('[art]\nchipset="aga"\n','256 0xFFFFFF\n'),
                        ('[art]\nchipset="aga"\n','1 -1\n'),
                        ('[art]\nchipset="aga"\ndepth=9\n[a]\nsource="a.txt"\n','1 0xFFFFFF\n')]:
            with self.subTest(cfg=cfg,pal=pal):
                p=self.project(cfg,pal)
                (p/'art/a.txt').write_text('colors\nA 0xFFFFFF\nframe\nA\n')
                with self.assertRaises(art.ArtError): art.build(p)

    def test_aga_sprite_and_own_palette_types(self):
        p=self.project('[art]\nchipset="aga"\n[s]\nsource="a.txt"\nattached=true\n[b]\nsource="a.txt"\nkind="bitmap"\npalette="auto"\ndepth=2\n','')
        (p/'art/a.txt').write_text('colors\nA 0x123456\nframe\nA\n')
        art.build(p)
        h=(p/'build/art/art.h').read_text()
        self.assertIn('artSApplyColors(ULONG *pPalette)',h)
        self.assertIn('const ULONG g_pArtBPalette[4]',h)

    def test_rd_input_palette_preserves_rgb24(self):
        import base64
        from agk import rd
        from agk.image import load_png_rgba
        p=self.project('[art]\nchipset="aga"\n', '255 0x123456\n')
        self.assertEqual(rd.sprite_colors_from(p, None), [0x123456])
        png=p/'pal.png';png.write_bytes(base64.b64decode(rd.palette_png([0x123456], aga=True)))
        self.assertEqual(load_png_rgba(png)[2], [(18,52,86,255)])
