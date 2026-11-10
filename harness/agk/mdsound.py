"""agk port's sound: a Mega Drive game's music and effects for Paula.

The Mega Drive's sound is a Z80 program feeding an FM chip (YM2612: six
voices of four operators, one of them a sample channel) and a PSG (three
squares and noise). None of that exists on an Amiga, and emulating it costs
more than an A1200 has. So the driver is run where it is cheap, on the host:
`game --sound-rip DIR` (recomp/md) plays every request the game can make and
logs what the chips were told. This module turns that log into what Paula can
play:

  notes        each key-on becomes a note: its FM patch, its pitch and level
               frame by frame (so vibrato, slides and fades come along)
  instruments  every patch is rendered once per octave it is used in, by an
               FM synthesiser here, to a short 8-bit sample: the attack and
               the decay, then a loop of one period. The slow part of its
               envelope is taken out of the sample and played as volume.
               PSG squares and noise are tiny loops; DAC samples are the
               game's own, resampled
  four voices  at any moment the four most important notes get Paula's four
               channels (decided here, not on the Amiga)
  the stream   per sixtieth of a second, per channel: start sample N, period,
               volume. The Amiga's player (recomp/mdamiga/sound.c) only does
               what the stream says
  requests     what each value written to the driver means (music, an effect
               over the music, stop, fade) is read from what it did to music
               that was playing

build(DIR) -> the data for the Amiga (see `pack`); wav(...) renders a stream
the way Paula would, for listening on the host.
"""
import math
import os
import struct
import zlib

PAULA = 3546895.0                     # Paula's clock (PAL); a sample rate is this over the period
FM_RATE = 7670453.0 / 144             # the YM2612's sample rate
Z80_CLOCK = 3579545.0
TICKS = 60
PSG_GAIN = 0.35                       # a square at full volume, against one FM carrier at full level
DAC_GAIN = 1.0
MAX_RATE = 28000.0
CARRIERS = {0: (3,), 1: (3,), 2: (3,), 3: (3,), 4: (1, 3), 5: (1, 2, 3), 6: (1, 2, 3), 7: (0, 1, 2, 3)}
SLOT_OP = (0, 2, 1, 3)                # the register's slot (bits 2,3 of its number) -> the operator


class SoundError(Exception):
    pass


class Note:
    __slots__ = ("kind", "voice", "t0", "t1", "patch", "f", "a", "pan", "inst", "end", "peak", "vol")

    def __init__(self, kind, voice, t0):
        self.kind, self.voice, self.t0, self.t1 = kind, voice, t0, None
        self.patch = None
        self.f, self.a = [], []           # (frame, Hz) and (frame, amplitude) where they change
        self.pan = 3


# ------------------------------------------------------------------ the log
def index(d):
    """sound.idx -> [(offset, addr, value, mode, frames, loop)]"""
    out = []
    for line in open(os.path.join(d, "sound.idx")):
        w = line.split()
        out.append((int(w[0]), int(w[1], 16), int(w[2], 16), int(w[3]), int(w[4]), int(w[5])))
    return out


