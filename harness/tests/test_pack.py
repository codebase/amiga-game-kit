"""The LZ packer, its Python unpacker, and the runtime's C unpacker on the host."""
import os
import random
import shutil
import subprocess
import tempfile
import unittest

from agk import pack
from agk.paths import ROOT


def samples():
    r = random.Random(7)
    planar = bytes(r.choice((0, 0, 0, 255, 0x55, 0xAA)) for _ in range(5000)) * 4   # a dithered picture
    noise = bytes(r.randrange(256) for _ in range(3000))
    return {"empty": b"", "one": b"x", "text": b"the the the the quick brown fox " * 50,
            "planar": planar, "noise": noise, "zeros": bytes(10000), "edge": b"ab" * 4100}


class PackTests(unittest.TestCase):
    def test_roundtrip_and_ratio(self):
        for name, data in samples().items():
            p = pack.pack(data)
            self.assertTrue(p.startswith(b"AGKZ"))
            self.assertEqual(pack.unpack(p), data, name)
        self.assertLess(len(pack.pack(samples()["planar"])), 20000 // 2)       # the repeats pack
        self.assertLess(len(pack.pack(samples()["zeros"])), 1300)             # 18-byte matches at most: 10000 / 18 x 2 bytes
        self.assertLess(len(pack.pack(samples()["noise"])), 3000 + 3000 // 8 + 16)   # noise costs a flag bit a byte

    def test_c_unpacker_agrees(self):
        cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
        if not cc:
            self.skipTest("no host C compiler")
        with tempfile.TemporaryDirectory() as d:
            driver = os.path.join(d, "drv.c")
            with open(driver, "w") as f:
                f.write('#include <stdio.h>\n#include <stdlib.h>\n#include "agk/pack.h"\n'
                        'int main(int c, char **v) { FILE *f = fopen(v[1], "rb"); fseek(f, 0, 2); long n = ftell(f);\n'
                        'rewind(f); unsigned char *s = malloc(n); fread(s, 1, n, f); fclose(f);\n'
                        'unsigned long size = agkUnpackedSize(s); unsigned char *o = malloc(size + 1);\n'
                        'unsigned long got = agkUnpack(s, o); f = fopen(v[2], "wb"); fwrite(o, 1, got, f); fclose(f);\n'
                        'free(s); free(o); return got == size ? 0 : 1; }\n')   /* (LeakSanitizer on Linux) */
            exe = os.path.join(d, "drv")
            r = subprocess.run([cc, "-std=c11", "-Wall", "-fsanitize=address,undefined", "-I", os.path.join(ROOT, "runtime", "include"),
                                os.path.join(ROOT, "runtime", "src", "unpack.c"), driver, "-o", exe],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            for name, data in samples().items():
                src, dst = os.path.join(d, name + ".pk"), os.path.join(d, name + ".out")
                open(src, "wb").write(pack.pack(data))
                r = subprocess.run([exe, src, dst], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, name + r.stderr)
                self.assertEqual(open(dst, "rb").read(), data, name)

    def test_build_packs_disk_dir(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(pack.build(d))
            os.makedirs(os.path.join(d, "disk"))
            open(os.path.join(d, "disk", "level1.bin"), "wb").write(bytes(1000))
            done = pack.build(d)
            self.assertEqual(done[0][:2], ("level1.bin.pk", 1000))
            self.assertTrue(os.path.exists(os.path.join(d, "build", "disk", "level1.bin.pk")))
            open(os.path.join(d, "disk", "x" * 28), "wb").write(b"1")
            with self.assertRaisesRegex(ValueError, "too long"):
                pack.build(d)


if __name__ == "__main__":
    unittest.main()
