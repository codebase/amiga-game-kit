"""agk release: the zip and the Aminet readme (synthetic files, no emulator)."""
import os
import tempfile
import unittest
import zipfile

from agk import release
from agk.image import Image


class ReleaseTests(unittest.TestCase):
    def test_defaults_and_short_limit(self):
        r = release.config({"name": "foo"}, {})
        self.assertEqual((r["title"], r["version"], r["type"]), ("FOO", "1.0", "game/misc"))
        with self.assertRaisesRegex(release.ReleaseError, "40"):
            release.config({"name": "foo"}, {"short": "x" * 41})

    def test_zip_and_readme(self):
        with tempfile.TemporaryDirectory() as d:
            adf, exe, png = (os.path.join(d, n) for n in ("g.adf", "g", "s.png"))
            open(adf, "wb").write(b"\0" * 100)
            open(exe, "wb").write(b"\0" * 10)
            Image(bytes((255, 0, 0, 0, 255, 0)), 2, 1).save_png(png)
            proj = {"name": "g"}
            r = release.config(proj, {"version": "2.1", "author": "A", "short": "S", "type": "game/shoot",
                                      "description": "D", "controls": "C"})
            z, rd = release.build_zip(proj, r, adf, exe, png, os.path.join(d, "out"), date="2026-11-02")
            self.assertEqual(os.path.basename(z), "g-2.1.zip")
            with zipfile.ZipFile(z) as zf:
                self.assertEqual(sorted(zf.namelist()), ["README.txt", "g", "g.adf", "g.png"])
                text = zf.read("README.txt").decode()
            self.assertIn("G 2.1", text)
            self.assertIn("Controls: C", text)
            self.assertIn("Built 2026-11-02", text)
            self.assertEqual(Image.load_png(os.path.join(d, "out", "g.png")).width, 4)   # 2x
            readme = open(rd).read()
            self.assertTrue(readme.startswith("Short:        S\n"))
            self.assertIn("Type:         game/shoot", readme)
            self.assertIn("Architecture: m68k-amigaos >= 1.3", readme)
            self.assertIn("g-2.1.zip", readme)


if __name__ == "__main__":
    unittest.main()
