"""agk profile: placing sampled program counters in the game, per game frame (synthetic data)."""
import os
import struct
import tempfile
import unittest

from agk import profile as prof

MAP = """
Linker script and memory map

.text           0x00000000     0x1000
                0x00000000                        __stext = .
 *(.text)
 .text          0x00000000       0xd8 /opt/m68k-amigaos/m68k-amigaos/libnix/lib/ncrt0.o
                0x00000000                start
 .text          0x000000d8      0x200 CMakeFiles/game.dir/src/logic.c.obj
                0x000000d8                logicUpdate
 .text          0x000002d8      0x300 CMakeFiles/game.dir/src/main.c.obj
                0x000002d8                main
                0x00000400                genericProcess
 .text          0x000005d8      0x100 agk/libagk.a(perf.c.obj)
                0x000005d8                agkPerfBegin
                0x00000650                g_szAgkPerfKey
.data           0x00001000       0x10
 .data          0x00001000        0x4 CMakeFiles/game.dir/src/main.c.obj
                0x00001000                notCode
"""

DIS = """
CMakeFiles/game.dir/src/main.c.obj:     file format amiga

00000000 <_main>:
00000080 <_blitQueueWait>:
00000128 <_genericProcess>:
"""


def symbols():
    s, objs = prof.parse_map(MAP)
    return prof.Symbols(s, objs, prof.parse_functions(DIS))


def write_profile(path, anchor, lines):
    with open(path, "wb") as f:
        f.write(b"AGKP" + struct.pack("<II", 2, anchor))
        for v, bus in lines:
            f.write(struct.pack("<III", v, bus[0] | bus[1] << 8 | bus[2] << 16 | bus[3] << 24,
                                bus[4] | bus[5] << 8 | bus[6] << 16 | bus[7] << 24))


class ProfileTests(unittest.TestCase):
    def test_map(self):
        s, objs = prof.parse_map(MAP)
        self.assertEqual(s["logicUpdate"], 0xd8)
        self.assertEqual(s["g_szAgkPerfKey"], 0x650)
        self.assertNotIn("notCode", s)                 # .data isn't code
        self.assertEqual(objs[2], (0x2d8, 0x300, "CMakeFiles/game.dir/src/main.c.obj"))

    def test_functions_from_disassembly(self):
        f = prof.parse_functions(DIS)
        self.assertEqual(f["CMakeFiles/game.dir/src/main.c.obj"][1], (0x80, "blitQueueWait"))

    def test_statics_and_sources(self):
        sy = symbols()
        self.assertEqual(sy.lookup(0x2d8 + 0x90), ("blitQueueWait", "src/main.c"))   # a static
        self.assertEqual(sy.lookup(0x100), ("logicUpdate", "src/logic.c"))
        self.assertEqual(sy.lookup(0x5e0), ("agkPerfBegin", "libagk perf.c"))
        self.assertIsNone(sy.lookup(0x2000))

    def test_frames_load_bus_and_places(self):
        sy = symbols()
        base = 0x20000                                   # where AmigaDOS loaded the code
        anchor = base + 0x650
        work, idle = (base + 0x2d8 + 0x90) | prof.WORKING, base + 0x100
        cpu, blit = (0, 100, 0, 0, 0, 0, 0, 0), (0, 20, 80, 0, 0, 0, 0, 0)
        lines = []
        # frame 0: 100 lines of work (blitQueueWait, blitter-heavy), then idle
        lines += [(work | prof.TICK | prof.NEW_FRAME, blit)] + [(work, blit)] * 99 + [(idle, cpu)] * 213
        # frame 1: 200 lines in logicUpdate, then a ROM interrupt, idle
        lines += [((base + 0xd8) | prof.WORKING | prof.TICK | prof.NEW_FRAME, cpu)]
        lines += [((base + 0xe0) | prof.WORKING, cpu)] * 199 + [(0xFC1234, cpu)] + [(idle, cpu)] * 112
        lines += [(idle | prof.TICK, cpu)]              # a third frame, no work yet
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "profile.bin")
            write_profile(path, anchor, lines)
            got_anchor, got = prof.load(path)
        self.assertEqual(got_anchor, anchor)
        a = prof.analyse(got_anchor, got, sy)
        self.assertEqual([f["work"] for f in a["frames"]], [100, 200, 0])
        self.assertEqual(a["work_funcs"][("blitQueueWait", "src/main.c")], 100)
        self.assertEqual(a["all_funcs"][("Kickstart ROM", "")], 1)
        text = "\n".join(prof.report(a))
        self.assertIn("worst 64% (game frame 1)", text)     # 200 / 313 lines
        self.assertIn("game frame 0: 32% | blitter 80%", text)
        with tempfile.TemporaryDirectory() as d:
            prof.chart(a, os.path.join(d, "load.png"))
            self.assertTrue(os.path.getsize(os.path.join(d, "load.png")) > 50)

    def test_no_perf_report_says_what_to_do(self):
        with self.assertRaisesRegex(prof.ProfileError, "agkPerfBegin"):
            prof.analyse(0, [], symbols())

    def test_archive_members_are_found_next_to_the_archive(self):
        with tempfile.TemporaryDirectory() as b:
            os.makedirs(os.path.join(b, "agk", "CMakeFiles", "agk.dir", "src"))
            open(os.path.join(b, "agk", "CMakeFiles", "agk.dir", "src", "perf.c.obj"), "w").close()
            self.assertEqual(prof.object_file(b, "agk/libagk.a(perf.c.obj)"),
                             os.path.join("agk", "CMakeFiles", "agk.dir", "src", "perf.c.obj"))
            self.assertIsNone(prof.object_file(b, "/opt/x/libnix.a(foo.o)"))


if __name__ == "__main__":
    unittest.main()
