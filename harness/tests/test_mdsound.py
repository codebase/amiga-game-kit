"""agk port's sound (agk/mdsound.py): the log of a sound rip read into notes, the FM synthesiser,
samples, the four-channel stream and the data the Amiga's player reads"""
import math
import struct
import unittest

from agk import mdsound as m

ORGAN = (7, 0, (1, 127, 0, 31, 0, 0, 0, 15), (1, 127, 0, 31, 0, 0, 0, 15), (1, 127, 0, 31, 0, 0, 0, 15), (1, 0, 0, 31, 0, 0, 0, 15))
PLUCK = (7, 0, (1, 127, 0, 31, 0, 0, 0, 15), (1, 127, 0, 31, 0, 0, 0, 15), (1, 127, 0, 31, 0, 0, 0, 15), (1, 0, 0, 31, 20, 20, 15, 15))


def ym(part, reg, val):
    return bytes([0x59, part, reg, val, 0, 0])


def log_of(*frames):
    """a request's log: each frame a list of records"""
    out = bytearray(b"R" + struct.pack("<HBB", 0x1B88, 0x81, 1))
    for f in frames:
        for r in f:
            out += r
        out += b"F"
    return bytes(out + b"E")


class Synth(unittest.TestCase):
    def test_a_carrier_alone_is_a_sine_at_the_note(self):
        y = m.synth(ORGAN, 440.0, 0.5, 32000)
        crossings = sum(1 for a, b in zip(y, y[1:]) if a < 0 <= b)
        self.assertAlmostEqual(crossings, 220, delta=2)
        self.assertAlmostEqual(max(y[1000:]), 1.0, delta=0.02)

    def test_a_modulator_changes_the_wave_not_the_pitch(self):
        fm = (0, 0, (1, 127, 0, 31, 0, 0, 0, 15), (1, 127, 0, 31, 0, 0, 0, 15), (2, 30, 0, 31, 0, 0, 0, 15), (1, 0, 0, 31, 0, 0, 0, 15))
        y = m.synth(fm, 200.0, 0.2, 40000)
        per = 200
        self.assertLess(max(abs(y[4000 + i] - y[4000 + per + i]) for i in range(per)), 0.02)   # it repeats every period
        self.assertGreater(max(abs(y[4000 + i] - math.sin(2 * math.pi * (4000 + i) / per)) for i in range(per)), 0.2)

    def test_a_decay_rate_is_the_chips(self):
        # rate 20 is 40, and 42 with this octave's key code even at scaling 0: 6658 steps of 1024 a second
        slow = (7, 0) + ORGAN[2:5] + ((1, 0, 0, 31, 20, 0, 15, 15),)
        y = m.synth(slow, 440.0, 0.2, 32000)
        peak = max(abs(v) for v in y[int(0.11 * 32000):int(0.12 * 32000)])
        self.assertAlmostEqual(20 * math.log10(peak), -70, delta=5)


class Samples(unittest.TestCase):
    def test_a_held_tone_is_an_attack_and_one_period_looped(self):
        s = m.fm_sample(ORGAN, 4, 1.0)
        self.assertEqual(s.looplen, s.per)
        self.assertEqual(s.loop + s.looplen, len(s.data))
        self.assertEqual(len(s.data) % 2, 0)
        self.assertLess(len(s.data), 1200)                      # nothing changes after the attack: it is cut there
        self.assertGreater(len(s.env), 50)                      # (its level is known for as long as it is held)

    def test_a_note_that_dies_away_plays_once(self):
        s = m.fm_sample(PLUCK, 4, 1.0)
        self.assertEqual(s.looplen, 0)

    def test_dac_runs_become_hits(self):
        def noise(seed, n):
            out = bytearray()
            for _ in range(n):
                seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
                out.append(seed >> 16 & 0xFF)
            return bytes(out)
        kick, snare = noise(1, 300), noise(2, 400)
        # the kick in two runs (the driver's update held it up), then a snare cut short by a kick in the same run
        bursts = [[0, 30000, kick[:150]], [35000, 30000, kick[150:]], [500000, 110000, snare[:250] + kick]]
        hits = m.dac_hits(bursts)
        self.assertEqual([len(h[2]) for h in hits], [300, 250, 300])
        self.assertEqual(hits[2][2], kick)
        self.assertAlmostEqual(hits[2][0], 500000 + 110000 * 250 / 550, delta=1)


