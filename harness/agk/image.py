"""Tiny RGB image helpers (stdlib only): vAmiga raw dumps, PNG in/out, diffs."""
import struct
import zlib

# vAmiga's regression-tester cutout: HPIXELS - 4*0x31 wide, VPIXELS - 2 - 26 high.
# Pixels are hires-wide (lores pixels appear 2 wide) and 1 line tall.
WIDTH, HEIGHT = 716, 285

# Where a standard PAL lores playfield (DIWSTRT $2C81, 320x256) lands in that
# cutout: game pixel (x, y) is image pixel (2x + 62, y + 18).
SCREEN_X0, SCREEN_Y0, SCREEN_W, SCREEN_H = 62, 18, 320, 256


class Image:
    def __init__(self, rgb: bytes, width=WIDTH, height=HEIGHT):
        if len(rgb) != width * height * 3:
            raise ValueError(f"expected {width * height * 3} bytes, got {len(rgb)}")
        self.rgb, self.width, self.height = rgb, width, height

    @classmethod
    def from_raw_file(cls, path):
        with open(path, "rb") as f:
            return cls(f.read())

    def pixel(self, x, y):
        i = (y * self.width + x) * 3
        return tuple(self.rgb[i:i + 3])

    def to_png(self) -> bytes:
        stride = self.width * 3
        rows = b"".join(b"\x00" + self.rgb[y * stride:(y + 1) * stride] for y in range(self.height))

        def chunk(tag, data):
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

        return (b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", struct.pack(">IIBBBBB", self.width, self.height, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(rows, 9))
                + chunk(b"IEND", b""))

    def canonical12(self):
        """Map every channel to its 4-bit Amiga value * 17 (0x0->0, 0xF->255).
        vAmiga shows OCS/ECS colour registers as n*16 and AGA ones as n*17; this
        makes a 12-bit colour look the same on every chipset."""
        lut = bytes((v >> 4) * 17 for v in range(256))
        return Image(self.rgb.translate(lut), self.width, self.height)

    def screen(self):
        """The 320x256 lores playfield, one image pixel per game pixel."""
        out = bytearray()
        for y in range(SCREEN_H):
            row = (SCREEN_Y0 + y) * self.width
            for x in range(SCREEN_W):
                i = (row + SCREEN_X0 + 2 * x) * 3
                out += self.rgb[i:i + 3]
        return Image(bytes(out), SCREEN_W, SCREEN_H)

    def save_png(self, path):
        with open(path, "wb") as f:
            f.write(self.to_png())

    @classmethod
    def load_png(cls, path):
        """Decode 8-bit RGB non-interlaced PNGs (what to_png writes)."""
        with open(path, "rb") as f:
            data = f.read()
        if data[:8] != b"\x89PNG\r\n\x1a\n":
            raise ValueError(f"{path}: not a PNG")
        pos, idat, width = 8, b"", None
        while pos < len(data):
            length, tag = struct.unpack(">I4s", data[pos:pos + 8])
            body = data[pos + 8:pos + 8 + length]
            if tag == b"IHDR":
                width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
                if (depth, ctype, interlace) != (8, 2, 0):
                    raise ValueError(f"{path}: only 8-bit RGB non-interlaced PNGs supported")
            elif tag == b"IDAT":
                idat += body
            pos += 12 + length
        raw = zlib.decompress(idat)
        stride = width * 3
        out, prev = bytearray(), bytearray(stride)
        for y in range(height):
            ftype = raw[y * (stride + 1)]
            line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
            for i in range(stride):
                a = line[i - 3] if i >= 3 else 0
                b = prev[i]
                c = prev[i - 3] if i >= 3 else 0
                if ftype == 1:
                    line[i] = (line[i] + a) & 255
                elif ftype == 2:
                    line[i] = (line[i] + b) & 255
                elif ftype == 3:
                    line[i] = (line[i] + (a + b) // 2) & 255
                elif ftype == 4:
                    p = a + b - c
                    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                    pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                    line[i] = (line[i] + pred) & 255
            out += line
            prev = line
        return cls(bytes(out), width, height)


def diff(a: Image, b: Image):
    """Return (count, bbox, diff_image). Differences are red on a dimmed copy of b."""
    if (a.width, a.height) != (b.width, b.height):
        return a.width * a.height, (0, 0, a.width - 1, a.height - 1), None
    out = bytearray(len(b.rgb))
    count, x0, y0, x1, y1 = 0, a.width, a.height, -1, -1
    for i in range(0, len(b.rgb), 3):
        if a.rgb[i:i + 3] != b.rgb[i:i + 3]:
            count += 1
            px, py = (i // 3) % a.width, (i // 3) // a.width
            x0, y0, x1, y1 = min(x0, px), min(y0, py), max(x1, px), max(y1, py)
            out[i:i + 3] = b"\xff\x00\x00"
        else:
            g = (b.rgb[i] + b.rgb[i + 1] + b.rgb[i + 2]) // 9
            out[i:i + 3] = bytes((g, g, g))
    bbox = (x0, y0, x1, y1) if count else None
    return count, bbox, Image(bytes(out), a.width, a.height)


def load_png_rgba(path):
    """Decode any common non-interlaced PNG (grey, RGB, palette, grey+alpha,
    RGBA; 8-bit, or 1/2/4-bit palette/grey) to (width, height, [(r,g,b,a)...]).
    Art tools and AI generators produce all of these."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, plte, trns = 8, b"", b"", b""
    width = height = depth = ctype = None
    while pos < len(data):
        length, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if interlace:
                raise ValueError(f"{path}: interlaced PNGs aren't supported - re-save without interlacing")
        elif tag == b"PLTE":
            plte = body
        elif tag == b"tRNS":
            trns = body
        elif tag == b"IDAT":
            idat += body
        pos += 12 + length
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    if depth == 16:
        raise ValueError(f"{path}: 16-bit PNGs aren't supported - export 8-bit")
    bits = channels * depth
    bpp = max(1, bits // 8)                    # filter unit in bytes
    stride = (width * bits + 7) // 8
    raw = zlib.decompress(idat)
    rows, prev = [], bytearray(stride)
    for y in range(height):
        ftype = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ftype == 1:
                line[i] = (line[i] + a) & 255
            elif ftype == 2:
                line[i] = (line[i] + b) & 255
            elif ftype == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 255
        rows.append(bytes(line))
        prev = line

    def samples(line):
        if depth == 8:
            return list(line)
        per = 8 // depth
        mask = (1 << depth) - 1
        return [(line[i // per] >> (8 - depth * (i % per + 1))) & mask for i in range(width * channels)]

    pixels = []
    for line in rows:
        v = samples(line)
        for x in range(width):
            px = v[x * channels:(x + 1) * channels]
            if ctype == 3:
                i = px[0]
                r, g, b = plte[i * 3:i * 3 + 3]
                a = trns[i] if i < len(trns) else 255
            elif ctype in (0, 4):
                g0 = px[0] * 255 // ((1 << depth) - 1)
                r = g = b = g0
                a = px[1] if ctype == 4 else 255
            else:
                r, g, b = px[:3]
                a = px[3] if ctype == 6 else 255
            pixels.append((r, g, b, a))
    return width, height, pixels
