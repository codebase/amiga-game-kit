#!/usr/bin/env python3
"""The car's crash frames, from its straight frame: rolled right, upside
down, rolled left (the tumble main.c plays while the car hops). Appended to
art/car.png after the first three frames (straight, lean left, lean right),
which stay as they are.

    python3 art/tools/car_crash.py
"""
import os
from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))
path = os.path.join(here, '..', 'car.png')
sheet = Image.open(path).convert('RGBA')
W, H = 48, 32
base = [sheet.crop((f * W, 0, f * W + W, H)) for f in range(3)]
car = base[0]

def rolled(angle):
    # a 28-px tall car rotated needs more room than 48x32: shrink it a little
    big = car.resize((W * 2, H * 2), Image.NEAREST).rotate(angle, resample=Image.NEAREST, expand=False)
    small = big.resize((W, H), Image.NEAREST)
    out = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    bb = small.getbbox()
    out.paste(small.crop(bb).resize((round((bb[2] - bb[0]) * 0.85), round((bb[3] - bb[1]) * 0.85)), Image.NEAREST),
              (0, 0))
    obb = out.getbbox()
    # centre it
    c = out.crop(obb)
    res = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    res.paste(c, ((W - c.width) // 2, (H - c.height) // 2))
    return res

frames = base + [rolled(-35), car.transpose(Image.FLIP_TOP_BOTTOM), rolled(35)]
out = Image.new('RGBA', (W * len(frames), H), (0, 0, 0, 0))
for i, f in enumerate(frames):
    out.paste(f, (i * W, 0))
out.save(path)
print(f'car.png: {len(frames)} frames')