class Notes(unittest.TestCase):
    def test_a_key_on_is_a_note_with_its_patch_pitch_and_level(self):
        setup = [ym(0, 0xB0, 0x07), ym(0, 0x4C, 0x10), ym(0, 0x40, 0x7F), ym(0, 0x44, 0x7F), ym(0, 0x48, 0x7F),
                 ym(0, 0x5C, 0x1F), ym(0, 0xB4, 0xC0), ym(0, 0xA4, 0x22), ym(0, 0xA0, 0x69), ym(0, 0x28, 0xF0)]
        notes, frames, dacs = m.parse(log_of(setup, [], [ym(0, 0x4C, 0x18)], [ym(0, 0x28, 0x00)], []), 0)
        self.assertEqual(frames, 5)
        self.assertEqual(len(notes), 1)
        n = notes[0]
        self.assertEqual((n.kind, n.voice, n.t0, n.t1), ("fm", 0, 0, 3))
        self.assertAlmostEqual(n.f[0][1], 0x269 * 16 * m.FM_RATE / (1 << 21), places=3)   # block 4, F-number $269: 261.6 Hz
        self.assertEqual(n.patch[0], 7)
        self.assertEqual(n.patch[5][1], 0)                     # the carrier's level is the note's, not the patch's
        self.assertEqual([t for t, a in n.a], [0, 2])          # ... and its change two frames in is the note's too

    def test_a_square_is_a_note_while_it_is_not_silent(self):
        on = [bytes([0x50, 0x80 | 0x0E, 0, 0]), bytes([0x50, 0x0F, 0, 0]), bytes([0x50, 0x90 | 2, 0, 0])]
        notes, frames, _ = m.parse(log_of(on, [], [bytes([0x50, 0x9F, 0, 0])], []), 0)
        self.assertEqual([(n.kind, n.t0, n.t1) for n in notes], [("psg", 0, 2)])
        self.assertAlmostEqual(notes[0].f[0][1], m.Z80_CLOCK / (32 * 0xFE), places=3)

    def test_music_that_goes_round_is_found(self):
        notes = []
        for t in range(0, 3000, 10):
            n = m.Note("fm", 0, t + (t // 700) % 2)             # (a tick of drift, as the driver's tempo gives)
            k = 0 if t < 200 else 1 + (((t - 200) % 700) // 10) ** 2 * 5 % 23
            n.f = [(t, 220.0 * 2 ** (k / 12))]
            notes.append(n)
        start, length = m.find_loop(notes, 3000)
        self.assertAlmostEqual(length, 700, delta=1)
        self.assertLessEqual(start, 210)


class Stream(unittest.TestCase):
    def play(self, data):
        """the player's side: -> per tick, per channel [sample, period, volume]"""
        st = [[None, 0, 0] for _ in range(4)]
        out = []
        p = 0
        while p < len(data):
            b = data[p]
            p += 1
            if b < 0x80:
                out += [[list(c) for c in st] for _ in range(b + 1)]
                continue
            for c in range(4):
                if b & (1 << c):
                    fl = data[p]
                    p += 1
                    if fl & 1:
                        st[c][0] = data[p]
                        p += 1
                    if fl & 2:
                        st[c][1] = data[p] << 8 | data[p + 1]
                        p += 2
                    if fl & 8:
                        st[c][1] += data[p] - 256 if data[p] > 127 else data[p]
                        p += 1
                    if fl & 4:
                        st[c][2] = data[p]
                        p += 1
            out.append([list(c) for c in st])
        return out

    def test_the_bytes_say_what_the_rows_do(self):
        rows = [[None] * 12 for _ in range(4)]
        rows[0][2] = (5, 400, 40)
        rows[0][3] = (-1, 402, 40)
        rows[0][4] = (-1, 402, 30)
        rows[2][4] = (9, 1000, 64)
        rows[2][5] = (-1, 700, 64)
        local = []
        data, _ = m.encode(rows, 0, 12, local)
        self.assertEqual(local, [5, 9])
        got = self.play(data)
        self.assertEqual(len(got), 12)
        self.assertEqual(got[2][0], [0, 400, 40])
        self.assertEqual(got[3][0], [0, 402, 40])
        self.assertEqual(got[4][0], [0, 402, 30])
        self.assertEqual(got[5][0][2], 0)                      # the note ended: silence
        self.assertEqual(got[5][2], [1, 700, 64])
        self.assertEqual(got[11][2][2], 0)

    def test_four_channels_go_to_the_loudest(self):
        notes = []
        for v, amp in enumerate((0.9, 0.8, 0.7, 0.6, 0.1)):
            n = m.Note("psg", v, 0)
            n.t1, n.f, n.a = 50, [(0, 440.0)], [(0, amp)]
            n.vol, n.end, n.peak = [amp] * 50, 50, amp
            notes.append(n)
        late = m.Note("psg", 9, 10)
        late.t1, late.f, late.a, late.vol, late.end, late.peak = 30, [(10, 440.0)], [(10, 1.0)], [1.0] * 20, 30, 1.0
        segs = m.arrange(notes + [late], 60)
        playing = {n.voice for ch in segs for n, a, b in ch}
        self.assertNotIn(4, playing)                           # the quiet one found no channel
        self.assertIn(9, playing)                              # the loud late one took the quietest's
        cut = [(n.voice, b) for ch in segs for n, a, b in ch if n.voice == 3]
        self.assertEqual(cut, [(3, 10)])


class Pack(unittest.TestCase):
    def test_the_players_data(self):
        pool = m.Pool()
        note = m.Note("psg", 6, 0)
        note.f = [(0, 440.0)]
        s = pool.get(note)
        stream = (2, bytes([0x81, 0x07, 0, 0x01, 0x00, 40, 9]), None, [s.index])
        snd = {"pool": pool, "streams": {(0x1B88, 0xA0): stream}, "kinds": {(0x1B88, 0xA0): "over", (0x1B88, 0xFD): "stop"},
               "requests": {(0x1B88, 0xA0): (0x1B88, 0xA0), (0x1B88, 0xFD): None}, "idle": bytes(8192), "addrs": [0x1B88]}
        blob = m.pack(snd, [(0x1E8000, 0x8000)], {0x1B8C: 0x1B88}, log=lambda *_: None)
        magic, n_s, n_st, n_a, _, o_s, o_st, o_rq, o_idle = struct.unpack_from(">4sHHHHIIII", blob, 0)
        self.assertEqual((magic, n_s, n_st, n_a), (b"MDSN", 1, 1, 2))
        silence, sfx = struct.unpack_from(">II", blob, 28)
        self.assertEqual((silence, sfx), (0x1E8000, 0x1E8004))
        self.assertEqual(struct.unpack_from(">H", blob, o_rq)[0], 0x1B88)
        self.assertEqual(struct.unpack_from(">H", blob, o_rq + 2 + 2 * 0xA0)[0], m.KINDS["over"] << 12 | 0)
        self.assertEqual(struct.unpack_from(">H", blob, o_rq + 2 + 2 * 0xFD)[0], m.KINDS["stop"] << 12 | 0xFFF)
        self.assertEqual(struct.unpack_from(">H", blob, o_rq + 514)[0], 0x1B8C)                 # the alias answers as $1B88
        self.assertEqual(blob[o_rq + 516:o_rq + 1028], blob[o_rq + 2:o_rq + 514])
        d, lp, nch, kind, nloc, lo = struct.unpack_from(">IIBBHI", blob, o_st)
        self.assertEqual((lp, nch, kind, nloc), (0, 2, m.KINDS["over"], 1))
        self.assertEqual(blob[d:d + 8], stream[1] + b"\xff")
        sd, ln, loop, looplen, flags = struct.unpack_from(">IHHHH", blob, o_s)
        self.assertEqual((ln * 2, loop, looplen * 2, flags), (len(s.data), 0, s.looplen, 1))
        self.assertEqual(blob[sd:sd + len(s.data)], s.data)


if __name__ == "__main__":
    unittest.main()
