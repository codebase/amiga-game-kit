#!/usr/bin/env python3
"""Two 320x256, 16-colour pictures as the display reads them: 4 bitplanes,
interleaved (per row: plane 0's 40 bytes, plane 1's, ...), 40960 bytes each,
in disk/, which the build packs onto the floppy. Drawn here with the
palette in art/palette.txt (index 0 = sky).

    python3 tools/make_pictures.py
"""
import math
import os

W, H, DEPTH = 320, 256, 4
here = os.path.dirname(os.path.abspath(__file__))


def picture(n):
    px = [[0] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            if n == 1:
                # a planet over a banded sky, with a dithered terminator
                dx, dy = x - 200, (y - 110) * 1.2
                r = math.sqrt(dx * dx + dy * dy)
                if r < 80:
                    shade = int((dx + 80) / 160 * 6)
                    c = 8 + min(5, max(0, shade + ((x + y) & 1 if (dx + 80) % 27 < 13 else 0)))
                else:
                    c = 1 + min(6, y * 7 // H)
                    if ((x * 7 + y * 13) % 97) == 0:
                        c = 15                              # a star
            else:
                # the cave: horizontal rock layers and a bright crystal
                layer = int(6 + 4 * math.sin(x / 23.0) + 3 * math.sin(x / 7.0 + y / 40.0))
                c = 2 + (y // 16 + layer) % 6
                if abs(x - 90) + abs(y - 150) < 40 and ((x + y) & 1 or abs(x - 90) + abs(y - 150) < 25):
                    c = 14 + ((x ^ y) & 1)
            px[y][x] = c
    out = bytearray()
    for y in range(H):
        for p in range(DEPTH):
            row = 0
            for x in range(W):
                row = (row << 1) | ((px[y][x] >> p) & 1)
            out += row.to_bytes(W // 8, "big")
    return bytes(out)


for n in (1, 2):
    path = os.path.join(here, "..", "disk", f"level{n}.pln")
    with open(path, "wb") as f:
        f.write(picture(n))
    print("wrote", os.path.relpath(path), len(picture(n)), "bytes")
