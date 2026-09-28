"""agk lint: finds per-frame game code that calls libgcc maths (no Docker needed:
it runs on a synthetic objdump -dr listing)."""
import unittest

from agk import lint

DIS = """
build/CMakeFiles/game.dir/src/main.c.obj:     file format amiga

Disassembly of section .text:

00000000 <_genericProcess>:
       0:	4eba 0010      	jsr %pc@(12 <_starsUpdate>)
       4:	4eb9 0000 0000 	jsr 0 <_logicUpdate>
			6: RELOC32	_logicUpdate
       a:	4e75           	rts

0000000c <_genericCreate>:
       c:	4eba 0020      	jsr %pc@(2e <_tablesMake>)
      10:	4e75           	rts

00000012 <_starsUpdate>:
      12:	49f9 0000 0000 	lea 0 <___mulsi3>,a4
			14: RELOC32	___mulsi3
      18:	4e94           	jsr a4@
      1a:	6000 0004      	braw 20 <_starsUpdate+0xe>
      1e:	4e75           	rts

build/CMakeFiles/game.dir/src/logic.c.obj:     file format amiga

00000000 <_logicUpdate>:
       0:	4eb9 0000 0000 	jsr 0 <___divsi3>
			2: RELOC32	___divsi3
       6:	4eb9 0000 0000 	jsr 0 <___divsi3>
			8: RELOC32	___divsi3
       c:	4eb9 0000 0000 	jsr 0 <___addsf3>
			e: RELOC32	___addsf3
      12:	4e75           	rts

0000002e <_tablesMake.part.0>:
      2e:	4eb9 0000 0000 	jsr 0 <___modsi3>
			30: RELOC32	___modsi3
      34:	4e75           	rts

00000036 <_levelInit>:
      36:	4eb9 0000 0000 	jsr 0 <___umodsi3>
			38: RELOC32	___umodsi3
      3e:	4e75           	rts
"""


class TestLint(unittest.TestCase):
    def setUp(self):
        self.parsed = lint.parse(DIS)

    def test_finds_helpers_per_function(self):
        found, _ = self.parsed
        by = {lint.clean(f): refs for (_, f), refs in found.items()}
        self.assertEqual(by["logicUpdate"], {"___divsi3": 2, "___addsf3": 1})
        self.assertEqual(by["starsUpdate"], {"___mulsi3": 1})      # through a register (lea + jsr a4@)

    def test_setup_code_is_left_out(self):
        items, skipped = lint.findings(self.parsed)
        names = [f for _, f, _ in items]
        # levelInit by its name; tablesMake because only genericCreate calls it
        self.assertEqual(names, ["logicUpdate", "starsUpdate"])
        self.assertEqual(skipped, 2)
        self.assertEqual(items[0][0], "src/logic.c")

    def test_allow_and_all(self):
        items, _ = lint.findings(self.parsed, allow=["starsUpdate"])
        self.assertEqual([f for _, f, _ in items], ["logicUpdate"])
        items, skipped = lint.findings(self.parsed, everything=True)
        self.assertEqual(len(items), 4)
        self.assertEqual(skipped, 0)

    def test_report_says_what_to_do(self):
        text = "\n".join(lint.report(*lint.findings(self.parsed)))
        self.assertIn("src/logic.c: logicUpdate() calls the maths library: floating point (addsf3 x1), "
                      "signed 32-bit divide (divsi3 x2)", text)
        self.assertIn("fixed point", text)
        self.assertIn("MULU/MULS", text)

    def test_generated_art_is_not_linted(self):
        import os
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            base = os.path.join(d, "build", "CMakeFiles", "g.dir")
            for rel in ("src/main.c.obj", "art/art.c.obj", "sound/sound.c.obj"):
                os.makedirs(os.path.dirname(os.path.join(base, rel)), exist_ok=True)
                open(os.path.join(base, rel), "w").close()
            objs = lint.objects({"dir": d, "name": "g"})
            self.assertEqual([os.path.relpath(o, base) for o in objs], ["src/main.c.obj"])


if __name__ == "__main__":
    unittest.main()