def parse(buf, off, limit=None):
    """one request of the log -> (notes, frames, dac samples {id: bytes}); mode 2 -> (key-ons, levels, frames)"""
    assert buf[off] == 0x52
    mode = buf[off + 4]
    p = off + 5
    reg = [bytearray(256), bytearray(256)]
    fnum = [0] * 6
    block = [0] * 6
    on = [False] * 6
    cur = [None] * 6                      # the note sounding on each FM voice
    fresh = [False] * 6
    notes = []
    frame = 0
    tone = [0, 0, 0]
    att = [15, 15, 15, 15]
    noise = 0
    latch = 0
    pcur = [None] * 4
    pstate = [None] * 4
    dacs = {}
    bursts = []
    keyons, levels = [], []

    def fm_state(c):
        part, ch = divmod(c, 3)
        r = reg[part]
        alg = r[0xB0 + ch] & 7
        tl = [0] * 4
        for s in range(4):
            tl[SLOT_OP[s]] = r[0x40 + s * 4 + ch] & 127
        base = min(tl[o] for o in CARRIERS[alg])
        return alg, tl, base

    def fm_patch(c):
        part, ch = divmod(c, 3)
        r = reg[part]
        alg, tl, base = fm_state(c)
        ops = [None] * 4
        for s in range(4):
            o = SLOT_OP[s]
            k = s * 4 + ch
            ops[o] = (r[0x30 + k] & 15, (tl[o] - base) if o in CARRIERS[alg] else tl[o], r[0x50 + k] >> 6, r[0x50 + k] & 31,
                      r[0x60 + k] & 31, r[0x70 + k] & 31, r[0x80 + k] >> 4, r[0x80 + k] & 15)
        return (alg, r[0xB0 + ch] >> 3 & 7) + tuple(ops)

    while True:
        c = buf[p]
        if c == 0x59:                     # 'Y'
            part, r, v = buf[p + 1], buf[p + 2], buf[p + 3]
            p += 6
            if mode == 2:
                if r == 0x28:
                    keyons.append((frame, v))
                else:
                    levels.append((frame, part, r, v))
                continue
            reg[part][r] = v
            if r >= 0xA0 and r < 0xA3:
                ch = part * 3 + r - 0xA0
                hi = reg[part][0xA4 + r - 0xA0]
                fnum[ch] = (hi & 7) << 8 | v
                block[ch] = hi >> 3 & 7
            elif part == 0 and r == 0x28:
                ch = v & 7
                if ch & 3 == 3:
                    continue
                ch = ch - 1 if ch > 3 else ch
                k = v >> 4 != 0
                if k and not on[ch]:
                    if cur[ch] is not None and cur[ch].t1 is None:
                        cur[ch].t1 = frame
                    if ch == 5 and reg[0][0x2B] & 0x80:
                        cur[ch] = None    # the sixth voice is the DAC just now
                    else:
                        cur[ch] = Note("fm", ch, frame)
                        notes.append(cur[ch])
                        fresh[ch] = True
                elif not k and on[ch] and cur[ch] is not None and cur[ch].t1 is None:
                    cur[ch].t1 = frame
                on[ch] = k
        elif c == 0x50:                   # 'P'
            v = buf[p + 1]
            p += 4
            if v & 0x80:
                latch = v >> 4 & 7
                ch = latch >> 1
                if latch & 1:
                    att[ch] = v & 15
                elif ch < 3:
                    tone[ch] = (tone[ch] & 0x3F0) | (v & 15)
                else:
                    noise = v & 7
            else:
                ch = latch >> 1
                if latch & 1:
                    att[ch] = v & 15
                elif ch < 3:
                    tone[ch] = (tone[ch] & 15) | (v & 0x3F) << 4
                else:
                    noise = v & 7
        elif c == 0x46:                   # 'F': the frame ends; what stands now is what sounds
            p += 1
            if mode == 1:
                for ch in range(6):
                    n = cur[ch]
                    if n is None:
                        continue
                    if fresh[ch]:
                        n.patch = fm_patch(ch)
                        n.pan = reg[ch // 3][0xB4 + ch % 3] >> 6 or 3
                        fresh[ch] = False
                    if n.t1 is not None and n.t1 < frame:
                        cur[ch] = None    # (its last levels were taken the frame it was released)
                        continue
                    f = fnum[ch] * (1 << block[ch]) * FM_RATE / (1 << 21)
                    if not n.f or n.f[-1][1] != f:
                        n.f.append((frame, f))
                    a = 10 ** (-0.75 * fm_state(ch)[2] / 20)
                    if not n.a or n.a[-1][1] != a:
                        n.a.append((frame, a))
                for ch in range(4):
                    if ch < 3:
                        per = tone[ch] or 1024
                        st = (per,) if att[ch] < 15 and per > 5 else None
                        f = Z80_CLOCK / (32 * per)
                    else:
                        r = noise & 3
                        div = (tone[2] or 1024) if r == 3 else (16, 32, 64)[r]
                        st = (noise >> 2,) if att[3] < 15 else None
                        f = Z80_CLOCK / (16 * div)      # the shift register's rate
                    n = pcur[ch]
                    if st is None or (n is not None and st != pstate[ch] and ch == 3):
                        if n is not None:
                            n.t1 = frame
                            pcur[ch] = n = None
                    if st is not None:
                        if n is None:
                            n = pcur[ch] = Note("psg" if ch < 3 else "noise", 6 + ch, frame)
                            n.patch = st if ch == 3 else None
                            notes.append(n)
                        if not n.f or n.f[-1][1] != f:
                            n.f.append((frame, f))
                        a = 10 ** (-2 * att[ch] / 20) * PSG_GAIN
                        if not n.a or n.a[-1][1] != a:
                            n.a.append((frame, a))
                    pstate[ch] = st
            frame += 1
            if limit and frame >= limit:
                break
        elif c == 0x44:                   # 'D': a run of DAC samples
            fr, cyc, n, span = struct.unpack_from("<4I", buf, p + 1)
            data = bytes(buf[p + 17:p + 17 + n])
            p += 17 + n
            if n and mode == 1:
                bursts.append([fr * FRAME_T + cyc, span, data])
        elif c == 0x4C:                   # 'L'
            p += 5
        elif c == 0x45:                   # 'E'
            break
        else:
            raise SoundError(f"sound.rip: unknown record {c:#x} at {p}")
    if mode == 2:
        return keyons, levels, frame
    notes = [n for n in notes if n.f and n.a and n.f[0][1] > 1]
    for t, span, data in dac_hits(bursts):
        key = zlib.crc32(data[:DAC_HEAD])
        if len(data) > len(dacs.get(key, b"")):
            dacs[key] = data
        fr = int(t // FRAME_T)
        note = Note("dac", 10, fr)
        note.patch = key
        note.f = [(fr, (len(data) - 1) * Z80_CLOCK / span)]
        note.a = [(fr, DAC_GAIN)]
        note.t1 = fr + max(1, int(span / Z80_CLOCK * 60))
        notes.append(note)
    notes.sort(key=lambda n: n.t0)
    return notes, frame, dacs


FRAME_T = 59666                       # Z80 T-states a frame
DAC_HEAD = 24


def dac_hits(bursts):
    """runs of DAC writes [[T-state, span, bytes]] -> one (T-state, span, bytes) per drum hit. The driver's music
    update holds its sample loop up every frame, so a hit arrives as several runs: runs closer than half a frame
    are one. And a hit that cuts the last one short starts in the middle of a run: every hit of a sample begins
    with the same bytes, so a run is cut wherever the beginning of a known hit turns up inside it."""
    runs = []
    for t, span, data in bursts:
        if runs and t - (runs[-1][0] + runs[-1][1]) < FRAME_T // 2:
            r = runs[-1]
            r[1] = t + span - r[0]
            r[2] += data
        else:
            runs.append([t, span, bytes(data)])
    runs = [r for r in runs if len(r[2]) >= 64 and r[1]]
    heads = {r[2][:DAC_HEAD] for r in runs if len(set(r[2][:DAC_HEAD])) >= 4}
    out = []
    for t, span, data in runs:
        a = 0
        n = len(data)
        while a < n:
            cut = n
            for h in heads:
                k = data.find(h, a + DAC_HEAD)
                if 0 < k < cut:
                    cut = k
            if cut - a >= 64:
                out.append((t + span * a / n, span * (cut - a) / n, data[a:cut]))
            a = cut
    return out


# ------------------------------------------------------------- the FM synth
def _rate(r, ks, kc):
    return 0 if r == 0 else min(63, 2 * r + (kc >> (3 - ks)))


def _ups(r):
    """an envelope rate (0..63) -> attenuation units (of 1024 for 96 dB) a second"""
    return 0.0 if r < 4 else FM_RATE / 3 * 2.0 ** ((r >> 2) - 11) * (4 + (r & 3)) / 8


def _keycode(f):
    block = 0
    fn = f * (1 << 21) / FM_RATE
    while fn >= 2048 and block < 7:
        fn /= 2
        block += 1
    fn = int(fn)
    f11, f10, f9, f8 = fn >> 10 & 1, fn >> 9 & 1, fn >> 8 & 1, fn >> 7 & 1
    return block << 2 | f11 << 1 | ((f11 & (f10 | f9 | f8)) | ((1 - f11) & f10 & f9 & f8))


def synth(patch, f, seconds, rate):
    """a note of the patch held for `seconds` at f Hz -> floats at `rate` (one carrier at full level = 1.0)"""
    alg, fb = patch[0], patch[1]
    kc = _keycode(f)
    n = int(seconds * rate)
    dt = 1.0 / rate
    two_pi = 2 * math.pi
    inc, tl, ka, kd, ksr, sl = [], [], [], [], [], []
    for mul, t, ks, ar, dr, sr, s, rr in patch[2:6]:
        inc.append(f * (mul or 0.5) * dt)
        tl.append(10 ** (-t * 0.75 / 20))
        ra = _rate(ar, ks, kc)
        ka.append(None if ra >= 62 else _ups(ra) / 16 * dt)
        kd.append(_ups(_rate(dr, ks, kc)) * dt)
        ksr.append(_ups(_rate(sr, ks, kc)) * dt)
        sl.append(992 if s == 15 else s * 32)
    att = [1023.0 if k is not None else 0.0 for k in ka]
    phase_ = [0 if k is not None else 1 for k in ka]     # 0 attack, 1 decay, 2 sustain
    ph = [0.0] * 4
    o = [0.0] * 4
    fb1 = fb2 = 0.0
    fbk = 4 * math.pi * 2.0 ** (fb - 7) if fb else 0.0
    car = CARRIERS[alg]
    out = [0.0] * n
    sin = math.sin
    M = 8 * math.pi
    DB = 96.0 / 1024 / 20
    for i in range(n):
        amp = [0.0] * 4
        for k in range(4):
            a = att[k]
            st = phase_[k]
            if st == 0:
                a -= (a + 1) * ka[k]
                if a <= 0.5:
                    a = 0.0
                    phase_[k] = 1
            elif st == 1:
                a += kd[k]
                if a >= sl[k]:
                    phase_[k] = 2
            elif a < 1023:
                a += ksr[k]
            att[k] = a
            amp[k] = 0.0 if a >= 1023 else tl[k] * 10 ** (-a * DB)
            ph[k] += inc[k]
        o1 = sin(two_pi * ph[0] + fbk * (fb1 + fb2) * 0.5) * amp[0]
        fb2 = fb1
        fb1 = o1
        if alg == 0:
            o2 = sin(two_pi * ph[1] + M * o1) * amp[1]
            o3 = sin(two_pi * ph[2] + M * o2) * amp[2]
            o4 = sin(two_pi * ph[3] + M * o3) * amp[3]
        elif alg == 1:
            o2 = sin(two_pi * ph[1]) * amp[1]
            o3 = sin(two_pi * ph[2] + M * (o1 + o2)) * amp[2]
            o4 = sin(two_pi * ph[3] + M * o3) * amp[3]
        elif alg == 2:
            o2 = sin(two_pi * ph[1]) * amp[1]
            o3 = sin(two_pi * ph[2] + M * o2) * amp[2]
            o4 = sin(two_pi * ph[3] + M * (o1 + o3)) * amp[3]
        elif alg == 3:
            o2 = sin(two_pi * ph[1] + M * o1) * amp[1]
            o3 = sin(two_pi * ph[2]) * amp[2]
            o4 = sin(two_pi * ph[3] + M * (o2 + o3)) * amp[3]
        elif alg == 4:
            o2 = sin(two_pi * ph[1] + M * o1) * amp[1]
            o3 = sin(two_pi * ph[2]) * amp[2]
            o4 = sin(two_pi * ph[3] + M * o3) * amp[3]
        elif alg == 5:
            o2 = sin(two_pi * ph[1] + M * o1) * amp[1]
            o3 = sin(two_pi * ph[2] + M * o1) * amp[2]
            o4 = sin(two_pi * ph[3] + M * o1) * amp[3]
        elif alg == 6:
            o2 = sin(two_pi * ph[1] + M * o1) * amp[1]
            o3 = sin(two_pi * ph[2]) * amp[2]
            o4 = sin(two_pi * ph[3]) * amp[3]
        else:
            o2 = sin(two_pi * ph[1]) * amp[1]
            o3 = sin(two_pi * ph[2]) * amp[2]
            o4 = sin(two_pi * ph[3]) * amp[3]
        o = (o1, o2, o3, o4)
        s = 0.0
        for k in car:
            s += o[k]
        out[i] = s
    return out


def carrier_slopes(patch, f):
    """-> (dB a second while held after the decay, dB a second once released), by the loudest carrier"""
    kc = _keycode(f)
    car = min(CARRIERS[patch[0]], key=lambda k: patch[2 + k][1])
    mul, t, ks, ar, dr, sr, s, rr = patch[2 + car]
    return _ups(_rate(sr, ks, kc)) * 96 / 1024, _ups(_rate(2 * rr + 1, ks, kc)) * 96 / 1024


class Sample:
    __slots__ = ("data", "loop", "looplen", "per", "env", "scale", "hold", "rel", "index", "base")
    # data: signed 8-bit; loop/looplen in bytes (looplen 0: plays once); per: samples a period (FM, PSG) or
    # the resampling ratio (DAC); env: its volume a tick (FM), scale: amplitude of full scale


def band(f):
    """-> (the octave band's index, samples a period there, the band's centre)"""
    b = max(0, min(7, int(math.floor(math.log2(max(f, 28.0) / 27.5)))))
    lo = 27.5 * 2 ** b
    per = 256 >> b if b < 6 else 4
    while lo * 2 * per > MAX_RATE and per > 2:
        per //= 2
    return b, per, lo * math.sqrt(2)


def carrier_level(patch, f, nticks):
    """the carriers' summed level at each tick of a held note, from the envelope generators alone"""
    kc = _keycode(f)
    sub = 16
    dt = 1.0 / TICKS / sub
    ops = []
    for k in CARRIERS[patch[0]]:
        mul, t, ks, ar, dr, sr, s, rr = patch[2 + k]
        ra = _rate(ar, ks, kc)
        ops.append([10 ** (-t * 0.75 / 20), None if ra >= 62 else _ups(ra) / 16 * dt, _ups(_rate(dr, ks, kc)) * dt,
                    _ups(_rate(sr, ks, kc)) * dt, 992 if s == 15 else s * 32, 0.0 if ra >= 62 else 1023.0, 1 if ra >= 62 else 0])
    out = []
    for _ in range(nticks):
        for _ in range(sub):
            for o in ops:
                if o[6] == 0:
                    o[5] -= (o[5] + 1) * o[1]
                    if o[5] <= 0.5:
                        o[5], o[6] = 0.0, 1
                elif o[6] == 1:
                    o[5] += o[2]
                    if o[5] >= o[4]:
                        o[6] = 2
                elif o[5] < 1023:
                    o[5] += o[3]
        out.append(sum(0.0 if o[5] >= 1023 else o[0] * 10 ** (-o[5] * 96.0 / 1024 / 20) for o in ops))
    return out


def fm_sample(patch, b, hold):
    """the patch in octave band b, for notes held up to `hold` seconds -> Sample"""
    lo = 27.5 * 2 ** b
    _, per, centre = band(lo * 1.01)
    whole = 2 if any(op[0] == 0 for op in patch[2:6]) else 1     # a half-multiple operator: two periods repeat
    rate = centre * per
    over = max(1, int(math.ceil(40000.0 / rate)))
    seconds = min(max(hold, 0.1), 0.4)
    lp = per * whole
    nper = max(3, int(math.ceil(seconds * centre / whole))) * whole
    y = synth(patch, centre, nper / centre + 0.01, rate * over)
    if over > 1:                          # down to the sample's rate: a windowed-sinc low pass, then every over-th
        taps = 4 * over
        h = []
        for k in range(-taps, taps + 1):
            x = k / over
            sinc = 1.0 if k == 0 else math.sin(math.pi * x) / (math.pi * x)
            h.append(sinc * (0.5 + 0.5 * math.cos(math.pi * k / (taps + 1))) / over)
        m = len(y)
        z = []
        for i in range(0, nper * per * over, over):
            acc = 0.0
            for j in range(max(0, i - taps), min(m, i + taps + 1)):
                acc += y[j] * h[j - i + taps]
            z.append(acc)
        y = z
    y = y[:nper * per]
    n = len(y)
    tick = rate / TICKS
    nt = int(math.ceil(n / tick))
    win = max(tick, lp)
    e = []
    for k in range(nt):
        a = int(k * tick)
        seg = y[a:int(a + win) + 1]
        e.append(max(max(seg), -min(seg), 1e-4))
    es = [e[0]]                           # the slow part of the envelope: a dB a tick at most; the rest stays in the sample
    step = 10 ** (1 / 20)
    for k in range(1, nt):
        es.append(min(max(e[k], es[-1] / step), es[-1] * step))
    yn = [y[i] / es[min(nt - 1, int(i / tick))] for i in range(n)]
    top = max(e)
    s = Sample()
    s.looplen = lp
    for k in range(nt):                   # died away: it plays once, to there
        if e[k] < top * 0.004 and all(x < top * 0.004 for x in e[k:]):
            n = max(lp, int(k * tick) // lp * lp)
            s.looplen = 0
            break
    if s.looplen:                         # the earliest period the wave has stopped changing at: the loop
        def rms(a, b_):
            return math.sqrt(sum((yn[a + i] - yn[b_ + i]) ** 2 for i in range(lp)) / lp)
        first = int(math.ceil(2 * tick / lp))
        for m in range(first, n // lp - 2):
            ref = math.sqrt(sum(yn[m * lp + i] ** 2 for i in range(lp)) / lp) or 1e-9
            if rms(m * lp, (m + 1) * lp) < 0.05 * ref and rms(m * lp, (m + 2) * lp) < 0.08 * ref:
                n = (m + 1) * lp
                break
    yn = yn[:n]
    peak = max(max(yn), -min(yn), 1e-6)
    s.data = bytes(int(round(v / peak * 127)) & 0xFF for v in yn)
    s.scale = peak
    nk = max(1, min(nt, int(n / tick)))
    want = min(900, int(hold * TICKS) + 2)
    if s.looplen and want > nk:           # held past the sample: the carriers' envelopes say how the level goes on
        lev = carrier_level(patch, centre, want)
        ref = max(lev[nk - 1], 1e-6)
        es = es[:nk] + [es[nk - 1] * lev[k] / ref for k in range(nk, want)]
    else:
        es = es[:nk]
    s.env = es
    s.per = per
    s.base = centre
    s.hold, s.rel = carrier_slopes(patch, centre)
    s.loop = n - s.looplen
    if n & 1:
        s.data += s.data[-1:]
    return s


def psg_sample(per):
    s = Sample()
    s.data = bytes([100] * (per // 2) + [156] * (per // 2))
    s.loop, s.looplen, s.per, s.scale, s.env = 0, per, per, 100 / 127, None
    return s


def noise_sample(periodic):
    s = Sample()
    if periodic:
        s.data = bytes(([100] + [156] * 15) * 2)
        s.loop, s.looplen = 0, 32
    else:
        lfsr = 0x8000
        out = bytearray()
        for _ in range(4096):
            out.append(90 if lfsr & 1 else 166)
            lfsr = (lfsr >> 1) | (((lfsr ^ (lfsr >> 3)) & 1) << 15)
        s.data = bytes(out)
        s.loop, s.looplen = 0, 4096
    s.per, s.scale, s.env = 1, 90 / 127, None
    return s


def dac_sample(data, top_rate):
    """the game's 8-bit samples (unsigned) -> Sample; halved until Paula can play its fastest use"""
    ratio = 1
    v = [b - 128 for b in data]
    while top_rate / ratio > 14000 and len(v) > 256:
        v = [(v[i] + v[i + 1]) // 2 for i in range(0, len(v) - 1, 2)]
        ratio *= 2
    if len(v) & 1:
        v.append(0)
    s = Sample()
    s.data = bytes(x & 0xFF for x in v)
    s.loop, s.looplen, s.per, s.scale, s.env = len(v), 0, ratio, 1.0, None
    return s


# ------------------------------------------------------------ notes -> Paula
class Pool:
    """every sample the streams use, made once"""

    def __init__(self):
        self.samples = {}                 # key -> Sample
        self.order = []
        self.hold = {}                    # (patch, band) -> the longest such a note is held, in seconds
        self.dacs = {}
        self.dac_rate = {}

    def want(self, notes, dacs):
        for k, d in dacs.items():
            if len(d) > len(self.dacs.get(k, b"")):
                self.dacs[k] = d
        for n in notes:
            if n.kind == "fm":
                key = (n.patch, band(n.f[0][1])[0])
                held = ((n.t1 if n.t1 is not None else n.t0 + 600) - n.t0) / TICKS
                self.hold[key] = max(self.hold.get(key, 0), held)
            elif n.kind == "dac":
                self.dac_rate[n.patch] = max(self.dac_rate.get(n.patch, 0), n.f[0][1])

    def get(self, n):
        if n.kind == "fm":
            key = (n.patch, band(n.f[0][1])[0])
        elif n.kind == "psg":
            f = max(x[1] for x in n.f)
            per = 32
            while f * per > MAX_RATE and per > 2:
                per //= 2
            key = ("psg", per)
        elif n.kind == "noise":
            key = ("noise", n.patch[0])
        else:
            key = ("dac", n.patch)
        s = self.samples.get(key)
        if s is None:
            if n.kind == "fm":
                s = fm_sample(key[0], key[1], self.hold.get(key, 0.3))
            elif n.kind == "psg":
                s = psg_sample(key[1])
            elif n.kind == "noise":
                s = noise_sample(not key[1])
            else:
                s = dac_sample(self.dacs[key[1]], self.dac_rate.get(key[1], 8000))
            s.index = len(self.order)
            self.order.append(s)
            self.samples[key] = s
        return s


def _track(pairs, t0, n):
    """[(frame, value)] -> the value at each of n ticks from t0"""
    out = []
    i = 0
    v = pairs[0][1]
    for t in range(t0, t0 + n):
        while i < len(pairs) and pairs[i][0] <= t:
            v = pairs[i][1]
            i += 1
        out.append(v)
    return out


def shape(n, pool, total):
    """a note's volume at each tick it can be heard (n.vol, amplitude; n.end = the tick it is gone), its sample"""
    s = n.inst = pool.get(n)
    last = n.t1 if n.t1 is not None else total
    if n.kind == "dac":
        dur = max(1, last - n.t0)
        n.vol = [n.a[0][1]] * dur
    elif n.kind != "fm":
        n.vol = _track(n.a, n.t0, max(1, last - n.t0))
    else:
        held = max(1, last - n.t0)
        lvl = _track(n.a, n.t0, held)
        env = s.env
        ne = len(env)
        vol = []
        cur = 0.0
        for k in range(held):
            if k < ne:
                cur = env[k]
            elif not s.looplen:
                cur = 0.0
            else:
                cur *= 10 ** (-s.hold / TICKS / 20)
            vol.append(cur * lvl[k] / 1.0)
        if n.t1 is not None and vol[-1] > 0:      # released: it dies away at the release rate
            step = 10 ** (-max(s.rel, 30.0) / TICKS / 20)
            v = vol[-1]
            k = held
            while v > 0.003 and len(vol) < held + 240 and (k < ne or s.looplen):
                v *= step
                vol.append(v)
                k += 1
        n.vol = vol
    while len(n.vol) > 1 and n.vol[-1] < 0.003:
        n.vol.pop()
    n.end = n.t0 + len(n.vol)
    n.peak = max(n.vol[:8])


WEIGHT = {"dac": 1.4, "fm": 1.0, "psg": 0.7, "noise": 0.5}
SIDE = (1, 2, 2, 1)                   # Paula's channels 0 and 3 are on the left, 1 and 2 on the right (bit 0 left)


def arrange(notes, total, nch=4):
    """the notes onto nch channels -> per channel [(note, from tick, to tick)]"""
    segs = [[] for _ in range(nch)]
    cur = [None] * nch
    load = [0, 0, 0]
    for n in sorted(notes, key=lambda n: (n.t0, -n.peak * WEIGHT[n.kind])):
        t = n.t0
        free = [c for c in range(nch) if cur[c] is None or cur[c].end <= t]
        imp = n.peak * WEIGHT[n.kind]
        if free:
            want = [c for c in free if nch == 4 and n.pan != 3 and SIDE[c] == (2 if n.pan == 1 else 1)]
            same = [c for c in free if cur[c] is not None and cur[c].voice == n.voice]
            c = (same or want or sorted(free, key=lambda c: (load[SIDE[c % 4]], c)))[0]
        else:
            def now(c):
                m = cur[c]
                return m.vol[min(len(m.vol) - 1, t - m.t0)] * WEIGHT[m.kind] * (0.6 if m.voice == n.voice else 1.0)
            c = min(range(nch), key=now)
            if now(c) > imp:
                continue                  # everything sounding matters more than this note
            m, a, _ = segs[c][-1]
            segs[c][-1] = (m, a, t)
        cur[c] = n
        load[SIDE[c % 4]] += 1
        segs[c].append((n, t, min(n.end, total)))
    return segs


def frames_of(segs, total, gain):
    """-> per channel, per tick: (sample index to start or -1, period, volume 0..64) or None while silent"""
    out = []
    for ch in segs:
        row = [None] * total
        for n, a, b in ch:
            s = n.inst
            f = _track(n.f, n.t0, b - n.t0)
            for t in range(a, min(b, total)):
                k = t - n.t0
                hz = max(1.0, f[k] if k < len(f) else f[-1])
                if n.kind == "dac":
                    per = PAULA / (hz / s.per)
                elif n.kind == "noise":
                    per = PAULA / hz
                else:
                    per = PAULA / (hz * s.per)
                per = int(round(min(65535, max(124, per))))
                v = n.vol[k] * s.scale * gain if k < len(n.vol) else 0
                vol = int(min(64, round(v * 64)))
                if t > a and row[t - 1] is not None:      # small changes wait: a few cents, a step of volume for three ticks
                    _, per0, vol0 = row[t - 1]
                    if abs(per - per0) * 400 < per0:
                        per = per0
                    if vol and abs(vol - vol0) < 2 and (t - a) % 3:
                        vol = vol0
                row[t] = (s.index if t == a and a == n.t0 else -1, per, vol)
        out.append(row)
    return out


def encode(rows, a, b, local, state=None, sync=False):
    """ticks a..b of the channels' rows as the player's bytes. A byte under $80: that many more ticks with no
    change; else its low bits say which channels follow, each a flags byte (1: start a sample, a byte: its
    number in the stream's own list `local`; 2: period, a word; 8: period, a signed byte's change; 4: volume,
    a byte). `sync`: the first tick sets every channel outright (a loop is entered there)"""
    nch = len(rows)
    st = state if state is not None else [[-1, 0] for _ in range(nch)]      # per channel: period, volume
    out = bytearray()
    wait = 0
    for t in range(a, b):
        ev = bytearray()
        mask = 0
        for c in range(nch):
            r = rows[c][t]
            fl = 0
            body = bytearray()
            if r is None:
                if st[c][1] != 0 or (sync and t == a):
                    fl = 4
                    body.append(0)
                    st[c][1] = 0
            else:
                inst, per, vol = r
                if inst < 0 and sync and t == a:          # sounding across the loop's start: started again there
                    k = t
                    while k > 0 and rows[c][k] is not None and rows[c][k][0] < 0:
                        k -= 1
                    inst = rows[c][k][0] if rows[c][k] is not None else -1
                if inst >= 0:
                    fl |= 1
                    if inst not in local:
                        if len(local) >= 255:
                            raise SoundError("a stream uses more than 255 samples")
                        local.append(inst)
                    body.append(local.index(inst))
                if inst >= 0 or (sync and t == a) or abs(per - st[c][0]) > 127:
                    fl |= 2
                    body += struct.pack(">H", per)
                    st[c][0] = per
                elif per != st[c][0]:
                    fl |= 8
                    body.append((per - st[c][0]) & 0xFF)
                    st[c][0] = per
                if vol != st[c][1] or inst >= 0:
                    fl |= 4
                    body.append(vol)
                    st[c][1] = vol
            if fl:
                mask |= 1 << c
                ev.append(fl)
                ev += body
        if mask:
            while wait:
                k = min(wait, 128)
                out.append(k - 1)
                wait -= k
            out.append(0x80 | mask)
            out += ev
        else:
            wait += 1
    while wait:
        k = min(wait, 128)
        out.append(k - 1)
        wait -= k
    return bytes(out), st


def find_loop(notes, total):
    """music that never ended: where it comes round -> (start, length) in ticks, or None"""
    keys = {}
    for n in notes:
        if n.kind == "fm" or n.kind == "dac":
            keys.setdefault((n.voice, round(math.log2(n.f[0][1]) * 12) if n.kind == "fm" else n.patch), []).append(n.t0)
    if not keys:
        return None
    sets = {k: set(v) for k, v in keys.items()}
    allk = [(k, t) for k, v in keys.items() for t in v]

    def match(k, t, lag):
        s = sets[k]
        return t - lag in s or t - lag - 1 in s or t - lag + 1 in s or t - lag - 2 in s or t - lag + 2 in s

    tail = [(k, t) for k, t in allk if t >= total * 0.55 and t < total - 4]
    if len(tail) < 16:
        return None
    ref = sorted(tail, key=lambda x: -x[1])[:6]
    lags = set()
    for k, t in ref:
        for u in keys[k]:
            if 120 <= t - u <= total * 0.5:
                lags.add(t - u)
    best = None
    for lag in sorted(lags):
        ok = sum(1 for k, t in tail if match(k, t, lag))
        if ok >= len(tail) * 0.97:
            best = lag
            break
    if best is None:
        return None
    # (the match allows a tick or two of drift: of the lags that close, the one most notes fit exactly)
    best = max(range(best, best + 5), key=lambda lag: sum(1 for k, t in tail if t - lag in sets[k]))
    # back from the tail: the earliest tick from which everything repeats a lag later
    late = sorted(t for k, t in allk if t >= best)
    bad = sorted(t for k, t in allk if t >= best and not match(k, t, best))
    start = 0
    import bisect
    for i in range(len(bad) - 1, -1, -1):  # the last tick before which more than a few notes are new
        after = len(late) - bisect.bisect_left(late, bad[i])
        if len(bad) - i > 0.03 * after + 2:
            start = bad[i] - best + 1
            break
    start = max(0, start)
    if start + 2 * best > total - 2:
        return None
    return start, best


# ----------------------------------------------------------------- requests
def classify(base, cand, own_notes, own_frames):
    """what a request does to music that is playing, from the key-ons with and without it"""
    bk, ck = base[0], cand[0]
    chans = sorted({v & 7 for _, v in bk})
    same = sum(1 for c in chans if [x for x in bk if x[1] & 7 == c] == [x for x in ck if x[1] & 7 == c])
    if same == len(chans):
        return "over" if own_notes else "none"
    if own_notes:
        used = {n.voice for n in own_notes if n.kind == "fm"}
        if same and same >= len(chans) - max(1, len(used)) and own_frames < 600:
            return "over"                 # the music's other voices went on: an effect borrowing some
        after = [f for f, v in ck if f > own_frames + 20 and v >> 4]
        return "jingle" if after and own_frames < 400 else "music"
    last = max([f for f, v in ck if v >> 4] or [0])
    if last < 30:
        return "stop"
    if last < base[2] - 60:
        return "fade"
    return "none"


# ------------------------------------------------------------------- build
KINDS = {"none": 0, "music": 1, "over": 2, "stop": 3, "fade": 4, "jingle": 5}


def build(d, log=print, only=None):
    """DIR with a sound rip -> {"streams", "requests", "pool", "idle", "addrs", "aliases"}"""
    idx = index(d)
    buf = memoryview(open(os.path.join(d, "sound.rip"), "rb").read())
    idle = open(os.path.join(d, "z80idle.bin"), "rb").read()
    addrs = sorted({a for _, a, v, m, _, _ in idx if m == 1})
    ends = sorted(o for o, *_ in idx) + [len(buf)]
    size = {o: ends[i + 1] - o for i, o in enumerate(ends[:-1])}
    seen = {}                             # a request's log (without its address) -> where it was first met
    parsed = {}
    reqs = {}                             # (addr, value) -> the key of its content
    pool = Pool()
    for off, a, v, m, frames, _ in idx:
        if m != 1 or (only and v not in only):
            continue
        notes, total, dacs = parse(buf, off)
        if not notes:
            reqs[(a, v)] = None
            continue
        t0 = notes[0].t0
        sig = hash(tuple((n.t0 - t0, n.voice, n.patch, n.f[0][1], (n.t1 or 0) - n.t0) for n in notes))
        key = seen.setdefault(sig, (a, v))    # (the same sound asked for at another address)
        reqs[(a, v)] = key
        if key != (a, v):
            continue
        loop = find_loop(notes, total) if frames >= 14000 else None
        if loop:
            total = min(total, loop[0] + 2 * loop[1] + 1)
            notes = [n for n in notes if n.t0 < total]
            for n in notes:
                if n.t1 is not None and n.t1 > total:
                    n.t1 = None
        pool.want(notes, dacs)
        parsed[key] = (notes, total, loop)
    # what each does to music playing
    mode2 = {}
    base = None
    for off, a, v, m, frames, _ in idx:
        if m == 2:
            r = parse(buf, off)
            if v == 0:
                base = r
            else:
                mode2[(a, v)] = r
    kinds = {}
    for (a, v), key in reqs.items():
        own = parsed.get(key) if key else None
        if base is None or (a, v) not in mode2:
            kinds[(a, v)] = ("music" if own[2] or own[1] > 600 else "over") if own else "none"
        else:
            kinds[(a, v)] = classify(base, mode2[(a, v)], own[0] if own else None, own[1] if own else 0)
    for key, (notes, total, loop) in parsed.items():
        for n in notes:
            shape(n, pool, total)
    peaks = sorted(n.peak * n.inst.scale for notes, _, _ in parsed.values() for n in notes)
    gain = 1.0 / max(peaks[int(len(peaks) * 0.98)], 1e-3) if peaks else 1.0
    streams = {}
    for key, (notes, total, loop) in parsed.items():
        kind = kinds.get(key, "over")
        nch = 4 if kind in ("music", "jingle") else 2
        rows = frames_of(arrange(notes, total, nch), total, gain)
        local = []
        if loop:                          # the start once, then a round taken from the second time through
            s, length = loop
            intro, st = encode(rows, 0, s, local)
            body, _ = encode(rows, s + length, min(total, s + 2 * length), local, st, sync=True)
            streams[key] = (nch, intro, body, local)
        else:
            data, _ = encode(rows, 0, total, local)
            streams[key] = (nch, data, None, local)
    log(f"sound: {len(streams)} streams ({sum(1 for s in streams.values() if s[2])} looping), {len(pool.order)} samples, "
        f"{sum(len(s.data) for s in pool.order)} bytes of samples, "
        f"{sum(len(s[1]) + len(s[2] or b'') for s in streams.values())} of streams")
    return {"streams": streams, "requests": reqs, "kinds": kinds, "pool": pool, "idle": idle, "addrs": addrs, "gain": gain}


def wav(path, stream, pool, seconds=60, rate=28000):
    """a stream as Paula would play it (channels 0,3 left and 1,2 right) -> a WAV file, for listening"""
    import wave
    nch, data, body, local = stream
    chan = [dict(s=None, pos=0.0, per=428, vol=0) for _ in range(4)]
    cmap = (0, 1, 2, 3) if nch == 4 else (3, 2)
    out = bytearray()
    src, p = data, 0
    wait = 0
    spt = rate // TICKS
    for _ in range(seconds * TICKS):
        if wait:
            wait -= 1
        else:
            while p >= len(src):
                if body is None:
                    src = b"\x7f"
                else:
                    src = body
                p = 0
            b = src[p]
            p += 1
            if b < 0x80:
                wait = b
            else:
                for c in range(nch):
                    if not b & (1 << c):
                        continue
                    fl = src[p]
                    p += 1
                    ch = chan[cmap[c]]
                    if fl & 1:
                        ch["s"] = pool.order[local[src[p]]]
                        ch["pos"] = 0.0
                        p += 1
                    if fl & 2:
                        ch["per"] = src[p] << 8 | src[p + 1]
                        p += 2
                    if fl & 8:
                        ch["per"] += src[p] - 256 if src[p] > 127 else src[p]
                        p += 1
                    if fl & 4:
                        ch["vol"] = src[p]
                        p += 1
        for _ in range(spt):
            mix = [0, 0]
            for i, ch in enumerate(chan):
                s = ch["s"]
                if s is None or not ch["vol"]:
                    if s is not None:
                        ch["pos"] += PAULA / ch["per"] / rate
                    continue
                pos = ch["pos"]
                n = len(s.data)
                if pos >= n:
                    if not s.looplen:
                        continue
                    pos = s.loop + (pos - s.loop) % s.looplen
                    ch["pos"] = pos
                v = s.data[int(pos)]
                mix[0 if i in (0, 3) else 1] += (v - 256 if v > 127 else v) * ch["vol"]
                ch["pos"] = pos + PAULA / ch["per"] / rate
            out += struct.pack("<hh", max(-32768, min(32767, mix[0] * 2)), max(-32768, min(32767, mix[1] * 2)))
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(out))


def aliases(d, addrs):
    """addresses of the Z80's RAM the game writes requests to that the rip's run didn't see used: written by
    the same instructions as a known one (hwsites.txt, from the analysis) -> {address: the known one}"""
    path = os.path.join(d, "hwsites.txt")
    out = {}
    if not os.path.exists(path):
        return out
    by_pc = {}
    for line in open(path):
        w = line.split()
        if len(w) >= 3 and w[1] == "w":
            a = int(w[2], 16)
            if 0xA00000 <= a < 0xA02000:
                by_pc.setdefault(w[0], set()).add(a & 0x1FFF)
    for pc, written in by_pc.items():
        known = sorted(written & set(addrs))
        if known and len(written) <= 8:
            for a in written - set(addrs):
                out[a] = known[0]
    return out


def pack(snd, regions, alias=None, log=print):
    """build()'s result -> the Amiga player's data (big-endian; offsets from its start):
      0 'MDSN'   4 samples.w streams.w addresses.w 0.w
      12 -> samples: data.l (offset) length.w loop.w (words; $FFFF plays once) looplen.w flags.w (1: an effect's: kept in chip RAM)
      16 -> streams: data.l loop.l (0: ends) channels.b kind.b locals.w -> its sample list.l (words; bit 15: left out)
      20 -> requests: per address: the Z80 address.w, then 256 words: kind << 12 | stream ($FFF none)
      24 -> the idle driver's RAM (8192 bytes: what the game reads back)
      28 chip RAM: silence.l effects.l music.l its size.l jingles.l its size.l
    `regions`: [(address, size)] of chip RAM free for samples. Samples stay in fast RAM; the effects' are copied to
    chip RAM once, a piece of music's when it starts."""
    pool = snd["pool"]
    streams = snd["streams"]
    kinds = snd["kinds"]
    uniq = {}                             # the same stream under several requests is kept once
    order = []
    sid = {}
    for key in sorted(streams):
        nch, data, body, local = streams[key]
        kind = kinds.get(key, "over")
        ident = (nch, data, body, tuple(local), kind in ("music", "jingle"))
        if ident not in uniq:
            uniq[ident] = len(order)
            order.append((nch, data, body, local, kind))
        sid[key] = uniq[ident]
    if len(order) >= 0xFFF:
        raise SoundError("too many streams")
    resident = set()
    for nch, data, body, local, kind in order:
        if kind not in ("music", "jingle"):
            resident.update(local)
    size = lambda i: len(pool.order[i].data)
    regions = sorted(([a, n] for a, n in regions), key=lambda r: -r[1])
    if not regions:
        raise SoundError("no chip RAM left for samples")
    silence = regions[0][0]
    regions[0][0] += 4
    regions[0][1] -= 4

    want = {"music": 0, "jingle": 0}
    for nch, data, body, local, kind in order:
        if kind in want:
            want[kind] = max(want[kind], sum(size(i) for i in local if i not in resident))
    # three blocks: the effects' samples (all of them, always there), a buffer for the music playing and one for a
    # jingle over it. The largest want first, each from the region with the most room left, cut down to it
    wants = sorted([("sfx", sum(size(i) for i in resident)), ("music", min(want["music"], 0x10000)), ("jingle", min(want["jingle"], 0x8000))],
                   key=lambda w: -w[1])
    got = {}
    left_out = set()
    for name, n in wants:
        r = max(regions, key=lambda r: r[1])
        room = r[1] & ~1
        if name == "sfx":
            for i in sorted(resident, key=size, reverse=True):
                if n <= room:
                    break
                left_out.add(i)
                n -= size(i)
        n = min(n, room) & ~1
        got[name] = (r[0], n)
        r[0] += n
        r[1] -= n
    need = got["sfx"][1]
    sfx_base = got["sfx"][0]
    resident -= left_out
    bufs = {"music": got["music"], "jingle": got["jingle"]}
    dropped = 0
    locals_ = []
    for nch, data, body, local, kind in order:
        words = []
        if kind in want:
            cap = bufs[kind][1]
            total = sum(size(i) for i in local if i not in resident)
            out = set()
            for i in sorted((i for i in local if i not in resident), key=size, reverse=True):
                if total <= cap:
                    break
                out.add(i)
                total -= size(i)
            dropped += len(out)
            words = [i | (0x8000 if i in out else 0) for i in local]
        else:
            words = [i | (0x8000 if i in left_out else 0) for i in local]
        locals_.append(words)
    addrs = list(snd["addrs"])
    table = {a: a for a in addrs}
    for a, known in (alias or {}).items():
        table[a] = known
    blob = bytearray(52)
    n_s = len(pool.order)
    off_samples = len(blob)
    blob += bytes(12 * n_s)
    off_streams = len(blob)
    blob += bytes(16 * len(order))
    off_req = len(blob)
    for a in sorted(table):
        blob += struct.pack(">H", a)
        for v in range(256):
            key = snd["requests"].get((table[a], v))
            kind = kinds.get((table[a], v), "none")
            stream = sid.get(key, 0xFFF) if key else 0xFFF
            if kind == "none" or (stream == 0xFFF and kind in ("music", "over", "jingle")):
                blob += struct.pack(">H", 0xFFF)
            else:
                blob += struct.pack(">H", KINDS[kind] << 12 | stream)
    if len(blob) & 1:
        blob.append(0)
    off_idle = len(blob)
    blob += snd["idle"]
    for k, (nch, data, body, local, kind) in enumerate(order):
        lo = len(blob)
        for w in locals_[k]:
            blob += struct.pack(">H", w)
        d = len(blob)
        blob += data
        blob.append(0xFF)                 # the end (or: go round)
        lp = 0
        if body is not None:
            lp = len(blob)
            blob += body
            blob.append(0xFF)
        if len(blob) & 1:
            blob.append(0)
        struct.pack_into(">IIBBHI", blob, off_streams + 16 * k, d, lp, nch, KINDS[kind], len(local), lo)
    for i, s in enumerate(pool.order):
        d = len(blob)
        blob += s.data
        if s.looplen:
            struct.pack_into(">IHHHH", blob, off_samples + 12 * i, d, len(s.data) // 2, s.loop // 2, s.looplen // 2, 1 if i in resident else 0)
        else:
            struct.pack_into(">IHHHH", blob, off_samples + 12 * i, d, len(s.data) // 2, 0xFFFF, 0, 1 if i in resident else 0)
    struct.pack_into(">4sHHHHIIII", blob, 0, b"MDSN", n_s, len(order), len(table), 0, off_samples, off_streams, off_req, off_idle)
    struct.pack_into(">IIIIII", blob, 28, silence, sfx_base or 0, bufs["music"][0], bufs["music"][1], bufs["jingle"][0], bufs["jingle"][1])
    log(f"sound: {len(order)} streams, {n_s} samples; chip RAM: {need} bytes of effects' samples"
        + (f" ({len(left_out)} left out)" if left_out else "") + f", {bufs['music'][1]} for a piece of music (the largest wants {want['music']}), "
        f"{bufs['jingle'][1]} for a jingle" + (f"; {dropped} samples of pieces too large for that are left out" if dropped else ""))
    return bytes(blob)
