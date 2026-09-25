import os
import struct
import tempfile
import unittest

from agk import scenario, sound


def project(files):
    d = tempfile.mkdtemp()
    os.makedirs(os.path.join(d, "sound"))
    for name, text in files.items():
        with open(os.path.join(d, "sound", name), "w") as f:
            f.write(text)
    return d


SONG = """
#title Test
#tempo 140
#inst lead pulse25 vol=40 decay=1
#inst bass triangle vol=56
#inst kick kick
A @lead o5 l8 e g > c < b a4 g4 | (ceg)2 (dfa)2
B @bass o2 l4 c c g g f f g g
D @kick o4 l4 c c c c
"""


class SfxTests(unittest.TestCase):
    def test_sweep_length_and_format(self):
        d = sound.synth_sfx("jump", {"wave": "square", "freq": [220, 660], "length": 0.1, "decay": 8})
        self.assertEqual(len(d) % 2, 0)                            # whole words for Paula
        self.assertAlmostEqual(len(d) / sound.SFX_RATE, 0.1, delta=0.001)
        vals = sound.sfx_to_float(d)
        self.assertGreater(max(vals), 0.5)
        self.assertLess(min(vals), -0.5)
        self.assertLess(abs(vals[-1]), 0.05)                       # release: no click at the end

    def test_steps_and_noise_are_deterministic(self):
        spec = {"wave": "noise", "steps": [[4000, 0.05], [800, 0.05]]}
        self.assertEqual(sound.synth_sfx("a", spec), sound.synth_sfx("a", spec))
        self.assertAlmostEqual(len(sound.synth_sfx("a", spec)) / sound.SFX_RATE, 0.1, delta=0.001)

    def test_errors_say_what_to_fix(self):
        with self.assertRaisesRegex(sound.SoundError, "unknown key 'frq'"):
            sound.synth_sfx("x", {"frq": 100, "length": 0.1})
        with self.assertRaisesRegex(sound.SoundError, "needs `freq`"):
            sound.synth_sfx("x", {"length": 0.1})
        with self.assertRaisesRegex(sound.SoundError, "wave must be one of"):
            sound.synth_sfx("x", {"wave": "kazoo", "freq": 100, "length": 0.1})


class MmlTests(unittest.TestCase):
    def test_mod_layout(self):
        mod, info = sound.compile_mml(SONG)
        self.assertEqual(mod[1080:1084], b"M.K.")
        self.assertEqual(mod[:4], b"Test")
        self.assertEqual(info["rows"], 32)                         # A: 8 + 8 + 16 rows of 16ths
        self.assertAlmostEqual(info["seconds"], 32 * 6 * 2.5 / 140)
        npat = max(mod[952:1080]) + 1
        samples = sum(struct.unpack(">H", mod[20 + 30 * i + 22:20 + 30 * i + 24])[0] * 2 for i in range(31))
        self.assertEqual(len(mod), 1084 + npat * 1024 + samples)
        # looped waveform: silent first word (ptplayer zeroes it), loop after it
        length, ft, vol, rep, replen = struct.unpack(">HBBHH", mod[20 + 22:20 + 30])
        self.assertEqual((vol, rep, replen), (40, 1, length - 1))
        # row 0: tempo 140 set in a free effect column
        row0 = [mod[1084 + c * 4:1084 + c * 4 + 4] for c in range(4)]
        self.assertIn((0xF, 140), [(b[2] & 15, b[3]) for b in row0])

    def test_chord_is_an_arpeggio_on_every_row(self):
        mod, _ = sound.compile_mml("#inst p square\nA @p o4 l4 (ceg)")
        for row in range(4):
            cell = mod[1084 + row * 16:1084 + row * 16 + 4]
            self.assertEqual((cell[2] & 15, cell[3]), (0x0, 0x47))

    def test_notes_map_to_protracker_periods(self):
        # a 32-sample cycle puts ProTracker C-1 (period 856) at o3 c
        mod, info = sound.compile_mml("#inst p square\nA @p o3 l4 c o5 b")
        self.assertEqual(info["instruments"][0][2], 32)
        per = lambda row: ((mod[1084 + row * 16] & 15) << 8) | mod[1084 + row * 16 + 1]
        self.assertEqual((per(0), per(4)), (856, 113))

    def test_dotted_default_length(self):
        _, info = sound.compile_mml("#inst p square\nA @p l4. c c c8 c8")   # 6 + 6 + 2 + 2
        self.assertEqual(info["rows"], 16)

    def test_short_channel_repeats(self):
        _, info = sound.compile_mml("#inst p square\nA @p l1 c d\nB @p l2 c")
        self.assertEqual(info["rows"], 32)

    def test_errors(self):
        cases = [("#inst p square\nA @p l4 c\nB @p l4. c", "must divide"),
                 ("A o4 c", "no instrument"),
                 ("#inst p square\nA @q c", "unknown instrument '@q'"),
                 ("#inst p square\nA @p o1 c o6 c", "split it into two"),
                 ("#inst p square\nA @p l3 c", "whole number of rows"),
                 ("#inst p square\nA @p (cx)", "only notes"),
                 ("#inst p square\nA @p t120 c", "#tempo")]
        for text, msg in cases:
            with self.assertRaisesRegex(sound.SoundError, msg, msg=text):
                sound.compile_mml(text)

    def test_preview_renders_every_channel(self):
        mod, info = sound.compile_mml(SONG)
        pcm = sound.render_mod(mod)
        self.assertAlmostEqual(len(pcm) / sound.PREVIEW_RATE, info["seconds"], delta=0.05)
        levels = sound.rms_windows(pcm, sound.PREVIEW_RATE)
        self.assertTrue(all(l > sound.AUDIBLE_RMS for l in levels[:-2]))


