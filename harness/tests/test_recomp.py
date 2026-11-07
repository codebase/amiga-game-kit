"""agk recomp's translator: operands, instructions, control flow, the
branch-following disassembly (objdump's output is faked: no docker here)."""
import os
import tempfile
import unittest
from unittest import mock

from agk import recomp


def lines_of(instrs, covered=None):
    t = recomp.Translator(instrs, covered if covered is not None else {a for a, *_ in instrs})
    return [l.strip() for l in t.translate()], t


class OperandTests(unittest.TestCase):
    def test_register_and_immediate(self):
        self.assertEqual(recomp.Operand("d3", 16, 0).read(), "(D[3] & MASK(16))")
        self.assertEqual(recomp.Operand("a2", 32, 0).read(), "A[2]")
        self.assertEqual(recomp.Operand("sp", 32, 0).read(), "A[7]")
        self.assertEqual(recomp.Operand("#-1", 8, 0).read(), "0xffffffffu")

    def test_indirect_forms(self):
        o = recomp.Operand("a0@+", 16, 0)
        self.assertEqual(o.setup, ["ea = A[0]; A[0] += 2;"])
        o = recomp.Operand("sp@-", 8, 0)                       # a byte push still moves the stack by 2
        self.assertEqual(o.setup, ["A[7] -= 2; ea = A[7];"])
        o = recomp.Operand("a6@(898)", 16, 0)
        self.assertEqual(o.setup, ["ea = A[6] + (898);"])
        o = recomp.Operand("a3@(0,d0:w)", 32, 0)
        self.assertIn("(u32)(s32)(s16)D[0]", o.setup[0])
        o = recomp.Operand("%pc@(0x43a)", 16, 0)
        self.assertEqual(o.setup, ["ea = 0x43au;"])
        o = recomp.Operand("0x1b044 <foo>", 16, 0)
        self.assertEqual(o.read(), "rd16(ea)")
        self.assertEqual(o.setup, ["ea = 0x1b044u;"])

    def test_unknown_operand_is_an_error(self):
        with self.assertRaises(recomp.RecompError):
            recomp.Operand("d0:d1", 16, 0)


class TranslateTests(unittest.TestCase):
    def test_straight_line_code(self):
        L, _ = lines_of([(0x100, 1, "moveq", "#5,d0"), (0x102, 1, "addw", "d0,d1"), (0x104, 1, "rts", "")])
        text = "\n".join(L)
        self.assertIn("L_000100:", text)
        self.assertIn("setd(0, 0x5u, 32);", text)
        self.assertIn("goto dispatch;", text)             # rts goes through the table

    def test_branches_become_gotos(self):
        L, t = lines_of([(0x100, 1, "tstw", "d0"), (0x102, 1, "bnes", "0x100"), (0x104, 2, "bsrw", "0x200"),
                         (0x108, 1, "rts", "")])
        text = "\n".join(L)
        self.assertIn("if (CC_NE) goto L_000100;", text)
        self.assertIn("take_interrupt(irq_pending, 0x102u)", text)   # a backward branch checks for interrupts
        self.assertIn("push32(0x108u); { cpu.pc = 0x200u; goto dispatch; }", text)   # a call to code we don't have
        self.assertEqual(t.unknown, {0x200: 0x104})

    def test_dbcc_and_scc(self):
        L, _ = lines_of([(0x100, 2, "dbf", "d0,0x100"), (0x104, 1, "seq", "d1"), (0x106, 1, "subqw", "#1,d2")])
        text = "\n".join(L)
        self.assertIn("goto L_000100", text)
        self.assertIn("CC_EQ", text)
        self.assertIn("op_sub(", text)                     # subqw isn't an scc

    def test_data_in_uncovered_code_stops(self):
        L, _ = lines_of([(0x100, 1, ".short", "0x4489")], covered=set())
        self.assertIn('trace_die("data reached as code", 0x100u);', "\n".join(L))

    def test_generate_has_a_table_entry_per_instruction(self):
        text, unknown = recomp.generate([(0x100, 1, "nop", ""), (0x102, 1, "rts", "")], {0x100, 0x102}, 0x100)
        self.assertIn("table[0x100u >> 1] = &&L_000100;", text)
        self.assertIn("table[0x102u >> 1] = &&L_000102;", text)
        self.assertEqual(unknown, {})


class CoverageTests(unittest.TestCase):
    def test_union_and_ranges(self):
        with tempfile.TemporaryDirectory() as d:
            a, b = bytearray(1 << 21), bytearray(1 << 21)
            for addr in (0x100, 0x102, 0x104):
                a[addr >> 3] |= 1 << (addr & 7)
            b[0x200 >> 3] |= 1 << (0x200 & 7)
            pa, pb = os.path.join(d, "coverage.bin"), os.path.join(d, "coverage_play.bin")
            open(pa, "wb").write(a)
            open(pb, "wb").write(b)
            ranges, covered = recomp.coverage_ranges([pa, pb])
        self.assertEqual(covered, {0x100, 0x102, 0x104, 0x200})
        self.assertEqual(ranges, [(0x100, 0x106), (0x200, 0x202)])


FAKE_OBJDUMP = """@ 0x200
     200:\t7000           \tmoveq #0,d0
     202:\t6000 0010      \tbraw 0x214
     206:\t4e71           \tnop
@ 0x300
     300:\t4e71           \tnop
     302:\tffff           \t.short 0xffff
@ 0x400
     400:\t4e71           \tnop
     402:\t4e71           \tnop
"""


class FollowTests(unittest.TestCase):
    def test_sweep_stops_at_jumps_data_and_known_code(self):
        known = {0x402}
        fake = mock.Mock(returncode=0, stdout=FAKE_OBJDUMP, stderr="")
        with mock.patch.object(recomp.subprocess, "run", return_value=fake):
            out = recomp.disassemble_from("chip.bin", [0x200, 0x300, 0x400], "img", "/tmp", known)
        addrs = [a for a, *_ in out]
        self.assertEqual(addrs, [0x200, 0x202, 0x300, 0x400])   # not past the bra, not the data, not into known code
        self.assertIn(0x202, known)


if __name__ == "__main__":
    unittest.main()
