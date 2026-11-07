"""agk rip: graphics out of a chip-RAM dump (agk inspect's 'dump-chip NAME').

    agk rip ram.bin --copper                  the copper lists in the dump: bitplane pointers,
                                              modulos, colour registers, where each list is
    agk rip ram.bin --screen -o screen.png    the picture the display was showing, from the
                                              copper list's bitplane pointers and colours
    agk rip ram.bin 0x4A000 320x256x4 -o x.png [--interleaved] [--modulo N] [--palette 0xRGB,... | @ADDR]
                                              a bitmap at an address: WIDTHxHEIGHTxPLANES
    agk rip ram.bin 0x30000 16x16x4 --count 64 -o tiles.png
                                              COUNT tiles stored one after another (each tile's planes
                                              together), as a sheet; the art pipeline reads it back
                                              with frame_width/frame_height

Planar data is bit 7 first, a plane's row is WIDTH/8 bytes (+ --modulo).
Non-interleaved: a whole plane, then the next (--planes-apart N overrides the
distance). Without a palette the planes are shown as a grey ramp, which is
enough to see what's there.
"""
import re
import struct

from .image import Image

CHIP = 0x80000
COLOR0 = 0x180


def _rgb12(c):
    return ((c >> 8 & 15) * 17, (c >> 4 & 15) * 17, (c & 15) * 17)


def planar_to_image(data, addr, w, h, planes, palette=None, interleaved=False, modulo=0, planes_apart=None):
    """-> Image (w x h) of the planar bitmap at addr in data"""
    row_bytes = w // 8
    px = bytearray(w * h * 3)
    if palette is None:
        n = 1 << planes
        palette = [int(i * 15 / max(1, n - 1)) * 0x111 for i in range(n)]
    pal = [_rgb12(c) for c in palette]
    if interleaved:
        plane_step, row_step = row_bytes + modulo, (row_bytes + modulo) * planes
    else:
        plane_step = planes_apart if planes_apart else (row_bytes + modulo) * h
        row_step = row_bytes + modulo
    for y in range(h):
        rows = [data[addr + y * row_step + p * plane_step: addr + y * row_step + p * plane_step + row_bytes]
                for p in range(planes)]
        for x in range(w):
            byte, bit = x >> 3, 7 - (x & 7)
            c = 0
            for p in range(planes):
                if byte < len(rows[p]) and rows[p][byte] >> bit & 1:
                    c |= 1 << p
            o = (y * w + x) * 3
            px[o:o + 3] = bytes(pal[c] if c < len(pal) else (255, 0, 255))
    return Image(bytes(px), w, h)