class BuildTests(unittest.TestCase):
    def test_build_writes_c_and_previews(self):
        d = project({"sound.toml": '[sfx.jump]\nfreq = [200, 400]\nlength = 0.1\n'
                                   '[music.theme]\nsource = "t.mml"\n',
                     "t.mml": SONG})
        res = sound.build(d)
        out = os.path.join(d, "build", "sound")
        with open(os.path.join(out, "sound.h")) as f:
            h = f.read()
        self.assertIn("#define SOUND_SFX_JUMP 0", h)
        self.assertIn("#define SOUND_MUSIC_THEME 0", h)
        with open(os.path.join(out, "sound.c")) as f:
            c = f.read()
        self.assertIn('"AGK sfx jump\\n"', c)
        self.assertIn("ptplayerSetMusicChannelMask(0x7)", c)     # default: effects on channel 3
        for f in ("theme.mod", "preview/jump.wav", "preview/jump.png", "preview/theme.png"):
            self.assertTrue(os.path.exists(os.path.join(out, f)), f)
        self.assertGreater(res["chip_bytes"], 0)
        # unchanged sources keep the generated files' mtime (no pointless rebuilds)
        t = os.path.getmtime(os.path.join(out, "sound.c"))
        os.utime(os.path.join(out, "sound.c"), (t - 100, t - 100))
        sound.build(d, previews=False)
        self.assertEqual(os.path.getmtime(os.path.join(out, "sound.c")), t - 100)

    def test_no_sound_dir(self):
        self.assertIsNone(sound.build(tempfile.mkdtemp()))

    def test_unknown_table(self):
        d = project({"sound.toml": "[sfxx.jump]\n"})
        with self.assertRaisesRegex(sound.SoundError, r"unknown table \[sfxx\]"):
            sound.build(d)


class ScenarioAudioTests(unittest.TestCase):
    def test_marks_and_checks(self):
        sc = scenario.parse("mark a\nwait 5\nmark b\nexpect-sound a b\nexpect-silence b end")
        self.assertIn("agk audio mark a", sc.lines)
        self.assertEqual(sc.audio, [("sound", "a", "b", 4), ("silence", "b", "end", 5)])

    def test_unknown_mark(self):
        with self.assertRaisesRegex(scenario.ScenarioError, "unknown mark 'x'"):
            scenario.parse("expect-sound start x")
        with self.assertRaisesRegex(scenario.ScenarioError, "built-in"):
            scenario.parse("mark end")


if __name__ == "__main__":
    unittest.main()
