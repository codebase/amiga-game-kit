#!/usr/bin/env python3
"""The rival cars: the player's car (ai/car_raw.png) repainted in playfield 1's
colours (palette.txt) - a yellow body instead of red - so the blitter can draw
it next to the palms. Writes art/ai/rival_raw.png for scale_sheet.py.

    python3 art/tools/rival.py && python3 art/tools/scale_sheet.py rival ai/rival_raw.png 10 1.2
"""
import colorsys
import os
from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))
art = os.path.join(here, '..')
OUTLINE, WHITE, YELLOW, BROWN, DARK = (0x11, 0x11, 0x11), (0xFF, 0xFF, 0xEE), (0xFF, 0xCC, 0x22), (0x99, 0x66, 0x33), (0x55, 0x33, 0x11)

def repaint(r, g, b):
    h, l, s = colorsys.rgb_to_hls(r / 255, g / 255, b / 255)
    if s > 0.35 and (h < 0.06 or h > 0.9):           # the red body: by brightness
        return YELLOW if l > 0.45 else BROWN if l > 0.3 else DARK
    if s > 0.35 and h < 0.17:                         # orange lights
        return YELLOW
    if 0.4 < h < 0.7 and s > 0.08:                    # the rear window: dark, a glint
        return WHITE if l > 0.88 else OUTLINE
    if l > 0.6:                                       # highlights
        return WHITE
    return OUTLINE if l < 0.3 else DARK               # tyres, glass, grille

im = Image.open(os.path.join(art, 'ai', 'car_raw.png')).convert('RGBA')
out = Image.new('RGBA', im.size, (0, 0, 0, 0))
for y in range(im.height):
    for x in range(im.width):
        r, g, b, a = im.getpixel((x, y))
        if a >= 128:
            out.putpixel((x, y), repaint(r, g, b) + (255,))
out.save(os.path.join(art, 'ai', 'rival_raw.png'))
