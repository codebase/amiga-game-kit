"""Motion checks (expect-scroll): does a region of the screen move smoothly?

A screenshot can't see a scroll that jumps: the backdrop in VOIDRUNNER
skipped 16 px every 64 frames and every golden matched. expect-scroll takes
a screenshot on each of FRAMES consecutive frames and measures how far the
region's content moved between each pair.

The shift of a frame is the (dx, dy) under which the most pixels match
exactly (compared as RGB bytes) (content moved by +dx: new[x] == old[x - dx]). Exact matching lets
sprites and objects crossing the region be outvoted by the background.
"""
import math
from fractions import Fraction


def parse_speed(tok):
    """'-1/4' -> Fraction(-1, 4); '2' -> 2; px per frame"""
    try:
        return Fraction(tok)
    except (ValueError, ZeroDivisionError):
        return None


def region(img, x0, y0, x1, y1):
    """A full frame (Image) -> the region's rows as RGB bytes, x0..x1, y0..y1 inclusive in game
    coordinates. Rows stay hires (two image pixels per game pixel): slicing is fast, and
    a game pixel's shift is 6 bytes."""
    from .image import SCREEN_X0, SCREEN_Y0
    rows = []
    for y in range(y0, y1 + 1):
        start = ((SCREEN_Y0 + y) * img.width + SCREEN_X0 + 2 * x0) * 3
        rows.append(img.rgb[start:start + (x1 - x0 + 1) * 6])
    return rows


def _ints(rows):
    return [int.from_bytes(r, "big") for r in rows]


def _score(a, b, width, dx, dy, step):
    """matching bytes between old rows a and new rows b (ints, `width` bytes each), content
    moved by (dx, dy) game px; every step-th row. XOR, then count zero bytes: all in C."""
    h = len(a)
    d = 6 * abs(dx)
    if d >= width:
        return 0.0
    n = width - d
    mask = (1 << (8 * n)) - 1
    same = rows = 0
    for y in range(max(0, dy), min(h, h + dy), step):
        ra, rb = a[y - dy], b[y]
        if dx >= 0:     # new[x] == old[x - dx]: old without its last d bytes, new without its first d
            x = (ra >> (8 * d)) ^ (rb & mask)
        else:
            x = (ra & mask) ^ (rb >> (8 * d))
        same += x.to_bytes(n, "big").count(0)
        rows += 1
    return same / (rows * n) if rows else 0.0


def shift(a, b, width, rx, ry, want_dy=0):
    """-> (dx, dy, fraction of bytes that match) for the best shift within +-rx, +-ry.
    Rows a, b as ints (_ints). First dx on a sample of rows, then dy around the best dx."""
    step = max(1, len(a) // 24)
    dys = sorted({math.floor(want_dy), math.ceil(want_dy)})
    ranked = sorted(((_score(a, b, width, dx, dy, step), dx, dy)
                     for dy in dys for dx in range(-rx, rx + 1)), reverse=True)[:3]
    cands = {(dx, dy) for _, dx, _ in ranked for dy in range(-ry, ry + 1)}
    best = max((_score(a, b, width, dx, dy, 1), dx, dy) for dx, dy in cands)
    return best[1], best[2], best[0]


def check(frames, want_dx, want_dy):
    """frames: region rows per frame -> (ok, steps [(dx, dy, match)], problems [text])"""
    rx = max(17, 2 * math.ceil(abs(want_dx)) + 2)     # wide enough to measure a 16 px jump
    ry = max(2, 2 * math.ceil(abs(want_dy)) + 2)
    width = len(frames[0][0])
    ints = [_ints(f) for f in frames]
    steps = [shift(a, b, width, rx, ry, want_dy) for a, b in zip(ints, ints[1:])]
    problems = []
    for axis, want, i in (("x", want_dx, 0), ("y", want_dy, 1)):
        lo, hi = math.floor(want), math.ceil(want)
        bad = [(n + 1, s[i]) for n, s in enumerate(steps) if not lo <= s[i] <= hi]
        if bad:
            shown = ", ".join(f"frame {n}: {v:+d}" for n, v in bad[:6]) + (", ..." if len(bad) > 6 else "")
            problems.append(f"{len(bad)} step(s) in {axis} outside {lo:+d}..{hi:+d} px ({shown})")
        total, expected = sum(s[i] for s in steps), want * len(steps)
        if abs(total - expected) > 1:
            problems.append(f"moved {total:+d} px in {axis} over {len(steps)} frames, "
                            f"expected {float(expected):+g}")
    weak = [n + 1 for n, s in enumerate(steps) if s[2] < 0.5]
    if weak:
        problems.append(f"frame(s) {', '.join(map(str, weak[:6]))}: under half the region matches any shift "
                        f"(the content changed, not moved - pick a region of just the layer that scrolls)")
    return not problems, steps, problems
