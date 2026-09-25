"""Sound pipeline: text recipes -> sound effects, MML text -> ProTracker
music, both linked into the game and played by ACE's ptplayer.

A project's sound lives in sound/:

    sound/sound.toml   one table per sound effect or song:

        [sound]
        sfx_channels = [3]        # Paula channels sound effects may use (music
                                  # keeps the others to itself); default [3]

        [sfx.jump]
        wave = "square"           # square pulse25 pulse12 saw triangle sine noise
        freq = [220, 660]         # Hz: one value, or start and end of a sweep
        length = 0.14             # seconds
        decay = 12                # optional: exponential fade, per second (0 = none)
        attack = 0.005            # optional: fade-in seconds
        release = 0.02            # optional: linear fade-out at the end (default 0.02)
        volume = 48               # playback volume 0-64 (default 48)
        priority = 2              # a higher priority takes the channel (default 1)

        [sfx.coin]                # a sequence of notes instead of a sweep:
        wave = "square"
        steps = [[988, 0.05], [1319, 0.2]]   # [Hz, seconds] ...
        decay = 6

        [music.theme]
        source = "theme.mml"      # MML text, see below

  Noise: `freq` is how often the noise changes value (Hz): ~8000 hiss,
  ~2000 crunch, ~500 rumble.

`agk sound` (and `agk build`) converts it into build/sound/sound.c + sound.h,
writes each song as a real .mod (build/sound/NAME.mod: open it in any
tracker) and previews: build/sound/preview/NAME.wav and NAME.png (spectrogram
with waveform, one column per 1/50 s), so you can look at a sound.

In the game:

    #include "sound.h"
    soundCreate();                       // after systemCreate: ptplayer + samples in chip RAM
    soundPlay(SOUND_SFX_JUMP);           // also prints "AGK sfx jump" (for tests)
    soundMusicStart(SOUND_MUSIC_THEME);  // loops; soundMusicStop()
    soundDestroy();

MML (Music Macro Language), one line per channel, lines of the same channel
are joined:

    #title Level 1
    #tempo 140                  ; beats per minute (quarter notes)
    #grid 4                     ; rows per quarter note: 4 (16ths) or 8 (32nds)
    #inst lead pulse25 vol=40 decay=2   ; NAME WAVE [vol=0-64] [decay=0-15]
    #inst bass triangle vol=56
    #inst kick kick
    A @lead o5 l8 v12 e g > c < b- a4 g4
    B @bass o2 l4 c c g g
    C @lead o4 l16 (ceg)4 (dfa)4
    D @kick o4 l4 c c c c

  Waves: square pulse25 pulse12 saw triangle sine (looped, any pitch within
  3 octaves per instrument) and drums kick snare hat tom crash (one-shot;
  `o4 c` is their natural pitch).
  Notes c d e f g a b, + or # sharp, - flat, then an optional length (4 =
  quarter, 8 = eighth, ... default `l`) and dots. `r` rest. `c4&c16` or
  `c4&16` tie. `o N` octave, `<` `>` octave down/up, `l N` default length,
  `v N` volume 0-15, `q N` gate 1-8 (q4 = notes sound for half their
  length), `@NAME` instrument, `[ ... ]N` repeat N times, `(ceg)` chord =
  fast arpeggio of up to 3 notes (spanning at most 15 semitones), `; ...`
  comment. `decay=N` on an instrument fades notes by N volume steps per tick.
  Channel A plays on Paula channel 0, B on 1, C on 2, D on 3. By default
  sound effects take channel 3, so put the part you can lose (drums) on D.
  A channel shorter than the song repeats to fill it (it must divide it).
"""
import math
import os
import struct
import tomllib
import wave

PAL_CLOCK = 3546895
SFX_PERIOD = 214                    # ProTracker C-3: the rate sound effects are made at
SFX_RATE = PAL_CLOCK / SFX_PERIOD   # 16574 Hz
PREVIEW_RATE = 22050
PERIODS = [856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
           428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
           214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113]
LOOPED = ("square", "pulse25", "pulse12", "saw", "triangle", "sine")
DRUMS = ("kick", "snare", "hat", "tom", "crash")
SFX_WAVES = LOOPED + ("noise",)
CHANNELS = "ABCD"


class SoundError(Exception):
    pass


# ------------------------------------------------------------------ synthesis

class _Noise:
    """Deterministic noise (an LCG), so builds are reproducible."""

    def __init__(self, seed=12345):
        self.s = seed

    def next(self):
        self.s = (self.s * 1103515245 + 12345) & 0x7FFFFFFF
        return (self.s >> 15) / 32768.0 * 2 - 1


def _osc(wave_name, phase):
    """One sample of a waveform, phase in cycles (0..1)."""
    p = phase % 1.0
    if wave_name == "square":
        return 1.0 if p < 0.5 else -1.0
    if wave_name == "pulse25":
        return 1.0 if p < 0.25 else -1.0
    if wave_name == "pulse12":
        return 1.0 if p < 0.125 else -1.0
    if wave_name == "saw":
        return 2 * p - 1
    if wave_name == "triangle":
        return 4 * p - 1 if p < 0.5 else 3 - 4 * p
    if wave_name == "sine":
        return math.sin(2 * math.pi * p)
    raise SoundError(f"unknown wave '{wave_name}'")


def _to_s8(samples, gain=0.9):
    peak = max((abs(v) for v in samples), default=0) or 1.0
    k = 127 * gain / peak
    out = bytearray(max(-128, min(127, round(v * k))) & 0xFF for v in samples)
    if len(out) % 2:
        out.append(0)
    return bytes(out)


