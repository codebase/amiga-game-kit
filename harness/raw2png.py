#!/usr/bin/env python3
"""Convert a vAmiga regression-tester .raw dump (packed RGB) to PNG.

vAmiga's `screenshot save` writes the visible texture cutout: 716x285 pixels
(HPIXELS - 4*0x31 wide, VPIXELS - 2 - (VBLANK_MAX + 1) high) at 3 bytes/pixel.
"""
import struct
import sys
import zlib

WIDTH, HEIGHT = 716, 285


def raw_to_png(raw: bytes, width: int = WIDTH, height: int = HEIGHT) -> bytes:
    stride = width * 3
    if len(raw) != stride * height:
        raise ValueError(f"expected {stride * height} bytes, got {len(raw)}")
    rows = b"".join(b"\x00" + raw[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9))
            + chunk(b"IEND", b""))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: raw2png.py in.raw out.png")
    with open(sys.argv[1], "rb") as f:
        png = raw_to_png(f.read())
    with open(sys.argv[2], "wb") as f:
        f.write(png)
