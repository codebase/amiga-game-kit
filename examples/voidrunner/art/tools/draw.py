#!/usr/bin/env python3
"""Draws VOIDRUNNER's round and animated objects as text art (art/*.txt), in
the playfield palette: rotating asteroids, the spinning mine, explosions and
the boss. Hand-made sprites live in art/*.txt too; this is the rest.

    python3 art/tools/draw.py
"""
import math
import os
import random

here = os.path.dirname(os.path.abspath(__file__))
art = os.path.join(here, '..')
COLORS = '''colors
  .  transparent
  K  0x112
  D  0x446
  M  0x88A
  L  0xDDF
  C  0x3BF
  O  0xF92
  R  0xE22
'''


def save(name, comment, frames):
    h, w = len(frames[0]), len(frames[0][0])
    with open(os.path.join(art, name + '.txt'), 'w') as f:
        f.write(''.join(f'# {line}\n' for line in comment.split('\n')))
        f.write('# (drawn by art/tools/draw.py - edit that, not this)\n')
        f.write(COLORS)
        for fr in frames:
            assert len(fr) == h and all(len(r) == w for r in fr)
            f.write('frame\n' + '\n'.join(''.join(r) for r in fr) + '\n')
    print(f'{name}: {w}x{h} x{len(frames)}')


