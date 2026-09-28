#!/usr/bin/env python3
"""Turns Retro Diffusion images (art/rd/*.png, made with `agk art-gen
--free-colors`) into VOIDRUNNER text art in the game's own colours.

The VR-1 is an attached sprite, 15 colours: 12 picked from the image,
and the starfield's three, which it has to
use in slots 29-31 anyway.

The Warden has 7 playfield colours: its greys and purples go onto the
metal ramp by brightness (--dither adds a 2x2 dither where a shade falls
between two steps; plain shades read cleaner); its reds become the core's
red and orange.

    python3 art/tools/from_rd.py [--dither]
"""
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
art = os.path.join(here, '..')
sys.path.insert(0, os.path.join(here, '..', '..', '..', '..', 'harness'))
from agk.image import load_png_rgba  # noqa: E402

RAMP = [('K', 0x112), ('D', 0x446), ('M', 0x88A), ('L', 0xDDF)]
BAYER = [[0.2, 0.6], [0.8, 0.4]]


def rgb12(c):
    return tuple(((c >> s) & 15) * 17 for s in (8, 4, 0))


def luma(r, g, b):
    return 0.3 * r + 0.59 * g + 0.11 * b


def ramp_char(y, x, row, dither):
    levels = [luma(*rgb12(c)) for _, c in RAMP]
    if y <= levels[0]:
        return RAMP[0][0]
    for i in range(len(levels) - 1):
        lo, hi = levels[i], levels[i + 1]
        if y <= hi:
            t = (y - lo) / (hi - lo)
            if dither and 0.3 < t < 0.7:
                return RAMP[i + 1][0] if t > BAYER[row & 1][x & 1] else RAMP[i][0]
            return RAMP[i + 1][0] if t >= 0.5 else RAMP[i][0]
    return RAMP[-1][0]


def boss(dither):
    w, h, px = load_png_rgba(os.path.join(art, 'rd', 'boss_rd.png'))
    frame = []
    for y in range(h):
        row = ''
        for x in range(w):
            r, g, b, a = px[y * w + x]
            if a < 128:
                row += '.'
            elif r > g + 80 and r > b + 60:       # the core: red, and hot at the middle
                row += 'O' if g > 90 else 'R'
            else:
                row += ramp_char(luma(r, g, b), x, y, dither)
        frame.append(row)
    # the same number of blank rows off the top and the bottom (the centre stays put):
    # a smaller copy to blit every frame
    blank = lambda rows: next(i for i, r in enumerate(rows) if r.strip('.'))
    cut = min(blank(frame), blank(frame[::-1]))
    frame = frame[cut:len(frame) - cut]
    flash = [r.replace('R', 'C').replace('O', 'L') for r in frame]
    with open(os.path.join(art, 'boss.txt'), 'w') as f:
        f.write('# The Warden, 96x%d: the belt\'s guardian. Frames: normal, the core flashing when hit.\n' % len(frame) +
                '# (from art/rd/boss_rd.png, a Retro Diffusion image, by art/tools/from_rd.py - edit that, not this)\n'
                'colors\n  .  transparent\n' +
                ''.join('  %s  0x%03X\n' % kc for kc in RAMP) +
                '  C  0x3BF\n  O  0xF92\n  R  0xE22\n')
        for fr in (frame, flash):
            f.write('frame\n' + '\n'.join(fr) + '\n')


STARS = [0x446, 0x99B, 0xFFF]            # sprite slots 29-31: the starfield's
# The other 12, picked from the image: outline, hull greys, wing blues, the
# canopy, the flame
SHIP = [0x012, 0x155, 0x567, 0x789, 0xABC, 0xCDE, 0x47A, 0x3BB, 0xAEE, 0xB53, 0xF95, 0xFE8]


def dist(a, b):
    return 2 * (a[0] - b[0]) ** 2 + 4 * (a[1] - b[1]) ** 2 + 3 * (a[2] - b[2]) ** 2


def ship():
    w, h, px = load_png_rgba(os.path.join(art, 'rd', 'ship_rd.png'))
    pal = [rgb12(c) for c in SHIP] + [rgb12(c) for c in STARS]
    def nearest(c):
        d = [dist(c, p) for p in pal]
        return d.index(min(d))
    hit = {nearest(p[:3]) for p in px if p[3] >= 128}
    used = sorted(hit - {12, 13, 14},
                  key=lambda i: luma(*pal[i]))
    order = used + [12, 13, 14]
    chars = 'abcdefghijkl'[:len(used)] + 'GHW'
    key = {i: chars[n] for n, i in enumerate(order)}
    frame = [''.join(key[nearest(px[y * w + x][:3])] if px[y * w + x][3] >= 128 else '.'
                     for x in range(w)) for y in range(h)]
    # the flame flickers: a pixel shorter every other frame
    flick = [('.' + r[1:]) if r[0] != '.' else r for r in frame]
    flick = [('.' + r[1:]) if r[1] != '.' and r[0] == '.' and y in (7, 9) else r for y, r in enumerate(flick)]
    hexes = lambda c: '0x%X%X%X' % tuple(v // 17 for v in c)
    with open(os.path.join(art, 'ship.txt'), 'w') as f:
        f.write("# The player's ship, VR-1: 32x16, 15 colours (attached sprites, channels 0-3).\n"
                '# From art/rd/ship_rd.png, a Retro Diffusion image, by art/tools/from_rd.py - edit that, not this.\n'
                '# Colours are listed in slot order (17-31): the last three are also the\n'
                "# starfield's (sprite channel 7 uses slots 29-31).\n"
                '# Frames: 0-1 cruising (the flame flickers), 2 climbing, 3 diving.\n'
                'colors\n  .  transparent\n' +
                ''.join('  %s  %s%s\n' % (key[i], hexes(pal[i]), ' @%d' % (17 + i) if i >= 12 else '')
                        for i in order))
        for fr in (frame, flick, frame, flick):
            f.write('frame\n' + '\n'.join(fr) + '\n')


if __name__ == '__main__':
    boss('--dither' in sys.argv)
    ship()
    print('wrote art/boss.txt, art/ship.txt')
