"""agk inspect's address reports and agk rip's planar reading (synthetic data)."""
import struct
import unittest

from agk import inspect as insp
from agk import profile as prof
from agk import rip, scenario
from agk.image import Image


def samples(pcs_per_frame, frames):
    lines = []
    for _ in range(frames):
        for i, pc in enumerate(pcs_per_frame):
            lines.append(((pc & 0xFFFFFF) | (prof.NEW_FRAME if i == 0 else 0), (0, 10, 0, 0, 0, 0, 0, 0)))
    return lines


class InspectTests(unittest.TestCase):
    def test_hot_regions_and_frame_shape(self):
        frame = [0x21D10] * 200 + [0x22610] * 100 + [0x21E00] * 13
        lines = samples(frame, 5)
        h = insp.hot_regions(lines)
        self.assertEqual(h["regions"][0][0], 0x21D00)
        self.assertAlmostEqual(h["regions"][0][1], 200 / 313, places=3)
        self.assertEqual(h["span"], (0x21D10, 0x22610))
        self.assertEqual(insp.frame_sequence(lines), [(0x21D00, 200), (0x22600, 100), (0x21E00, 13)])
        text = "\n".join(insp.report(lines))
        self.assertIn("$21D00x200 $22600x100 $21E00x13", text)
        self.assertNotIn("mostly in the ROM", text)

    def test_rom_hint(self):
        text = "\n".join(insp.report(samples([0xFC0F00] * 313, 3)))
        self.assertIn("mostly in the ROM", text)
        self.assertIn("100.0% in the Kickstart ROM", text)

    def test_scenario_snapshot_and_dump_chip(self):
        sc = scenario.parse("screenshot title\nsnapshot title\ndump-chip ram\n")
        self.assertIn("agk snapsave {out}/title.vasnap", sc.lines)
        self.assertIn("mem save bin {out}/ram.bin 0 524288", sc.lines)


class RipTests(unittest.TestCase):
    def planar(self, img, planes, interleaved):
        """an Image (grey levels 0..) -> planar bytes with the pixel's level as the colour index"""
        w, h = img.width, img.height
        out = bytearray()
        rows = [[img.rgb[(y * w + x) * 3] // 17 for x in range(w)] for y in range(h)]
        def plane_row(y, p):
            bits = 0
            for x in range(w):
                bits = (bits << 1) | ((rows[y][x] >> p) & 1)
            return bits.to_bytes(w // 8, "big")
        if interleaved:
            for y in range(h):
                for p in range(planes):
                    out += plane_row(y, p)
        else:
            for p in range(planes):
                for y in range(h):
                    out += plane_row(y, p)
        return bytes(out)

    def picture(self):
        w, h = 32, 4
        px = bytearray()
        for y in range(h):
            for x in range(w):
                c = (x * 3 + y) & 7
                px += bytes((c * 17, c * 17, c * 17))
        return Image(bytes(px), w, h)

    def test_planar_round_trip_both_layouts(self):
        pic = self.picture()
        pal = [i * 0x111 for i in range(8)]
        for inter in (False, True):
            data = bytes(64) + self.planar(pic, 3, inter) + bytes(64)
            got = rip.planar_to_image(data, 64, 32, 4, 3, pal, interleaved=inter)
            self.assertEqual(got.rgb, pic.rgb, f"interleaved={inter}")

    def test_copper_list_and_screen(self):
        pic = self.picture()
        planes = self.planar(pic, 3, False)        # 3 planes x 4 rows x 4 bytes
        base = 0x1000
        data = bytearray(0x2000)
        data[base:base + len(planes)] = planes
        cop = bytearray()
        def move(reg, val):
            cop.extend(struct.pack(">HH", reg, val))
        move(0x100, 0x3200)                        # BPLCON0: 3 planes
        move(0x108, 0)
        move(0x10A, 0)
        for p in range(3):
            addr = base + p * 16
            move(0xE0 + 4 * p, addr >> 16)
            move(0xE2 + 4 * p, addr & 0xFFFF)
        for i in range(8):
            move(0x180 + 2 * i, i * 0x111)
        cop.extend(struct.pack(">HH", 0xFFFF, 0xFFFE))
        data[0x1800:0x1800 + len(cop)] = cop
        lists = rip.find_copper_lists(bytes(data))
        self.assertEqual(len(lists), 1)
        c = lists[0]
        self.assertEqual((c["addr"], c["bplcon0"], c["bplpt"][2]), (0x1800, 0x3200, base + 32))
        self.assertEqual(c["colors"][7], 0x777)
        regs = {"ddfstrt": 0x38, "ddfstop": 0x40}   # a 2-word (32 px) fetch
        screen = rip.screen_from_copper(bytes(data), c, width=32, height=4, regs=regs)
        self.assertEqual(screen.rgb, pic.rgb)
        # a pointer change part-way down: WAIT for line $2C + 1, then BPL1PT back to row 0
        cop2 = bytearray(cop[:-4])
        cop2.extend(struct.pack(">HH", ((0x2C + 1) << 8) | 0x01, 0xFFFE))
        cop2.extend(struct.pack(">HH", 0xE0, base >> 16) + struct.pack(">HH", 0xE2, base & 0xFFFF))
        cop2.extend(struct.pack(">HH", 0xFFFF, 0xFFFE))
        data[0x1800:0x1800 + len(cop2)] = cop2
        c2 = rip.find_copper_lists(bytes(data))[0]
        self.assertEqual(c2["waits"], 2)
        s2 = rip.screen_from_copper(bytes(data), c2, width=32, height=4, regs=regs)
        self.assertEqual(s2.rgb[:32 * 3], pic.rgb[:32 * 3])                  # row 0 as before
        self.assertNotEqual(s2.rgb[32 * 3:32 * 2 * 3], pic.rgb[32 * 3:32 * 2 * 3])   # row 1: plane 1 restarted
        self.assertIn("BPL1 $001000", "\n".join(rip.copper_report(lists)))

    def test_tiles_sheet_and_palette_text(self):
        pic = self.picture()                       # 32x4: two 16x4 "tiles" side by side? no: stack them
        tile = Image(pic.rgb[:16 * 3] + pic.rgb[32 * 3:32 * 3 + 16 * 3] + pic.rgb[64 * 3:64 * 3 + 16 * 3]
                     + pic.rgb[96 * 3:96 * 3 + 16 * 3], 16, 4)
        data = self.planar(tile, 3, False) * 2
        sheet = rip.tiles_sheet(data, 0, 16, 4, 3, 2, [i * 0x111 for i in range(8)])
        self.assertEqual((sheet.width, sheet.height), (16, 8))
        self.assertEqual(sheet.rgb[:16 * 4 * 3], tile.rgb)
        self.assertEqual(rip.parse_palette("0xFFF, 0x000"), [0xFFF, 0])
        self.assertEqual(rip.parse_palette("@2", bytes((0, 0, 0x0F, 0xFF, 0x0A, 0xBC)) + bytes(64))[:2], [0xFFF, 0xABC])


if __name__ == "__main__":
    unittest.main()