def find_copper_lists(data, min_moves=6):
    """Copper lists in a dump: runs of MOVEs (an even custom-register offset
    $20..$1FE, then a value) and WAITs/SKIPs (odd first word), ending at
    WAIT $FFFF,$FFFE or where the run stops looking like a list.
    -> [{'addr', 'end', 'moves', 'waits', 'cmds': [(line or None, reg, value)], 'bplcon0',
         'bplpt': {plane: address} (the first set), 'colors': {n: 0xRGB} (the first set)}]
    A command's line is the beam line the preceding WAIT waited for (None
    before any WAIT: the top of the frame)."""
    lists = []
    i, n = 0, len(data)
    while i + 4 <= n:
        reg = struct.unpack_from(">H", data, i)[0]
        if reg & 1 or reg >= 0x200 or reg < 0x20:
            i += 2
            continue
        j, moves, waits = i, 0, 0
        line, past255 = None, False
        cmds = []
        while j + 4 <= n:
            a, v = struct.unpack_from(">HH", data, j)
            if a & 1:
                j += 4
                waits += 1
                if a == 0xFFFF and v == 0xFFFE:
                    break
                if v & 1 == 0:                          # a WAIT (SKIP has bit 0 of the mask word set)
                    vpos = (a >> 8) & 0xFF
                    if vpos == 0xFF and (a & 0xFE) >= 0xDE and (v & 0x80) == 0:
                        past255 = True                  # the "end of line 255" wait
                    else:
                        line = vpos + (256 if past255 else 0)
                continue
            if a >= 0x200 or a < 0x20:
                break
            moves += 1
            cmds.append((line, a, v))
            j += 4
        if moves >= min_moves:
            info = {"addr": i, "end": j, "moves": moves, "waits": waits, "cmds": cmds,
                    "bplcon0": None, "bplpt": {}, "colors": {}}
            hi, lo = {}, {}
            for _, a, v in cmds:
                if a == 0x100 and info["bplcon0"] is None:
                    info["bplcon0"] = v
                elif 0x0E0 <= a < 0x100:
                    p_, is_lo = (a - 0xE0) // 4, (a - 0xE0) % 4 == 2
                    d = lo if is_lo else hi
                    d.setdefault(p_, v)
                elif COLOR0 <= a < COLOR0 + 64:
                    info["colors"].setdefault((a - COLOR0) // 2, v & 0xFFF)
            info["bplpt"] = {p_: (hi[p_] << 16 | lo[p_]) & 0xFFFFFF for p_ in hi
                             if p_ in lo and p_ < 6 and 0 < ((hi[p_] << 16 | lo[p_]) & 0xFFFFFF) < CHIP}
            lists.append(info)
            i = j
        else:
            i += 2
    return lists


def display_lists(lists):
    """The lists that set up a display (bitplane pointers into chip RAM), best
    first: a BPLCON0 with planes, then the planes pointed at, then MOVEs"""
    def key(c):
        depth = (c["bplcon0"] >> 12) & 7 if c["bplcon0"] else 0
        return (min(depth, 6), len(c["bplpt"]), c["moves"] + c["waits"])
    return sorted((c for c in lists if c["bplpt"]), key=key, reverse=True)


def read_regs(path):
    """NAME.regs.txt (agk regs) -> {'colors': [32], 'bplcon0': ..., 'diwstrt': ..., 'bpl1mod': ..., 'bpl2mod': ...}"""
    regs = {"colors": [0] * 32}
    for line in open(path):
        parts = line.split()
        if len(parts) != 2:
            continue
        name, val = parts[0], int(parts[1], 16)
        if name.startswith("COLOR"):
            regs["colors"][int(name[5:])] = val & 0xFFF
        else:
            regs[name.lower()] = val - 0x10000 if name.endswith("MOD") and val >= 0x8000 else val
    return regs


def screen_from_copper(data, cop, width=320, height=256, regs=None):
    """The picture a copper list shows, interpreted line by line: bitplane
    pointers advance a row at a time (width + modulo), a MOVE to BPLxPT at a
    line restarts that plane there, colours change where the list changes
    them. Dual playfield composites the odd planes over the even ones.
    Sprites aren't drawn. -> Image"""
    cmds = cop["cmds"]
    # the display window's top line: DIWSTRT's high byte (default $2C)
    top = 0x2C
    for ln, a, v in cmds:
        if a == 0x08E and ln is None:
            top = v >> 8
    at_top = [c for c in cmds if c[0] is None]
    later = {}
    for ln, a, v in cmds:
        if ln is not None:
            later.setdefault(ln, []).append((a, v))
    # The registers as they were at the dump (the palette, usually set once
    # from the CPU and never by the list), then what the list does
    regs = regs or {}
    state = {"bplcon0": regs.get("bplcon0", 0), "bplcon1": regs.get("bplcon1", 0),
             "mod": [regs.get("bpl1mod", 0), regs.get("bpl2mod", 0)],
             "ddfstrt": regs.get("ddfstrt", 0x38), "ddfstop": regs.get("ddfstop", 0xD0),
             "ptr": {}, "hi": {}, "lo": {}, "colors": list(regs.get("colors", [0] * 32))}
    if "diwstrt" in regs:
        top = regs["diwstrt"] >> 8
    bottom = top + height
    if "diwstop" in regs:
        bottom = (regs["diwstop"] >> 8) + (0x100 if (regs["diwstop"] >> 8) < 0x80 else 0)

    def apply(a, v):
        if a == 0x100:
            state["bplcon0"] = v
        elif a == 0x102:
            state["bplcon1"] = v
        elif a == 0x092:
            state["ddfstrt"] = v
        elif a == 0x094:
            state["ddfstop"] = v
        elif a == 0x108:
            state["mod"][0] = v - 0x10000 if v >= 0x8000 else v
        elif a == 0x10A:
            state["mod"][1] = v - 0x10000 if v >= 0x8000 else v
        elif 0x0E0 <= a < 0x100:
            p, is_lo = (a - 0xE0) // 4, (a - 0xE0) % 4 == 2
            (state["lo"] if is_lo else state["hi"])[p] = v
            if p in state["hi"] and p in state["lo"]:
                state["ptr"][p] = (state["hi"][p] << 16 | state["lo"][p]) & 0xFFFFFF
        elif COLOR0 <= a < COLOR0 + 64:
            state["colors"][(a - COLOR0) // 2] = v & 0xFFF

    for _, a, v in at_top:
        apply(a, v)
    px = bytearray(width * height * 3)
    for y in range(height):
        for a, v in later.get(top + y, []):
            apply(a, v)
        depth = (state["bplcon0"] >> 12) & 7
        dpf = bool(state["bplcon0"] & 0x400)
        # What the DMA fetches per row: (DDFSTOP - DDFSTRT) / 8 + 1 words (lores);
        # a fetch starting before $38 is words left of the window (fine
        # scrolling's early word), and BPLCON1 delays each playfield 0-15 px
        words = max(1, ((state["ddfstop"] - state["ddfstrt"]) >> 3) + 1)
        skip = max(0, (0x38 - state["ddfstrt"]) >> 3)
        row_bytes = words * 2
        shift = (state["bplcon1"] & 15, (state["bplcon1"] >> 4) & 15)
        visible = top + y < bottom
        rows = []
        for p in range(depth):
            base = state["ptr"].get(p)
            rows.append(int.from_bytes(data[base:base + row_bytes], "big")
                        if base is not None and base + row_bytes <= len(data) else 0)
        rgb = [_rgb12(c) for c in state["colors"]]
        nbits = row_bytes * 8
        for x in range(width):
            c = 0
            if visible:
                if dpf:
                    c1 = c2 = 0
                    for p in range(depth):
                        bit = nbits - 1 - (16 * skip + x - shift[p & 1])
                        if 0 <= bit < nbits and rows[p] >> bit & 1:
                            if p & 1:
                                c2 |= 1 << (p >> 1)
                            else:
                                c1 |= 1 << (p >> 1)
                    c = c1 if c1 else (8 + c2 if c2 else 0)
                else:
                    bit = nbits - 1 - (16 * skip + x - shift[0])
                    if 0 <= bit < nbits:
                        for p in range(depth):
                            if rows[p] >> bit & 1:
                                c |= 1 << p
            o = (y * width + x) * 3
            px[o:o + 3] = bytes(rgb[c])
        for p in range(depth):                              # the hardware adds the modulo after each row
            if p in state["ptr"] and visible:
                state["ptr"][p] += row_bytes + state["mod"][p & 1]
    return Image(bytes(px), width, height)


def copper_report(lists):
    out = []
    shown = display_lists(lists)
    if len(lists) > len(shown):
        out.append(f"({len(lists) - len(shown)} runs of MOVEs set no display: left out)")
    for c in shown:
        bpl = ", ".join(f"BPL{p + 1} ${a:06X}" for p, a in sorted(c["bplpt"].items()))
        cols = " ".join(f"{v:03X}" for _, v in sorted(c["colors"].items())[:16])
        mods = [v for _, a, v in c["cmds"] if a in (0x108, 0x10A)]
        out.append(f"copper list at ${c['addr']:06X}..${c['end']:06X}: {c['moves']} MOVEs, {c['waits']} WAITs"
                   + (f"; BPLCON0 ${c['bplcon0']:04X}" if c["bplcon0"] is not None else "")
                   + (f"; modulos {', '.join(str(m - 0x10000 if m >= 0x8000 else m) for m in mods[:2])}" if mods else ""))
        if bpl:
            out.append(f"  planes: {bpl}")
        if cols:
            out.append(f"  colours: {cols}" + (" ..." if len(c["colors"]) > 16 else ""))
    return out or ["no copper lists found (a game that writes its display registers from the CPU has none)"]


def tiles_sheet(data, addr, w, h, planes, count, palette=None, interleaved=False):
    """COUNT tiles stored one after another -> one sheet Image, tiles stacked vertically"""
    tile_bytes = (w // 8) * h * planes
    px = bytearray()
    for i in range(count):
        img = planar_to_image(data, addr + i * tile_bytes, w, h, planes, palette, interleaved=interleaved,
                              planes_apart=(w // 8) * h)
        px += img.rgb
    return Image(bytes(px), w, h * count)


def parse_palette(text, data=None):
    """'0xFFF,0x000,...' or '@ADDR' (16-bit colour words in the dump)"""
    if text.startswith("@"):
        addr = int(text[1:], 0)
        return [struct.unpack_from(">H", data, addr + 2 * i)[0] & 0xFFF for i in range(32)]
    return [int(t, 0) for t in re.split(r"[,\s]+", text.strip()) if t]