def outline(g):
    """A black edge around the shape."""
    h, w = len(g), len(g[0])
    out = [r[:] for r in g]
    for y in range(h):
        for x in range(w):
            if g[y][x] != '.':
                continue
            if any(0 <= y + dy < h and 0 <= x + dx < w and g[y + dy][x + dx] not in '.K'
                   for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                out[y][x] = 'K'
    return out


def shade(v):
    """Light 0..1 -> a metal/rock ramp."""
    return 'D' if v < 0.33 else 'M' if v < 0.7 else 'L'


# ------------------------------------------------------------ asteroids
def asteroid(size, seed, frames=4):
    rnd = random.Random(seed)
    lumps = [(rnd.uniform(0, 2 * math.pi), rnd.uniform(0.08, 0.2), rnd.randint(2, 5)) for _ in range(4)]
    craters = [(rnd.uniform(0, 2 * math.pi), rnd.uniform(0.15, 0.6), rnd.uniform(0.12, 0.22)) for _ in range(5)]
    c = (size - 1) / 2
    r0 = size / 2 - 1.5
    out = []
    for f in range(frames):
        rot = f * 2 * math.pi / frames / 3          # a slow tumble: a third of a turn over the loop
        g = [['.'] * size for _ in range(size)]
        for y in range(size):
            for x in range(size):
                dx, dy = x - c, y - c
                a = math.atan2(dy, dx) - rot
                d = math.hypot(dx, dy)
                r = r0 * (1 + sum(amp * math.sin(k * a + ph) for ph, amp, k in lumps) * 0.5)
                if d > r:
                    continue
                # light from the top left, a sphere's normal
                nz = math.sqrt(max(0.0, 1 - (d / r) ** 2))
                light = 0.55 * nz + 0.45 * (-(dx + dy) / (1.41 * r)) + 0.1
                for ca, cd, cr in craters:
                    cx = c + math.cos(ca + rot) * cd * r0
                    cy = c + math.sin(ca + rot) * cd * r0
                    e = math.hypot(x - cx, y - cy) / (cr * r0 * 2)
                    if e < 1:
                        light += -0.35 if (x - cx) + (y - cy) < 0 else 0.15   # shadow on the lit side's rim
                g[y][x] = shade(light)
        out.append(outline(g))
    return out


# ---------------------------------------------------------------- mine
def mine(frames=4):
    size, c = 16, 7.5
    out = []
    for f in range(frames):
        g = [['.'] * size for _ in range(size)]
        rot = f * math.pi / 2 / frames
        for y in range(size):
            for x in range(size):
                dx, dy = x - c, y - c
                d = math.hypot(dx, dy)
                a = math.atan2(dy, dx) - rot
                spike = abs(math.cos(2 * a)) ** 8        # four spikes
                if d <= 4.6:
                    light = 0.6 * math.sqrt(max(0, 1 - (d / 4.6) ** 2)) - 0.4 * (dx + dy) / 6.5 + 0.2
                    g[y][x] = shade(light)
                elif d <= 4.6 + 3.2 * spike:
                    g[y][x] = 'M' if spike > 0.6 else 'D'
        # the eye: red, blinking bright
        g[7][7] = g[7][8] = g[8][7] = g[8][8] = 'R' if f % 2 else 'O'
        out.append(outline(g))
    return out


# ----------------------------------------------------------- explosions
def explosion(size, frames, seed):
    rnd = random.Random(seed)
    c = (size - 1) / 2
    puffs = [(rnd.uniform(0, 2 * math.pi), rnd.uniform(0.0, 0.6), rnd.uniform(0.3, 0.6)) for _ in range(7)]
    out = []
    for f in range(frames):
        t = (f + 1) / frames                              # 0..1
        g = [['.'] * size for _ in range(size)]
        for y in range(size):
            for x in range(size):
                heat = 0.0
                for a, dist, rad in puffs:
                    px = c + math.cos(a) * dist * c * t * 1.3
                    py = c + math.sin(a) * dist * c * t * 1.3
                    r = rad * c * (0.4 + 0.9 * t)
                    d = math.hypot(x - px, y - py)
                    if d < r:
                        heat = max(heat, 1 - d / r)
                if heat <= 0:
                    continue
                # hot and bright early, dark smoke late, ragged edges
                v = heat * (1.25 - t) + rnd.uniform(-0.12, 0.12)
                if t > 0.7 and v < 0.35:
                    if rnd.random() < 0.55:
                        g[y][x] = 'D'
                    continue
                g[y][x] = 'L' if v > 0.8 else 'O' if v > 0.45 else 'R' if v > 0.15 else ('D' if t > 0.4 else 'R')
        out.append(g)
    return out


# ----------------------------------------------------------------- boss
def boss():
    """The Warden: a 96x64 battleship guarding the belt. Frames: normal, and
    the core flashing when hit."""
    W, H = 96, 64
    g = [['.'] * W for _ in range(H)]

    def fill(x0, y0, x1, y1, ch):
        for y in range(max(0, y0), min(H, y1 + 1)):
            for x in range(max(0, x0), min(W, x1 + 1)):
                g[y][x] = ch

    # the hull: a wedge pointing left, thick at the back
    for y in range(8, 56):
        dy = abs(y - 31.5)
        x0 = int(10 + dy * 1.25)
        x1 = 88 - int(max(0, dy - 18) * 1.5)
        for x in range(x0, x1):
            light = 0.75 - (y - 8) / 48 * 0.8 + (0.12 if (x // 8 + y // 6) % 2 else 0)
            g[y][x] = shade(light)
    # armour seams
    for x in range(20, 88, 12):
        for y in range(10, 54):
            if g[y][x] != '.':
                g[y][x] = 'D'
    # upper and lower fins
    for i in range(10):
        fill(58 + i, 1 + i // 2 * 0, 70 - i // 3, 7 + i // 2, '.')
    for y in range(0, 10):
        for x in range(56 + y, 80):
            g[y][x] = 'M' if y > 2 else 'L'
    for y in range(54, 64):
        for x in range(56 + (63 - y), 80):
            g[y][x] = 'D' if y < 61 else 'M'
    # engine block at the back, glowing
    fill(86, 18, 91, 45, 'D')
    for y in range(20, 44, 5):
        fill(90, y, 95, y + 2, 'O')
        fill(92, y + 1, 95, y + 1, 'L')
    # gun turrets top and bottom (they fire the spreads)
    for ty in (12, 46):
        fill(34, ty, 44, ty + 5, 'M')
        fill(36, ty + 1, 42, ty + 4, 'D')
        fill(24, ty + 2, 35, ty + 3, 'L')       # the barrel
        g[ty + 2][24] = g[ty + 3][24] = 'K'
    # the core: the weak spot, an eye in the middle
    core = []
    for y in range(22, 42):
        for x in range(44, 64):
            d = math.hypot((x - 53.5) / 1.1, y - 31.5)
            if d < 9.5:
                core.append((x, y, d))
    base = [r[:] for r in g]
    frames = []
    for hit in (False, True):
        g = [r[:] for r in base]
        for x, y, d in core:
            if d > 8:
                g[y][x] = 'K'
            elif d > 6.5:
                g[y][x] = 'D'
            else:
                g[y][x] = ('L' if d < 2.5 else 'O' if d < 4.5 else 'R') if not hit else ('L' if d < 5 else 'C')
        frames.append(outline(g))
    return frames


def small_frames(frames):
    return frames




# ------------------------------------------------------------- the backdrop
def backdrop():
    """The far view, 640x232 looping (playfield 2, 2 bitplanes: 3 colours +
    transparent, where the stars show through): a ringed gas giant, a moon and
    faint nebula. Written as a PNG in exactly the backdrop palette's colours."""
    from PIL import Image
    W, H = 640, 232
    C = [None, (0x22, 0x11, 0x33), (0x44, 0x22, 0x55), (0x77, 0x44, 0x99)]   # backdrop_palette.txt
    img = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    px = img.load()
    rnd = random.Random(42)
    bayer = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]

    def put(x, y, level):   # level 0..3 in quarters, ordered dither between steps
        x %= W
        if 0 <= y < H:
            base, frac = int(level), level - int(level)
            v = base + (1 if frac * 16 > bayer[y & 3][x & 3] else 0)
            v = max(0, min(3, v))
            if v:
                px[x, y] = C[v] + (255,)

    # nebula: soft noise clouds in the darkest colour, wrapping at the edges
    blobs = [(rnd.uniform(0, W), rnd.uniform(0, H), rnd.uniform(20, 60)) for _ in range(26)]
    for y in range(H):
        for x in range(W):
            d = 0.0
            for bx, by, br in blobs:
                dx = min(abs(x - bx), W - abs(x - bx))
                d += math.exp(-((dx / br) ** 2 + ((y - by) / (br * 0.5)) ** 2))
            if d > 0.55:
                put(x, y, min(1.3, (d - 0.55) * 2.2))
    # the gas giant: bands, lit from the left, a dark limb
    cx, cy, r = 470, 128, 70
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            dx, dy = (x - cx) / r, (y - cy) / r
            if dx * dx + dy * dy > 1:
                continue
            nz = math.sqrt(max(0.0, 1 - dx * dx - dy * dy))
            light = 0.55 * nz + 0.45 * (-dx) + 0.15
            band = 0.35 * math.sin(dy * 9 + 0.6 * math.sin(dx * 3)) + 0.2 * math.sin(dy * 23)
            put(x, y, max(0.5, min(3.0, 0.4 + 2.6 * light + band)))
    # its ring: an ellipse in front below the equator, behind above it
    for t in range(0, 3600):
        a = t / 3600 * 2 * math.pi
        for rr in (1.55, 1.62, 1.7, 1.85, 1.9):
            x = cx + math.cos(a) * r * rr
            y = cy + math.sin(a) * r * rr * 0.22 + math.cos(a) * 8
            inside = ((x - cx) / r) ** 2 + ((y - cy) / r) ** 2 < 1
            if inside and math.sin(a) < 0:
                continue                     # behind the planet
            lvl = 2.6 if rr < 1.8 else 1.6
            put(int(x), int(y), lvl * (0.7 + 0.3 * math.cos(a + 0.8)))
    # a moon
    mx, my, mr = 150, 60, 11
    for y in range(my - mr, my + mr + 1):
        for x in range(mx - mr, mx + mr + 1):
            dx, dy = (x - mx) / mr, (y - my) / mr
            if dx * dx + dy * dy <= 1:
                nz = math.sqrt(max(0.0, 1 - dx * dx - dy * dy))
                put(x, y, max(0.6, 3.2 * (0.5 * nz + 0.5 * (-dx - dy * 0.3))))
    img.save(os.path.join(art, 'backdrop.png'))
    print('backdrop: 640x232')


# -------------------------------------------------------- words and the logo
FONT = {
    'A': ['.###.', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'],
    'B': ['####.', '#...#', '#...#', '####.', '#...#', '#...#', '####.'],
    'C': ['.###.', '#...#', '#....', '#....', '#....', '#...#', '.###.'],
    'D': ['####.', '#...#', '#...#', '#...#', '#...#', '#...#', '####.'],
    'E': ['#####', '#....', '#....', '####.', '#....', '#....', '#####'],
    'F': ['#####', '#....', '#....', '####.', '#....', '#....', '#....'],
    'G': ['.###.', '#...#', '#....', '#.###', '#...#', '#...#', '.####'],
    'I': ['.###.', '..#..', '..#..', '..#..', '..#..', '..#..', '.###.'],
    'L': ['#....', '#....', '#....', '#....', '#....', '#....', '#####'],
    'M': ['#...#', '##.##', '#.#.#', '#.#.#', '#...#', '#...#', '#...#'],
    'N': ['#...#', '##..#', '##..#', '#.#.#', '#..##', '#..##', '#...#'],
    'O': ['.###.', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'],
    'P': ['####.', '#...#', '#...#', '####.', '#....', '#....', '#....'],
    'R': ['####.', '#...#', '#...#', '####.', '#.#..', '#..#.', '#...#'],
    'S': ['.####', '#....', '#....', '.###.', '....#', '....#', '####.'],
    'T': ['#####', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'],
    'U': ['#...#', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'],
    'V': ['#...#', '#...#', '#...#', '#...#', '#...#', '.#.#.', '..#..'],
    'W': ['#...#', '#...#', '#...#', '#.#.#', '#.#.#', '##.##', '#...#'],
    '1': ['..#..', '.##..', '..#..', '..#..', '..#..', '..#..', '.###.'],
    '-': ['.....', '.....', '.....', '#####', '.....', '.....', '.....'],
    ' ': ['.....'] * 7,
}


def words(lines, scale, top, bottom, width=None):
    """Text in the 5x7 font, `scale` px per font pixel: colour `top` on the
    upper half of each letter, `bottom` on the lower (a sheen), a black
    outline and a dark drop shadow. -> rows of characters."""
    lh = 7 * scale
    w = max(len(l) for l in lines) * 6 * scale + 4
    W = width or ((w + 15) // 16 * 16)
    H = len(lines) * (lh + 3 * scale) + 2
    g = [['.'] * W for _ in range(H)]
    for li, text in enumerate(lines):
        x0 = (W - (len(text) * 6 * scale - scale)) // 2
        y0 = 1 + li * (lh + 3 * scale)
        for ci, ch in enumerate(text):
            for fy, row in enumerate(FONT[ch]):
                for fx, bit in enumerate(row):
                    if bit != '#':
                        continue
                    for sy in range(scale):
                        for sx in range(scale):
                            x = x0 + ci * 6 * scale + fx * scale + sx
                            y = y0 + fy * scale + sy
                            g[y][x] = top if fy * scale + sy < lh * 0.5 else bottom
    # shadow one step down-right, then the outline
    shadow = [r[:] for r in g]
    for y in range(H - 1, 0, -1):
        for x in range(W - 1, 0, -1):
            if g[y][x] == '.' and g[y - 1][x - 1] not in '.K':
                shadow[y][x] = 'D'
    return outline(shadow)


if __name__ == '__main__':
    save('rock', 'Asteroid, 32x32: tumbles (4 frames). 4 hits; breaks into two pebbles.', asteroid(32, 11))
    save('pebble', 'A small asteroid, 16x16, tumbling (4 frames). 1 hit.', asteroid(16, 5))
    save('mine', 'Spinning mine, 16x16 (4 frames): stops, stares, fires at you.', mine())
    save('boom', 'Explosion, 32x32, 6 frames.', explosion(32, 6, 3))
    save('pop', 'Small explosion, 16x16, 5 frames.', explosion(16, 5, 8))
    save('boss', 'The Warden, 96x64: the belt\'s guardian. Frames: normal, the core flashing when hit.', boss())
    backdrop()
    save('logo', 'The title: VOIDRUNNER in big chunky letters.', [words(['VOIDRUNNER'], 3, 'L', 'C', 208)])
    save('msg_stage', '"STAGE 1 - OUTER BELT"', [words(['STAGE 1', 'OUTER BELT'], 2, 'L', 'C')])
    save('msg_warning', '"WARNING", blinking red and orange: the boss is coming.',
         [words(['WARNING'], 3, 'O', 'R'), words(['WARNING'], 3, 'R', 'O')])
    save('msg_clear', '"STAGE CLEAR"', [words(['STAGE CLEAR'], 2, 'L', 'C')])
    save('msg_over', '"GAME OVER"', [words(['GAME OVER'], 2, 'O', 'R')])
    save('msg_fire', '"PRESS FIRE"', [words(['PRESS FIRE'], 1, 'L', 'M')])
