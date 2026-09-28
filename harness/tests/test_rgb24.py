import tempfile
import unittest
from pathlib import Path
from agk import scenario, art
from agk.image import Image
from agk.runner import _check_rgb24

class Rgb24Tests(unittest.TestCase):
    def test_parse_exact_low_bits(self):
        sc=scenario.parse('screenshot a\nexpect-rgb a 1 0 0x123456 near 2')
        self.assertEqual(sc.rgbs,[('a',1,0,(18,52,86),2,2)])
        self.assertEqual(sc.colors,[])
        for text in ['expect-rgb b 0 0 0x123456','screenshot b\nexpect-rgb b 0 0 0x1000000']:
            with self.assertRaises(scenario.ScenarioError):scenario.parse(text)

    def test_low_nibble_mismatch_and_near_bounds(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'a.png';Image(bytes.fromhex('123456123457'),2,1).save_png(p)
            for spec,ok in [('0 0 0x123456',True),('1 0 0x123456',False),('1 0 0x123456 near 1',True)]:
                sc=scenario.parse('screenshot a\nexpect-rgb a '+spec)
                result={'screenshots':{'a':{'screen_png':p}},'ok':True,'failures':[]}
                _check_rgb24(sc,result);self.assertEqual(result['ok'],ok)
                if not ok:self.assertIn('0x123457',result['failures'][0])

    def test_export_retains_aga_colors(self):
        with tempfile.TemporaryDirectory() as d:
            src=Path(d)/'a.png';dst=Path(d)/'a.txt'
            Image(bytes.fromhex('123456abcdef'),2,1).save_png(src)
            art.export_text(src,dst,aga=True)
            self.assertEqual(art.parse_text_art(dst)[2],[[[0x123456,0xABCDEF]]])
