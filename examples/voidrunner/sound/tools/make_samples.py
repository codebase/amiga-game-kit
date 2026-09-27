#!/usr/bin/env python3
"""VOIDRUNNER's instruments, synthesized (no recordings): a filtered saw
bass, a pluck for the arpeggios, a bright lead with vibrato, and drums.
Writes sound/samples/*.wav for sound/theme.mml (#inst NAME FILE.wav root=NOTE).

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
rnd = random.Random(2049)


def hz(midi):
    return 440.0 * 2 ** ((midi - 69) / 12)


def saw(ph):
    return 2 * (ph % 1.0) - 1


def svf_lowpass(x, cutoffs, res=0.35):
    """A resonant lowpass whose cutoff moves (a synth's filter sweep)."""
    low = band = 0.0
    y = []
    for v, fc in zip(x, cutoffs):
        f = 2 * math.sin(math.pi * min(fc, RATE / 6) / RATE)
        high = v - low - res * band
        band += f * high
        low += f * band
        y.append(low)
    return y


def envelope(x, attack=0.002, release=0.02):
    n = len(x)
    a, r = max(1, int(attack * RATE)), max(1, int(release * RATE))
    return [v * min(1.0, i / a) * min(1.0, (n - i) / r) for i, v in enumerate(x)]


def write(name, x, level=0.95):
    os.makedirs(out, exist_ok=True)
    p = max(abs(v) for v in x) or 1.0
    with wave.open(os.path.join(out, name + '.wav'), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b''.join(struct.pack('<h', int(v * level / p * 32767)) for v in x))
    print(f'{name}: {len(x) / RATE:.2f} s')


# bass: two detuned saws, a filter that snaps shut (E2)
n = int(0.45 * RATE)
x = [(saw(hz(40) * i / RATE) + saw(hz(40) * 1.006 * i / RATE)) * 0.5 for i in range(n)]
fc = [180 + 2600 * math.exp(-i / RATE * 14) for i in range(n)]
write('bass', envelope([v * math.exp(-i / RATE * 2.5) for i, v in enumerate(svf_lowpass(x, fc, 0.5))], release=0.05))

# pluck: a square-ish pulse through a quick filter decay (E4)
n = int(0.35 * RATE)
x = [(1.0 if (hz(64) * i / RATE) % 1 < 0.3 else -1.0) for i in range(n)]
fc = [400 + 5000 * math.exp(-i / RATE * 18) for i in range(n)]
write('pluck', envelope([v * math.exp(-i / RATE * 7) for i, v in enumerate(svf_lowpass(x, fc, 0.3))], release=0.05))

# lead: saw + a fifth-octave square, vibrato coming in, a gentle filter (E5)
n, ph, ph2, x = int(1.0 * RATE), 0.0, 0.0, []
for i in range(n):
    t = i / RATE
    vib = 1 + 0.01 * math.sin(2 * math.pi * 5.8 * t) * min(1.0, max(0.0, (t - 0.12) / 0.25))
    ph += hz(76) * vib / RATE
    ph2 += hz(88) * vib / RATE
    x.append(saw(ph) * 0.8 + (0.35 if ph2 % 1 < 0.5 else -0.35))
x = svf_lowpass(x, [3200 + 1500 * math.exp(-i / RATE * 6) for i in range(n)], 0.2)
write('lead', envelope(x, attack=0.006, release=0.25))

# drums
n, ph, kick = int(0.28 * RATE), 0.0, []
for i in range(n):
    t = i / RATE
    ph += (48 + 130 * math.exp(-t * 32)) / RATE
    kick.append(math.tanh(2.0 * math.sin(2 * math.pi * ph) * math.exp(-t * 10)) + rnd.uniform(-1, 1) * math.exp(-t * 500) * 0.5)
write('kick', kick)

n, ph, snare = int(0.3 * RATE), 0.0, []
for i in range(n):
    t = i / RATE
    ph += 200 / RATE
    snare.append(0.5 * math.sin(2 * math.pi * ph) * math.exp(-t * 30) +
                 rnd.uniform(-1, 1) * (0.9 * math.exp(-t * 18) + 0.2 * math.exp(-t * 6)))
write('snare', snare)

n, hat, prev = int(0.07 * RATE), [], 0.0
for i in range(n):
    v = rnd.uniform(-1, 1)
    hat.append((v - prev) * math.exp(-i / RATE * 60))
    prev = v
write('hat', hat, level=0.7)
