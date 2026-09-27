#!/usr/bin/env python3
"""The soundtrack's instruments, synthesized here (no recordings needed):
distorted guitars from plucked strings (Karplus-Strong) through a clipper and
a speaker-cabinet filter, a bass, and drums. Writes sound/samples/*.wav,
which sound/theme.mml plays as `#inst NAME samples/FILE.wav root=NOTE`.

    python3 sound/tools/make_samples.py
"""
import math
import os
import random
import struct
import wave

RATE = 33148                     # twice the Amiga's 16.5 kHz (agk resamples)
here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, '..', 'samples')
rnd = random.Random(1987)


def hz(midi):
    return 440.0 * 2 ** ((midi - 69) / 12)


def pluck(freq, seconds, damp=0.996, bright=0.5):
    """Karplus-Strong: a noise burst in a delay line, averaged as it goes round."""
    n = int(seconds * RATE)
    period = RATE / freq
    size = int(period)
    frac = period - size
    buf = [rnd.uniform(-1, 1) for _ in range(size + 2)]
    # a softer pick: smooth the burst a little
    for k in range(2):
        buf = [(buf[i] + buf[i - 1]) * 0.5 for i in range(len(buf))]
    res, idx = [], 0
    for _ in range(n):
        a = buf[idx % len(buf)]
        b = buf[(idx + 1) % len(buf)]
        v = a + (b - a) * frac
        res.append(v)
        nv = damp * ((1 - bright) * v + bright * (v + b) * 0.5)
        buf[idx % len(buf)] = nv
        idx += 1
    return res


def lowpass(x, cutoff, poles=2):
    a = math.exp(-2 * math.pi * cutoff / RATE)
    for _ in range(poles):
        y, prev = [], 0.0
        for v in x:
            prev = (1 - a) * v + a * prev
            y.append(prev)
        x = y
    return x


def highpass(x, cutoff):
    a = math.exp(-2 * math.pi * cutoff / RATE)
    y, prev_x, prev_y = [], 0.0, 0.0
    for v in x:
        prev_y = a * (prev_y + v - prev_x)
        prev_x = v
        y.append(prev_y)
    return y


def peak(x, freq, gain_db, q=1.0):
    """A biquad peaking EQ (the cabinet's mid bump)."""
    A = 10 ** (gain_db / 40)
    w = 2 * math.pi * freq / RATE
    al = math.sin(w) / (2 * q)
    b0, b1, b2 = 1 + al * A, -2 * math.cos(w), 1 - al * A
    a0, a1, a2 = 1 + al / A, -2 * math.cos(w), 1 - al / A
    y, x1, x2, y1, y2 = [], 0.0, 0.0, 0.0, 0.0
    for v in x:
        o = (b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0
        x2, x1, y2, y1 = x1, v, y1, o
        y.append(o)
    return y


def amp(x, drive):
    """Tube-ish clipping: a bit asymmetric, into a 4x12 cabinet."""
    x = highpass(x, 120)
    x = [math.tanh(drive * v + 0.1) - math.tanh(0.1) for v in x]
    x = [math.tanh(1.5 * v) for v in x]
    x = peak(x, 800, 5, 0.8)
    x = lowpass(x, 3800, poles=3)
    return highpass(x, 70)


def envelope(x, attack=0.002, release=0.02):
    n = len(x)
    a, r = int(attack * RATE), int(release * RATE)
    return [v * min(1.0, i / a if a else 1.0) * min(1.0, (n - i) / r if r else 1.0) for i, v in enumerate(x)]


def mix(*parts):
    n = max(len(p) for p in parts)
    return [sum(p[i] for p in parts if i < len(p)) for i in range(n)]


def power_chord(root, seconds, damp, bright):
    strings = [pluck(hz(root), seconds, damp, bright),
               pluck(hz(root + 7), seconds, damp, bright),
               pluck(hz(root + 12), seconds, damp, bright)]
    return mix(*[[v * g for v in s] for s, g in zip(strings, (1.0, 0.8, 0.6))])


def normalize(x, level=0.95):
    p = max(abs(v) for v in x) or 1.0
    return [v * level / p for v in x]


def write(name, x):
    os.makedirs(out, exist_ok=True)
    x = normalize(x)
    with wave.open(os.path.join(out, name + '.wav'), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b''.join(struct.pack('<h', int(v * 32767)) for v in x))
    print(f'{name}: {len(x) / RATE:.2f} s')


E2 = 40
# palm-muted chug: the strings choked fast, the amp still roaring
write('chug', envelope(amp(power_chord(E2, 0.22, 0.975, 0.3), 9.0), release=0.04))
# the open chord ringing out
write('chord', envelope(amp(power_chord(E2, 1.3, 0.9985, 0.5), 7.0), release=0.25))
# bass: a round pluck an octave down, a little grit
b = pluck(hz(E2 - 12), 0.7, 0.997, 0.2)
b = [math.tanh(2.5 * v) for v in lowpass(b, 1200)]
write('bass', envelope(b, release=0.1))

# lead: a saw with vibrato coming in, through the amp (E5 = midi 76)
n, ph, lead = int(1.1 * RATE), 0.0, []
for i in range(n):
    t = i / RATE
    vib = 1 + 0.012 * math.sin(2 * math.pi * 5.5 * t) * min(1.0, max(0.0, (t - 0.15) / 0.3))
    ph += hz(76) * vib / RATE
    lead.append(2 * (ph % 1) - 1 + 0.4 * (2 * ((2 * ph) % 1) - 1))
write('lead', envelope(amp(lead, 4.0), attack=0.004, release=0.3))

# drums
n, ph, kick = int(0.3 * RATE), 0.0, []
for i in range(n):
    t = i / RATE
    ph += (50 + 110 * math.exp(-t * 35)) / RATE
    click = rnd.uniform(-1, 1) * math.exp(-t * 400) * 0.6
    kick.append(math.tanh(2.2 * math.sin(2 * math.pi * ph) * math.exp(-t * 9)) + click)
write('kick', kick)

n, ph, snare = int(0.35 * RATE), 0.0, []
for i in range(n):
    t = i / RATE
    ph += (185 + 40 * math.exp(-t * 60)) / RATE
    body = math.sin(2 * math.pi * ph) * math.exp(-t * 28)
    noise = rnd.uniform(-1, 1) * (0.8 * math.exp(-t * 16) + 0.15 * math.exp(-t * 5))
    snare.append(0.55 * body + noise)
write('snare', highpass(snare, 150))

n, crash = int(1.4 * RATE), []
for i in range(n):
    t = i / RATE
    crash.append(rnd.uniform(-1, 1) * (math.exp(-t * 2.6) + 0.5 * math.exp(-t * 20)))
crash = highpass(highpass(crash, 3000), 2000)
write('crash', envelope(crash, attack=0.001, release=0.2))
