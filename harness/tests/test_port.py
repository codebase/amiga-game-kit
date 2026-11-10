"""agk port's rewriter: a Mega Drive game's instructions for the Amiga's memory map."""
import collections
import unittest

from agk import port


def rewrite(code, hw=None, hints=None):
    lay = port.layout(0x100000)
    h = port.read_hints("/nonexistent")
    for k, v in (hints or {}).items():
        h[k] = v
    rw = port.Rewriter(code, lay, hw or {}, h)
    return {a: [l.strip() for l in rw.instr(a)] for a in sorted(code)}, rw, lay


class RewriteTests(unittest.TestCase):
    def test_layout_keeps_the_low_word_of_ram_pointers(self):
        lay = port.layout(0x100000)
        self.assertEqual(lay["ram"] & 0xFFFF, 0)
        self.assertEqual(lay["D"], lay["ram"] + 0x10000)

    def test_ram_operands_move(self):
        out, rw, lay = rewrite({0x200: (4, "tstb", ["0xfffff62a"]), 0x204: (6, "lea", ["0xfffffe00", "a6"])})
        self.assertEqual(out[0x200], [f"tstb {lay['ram'] + 0xf62a:#x}"])
        self.assertEqual(out[0x204], [f"lea {lay['ram'] + 0xfe00:#x},a6"])
        self.assertIn(0x200, rw.unsafe)                     # can't run from its old address

    def test_hardware_writes_go_through_the_cell(self):
        out, rw, lay = rewrite({0x200: (6, "movew", ["#14", "0xc00000"])})
        cell, w16 = lay["cell"], lay["shim"] + port.SHIM_ENTRIES + 8 * port.SHIM_NAMES.index("w16")
        self.assertEqual(out[0x200], [f"movew #14,{cell:#x}", "pea 0xc00000", f"jsr {w16:#x}", "addql #4,sp"])

    def test_hardware_through_a_register_from_the_analysis(self):
        out, rw, lay = rewrite({0x200: (2, "movew", ["a5@", "d0"]), 0x202: (2, "movel", ["a0@+", "a6@"])},
                               hw={0x200: {"r"}, 0x202: {"w"}})
        r16 = lay["shim"] + port.SHIM_ENTRIES + 8 * port.SHIM_NAMES.index("r16")
        self.assertEqual(out[0x200], ["pea a5@", f"jsr {r16:#x}", "addql #4,sp", f"movew {lay['cell']:#x},d0"])
        self.assertEqual(out[0x202][0], f"movel a0@+,{lay['cell']:#x}")   # the source still steps

    def test_pc_relative_data_stays_where_it_was(self):
        out, rw, lay = rewrite({0x200: (4, "lea", ["%pc@(0x294)", "a5"]), 0x204: (4, "movew", ["%pc@(0x45c,d0:w)", "d0"])})
        self.assertEqual(out[0x200], ["lea 0x294,a5"])
        self.assertEqual(out[0x204], ["moveml a0,sp@-", "lea 0x45c,a0", "movew a0@(0,d0:w),d0", "moveal sp@+,a0"])
        self.assertNotIn(0x204, rw.unsafe)                  # in place, the original still works

    def test_table_jump_goes_to_the_old_address(self):
        out, rw, lay = rewrite({0x200: (4, "jmp", ["%pc@(0x206,d1:w)"])})
        self.assertEqual(out[0x200][-1], "rts")
        self.assertIn("lea 0x206,a0", out[0x200])

    def test_word_pointers_get_D(self):
        out, rw, lay = rewrite({0x200: (4, "moveaw", ["a0@(62)", "a1"]), 0x204: (4, "moveaw", ["#-20480", "a2"])})
        self.assertEqual(out[0x200], ["moveaw a0@(62),a1", f"addal #{lay['D']:#x},a1"])
        self.assertEqual(out[0x204], [f"moveal #{lay['ram'] + 0xb000:#x},a2"])

    def test_fixa_hint(self):
        out, rw, lay = rewrite({0x200: (2, "moveal", ["d1", "a1"])}, hints={"fixa": collections.defaultdict(set, {0x200: {"a1"}})})
        self.assertEqual(out[0x200][1], f"jsr {lay['shim'] + port.SHIM_ENTRIES + 8 * port.SHIM_NAMES.index('fixa1'):#x}")

    def test_branches_use_labels(self):
        out, rw, lay = rewrite({0x200: (2, "bnes", ["0x204"]), 0x202: (2, "nop", []), 0x204: (2, "rts", [])})
        self.assertEqual(out[0x200], ["jbne X_000204"])

    def test_gas_operand(self):
        self.assertEqual(port.gas_operand("a0@(1e,d2:w)"), "a0@(30,d2:w)")
        self.assertEqual(port.gas_operand("a0@(fffffffffffffffc,d0:w)"), "a0@(-4,d0:w)")


class OldSpaceTests(unittest.TestCase):
    def test_entries_get_jumps_and_the_rest_is_poison(self):
        code = {0x200: (2, "moveq", ["#0", "d0"]), 0x202: (4, "tstb", ["0xfffff62a"]), 0x206: (2, "rts", []),
                0x208: (2, "rts", [])}
        out, rw, lay = rewrite(code)
        rom = bytes(0x300)
        new = {a: 0x120000 + i * 16 for i, a in enumerate(sorted(code))}
        img, stats, failed, plan, inplace = port.patch_old(rom, code, rw, {0x200}, new, lay, log=lambda *a: None)
        self.assertEqual(img[0x200:0x206], b"\x4E\xF9" + (0x120000).to_bytes(4, "big"))
        self.assertEqual(img[0x206:0x208], port.POISON.to_bytes(2, "big"))
        self.assertEqual(img[0x208:0x20A], bytes(2))        # after an rts: a routine of its own, short and safe, left as it was
        self.assertEqual(failed, [])


if __name__ == "__main__":
    unittest.main()