def synth_sfx(name, spec):
    """Render a [sfx.NAME] table to 8-bit signed samples at SFX_RATE."""
    known = {"wave", "freq", "steps", "length", "decay", "attack", "release", "volume", "priority"}
    for k in spec:
        if k not in known:
            raise SoundError(f"sfx.{name}: unknown key '{k}' (use {', '.join(sorted(known))})")
    wave_name = spec.get("wave", "square")
    if wave_name not in SFX_WAVES:
        raise SoundError(f"sfx.{name}: wave must be one of {', '.join(SFX_WAVES)}, got '{wave_name}'")
    if "steps" in spec:
        steps = spec["steps"]
        if not steps or any(not isinstance(s, list) or len(s) != 2 for s in steps):
            raise SoundError(f"sfx.{name}: steps must be a list of [Hz, seconds] pairs")
        segs = [(float(f), float(f), float(d)) for f, d in steps]
    else:
        f = spec.get("freq")
        if f is None:
            raise SoundError(f"sfx.{name}: needs `freq` (Hz, or [start, end]) or `steps`")
        f0, f1 = (float(f), float(f)) if not isinstance(f, list) else (float(f[0]), float(f[-1]))
        if "length" not in spec:
            raise SoundError(f"sfx.{name}: needs `length` in seconds")
        segs = [(f0, f1, float(spec["length"]))]
    total = sum(d for _, _, d in segs)
    if not 0.01 <= total <= 3.0:
        raise SoundError(f"sfx.{name}: length must be 0.01-3 s (it's {total:.2f} s)")
    for f0, f1, _ in segs:
        if not 20 <= min(f0, f1) and max(f0, f1) <= SFX_RATE / 2:
            raise SoundError(f"sfx.{name}: frequencies must be 20-{int(SFX_RATE / 2)} Hz")
    decay = float(spec.get("decay", 0))
    attack = float(spec.get("attack", 0.002))
    release = float(spec.get("release", 0.02))
    noise, held, phase = _Noise(), 0.0, 0.0
    out, t_abs = [], 0.0
    for f0, f1, dur in segs:
        n = max(1, round(dur * SFX_RATE))
        for i in range(n):
            u = i / n
            freq = f0 * (f1 / f0) ** u if f0 != f1 else f0      # exponential sweep sounds even
            prev = phase
            phase += freq / SFX_RATE
            if wave_name == "noise":
                if int(phase) != int(prev):
                    held = noise.next()
                v = held
            else:
                v = _osc(wave_name, phase)
            t = t_abs + i / SFX_RATE
            amp = math.exp(-decay * t) if decay else 1.0
            if t < attack:
                amp *= t / attack
            if total - t < release:
                amp *= max(0.0, (total - t) / release)
            out.append(v * amp)
        t_abs += dur
    return _to_s8(out)


def synth_drum(kind):
    """One-shot drum at SFX_RATE (it is ProTracker's C-3, so `o4 c` plays it as made)."""
    noise = _Noise(777)
    out, phase = [], 0.0
    if kind == "kick":
        n = int(0.18 * SFX_RATE)
        for i in range(n):
            t = i / SFX_RATE
            phase += (45 + 120 * math.exp(-t * 30)) / SFX_RATE
            out.append(math.sin(2 * math.pi * phase) * math.exp(-t * 14))
    elif kind == "tom":
        n = int(0.22 * SFX_RATE)
        for i in range(n):
            t = i / SFX_RATE
            phase += (110 + 110 * math.exp(-t * 18)) / SFX_RATE
            out.append(math.sin(2 * math.pi * phase) * math.exp(-t * 12))
    elif kind == "snare":
        n = int(0.16 * SFX_RATE)
        for i in range(n):
            t = i / SFX_RATE
            phase += 190 / SFX_RATE
            tone = math.sin(2 * math.pi * phase) * math.exp(-t * 40)
            out.append(0.45 * tone + 0.8 * noise.next() * math.exp(-t * 22))
    elif kind in ("hat", "crash"):
        n = int((0.06 if kind == "hat" else 0.6) * SFX_RATE)
        rate = 90 if kind == "hat" else 7
        prev = 0.0
        for i in range(n):
            t = i / SFX_RATE
            v = noise.next()
            out.append((v - prev) * 0.6 * math.exp(-t * rate))   # difference = brighter
            prev = v
    else:
        raise SoundError(f"unknown drum '{kind}'")
    s = bytearray(_to_s8(out))
    s[0:2] = b"\0\0"          # ProTracker: a one-shot's first word is its silent loop
    return bytes(s)


def waveform(kind, n):
    """One looped cycle of n samples."""
    return _to_s8([_osc(kind, (i + 0.5) / n) for i in range(n)], gain=0.7)


# ------------------------------------------------------------------------ MML

def _base_note(n):
    """MIDI note of ProTracker C-1 for a looped cycle of n samples."""
    return 48 - 12 * round(math.log2(n / 32))


class _Inst:
    def __init__(self, name, wave_name, vol, decay):
        self.name, self.wave, self.vol, self.decay = name, wave_name, vol, decay
        self.notes = set()
        self.sample_no = 0
        self.cycle = 0
        self.data = b""

    @property
    def looped(self):
        return self.wave in LOOPED


