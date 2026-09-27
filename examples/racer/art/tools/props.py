#!/usr/bin/env python3
"""Roadside props, drawn here in playfield 1's colours (art/palette.txt):
a bush, curve warning signs (chevrons left and right) and a checkpoint
pillar. Writes art/ai/NAME_raw.png at full size (the nearest row); then
scale_sheet.py makes the sizes:

    python3 art/tools/props.py
    for n in bush sign_l sign_r gate; do python3 art/tools/scale_sheet.py $n ai/${n}_raw.png 10; done
"""
import os
import random
from PIL import Image, ImageDraw

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, '..', 'ai')
OUT, WHITE, YELLOW = (0x11, 0x11, 0x11, 255), (0xFF, 0xFF, 0xEE, 255), (0xFF, 0xCC, 0x22, 255)
BROWN, DARK = (0x99, 0x66, 0x33, 255), (0x55, 0x33, 0x11, 255)
GREEN, DGREEN = (0x77, 0xCC, 0x33, 255), (0x22, 0x66, 0x22, 255)


def outline(im):
    """A 1-px black outline around everything that isn't transparent."""
    px = im.load()
    w, h = im.size
    res = im.copy()
    rp = res.load()
    for y in range(h):
        for x in range(w):
            if px[x, y][3]:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                if 0 <= x + dx < w and 0 <= y + dy < h and px[x + dx, y + dy][3] and px[x + dx, y + dy] != OUT:
                    rp[x, y] = OUT
                    break
    return res


def bush():
    rnd = random.Random(7)
    w, h = 46, 22
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for cx, cy, r in ((12, 13, 9), (23, 9, 10), (34, 13, 9), (18, 15, 7), (29, 16, 7)):
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=DGREEN)
    px = im.load()
    for y in range(h):
        for x in range(w):
            if px[x, y][3] and y < h - 4:
                # light from the top left, leafy speckle
                light = (x - 23) * -0.3 + (y - 11) * -1.0 + rnd.uniform(-4, 4)
                if light > 1.5:
                    px[x, y] = GREEN
    return outline(im.crop((0, 0, w, h - 1)))


def sign(direction):
    w, h = 40, 34
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for px_ in (7, 30):                          # two posts
        d.rectangle((px_, 20, px_ + 2, h - 1), fill=BROWN)
        d.line((px_ + 2, 20, px_ + 2, h - 1), fill=DARK)
    d.rectangle((1, 1, w - 2, 21), fill=YELLOW)
    for i in range(3):                          # chevrons
        x0 = 6 + i * 11
        pts = [(x0 + 7, 4), (x0 + 2, 11), (x0 + 7, 18), (x0 + 4, 18), (x0 - 1, 11), (x0 + 4, 4)]
        d.polygon(pts, fill=OUT)
    if direction > 0:
        im = im.transpose(Image.FLIP_LEFT_RIGHT)
    return outline(im)


def gate():
    w, h = 30, 104
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for y in range(8, h, 12):                   # a striped pillar
        d.rectangle((3, y, 12, min(h - 1, y + 5)), fill=WHITE)
        d.rectangle((3, y + 6, 12, min(h - 1, y + 11)), fill=YELLOW)
    d.rectangle((1, 2, 14, 8), fill=YELLOW)     # the top
    for y in range(10, 22):                     # a chequered flag
        for x in range(15, 29):
            im.putpixel((x, y), OUT if ((x - 15) // 3 + (y - 10) // 3) % 2 else WHITE)
    return outline(im)


os.makedirs(out, exist_ok=True)
for name, img in (('bush', bush()), ('sign_l', sign(-1)), ('sign_r', sign(1)), ('gate', gate())):
    img.save(os.path.join(out, f'{name}_raw.png'))
    print(name, img.size)
