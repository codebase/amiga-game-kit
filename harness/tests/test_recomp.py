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

    def test_immediates_are_read_from_the_code(self):
        L, _ = lines_of([(0x100, 2, "mulsw", "#32710,d1"), (0x104, 3, "addil", "#100,d2"), (0x10a, 1, "moveq", "#5,d0"),
                         (0x10c, 1, "addqw", "#1,d3")])
        text = "\n".join(L)
        self.assertIn("tmp = (rd16(0x102u) & MASK(16));", text)            # a 3D engine patches these
        self.assertIn("rd32(0x106u)", text)
        self.assertIn("setd(0, 0x5u, 32)", text)                           # in the opcode word: constant
        self.assertIn("tmp = 0x1u;", text)                                  # addq's constant: in the opcode word

    def test_hooks_run_before_the_instruction(self):
        text, _ = recomp.generate([(0x100, 1, "nop", ""), (0x102, 1, "rts", "")], {0x100, 0x102}, 0x100,
                                  hooks={0x100: "my_hook"})
        self.assertIn("int my_hook(void);", text)
        self.assertIn("L_000100:\n\tif (dirty[0]) { cpu.pc = 0x100u; goto dispatch; }\n\tif (my_hook()) goto dispatch;", text)

    def test_rewritten_instructions_hand_their_block_to_the_interpreter(self):
        # a loop: $100 is a branch target (a block start), $102 a muls the game may rewrite
        instrs = [(0x100, 1, "nop", ""), (0x102, 2, "mulsw", "#4,d2"), (0x106, 2, "dbf", "d0,0x100"), (0x10a, 1, "rts", "")]
        text, _ = recomp.generate(instrs, {a for a, *_ in instrs}, 0x100, memory=[(0x100, bytes.fromhex("4e71 c5fc 0004 51c8 fff8 4e75".replace(" ", "")))])
        self.assertIn("L_000100:\n\tif (dirty[0]) { cpu.pc = 0x100u; goto dispatch; }", text)   # the block's first instruction asks
        self.assertNotIn("L_000102:\n\tif (dirty", text)                                    # mid-block: no check
        self.assertIn("dirty[blk[pc >> 1]] == 0", text)                                       # translated() says no for a dirty block
        self.assertIn("code_bits[0x102u >> 4] = 1", text)
        self.assertIn("void code_written(u32 a, int bits, u32 v)", text)
        # once patched.txt names it, the instruction checks its own opcode instead
        text, _ = recomp.generate(instrs, {a for a, *_ in instrs}, 0x100, memory=[(0x100, bytes.fromhex("4e71 c5fc 0004 51c8 fff8 4e75".replace(" ", "")))], guards={0x102})
        self.assertIn("L_000102:\n\tif (rd16(0x102u) != 0xc5fcu) { cpu.pc = 0x102u; goto dispatch; }", text)
        self.assertIn("guarded_at[] = { 0x102u }", text)

    def test_index_scale_bits_are_ignored_on_a_68000(self):
        a = recomp.Operand("a4@(30,a0:w:2)", 8, 0x1000)
        b = recomp.Operand("a4@(30,a0:w)", 8, 0x1000)
        self.assertEqual(a.setup, b.setup)

    def test_bcd(self):
        L, _ = lines_of([(0x100, 1, "sbcd", "d0,d3"), (0x102, 1, "abcd", "a1@-,a2@-"), (0x104, 1, "nbcd", "d1")])
        text = "\n".join(L)
        self.assertIn("setd(3, bcd_sub(D[0] & 0xFF, D[3] & 0xFF), 8);", text)
        self.assertIn("A[1] -= 1; tmp = rd8(A[1]); A[2] -= 1; wr8(A[2], bcd_add(tmp, rd8(A[2])));", text)
        self.assertIn("bcd_sub((D[1] & MASK(8)), 0)", text)

    def test_movep(self):
        L, _ = lines_of([(0x100, 2, "movepw", "a0@(2038),d3"), (0x104, 2, "movepl", "d2,a1@(0)")])
        text = "\n".join(L)
        self.assertIn("ea = A[0] + 2038u;", text)
        self.assertIn("setd(3, rd8(ea) << 8 | rd8(ea + 2), 16);", text)
        self.assertIn("wr8(ea, D[2] >> 24); wr8(ea + 2, D[2] >> 16); wr8(ea + 4, D[2] >> 8); wr8(ea + 6, D[2]);", text)

    def test_unparseable_uncovered_code_is_data(self):
        L, _ = lines_of([(0x100, 3, "btst", "#-54,@(0,d0:l:4)"), (0x106, 1, "rts", "")], covered={0x106})
        self.assertIn("{ cpu.pc = 0x100u; goto dispatch; }", "\n".join(L))

    def test_usp_moves_are_longs(self):
        L, _ = lines_of([(0x100, 1, "movel", "a3,%usp"), (0x102, 1, "movel", "%usp,sp")])
        text = "\n".join(L)
        self.assertIn("if (cpu.s) cpu.usp = A[3]; else A[7] = A[3];", text)
        self.assertIn("A[7] = (cpu.s ? cpu.usp : A[7]);", text)

    def test_data_in_uncovered_code_stops(self):
        L, _ = lines_of([(0x100, 1, ".short", "0x4489")], covered=set())
        self.assertIn("{ cpu.pc = 0x100u; goto dispatch; }", "\n".join(L))   # the interpreter's, if ever reached

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
@ 0x500
     500:\t0c79 0013 0001 \tcmpiw #19,0x1afb2
     506:\tafb2 
     508:\t4e71           \tnop
"""


class FollowTests(unittest.TestCase):
    def test_sweep_stops_at_jumps_data_and_known_code(self):
        known = {0x402: 0x402}
        fake = mock.Mock(returncode=0, stdout=FAKE_OBJDUMP, stderr="")
        with mock.patch.object(recomp.subprocess, "run", return_value=fake):
            out, displaced = recomp.disassemble_from([(0, "chip.bin", 0x80000)], [0x200, 0x300, 0x400, 0x500], "img", "/tmp", known)
        addrs = [a for a, *_ in out]
        self.assertEqual(addrs, [0x200, 0x202, 0x300, 0x400, 0x500, 0x508])   # not past the bra, not the data, not into known code
        self.assertIn(0x202, known)
        self.assertEqual(out[-2][1], 4)                          # the wrapped 8-byte instruction is 4 words
        self.assertIn(0x506, known)
        self.assertEqual(displaced, set())

    def test_anchor_displaces_a_sweep_that_decoded_padding(self):
        known = {0x504: 0x504, 0x506: 0x504}                      # a sweep read "2a2a 0c79" as one instruction at $504
        fake = mock.Mock(returncode=0, stdout=FAKE_OBJDUMP, stderr="")
        with mock.patch.object(recomp.subprocess, "run", return_value=fake):
            out, displaced = recomp.disassemble_from([(0, "chip.bin", 0x80000)], [0x500], "img", "/tmp", known)
        self.assertEqual([a for a, *_ in out if a >= 0x500], [0x500, 0x508])
        self.assertEqual(displaced, {0x504})
        with mock.patch.object(recomp.subprocess, "run", return_value=fake):
            out, displaced = recomp.disassemble_from([(0, "chip.bin", 0x80000)], [0x500], "img", "/tmp", {0x504: 0x504, 0x506: 0x504}, trusted=frozenset({0x504}))
        self.assertNotIn(0x500, [a for a, *_ in out])             # but never a coverage instruction


if __name__ == "__main__":
    unittest.main()
