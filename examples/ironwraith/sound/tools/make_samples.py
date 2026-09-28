#!/usr/bin/env python3
"""Furnace Protocol II instruments, synthesized here (no recordings needed):
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
rnd = random.Random(271993)


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


# Shared synthesis building blocks adapted from the kit's racer example;
# these timbres and the arrangement are specific to IRON WRAITH.
E2 = 40
write('chug', envelope(amp(power_chord(E2, .19, .985, .24), 14), release=.055))
write('chord', envelope(amp(power_chord(E2, .78, .9988, .4), 10), release=.16))
b = pluck(hz(28), .36, .998, .18)
b = [math.tanh(4*v) for v in lowpass(b, 1600)]
write('bass', envelope(b, release=.08))
# Tight kick with a short pitch dive and saturated low end.
kick, phase = [], 0
for i in range(int(RATE*.23)):
 t=i/RATE; phase += (48+135*math.exp(-45*t))/RATE
 kick.append(math.tanh(2.6*math.sin(2*math.pi*phase))*math.exp(-t*16)+rnd.uniform(-1,1)*math.exp(-t*330)*.35)
# Layer body, filtered rattle and a short gated room tail.
noise=highpass([rnd.uniform(-1,1) for _ in range(int(RATE*.25))],1000)
snare=[.65*math.sin(2*math.pi*178*i/RATE)*math.exp(-i/RATE*30)+v*(math.exp(-i/RATE*18)+.24*math.exp(-i/RATE*8)) for i,v in enumerate(noise)]
hat=highpass([rnd.uniform(-1,1)*math.exp(-i/RATE*55) for i in range(int(RATE*.12))],5500)
write('kickhat', envelope(mix(kick,[v*.45 for v in hat]),release=.02))
write('snarehat', envelope(mix(snare,[v*.3 for v in hat]),release=.045))
write('hat', envelope(hat,release=.025))
# Low tom for phrase-ending fills.
tom=[math.sin(2*math.pi*(92*i/RATE+.8*(1-math.exp(-25*i/RATE))))*math.exp(-i/RATE*16) for i in range(int(RATE*.25))]
write('tom', envelope(tom,release=.03))
# Metallic downbeat: kick + inharmonic struck cymbal, all one Paula voice.
metal=[sum(math.sin(2*math.pi*f*i/RATE) for f in (1379,1871,2347,3191,4517))*.08*math.exp(-i/RATE*8) for i in range(int(RATE*.42))]
write('impact',envelope(mix(kick,metal),release=.08))