def _parse_header(line, lineno, song):
    toks = line[1:].split()
    if not toks:
        raise SoundError(f"line {lineno}: empty # directive")
    key = toks[0]
    if key == "title":
        song["title"] = " ".join(toks[1:])[:20]
    elif key in ("tempo", "grid"):
        if len(toks) != 2 or not toks[1].isdigit():
            raise SoundError(f"line {lineno}: usage: #{key} N")
        v = int(toks[1])
        if key == "tempo" and not 32 <= v <= 255:
            raise SoundError(f"line {lineno}: tempo must be 32-255 BPM")
        if key == "grid" and v not in (4, 8):
            raise SoundError(f"line {lineno}: grid must be 4 or 8 rows per quarter note")
        song[key] = v
    elif key == "inst":
        if len(toks) < 3:
            raise SoundError(f"line {lineno}: usage: #inst NAME WAVE [vol=0-64] [decay=0-15]")
        name, wave_name = toks[1], toks[2]
        if wave_name not in LOOPED + DRUMS:
            raise SoundError(f"line {lineno}: wave '{wave_name}' - use one of {', '.join(LOOPED + DRUMS)}")
        opts = {"vol": 48, "decay": 0}
        for kv in toks[3:]:
            k, _, v = kv.partition("=")
            if k not in opts or not v.isdigit():
                raise SoundError(f"line {lineno}: bad option '{kv}' (use vol=0-64, decay=0-15)")
            opts[k] = int(v)
        if opts["vol"] > 64 or opts["decay"] > 15:
            raise SoundError(f"line {lineno}: vol is 0-64, decay 0-15")
        if name in song["insts"]:
            raise SoundError(f"line {lineno}: instrument '{name}' defined twice")
        song["insts"][name] = _Inst(name, wave_name, opts["vol"], opts["decay"])
    else:
        raise SoundError(f"line {lineno}: unknown directive #{key} (use #title #tempo #grid #inst)")


NOTE_PC = {"c": 0, "d": 2, "e": 4, "f": 5, "g": 7, "a": 9, "b": 11}


class _Track:
    """Parses one channel's MML into events: (row, rows, midi|None, inst, vol, arp, gate)."""

    def __init__(self, ch, text, song):
        self.ch, self.text, self.song = ch, text, song
        self.i = 0
        self.octave, self.length, self.vol, self.gate = 4, 4, 15, 8
        self.length_dots = 0
        self.inst = None
        self.row = 0
        self.events = []

    def err(self, msg):
        ctx = self.text[max(0, self.i - 12):self.i + 12].replace("\n", " ")
        raise SoundError(f"channel {self.ch}: {msg} (near '{ctx}')")

    def peek(self):
        return self.text[self.i] if self.i < len(self.text) else ""

    def num(self, required=True):
        j = self.i
        while self.i < len(self.text) and self.text[self.i].isdigit():
            self.i += 1
        if j == self.i:
            if required:
                self.err("expected a number")
            return None
        return int(self.text[j:self.i])

    def rows(self, default=True):
        """Length (+ dots) in rows."""
        n = self.num(required=False)
        per_quarter = self.song["grid"]
        base = self.length if n is None else n
        if base <= 0:
            self.err("length must be > 0")
        whole = per_quarter * 4
        if whole % base:
            self.err(f"length {base} isn't a whole number of rows (#grid {per_quarter}: "
                     f"shortest note is {whole})")
        r = add = whole // base
        dots = self.length_dots if n is None else 0
        while self.peek() == ".":
            self.i += 1
            dots += 1
        for _ in range(dots):
            if add % 2:
                self.err("dotted length isn't a whole number of rows")
            add //= 2
            r += add
        return r

    def pitch(self):
        c = self.text[self.i].lower()
        self.i += 1
        pc = NOTE_PC[c]
        while self.peek() in ("+", "#", "-"):
            pc += -1 if self.text[self.i] == "-" else 1
            self.i += 1
        return 12 * (self.octave + 1) + pc

    def emit(self, midi, rows, arp=()):
        if midi is not None and self.inst is None:
            self.err("no instrument: start the channel with @NAME")
        self.events.append((self.row, rows, midi, self.inst, self.vol, tuple(arp), self.gate))
        if midi is not None:
            self.inst.notes.add(midi)
            for a in arp:
                self.inst.notes.add(midi + a)
        self.row += rows

    def parse(self, end=None):
        while self.i < len(self.text):
            c = self.text[self.i].lower()
            if c.isspace() or c == "|":
                self.i += 1
            elif c == end:
                self.i += 1
                return
            elif c in NOTE_PC:
                midi = self.pitch()
                rows = self.rows()
                while self.peek() == "&":        # ties: c4&c16 or c4&16
                    self.i += 1
                    if self.peek().lower() in NOTE_PC:
                        save = self.octave
                        if self.pitch() != midi:
                            self.err("a tie joins notes of the same pitch")
                        self.octave = save
                    rows += self.rows()
                self.emit(midi, rows)
            elif c == "r":
                self.i += 1
                self.emit(None, self.rows())
            elif c == "(":
                self.i += 1
                notes, octave = [], self.octave
                while self.peek() and self.peek() != ")":
                    ch = self.peek().lower()
                    if ch in NOTE_PC:
                        notes.append(self.pitch())
                    elif ch == ">":
                        self.octave += 1; self.i += 1
                    elif ch == "<":
                        self.octave -= 1; self.i += 1
                    elif ch.isspace():
                        self.i += 1
                    else:
                        self.err(f"only notes and < > inside a chord, got '{ch}'")
                if self.peek() != ")":
                    self.err("chord not closed with )")
                self.i += 1
                self.octave = octave
                if not 2 <= len(notes) <= 3:
                    self.err("a chord has 2 or 3 notes")
                arp = [n - notes[0] for n in notes[1:]]
                if any(not 0 < a <= 15 for a in arp):
                    self.err("chord notes must rise from the first, within 15 semitones")
                self.emit(notes[0], self.rows(), arp)
            elif c == "o":
                self.i += 1
                self.octave = self.num()
                if not 0 <= self.octave <= 8:
                    self.err("octave must be 0-8")
            elif c == ">":
                self.i += 1; self.octave += 1
            elif c == "<":
                self.i += 1; self.octave -= 1
            elif c == "l":
                self.i += 1
                self.length = self.num()
                if self.length <= 0:
                    self.err("length must be > 0")
                self.length_dots = 0
                while self.peek() == ".":
                    self.i += 1
                    self.length_dots += 1
            elif c == "v":
                self.i += 1
                self.vol = self.num()
                if self.vol > 15:
                    self.err("volume must be 0-15")
            elif c == "q":
                self.i += 1
                self.gate = self.num()
                if not 1 <= self.gate <= 8:
                    self.err("gate must be 1-8")
            elif c == "@":
                self.i += 1
                j = self.i
                while self.i < len(self.text) and (self.text[self.i].isalnum() or self.text[self.i] == "_"):
                    self.i += 1
                name = self.text[j:self.i]
                if name not in self.song["insts"]:
                    self.err(f"unknown instrument '@{name}' (define it with #inst {name} WAVE)")
                self.inst = self.song["insts"][name]
            elif c == "[":
                self.i += 1
                start_ev, start_row = len(self.events), self.row
                self.parse(end="]")
                times = self.num(required=False) or 2
                body, body_rows = self.events[start_ev:], self.row - start_row
                for k in range(1, times):
                    for (r, n, m, ins, v, a, g) in body:
                        self.events.append((r + k * body_rows, n, m, ins, v, a, g))
                self.row += (times - 1) * body_rows
            elif c == "t":
                self.err("set the tempo with #tempo N")
            else:
                self.err(f"unexpected '{self.text[self.i]}'")
        if end:
            self.err(f"missing '{end}'")


def parse_mml(text, name="song"):
    song = {"title": name[:20], "tempo": 125, "grid": 4, "insts": {}}
    chans = {c: [] for c in CHANNELS}
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        if line.startswith("#"):
            _parse_header(line, lineno, song)
        elif line[0].upper() in CHANNELS and (len(line) == 1 or line[1].isspace()):
            chans[line[0].upper()].append(line[1:])
        else:
            raise SoundError(f"line {lineno}: a line starts with #directive or a channel letter "
                             f"A-D and a space, got '{line[:20]}'")
    tracks = {}
    for c, parts in chans.items():
        if parts:
            t = _Track(c, " ".join(parts), song)
            t.parse()
            tracks[c] = t
    if not tracks:
        raise SoundError("no channels: write lines like 'A @lead o4 l8 c d e f'")
    total = max(t.row for t in tracks.values())
    for c, t in tracks.items():
        if t.row == 0:
            raise SoundError(f"channel {c} has no notes or rests")
        if total % t.row:
            lengths = ", ".join(f"{k}={v.row}" for k, v in tracks.items())
            raise SoundError(f"channel {c} is {t.row} rows but the song is {total}: a shorter "
                             f"channel repeats, so its length must divide the song's (rows: {lengths})")
        base = list(t.events)
        for k in range(1, total // t.row):
            t.events += [(r + k * t.row, *rest) for (r, *rest) in base]
    song["rows"], song["tracks"] = total, tracks
    return song


def _assign_samples(song):
    """Pick each instrument's cycle length so its notes fit ProTracker's 3 octaves."""
    used = [i for i in song["insts"].values() if i.notes]
    if len(used) > 31:
        raise SoundError("more than 31 instruments")
    for no, inst in enumerate(used, 1):
        inst.sample_no = no
        lo, hi = min(inst.notes), max(inst.notes)
        if inst.looped:
            for n in (256, 128, 64, 32, 16, 8, 4):
                b = _base_note(n)
                if lo - b >= 0 and hi - b <= 35:
                    inst.cycle = n
                    break
            else:
                raise SoundError(f"instrument '{inst.name}' spans {_note_name(lo)}..{_note_name(hi)}: "
                                 f"one instrument covers 3 octaves - split it into two #inst lines")
            # ptplayer zeroes every sample's first word: a silent word, then the loop
            inst.data = b"\0\0" + waveform(inst.wave, inst.cycle)
        else:
            if lo < 36 or hi > 71:
                raise SoundError(f"drum '{inst.name}' is played at {_note_name(lo)}..{_note_name(hi)}: "
                                 f"use o2 c .. o4 b (o4 c = its natural pitch)")
            inst.data = synth_drum(inst.wave)
    return used


def _note_name(midi):
    return f"o{midi // 12 - 1}{'c c+d d+e f f+g g+a a+b '[2 * (midi % 12):2 * (midi % 12) + 2].strip()}"


def _period_index(inst, midi):
    return midi - (_base_note(inst.cycle) if inst.looped else 36)


def compile_mml(text, name="song"):
    """MML -> (MOD file bytes, info dict)."""
    song = parse_mml(text, name)
    used = _assign_samples(song)
    rows = song["rows"]
    # cells[row][ch] = [sample, period, cmd, param]
    cells = [[[0, 0, 0, 0] for _ in range(4)] for _ in range(rows)]
    speed = 24 // song["grid"]
    for ci, c in enumerate(CHANNELS):
        t = song["tracks"].get(c)
        if not t:
            continue
        sounding = False
        for (r, n, midi, inst, vol, arp, gate) in t.events:
            if midi is None:
                if sounding:
                    cells[r][ci][2:] = [0xC, 0]            # rest: cut
                sounding = False
                continue
            cell = cells[r][ci]
            cell[0], cell[1] = inst.sample_no, PERIODS[_period_index(inst, midi)]
            v = round(inst.vol * vol / 15)
            if arp:
                x = arp[0]
                y = arp[1] if len(arp) > 1 else 0
                for k in range(r, r + n):
                    cells[k][ci][2:] = [0x0, (x << 4) | y]
                if v != inst.vol:
                    cell[2:] = [0xC, v]                  # volume wins the first row
            elif v != inst.vol:
                cell[2:] = [0xC, v]
            if inst.decay and not arp and inst.looped:
                for k in range(r + 1, r + n):
                    cells[k][ci][2:] = [0xA, inst.decay]
            sounding = inst.looped
            cut = r + max(1, (n * gate + 7) // 8)
            if inst.looped and cut < r + n:
                cells[cut][ci][2:] = [0xC, 0]
                for k in range(cut + 1, r + n):
                    cells[k][ci][2:] = [0, 0]
                sounding = False

    def free_slot(row):
        for ch in (3, 2, 1, 0):
            if cells[row][ch][2:] == [0, 0]:
                return cells[row][ch]
        return None

    # Speed and tempo on row 0, a pattern break at the end of a partial pattern
    for cmd, param in ((0xF, speed), (0xF, song["tempo"])) if song["tempo"] != 125 or speed != 6 else ():
        slot = free_slot(0)
        if slot is None:
            raise SoundError("row 0 has no free effect column for the tempo: start a channel "
                             "with a rest, or at the instrument's own volume (v15)")
        slot[2:] = [cmd, param]
    if rows % 64:
        slot = free_slot(rows - 1)
        if slot is None:
            raise SoundError("the last row has no free effect column for the loop point: "
                             "end a channel with a rest")
        slot[2:] = [0xD, 0]
    # Patterns (identical ones are shared)
    pats, order = [], []
    for p0 in range(0, rows, 64):
        data = bytearray()
        for r in range(p0, p0 + 64):
            for ch in range(4):
                s, per, cmd, par = cells[r][ch] if r < rows else (0, 0, 0, 0)
                data += bytes(((s & 0xF0) | (per >> 8), per & 0xFF, ((s & 0x0F) << 4) | cmd, par))
        if bytes(data) not in pats:
            pats.append(bytes(data))
        order.append(pats.index(bytes(data)))
    if len(order) > 128:
        raise SoundError(f"song is {len(order)} patterns of 64 rows; the limit is 128")
    # Header
    head = song["title"].encode("latin-1", "replace")[:20].ljust(20, b"\0")
    by_no = {i.sample_no: i for i in used}
    for no in range(1, 32):
        inst = by_no.get(no)
        if not inst:
            head += b"\0" * 22 + struct.pack(">HBBHH", 0, 0, 0, 0, 1)
            continue
        words = len(inst.data) // 2
        loop = (1, words - 1) if inst.looped else (0, 1)
        finetune = 1 if inst.looped else 0   # C-1 of a 32-sample cycle is ~17 cents flat
        head += inst.name.encode()[:22].ljust(22, b"\0") + struct.pack(">HBBHH", words, finetune, inst.vol, *loop)
    head += bytes((len(order), 127)) + bytes(order).ljust(128, b"\0") + b"M.K."
    mod = head + b"".join(pats) + b"".join(by_no[n].data for n in sorted(by_no))
    seconds = rows * speed * 2.5 / song["tempo"]
    info = {"rows": rows, "patterns": len(pats), "positions": len(order), "seconds": seconds,
            "tempo": song["tempo"], "grid": song["grid"],
            "instruments": [(i.name, i.wave, i.cycle, len(i.data)) for i in used],
            "sample_bytes": sum(len(i.data) for i in used)}
    return mod, info


# --------------------------------------------------------------- MOD preview

def _parse_mod(mod):
    samples, off = [], 20
    for _ in range(31):
        length, ft, vol, rep, replen = struct.unpack(">HBBHH", mod[off + 22:off + 30])
        samples.append({"len": length * 2, "ft": ft & 15, "vol": vol, "rep": rep * 2, "replen": replen * 2})
        off += 30
    npos = mod[950]
    order = list(mod[952:952 + npos])
    npat = max(mod[952:1080]) + 1
    pat0 = 1084
    data = pat0 + npat * 1024
    for s in samples:
        s["data"] = mod[data:data + s["len"]]
        data += s["len"]
    return samples, order, mod[pat0:pat0 + npat * 1024]


def render_mod(mod, seconds=None, rate=PREVIEW_RATE):
    """A simple ProTracker renderer for previews (notes, C, A, 0xy, F, D)."""
    samples, order, pats = _parse_mod(mod)
    chans = [{"s": None, "per": 0, "vol": 0, "pos": 0.0, "arp": 0, "slide": 0, "on": False} for _ in range(4)]
    speed, bpm, out = 6, 125, []
    frac = 0.0
    limit = seconds * rate if seconds else None
    for pos in order:
        brk = False
        for row in range(64):
            if brk:
                break
            for ch in range(4):
                o = pos * 1024 + row * 16 + ch * 4
                b0, b1, b2, b3 = pats[o:o + 4]
                sno, per, cmd = (b0 & 0xF0) | (b2 >> 4), ((b0 & 0x0F) << 8) | b1, b2 & 0x0F
                c = chans[ch]
                c["arp"], c["slide"] = 0, 0
                if sno:
                    c["s"] = samples[sno - 1]
                    c["vol"] = c["s"]["vol"]
                if per:
                    c["per"], c["pos"], c["on"] = per, 0.0, True
                if cmd == 0xC:
                    c["vol"] = min(64, b3)
                elif cmd == 0xA:
                    c["slide"] = (b3 >> 4) - (b3 & 15)
                elif cmd == 0x0 and b3:
                    c["arp"] = b3
                elif cmd == 0xF:
                    if b3 < 32:
                        speed = max(1, b3)
                    else:
                        bpm = b3
                elif cmd == 0xD:
                    brk = True
            for tick in range(speed):
                if tick:
                    for c in chans:
                        c["vol"] = max(0, min(64, c["vol"] + c["slide"]))
                frac += rate * 2.5 / bpm
                n = int(frac)
                frac -= n
                buf = [0.0] * n
                for c in chans:
                    s = c["s"]
                    if not c["on"] or not s or not c["per"] or not s["len"]:
                        continue
                    per = c["per"]
                    if c["arp"]:
                        semis = (0, c["arp"] >> 4, c["arp"] & 15)[tick % 3]
                        per = per / 2 ** (semis / 12)
                    per = per / 2 ** ((s["ft"] if s["ft"] < 8 else s["ft"] - 16) / 96)
                    step = PAL_CLOCK / per / rate
                    vol = c["vol"] / 64 / 4
                    data, p = s["data"], c["pos"]
                    looped = s["replen"] > 2
                    end = s["rep"] + s["replen"] if looped else s["len"]
                    for i in range(n):
                        if p >= end:
                            if not looped:
                                c["on"] = False
                                break
                            p = s["rep"] + (p - end) % s["replen"]
                        v = data[int(p)]
                        buf[i] += (v - 256 if v > 127 else v) / 128 * vol
                        p += step
                    c["pos"] = p
                out += buf
                if limit and len(out) >= limit:
                    return out[:int(limit)]
    return out


def sfx_to_float(data):
    return [(b - 256 if b > 127 else b) / 128 for b in data]


def resample(samples, src_rate, dst_rate):
    n = int(len(samples) * dst_rate / src_rate)
    return [samples[min(len(samples) - 1, int(i * src_rate / dst_rate))] for i in range(n)]


def write_wav(path, samples, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", max(-32768, min(32767, int(v * 32767)))) for v in samples))


def read_wav(path):
    """Mono float samples and the rate of a 16-bit WAV."""
    with wave.open(path, "rb") as w:
        rate, n, ch = w.getframerate(), w.getnframes(), w.getnchannels()
        raw = w.readframes(n)
    vals = struct.unpack(f"<{len(raw) // 2}h", raw)
    if ch == 2:
        vals = [(vals[i] + vals[i + 1]) / 2 for i in range(0, len(vals), 2)]
    return [v / 32768 for v in vals], rate


# ------------------------------------------------------- analysis & pictures

def rms_windows(samples, rate, window=None):
    """RMS level of each 1/50 s window (one per PAL frame)."""
    window = window or rate // 50
    out = []
    for i in range(0, len(samples), window):
        w = samples[i:i + window]
        out.append(math.sqrt(sum(v * v for v in w) / len(w)) if w else 0.0)
    return out


AUDIBLE_RMS = 0.01        # about -40 dBFS


def _fft(re_, im_):
    n = len(re_)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            re_[i], re_[j] = re_[j], re_[i]
            im_[i], im_[j] = im_[j], im_[i]
    size = 2
    while size <= n:
        ang = -2 * math.pi / size
        wr, wi = math.cos(ang), math.sin(ang)
        for start in range(0, n, size):
            cr, ci = 1.0, 0.0
            half = size // 2
            for k in range(start, start + half):
                tr = cr * re_[k + half] - ci * im_[k + half]
                ti = cr * im_[k + half] + ci * re_[k + half]
                re_[k + half], im_[k + half] = re_[k] - tr, im_[k] - ti
                re_[k] += tr
                im_[k] += ti
                cr, ci = cr * wr - ci * wi, cr * wi + ci * wr
        size *= 2


def _c8(v):
    return int(255 * max(0.0, min(1.0, v)))


def spectrogram_png(path, samples, rate, marks=(), max_seconds=60):
    """Spectrogram (log frequency 50 Hz - 8 kHz, one column per 1/50 s) over a
    waveform strip. Marks: [(name, sample_index)] drawn as yellow lines."""
    from .image import Image
    if rate != PREVIEW_RATE:
        samples = resample(samples, rate, PREVIEW_RATE)
        marks = [(n, int(i * PREVIEW_RATE / rate)) for n, i in marks]
        rate = PREVIEW_RATE
    samples = samples[:max_seconds * rate]
    hop, size, height, wave_h = rate // 50, 512, 128, 40
    cols = max(1, len(samples) // hop)
    win = [0.5 - 0.5 * math.cos(2 * math.pi * i / size) for i in range(size)]
    fmin, fmax = 50.0, 8000.0
    bins = [min(size // 2 - 1, int(fmin * (fmax / fmin) ** (1 - y / (height - 1)) * size / rate))
            for y in range(height)]
    w = cols
    rgb = bytearray(w * (height + wave_h) * 3)

    def put(x, y, col):
        o = (y * w + x) * 3
        rgb[o:o + 3] = bytes(col)

    for x in range(cols):
        seg = samples[x * hop:x * hop + size]
        seg = seg + [0.0] * (size - len(seg))
        re_ = [seg[i] * win[i] for i in range(size)]
        im_ = [0.0] * size
        _fft(re_, im_)
        for y in range(height):
            b = bins[y]
            mag = math.sqrt(re_[b] ** 2 + im_[b] ** 2)
            db = 20 * math.log10(mag / (size / 4) + 1e-9)    # 0 dB = a full-scale sine
            v = max(0.0, min(1.0, (db + 70) / 70))           # -70..0 dB
            put(x, y, (_c8(v * 1.8), _c8(v * 1.6 - 0.5), _c8((1 - v) * v * 2.5)))
        peak = max((abs(v) for v in samples[x * hop:(x + 1) * hop]), default=0.0)
        mid = height + wave_h // 2
        h = int(peak * (wave_h // 2 - 1))
        for y in range(height, height + wave_h):
            put(x, y, (40, 200, 120) if abs(y - mid) <= h else (16, 16, 24))
    for _name, idx in marks:
        x = idx // hop
        if 0 <= x < w:
            for y in range(height + wave_h):
                put(x, y, (255, 230, 0))
    Image(bytes(rgb), w, height + wave_h).save_png(path)


# ------------------------------------------------------------------- project

def _c_name(name):
    return "".join(p.capitalize() for p in name.replace("-", "_").split("_"))


def _bytes_c(data):
    return ",\n".join("\t" + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) for i in range(0, len(data), 16))


def build(project_dir, out_dir=None, previews=True):
    """Convert sound/ into build/sound/. Returns a summary dict, or None when
    the project has no sound/sound.toml."""
    sdir = os.path.join(project_dir, "sound")
    toml_path = os.path.join(sdir, "sound.toml")
    if not os.path.exists(toml_path):
        return None
    try:
        with open(toml_path, "rb") as f:
            cfg = tomllib.load(f)
    except tomllib.TOMLDecodeError as e:
        raise SoundError(f"sound/sound.toml: {e}")
    for k in cfg:
        if k not in ("sound", "sfx", "music"):
            raise SoundError(f"sound/sound.toml: unknown table [{k}] (use [sound], [sfx.NAME], [music.NAME])")
    chans = cfg.get("sound", {}).get("sfx_channels", [3])
    if not chans or any(c not in (0, 1, 2, 3) for c in chans):
        raise SoundError("[sound] sfx_channels must list Paula channels 0-3")
    out_dir = out_dir or os.path.join(project_dir, "build", "sound")
    os.makedirs(os.path.join(out_dir, "preview"), exist_ok=True)
    sfx, music = [], []
    for name, spec in cfg.get("sfx", {}).items():
        data = synth_sfx(name, spec)
        vol, prio = int(spec.get("volume", 48)), int(spec.get("priority", 1))
        if not 0 <= vol <= 64 or not 1 <= prio <= 255:
            raise SoundError(f"sfx.{name}: volume is 0-64, priority 1-255")
        sfx.append({"name": name, "data": data, "volume": vol, "priority": prio,
                    "seconds": len(data) / SFX_RATE})
    for name, spec in cfg.get("music", {}).items():
        src = spec.get("source")
        if not src:
            raise SoundError(f"music.{name}: needs source = \"FILE.mml\"")
        path = os.path.join(sdir, src)
        if not os.path.exists(path):
            raise SoundError(f"music.{name}: {src} not found in sound/")
        try:
            mod, info = compile_mml(open(path).read(), name)
        except SoundError as e:
            raise SoundError(f"{src}: {e}")
        _write_if_changed(os.path.join(out_dir, f"{name}.mod"), mod)
        music.append({"name": name, "mod": mod, "info": info})
    if previews:
        for s in sfx:
            pcm = resample(sfx_to_float(s["data"]), SFX_RATE, PREVIEW_RATE)
            pcm = [v * s["volume"] / 64 for v in pcm]
            write_wav(os.path.join(out_dir, "preview", f"{s['name']}.wav"), pcm, PREVIEW_RATE)
            spectrogram_png(os.path.join(out_dir, "preview", f"{s['name']}.png"), pcm, PREVIEW_RATE)
        for m in music:
            pcm = render_mod(m["mod"], seconds=min(60, m["info"]["seconds"]))
            write_wav(os.path.join(out_dir, "preview", f"{m['name']}.wav"), pcm, PREVIEW_RATE)
            spectrogram_png(os.path.join(out_dir, "preview", f"{m['name']}.png"), pcm, PREVIEW_RATE)
    _write_c(sfx, music, chans, out_dir)
    chip = sum(len(s["data"]) for s in sfx) + sum(m["info"]["sample_bytes"] for m in music)
    return {"sfx": sfx, "music": music, "chip_bytes": chip, "out_dir": out_dir}


def _write_c(sfx, music, chans, out_dir):
    mask = 0
    for c in range(4):
        if c not in chans:
            mask |= 1 << c
    h = ["// Generated by agk sound from sound/ - don't edit.", "#ifndef _SOUND_H_", "#define _SOUND_H_", "",
         "#include <ace/managers/ptplayer.h>", ""]
    for i, s in enumerate(sfx):
        h.append(f"#define SOUND_SFX_{s['name'].upper()} {i}   // {s['seconds']:.2f} s")
    h.append(f"#define SOUND_SFX_COUNT {len(sfx)}")
    for i, m in enumerate(music):
        h.append(f"#define SOUND_MUSIC_{m['name'].upper()} {i}   // {m['info']['seconds']:.1f} s, loops")
    h.append(f"#define SOUND_MUSIC_COUNT {len(music)}")
    h += ["",
          "/** ptplayer (CIA-B timer) + every sample copied to chip RAM. After systemCreate(). */",
          "void soundCreate(void);",
          "void soundDestroy(void);",
          "/** Play a sound effect with its sound.toml volume and priority; prints \"AGK sfx NAME\". */",
          "void soundPlay(UBYTE ubSfx);",
          "/** Start a song from the top (it loops); prints \"AGK music NAME\". */",
          "void soundMusicStart(UBYTE ubSong);",
          "void soundMusicStop(void);",
          "", "#endif", ""]
    c = ["// Generated by agk sound from sound/ - don't edit.", "#include \"sound.h\"",
         "#include <stddef.h>", "#include <ace/managers/memory.h>", "#include <ace/managers/system.h>",
         "#include <ace/utils/custom.h>", "#include <agk/debug.h>", "",
         "// A MOD file's first 1084 bytes are exactly ptplayer's tPtplayerMod header",
         "_Static_assert(offsetof(tPtplayerMod, pPatterns) == 1084, \"tPtplayerMod layout\");", ""]
    for s in sfx:
        c += [f"static const UBYTE s_pSfx{_c_name(s['name'])}[{len(s['data'])}] __attribute__((aligned(2))) = {{",
              _bytes_c(s["data"]), "};"]
    names = ", ".join(f'"AGK sfx {s["name"]}\\n"' for s in sfx) or '""'
    c += ["", "static const struct { const UBYTE *pData; UWORD uwBytes; UBYTE ubVolume, ubPriority; } s_pSfxDefs[] = {"]
    c += [f"\t{{s_pSfx{_c_name(s['name'])}, {len(s['data'])}, {s['volume']}, {s['priority']}}}," for s in sfx] or ["\t{0, 0, 0, 0},"]
    c += ["};", f"static const char *const s_pSfxNames[] = {{{names}}};", ""]
    for m in music:
        mod = m["mod"]
        npat = max(mod[952:1080]) + 1
        head_len = 1084 + npat * 1024
        c += [f"static const UBYTE s_pMod{_c_name(m['name'])}[{head_len}] __attribute__((aligned(2))) = {{",
              _bytes_c(mod[:head_len]), "};",
              f"static const UBYTE s_pMod{_c_name(m['name'])}Samples[{len(mod) - head_len}] __attribute__((aligned(2))) = {{",
              _bytes_c(mod[head_len:]), "};"]
    mnames = ", ".join(f'"AGK music {m["name"]}\\n"' for m in music) or '""'
    c += ["", "static const struct { const UBYTE *pMod; ULONG ulHeadBytes; const UBYTE *pSamples; ULONG ulSampleBytes; } s_pMusicDefs[] = {"]
    for m in music:
        npat = max(m["mod"][952:1080]) + 1
        head_len = 1084 + npat * 1024
        n = _c_name(m["name"])
        c.append(f"\t{{s_pMod{n}, {head_len}, s_pMod{n}Samples, {len(m['mod']) - head_len}}},")
    if not music:
        c.append("\t{0, 0, 0, 0},")
    c += ["};", f"static const char *const s_pMusicNames[] = {{{mnames}}};", "",
          f"static tPtplayerSfx s_pSfx[{max(1, len(sfx))}];",
          f"static tPtplayerMod s_pMods[{max(1, len(music))}];",
          f"static UBYTE *s_pChip[{max(1, len(sfx) + len(music))}];",
          f"static ULONG s_pChipSize[{max(1, len(sfx) + len(music))}];", "",
          "static UBYTE *copyToChip(UBYTE ubSlot, const UBYTE *pSrc, ULONG ulBytes) {",
          "\tUBYTE *pDst = memAllocChip(ulBytes);",
          "\tfor(ULONG i = 0; i < ulBytes; ++i) pDst[i] = pSrc[i];",
          "\ts_pChip[ubSlot] = pDst;", "\ts_pChipSize[ubSlot] = ulBytes;", "\treturn pDst;", "}", "",
          "void soundCreate(void) {",
          "\tptplayerCreate(systemIsPal());",
          f"\tptplayerSetMusicChannelMask(0x{mask:X}); // sound effects use channel(s) {', '.join(map(str, chans))}",
          f"\tfor(UBYTE i = 0; i < SOUND_SFX_COUNT; ++i) {{",
          "\t\ts_pSfx[i].pData = (UWORD*)copyToChip(i, s_pSfxDefs[i].pData, s_pSfxDefs[i].uwBytes);",
          "\t\ts_pSfx[i].uwWordLength = s_pSfxDefs[i].uwBytes / 2;",
          f"\t\ts_pSfx[i].uwPeriod = systemIsPal() ? {SFX_PERIOD} : {round(3579545 / SFX_RATE)};",
          "\t}",
          "\tfor(UBYTE i = 0; i < SOUND_MUSIC_COUNT; ++i) {",
          "\t\ttPtplayerMod *pMod = &s_pMods[i];",
          "\t\tconst UBYTE *pSrc = s_pMusicDefs[i].pMod;",
          "\t\tfor(UWORD b = 0; b < 1084; ++b) ((UBYTE*)pMod)[b] = pSrc[b];",
          "\t\tpMod->pPatterns = (UBYTE*)&pSrc[1084];",
          "\t\tpMod->ulPatternsSize = s_pMusicDefs[i].ulHeadBytes - 1084;",
          "\t\tpMod->isOwningSamples = 0;",
          "\t\tUBYTE *pChip = copyToChip(SOUND_SFX_COUNT + i, s_pMusicDefs[i].pSamples, s_pMusicDefs[i].ulSampleBytes);",
          "\t\tfor(UBYTE s = 0; s < PTPLAYER_MOD_SAMPLE_COUNT; ++s) {",
          "\t\t\tUWORD uwWords = pMod->pSampleHeaders[s].uwLength;",
          "\t\t\tpMod->pSampleStarts[s] = uwWords ? (UWORD*)pChip : 0;",
          "\t\t\tpChip += uwWords * 2;",
          "\t\t}",
          "\t}",
          "}", "",
          "void soundDestroy(void) {",
          "\tptplayerDestroy();",
          "\tfor(UBYTE i = 0; i < SOUND_SFX_COUNT + SOUND_MUSIC_COUNT; ++i) {",
          "\t\tif(s_pChip[i]) memFree(s_pChip[i], s_pChipSize[i]);",
          "\t\ts_pChip[i] = 0;",
          "\t}",
          "}", "",
          "void soundPlay(UBYTE ubSfx) {",
          "\tif(ubSfx >= SOUND_SFX_COUNT) return;",
          "\tptplayerSfxPlay(&s_pSfx[ubSfx], PTPLAYER_SFX_CHANNEL_ANY, s_pSfxDefs[ubSfx].ubVolume, s_pSfxDefs[ubSfx].ubPriority);",
          "\tagkPrint(s_pSfxNames[ubSfx]); // one host transfer: \"AGK sfx NAME\"",
          "}", "",
          "void soundMusicStart(UBYTE ubSong) {",
          "\tif(ubSong >= SOUND_MUSIC_COUNT) return;",
          "\tptplayerLoadMod(&s_pMods[ubSong], 0, 0);",
          "\tptplayerConfigureSongRepeat(1, 0);",
          "\tptplayerEnableMusic(1);",
          "\tagkPrint(s_pMusicNames[ubSong]);",
          "}", "",
          "void soundMusicStop(void) {",
          "\tptplayerStop();",
          "}", ""]
    _write_if_changed(os.path.join(out_dir, "sound.h"), "\n".join(h).encode())
    _write_if_changed(os.path.join(out_dir, "sound.c"), "\n".join(c).encode())


def _write_if_changed(path, data):
    """Keep the old mtime when nothing changed, so the ADF isn't rebuilt for nothing."""
    if os.path.exists(path):
        with open(path, "rb") as f:
            if f.read() == data:
                return
    with open(path, "wb") as f:
        f.write(data)
