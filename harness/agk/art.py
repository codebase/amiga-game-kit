"""Art pipeline: text or PNG art -> Amiga sprites/BOBs, checked against the
hardware's rules, with previews an agent can look at.

A project's art lives in art/:

    art/palette.txt   the game palette (up to 32 colours):
                          0  0x113  background
                          1  0x468  wall
    art/art.toml      one table per asset:
                          [player]
                          source = "player.txt"   # or a .png
                          kind = "sprite"         # sprite | bob | bitmap
                          channel = 0             # sprites: hardware channel 0-7
                          [enemy]
                          source = "enemy.png"
                          kind = "bob"
                          frame_width = 32        # PNG sheets: frames side by side
    art/*.txt         text art - see TEXT FORMAT below
    art/*.png         any PNG (RGBA, indexed, RGB); alpha < 128 = transparent

`agk art` converts everything into build/art/art.c + art.h and writes
previews to build/art/preview/ (4x, transparency as a checkerboard, exactly
the colours the Amiga will show). `agk build` runs it automatically.

TEXT FORMAT (one char per pixel, frames separated by `frame` lines):

    colors
      .  transparent
      W  0xFFF
      O  0xFA0
    frame
    ...WWWW...
    ..WOOOOW..
    frame
    ...

Sprite options:
  attached = true   15 colours (+ transparent) from pairs of channels; width
                    up to 64 (each 16 px column uses 2 channels, from an even
                    `channel`). Colours go to slots 17-31, shared by all
                    attached sprites. artXCreate(frame, part), part p ->
                    channel CHANNEL+p, drawn at x + (p/2)*16; call
                    spriteSetAttached() on the odd parts.
  mirror = true     adds left-facing copies of every frame (hardware sprites
                    can't flip): frame f facing left = f + ART_X_MIRROR.

Editing AI art by hand: `agk art-export NAME` turns art/NAME.png into text
art art/NAME.txt (one letter per colour) - point art.toml at the .txt and
edit pixels or add animation frames.

Rules enforced (errors say what to change):
  sprite: width <= 16, at most 3 colours + transparent. Channels 0/1 share
          colours 17-19, 2/3 share 21-23, 4/5 share 25-27, 6/7 share 29-31, so
          sprites on a pair must agree on colours.
  bob:    every colour must be in palette.txt (depth = [art] depth or enough
          bits for the palette); width is padded to a multiple of 16.
  bitmap: like bob but without a mask (backgrounds, parallax bands, tilesets:
          frames/tiles stacked vertically). `depth = N` per asset overrides
          [art] depth; `wrap_x = 320` appends the first 320 columns at the
          right so a looping band has no seam (ART_X_LOOP_W = original width).
PNG colours are rounded to the Amiga's 12-bit palette. A sprite PNG with more
than 3 colours is reduced (warning + preview); a BOB colour that isn't in the
palette maps to the nearest one (warning).
"""
import os
import tomllib

from .image import Image

TRANSPARENT = None
# First visible colour slot per sprite channel (slot base-1 is transparent)
SPRITE_BASE = {0: 17, 1: 17, 2: 21, 3: 21, 4: 25, 5: 25, 6: 29, 7: 29}


class ArtError(Exception):
    pass


# ------------------------------------------------------------------ colours

def rgb12(r, g, b):
    """8-bit RGB -> 12-bit Amiga colour, nearest 4-bit level per channel."""
    return ((r * 15 + 127) // 255) << 8 | ((g * 15 + 127) // 255) << 4 | ((b * 15 + 127) // 255)


def to_rgb8(c):
    return ((c >> 8 & 15) * 17, (c >> 4 & 15) * 17, (c & 15) * 17)


def dist(a, b):
    ra, ga, ba = to_rgb8(a)
    rb, gb, bb = to_rgb8(b)
    # Weighted RGB distance (the eye is most sensitive to green)
    return 2 * (ra - rb) ** 2 + 4 * (ga - gb) ** 2 + 3 * (ba - bb) ** 2


def parse_palette(path):
    pal = {}
    with open(path) as f:
        text = f.read()
    for n, line in enumerate(text.splitlines(), 1):
        line = line.split("#", 1)[0].split()
        if not line:
            continue
        if len(line) < 2:
            raise ArtError(f"{path}:{n}: expected 'INDEX 0xRGB [name]'")
        idx, col = int(line[0], 0), int(line[1], 0)
        if not 0 <= idx < 32 or col > 0xFFF:
            raise ArtError(f"{path}:{n}: index must be 0-31 and colour 0x000-0xFFF")
        pal[idx] = col
    return pal


# ------------------------------------------------------------------ sources

def parse_text_art(path):
    """-> (width, height, [frame][y][x] of 12-bit colour or None)"""
    colors, frames, mode, cur = {}, [], None, None
    with open(path) as f:
        text = f.read()
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.rstrip("\n")
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if stripped == "colors":
            mode = "colors"
            continue
        if stripped == "frame":
            mode, cur = "frame", []
            frames.append(cur)
            continue
        if mode == "colors":
            parts = stripped.split()
            if len(parts) != 2 or len(parts[0]) != 1:
                raise ArtError(f"{path}:{n}: colour lines are 'X 0xRGB' or '. transparent'")
            ch, val = parts
            colors[ch] = TRANSPARENT if val == "transparent" else int(val, 0)
        elif mode == "frame":
            if not colors:
                raise ArtError(f"{path}:{n}: start with a 'colors' section (e.g. '. transparent', "
                               f"'W 0xFFF') before the first 'frame'")
            row = []
            for x, ch in enumerate(stripped):
                if ch not in colors:
                    raise ArtError(f"{path}:{n}: '{ch}' at column {x + 1} isn't defined under 'colors'")
                row.append(colors[ch])
            cur.append(row)
        else:
            raise ArtError(f"{path}:{n}: start with a 'colors' section, then 'frame' sections")
    if not frames:
        raise ArtError(f"{path}: no 'frame' section")
    w, h = len(frames[0][0]), len(frames[0])
    for i, fr in enumerate(frames):
        if len(fr) != h or any(len(r) != w for r in fr):
            raise ArtError(f"{path}: frame {i + 1} isn't {w}x{h} like frame 1 (all rows/frames must match)")
    return w, h, frames


def load_png_art(path, frame_width=None, frame_height=None):
    from .image import load_png_rgba
    W, H, px = load_png_rgba(path)
    fw, fh = frame_width or W, frame_height or H
    if W % fw or H % fh:
        raise ArtError(f"{path}: {W}x{H} isn't a whole number of {fw}x{fh} frames")
    frames = []
    for fy in range(0, H, fh):
        for fx in range(0, W, fw):
            frames.append([[None if px[(fy + y) * W + fx + x][3] < 128
                            else rgb12(*px[(fy + y) * W + fx + x][:3])
                            for x in range(fw)] for y in range(fh)])
    return fw, fh, frames


# ------------------------------------------------------------------ asset

class Asset:
    def __init__(self, name, cfg, art_dir, palette, depth):
        self.name, self.cfg = name, cfg
        self.attached = False
        self.kind = cfg.get("kind", "sprite")
        self.warnings = []
        src = cfg.get("source")
        if not src:
            raise ArtError(f"[{name}]: missing 'source'")
        self.source = os.path.join(art_dir, src)
        if not os.path.exists(self.source):
            raise ArtError(f"[{name}]: {src} not found in art/")
        if src.endswith(".txt"):
            self.w, self.h, self.frames = parse_text_art(self.source)
        elif src.endswith(".png"):
            self.w, self.h, self.frames = load_png_art(self.source, cfg.get("frame_width"), cfg.get("frame_height"))
        else:
            raise ArtError(f"[{name}]: source must be .txt or .png")
        if self.kind == "sprite":
            self._check_sprite()
        elif self.kind in ("bob", "bitmap"):
            if self.kind == "bitmap" and cfg.get("wrap_x"):
                # Append the first wrap_x columns at the right edge, so a
                # looping band can be displayed at any offset without a seam.
                wx = cfg["wrap_x"]
                if wx > self.w:
                    raise ArtError(f"[{name}]: wrap_x ({wx}) is wider than the image ({self.w})")
                self.frames = [[row + row[:wx] for row in fr] for fr in self.frames]
                self.loop_w = self.w
                self.w += wx
            own = cfg.get("palette")
            if own == "auto":
                palette = self._auto_palette(cfg.get("depth", depth))
            elif own:
                palette = parse_palette(os.path.join(art_dir, own))
            self.own_palette = palette if own else None
            self._map_bob(palette, cfg.get("depth", depth))
        else:
            raise ArtError(f"[{name}]: kind must be 'sprite', 'bob' or 'bitmap'")

    def colors_used(self):
        seen = {}
        for fr in self.frames:
            for row in fr:
                for c in row:
                    if c is not None:
                        seen[c] = seen.get(c, 0) + 1
        return seen

    def _check_sprite(self):
        ch = self.cfg.get("channel", 0)
        if ch not in SPRITE_BASE:
            raise ArtError(f"[{self.name}]: channel must be 0-7")
        self.channel = ch
        self.attached = bool(self.cfg.get("attached"))
        if self.cfg.get("mirror"):
            # Hardware sprites can't flip: add left-facing copies after the originals
            self.mirror_offset = len(self.frames)
            self.frames += [[row[::-1] for row in fr] for fr in self.frames]
        self.columns = (self.w + 15) // 16
        if self.attached:
            if ch % 2:
                raise ArtError(f"[{self.name}]: attached sprites start on an even channel (0, 2, 4, 6)")
            if ch + 2 * self.columns > 8:
                raise ArtError(f"[{self.name}]: {self.w} px attached needs {2 * self.columns} channels "
                               f"from {ch}, but there are only 8 (0-7)")
            n_colors = 15
        else:
            if self.w > 16:
                raise ArtError(f"[{self.name}]: {self.w} px wide, but a hardware sprite is 16 px. "
                               f"Use attached = true (15 colours, 16 px per channel pair), or kind = \"bob\"")
            n_colors = 3
        used = self.colors_used()
        if len(used) > n_colors:
            self.warnings.append(f"{len(used)} colours, but this sprite has {n_colors} (+ transparent): "
                                 f"reduced to the {n_colors} most important - check the preview")
            keep = _reduce(used, n_colors)
            remap = {c: min(keep, key=lambda k: dist(c, k)) for c in used}
            self.frames = [[[None if c is None else remap[c] for c in row] for row in fr] for fr in self.frames]
            used = {c: 0 for c in keep}
        # Colour order: as first seen (text art: declaration order is usually that too)
        order = []
        for fr in self.frames:
            for row in fr:
                for c in row:
                    if c is not None and c not in order:
                        order.append(c)
        self.sprite_colors = order + [0x000] * (n_colors - len(order))
        self.index = {c: i + 1 for i, c in enumerate(order)}

    def _auto_palette(self, depth):
        """Index 0 = transparent / background (the copper can colour it);
        1..2^depth-1 = the image's most important colours, dark to light."""
        used = self.colors_used()
        if not used:
            raise ArtError(f"[{self.name}]: image is fully transparent")
        n = (1 << depth) - 1
        keep = _reduce(used, n) if len(used) > n else list(used)
        if len(used) > n:
            self.warnings.append(f"{len(used)} colours reduced to {n} for {depth} bitplanes - check the preview")
        keep.sort(key=lambda c: sum(to_rgb8(c)))
        pal = {0: 0x000}
        pal.update({i + 1: c for i, c in enumerate(keep)})
        # Remap every pixel onto the kept colours now (map_bob then finds exact matches)
        remap = {c: min(keep, key=lambda k: dist(c, k)) for c in used}
        self.frames = [[[None if c is None else remap[c] for c in row] for row in fr] for fr in self.frames]
        return pal

    def _map_bob(self, palette, depth):
        if not palette:
            raise ArtError(f"[{self.name}]: BOBs use the game palette - create art/palette.txt")
        self.depth = depth
        # Transparent pixels become index 0 in a bitmap, so colour matches start at 1
        # for auto palettes (index 0 is reserved there).
        allowed = {i: c for i, c in palette.items() if i < (1 << depth)
                   and not (i == 0 and self.cfg.get("palette") == "auto")}
        by_color = {}
        for i, c in sorted(allowed.items()):
            by_color.setdefault(c, i)
        index, missing = {}, {}
        for c in self.colors_used():
            if c in by_color:
                index[c] = by_color[c]
            else:
                near = min(by_color, key=lambda k: dist(c, k))
                index[c] = by_color[near]
                missing[c] = near
        for c, near in missing.items():
            self.warnings.append(f"colour 0x{c:03X} isn't in palette.txt (depth {depth}); "
                                 f"used the nearest, 0x{near:03X} (index {by_color[near]})")
        if 0 in index.values():
            self.warnings.append("uses palette index 0 (the background) as a solid colour - "
                                 "that's fine, but it isn't transparent; transparency comes from the mask")
        self.index = index
        self.words = (self.w + 15) // 16

    # -------------------------------------------------------------- output

    def c_name(self):
        return "".join(p.capitalize() for p in self.name.split("_"))

    def planar(self):
        """Per frame, per row: sprite -> [plane0, plane1];
        bob -> [plane words..., then mask words]."""
        out = []
        for fr in self.frames:
            for row in fr:
                if self.kind == "sprite" and self.attached:
                    continue  # handled per part below
                if self.kind == "sprite":
                    p0 = p1 = 0
                    for x, c in enumerate(row):
                        if c is None:
                            continue
                        i = self.index[c]
                        bit = 0x8000 >> x
                        p0 |= bit if i & 1 else 0
                        p1 |= bit if i & 2 else 0
                    out += [p0, p1]
                else:  # bob / bitmap (bitmap ignores the mask words)
                    planes = [[0] * self.words for _ in range(self.depth)]
                    mask = [0] * self.words
                    for x, c in enumerate(row):
                        if c is None:
                            continue
                        i = self.index[c]
                        bit = 0x8000 >> (x & 15)
                        mask[x >> 4] |= bit
                        for p in range(self.depth):
                            if i >> p & 1:
                                planes[p][x >> 4] |= bit
                    for p in range(self.depth):
                        out += planes[p]
                    if self.kind == "bob":
                        out += mask
        if self.kind == "sprite" and self.attached:
            # Per frame, per part (column * 2 + odd), per row: 2 words.
            # Even channel shows colour bits 0-1, the odd (attached) one bits 2-3.
            for fr in self.frames:
                for col in range(self.columns):
                    for odd in (0, 1):
                        for row in fr:
                            pa = pb = 0
                            for x in range(16):
                                sx = col * 16 + x
                                c = row[sx] if sx < len(row) else None
                                if c is None:
                                    continue
                                i = self.index[c] >> (2 * odd)
                                bit = 0x8000 >> x
                                pa |= bit if i & 1 else 0
                                pb |= bit if i & 2 else 0
                            out += [pa, pb]
        return out

    def preview(self, zoom=4, palette=None):
        """Frames side by side, as the Amiga will show them; transparency is a
        grey checkerboard."""
        gap = 2
        W = len(self.frames) * (self.w + gap) - gap
        rgb = bytearray()
        for y in range(self.h * zoom):
            for f, fr in enumerate(self.frames):
                for x in range(self.w * zoom):
                    c = fr[y // zoom][x // zoom]
                    if c is None and self.kind == "bitmap":
                        rgb += bytes(to_rgb8(palette.get(0, 0)))  # bitmaps have no transparency
                    elif c is None:
                        shade = 0x55 if ((x // 4) + (y // 4)) % 2 else 0x77
                        rgb += bytes((shade, shade, shade))
                    else:
                        shown = c
                        if self.kind != "sprite":
                            shown = palette[self.index[c]]
                        rgb += bytes(to_rgb8(shown))
                if f < len(self.frames) - 1:
                    rgb += bytes((255, 0, 255)) * (gap * zoom)
        return Image(bytes(rgb), W * zoom, self.h * zoom)


def _reduce(counts, n):
    """Pick n representative colours: most frequent first, then farthest."""
    ranked = sorted(counts, key=lambda c: -counts[c])
    keep = [ranked[0]]
    while len(keep) < min(n, len(ranked)):
        keep.append(max((c for c in ranked if c not in keep),
                        key=lambda c: min(dist(c, k) for k in keep) * (counts[c] ** 0.5)))
    return keep


# ------------------------------------------------------------------ project

def build(project_dir, out_dir=None):
    """Convert art/ -> build/art/{art.c,art.h,preview/}. Returns (assets, warnings)
    or raises ArtError. No art/art.toml -> nothing to do (returns None)."""
    art_dir = os.path.join(project_dir, "art")
    cfg_path = os.path.join(art_dir, "art.toml")
    out_dir = out_dir or os.path.join(project_dir, "build", "art")
    if not os.path.exists(cfg_path):
        return None
    with open(cfg_path, "rb") as f:
        cfg = tomllib.load(f)
    settings = cfg.pop("art", {})
    pal_path = os.path.join(art_dir, "palette.txt")
    palette = parse_palette(pal_path) if os.path.exists(pal_path) else {}
    depth = settings.get("depth") or max(1, (max(palette) if palette else 1).bit_length())
    assets = [Asset(name, a, art_dir, palette, depth) for name, a in cfg.items()]

    # Attached sprites all use colour slots 17-31, so they share ONE palette:
    # build it from the union of their colours (first asset first) and
    # re-index every attached asset against it.
    attached = [a for a in assets if a.kind == "sprite" and a.attached]
    if len(attached) > 1:
        shared = []
        for a in attached:
            for c in a.sprite_colors:
                if c not in shared and any(c in used for used in (a.colors_used(),)):
                    shared.append(c)
        if len(shared) > 15:
            names = ", ".join(a.name for a in attached)
            raise ArtError(f"attached sprites [{names}] share colour slots 17-31 but use {len(shared)} "
                           f"different colours together (max 15). Draw them with a common set of colours "
                           f"(e.g. reuse the first sprite's colours)")
        for a in attached:
            a.sprite_colors = shared + [0x000] * (15 - len(shared))
            a.index = {c: i + 1 for i, c in enumerate(shared)}

    # Sprites sharing a channel pair share their 3 colours
    pairs = {}
    for a in assets:
        if a.kind == "sprite" and not a.attached:
            base = SPRITE_BASE[a.channel]
            if base in pairs and pairs[base].sprite_colors != a.sprite_colors:
                o = pairs[base]
                raise ArtError(
                    f"[{a.name}] and [{o.name}] are on channels that share colours {base}-{base + 2}, "
                    f"but use different colours ({_hex(a.sprite_colors)} vs {_hex(o.sprite_colors)}). "
                    f"Give them the same 3 colours, or put one on another channel pair")
            pairs[base] = a
        if a.kind == "sprite" and not a.attached and palette and any(i in palette for i in range(SPRITE_BASE[a.channel], SPRITE_BASE[a.channel] + 3)):
            a.warnings.append(f"palette.txt also defines slots {SPRITE_BASE[a.channel]}-{SPRITE_BASE[a.channel] + 2}; "
                              f"art{a.c_name()}ApplyColors() will overwrite them")

    os.makedirs(os.path.join(out_dir, "preview"), exist_ok=True)
    for a in assets:
        a.preview(palette=getattr(a, "own_palette", None) or palette).save_png(
            os.path.join(out_dir, "preview", f"{a.name}.png"))
    _write_c(assets, palette, depth, out_dir)
    return assets


def _hex(cols):
    return "/".join(f"0x{c:03X}" for c in cols)


def _words(ws):
    lines = []
    for i in range(0, len(ws), 8):
        lines.append("\t" + ", ".join(f"0x{w:04X}" for w in ws[i:i + 8]) + ",")
    return "\n".join(lines)


def _write_c(assets, palette, depth, out_dir):
    h = ["// Generated by agk art from art/ - do not edit; edit art/ and rebuild.",
         "#ifndef _AGK_ART_H_", "#define _AGK_ART_H_", "",
         "#include <ace/types.h>", "#include <ace/utils/bitmap.h>", ""]
    c = ["// Generated by agk art from art/ - do not edit; edit art/ and rebuild.",
         '#include "art.h"', ""]
    if palette:
        size = max(palette) + 1
        h += [f"#define ART_PALETTE_SIZE {size}", f"#define ART_DEPTH {depth}",
              "/** Copy art/palette.txt into a viewport palette (pVPort->pPalette). */",
              "void artPaletteApply(UWORD *pPalette);", ""]
        c += ["void artPaletteApply(UWORD *pPalette) {"]
        c += [f"\tpPalette[{i}] = 0x{col:03X};" for i, col in sorted(palette.items())]
        c += ["}", ""]
    for a in assets:
        N, U = a.c_name(), a.name.upper()
        data = a.planar()
        h += [f"// [{a.name}] {a.kind}, {a.w}x{a.h}, {len(a.frames)} frame(s), from art/{os.path.basename(a.source)}",
              f"#define ART_{U}_W {a.w}", f"#define ART_{U}_H {a.h}", f"#define ART_{U}_FRAMES {len(a.frames)}"]
        c += [f"static const UWORD s_pArt{N}[] = {{", _words(data), "};", ""]
        if a.kind == "sprite" and a.attached:
            parts = a.columns * 2
            h += [f"#define ART_{U}_CHANNEL {a.channel}  // uses channels {a.channel}-{a.channel + parts - 1}",
                  f"#define ART_{U}_PARTS {parts}  // hardware sprites per frame: part p -> channel CHANNEL+p",
                  f"#define ART_{U}_COLUMNS {a.columns}  // part p is drawn at x + (p / 2) * 16"]
            if getattr(a, "mirror_offset", None) is not None:
                h += [f"#define ART_{U}_MIRROR {a.mirror_offset}  // frame f facing left = f + ART_{U}_MIRROR"]
            h += [f"/** 15-colour attached sprite: one bitmap per frame and part (p = column*2 + odd).",
                  f" *  spriteAdd(ART_{U}_CHANNEL + p, bm); spriteSetAttached() on the odd parts. */",
                  f"tBitMap *art{N}Create(UBYTE ubFrame, UBYTE ubPart);",
                  f"/** Set the 15 attached-sprite colours (slots 17-31, shared by all attached sprites). */",
                  f"void art{N}ApplyColors(UWORD *pPalette);", ""]
            c += [f"tBitMap *art{N}Create(UBYTE ubFrame, UBYTE ubPart) {{",
                  f"\ttBitMap *pBm = bitmapCreate(16, ART_{U}_H + 2, 2, BMF_CLEAR | BMF_INTERLEAVED);",
                  f"\tconst UWORD *pSrc = &s_pArt{N}[(ubFrame * ART_{U}_PARTS + ubPart) * ART_{U}_H * 2];",
                  f"\tUWORD uwWordsPerRow = pBm->BytesPerRow / 2;",
                  f"\tfor(UWORD y = 0; y < ART_{U}_H; ++y) {{",
                  f"\t\tUWORD *pRow = (UWORD *)pBm->Planes[0] + (y + 1) * uwWordsPerRow;",
                  f"\t\tpRow[0] = *pSrc++;",
                  f"\t\tpRow[1] = *pSrc++;",
                  f"\t}}",
                  f"\treturn pBm;", "}", "",
                  f"void art{N}ApplyColors(UWORD *pPalette) {{"]
            c += [f"\tpPalette[{17 + i}] = 0x{col:03X};" for i, col in enumerate(a.sprite_colors)]
            c += ["}", ""]
        elif a.kind == "sprite":
            base = SPRITE_BASE[a.channel]
            h += [f"#define ART_{U}_CHANNEL {a.channel}",
                  f"/** Sprite bitmap for frame ubFrame, ready for spriteAdd()/spriteSetBitmap().",
                  f" *  One bitmap per frame: the sprite manager writes control words into it. */",
                  f"tBitMap *art{N}Create(UBYTE ubFrame);",
                  f"/** Set this sprite's colours in palette slots {base}-{base + 2}. */",
                  f"void art{N}ApplyColors(UWORD *pPalette);", ""]
            c += [f"tBitMap *art{N}Create(UBYTE ubFrame) {{",
                  f"\t// 16 px, 2 bitplanes, interleaved, + an empty first/last line for control words",
                  f"\ttBitMap *pBm = bitmapCreate(16, ART_{U}_H + 2, 2, BMF_CLEAR | BMF_INTERLEAVED);",
                  f"\tconst UWORD *pSrc = &s_pArt{N}[ubFrame * ART_{U}_H * 2];",
                  f"\tUWORD uwWordsPerRow = pBm->BytesPerRow / 2;",
                  f"\tfor(UWORD y = 0; y < ART_{U}_H; ++y) {{",
                  f"\t\tUWORD *pRow = (UWORD *)pBm->Planes[0] + (y + 1) * uwWordsPerRow;",
                  f"\t\tpRow[0] = *pSrc++;",
                  f"\t\tpRow[1] = *pSrc++;",
                  f"\t}}",
                  f"\treturn pBm;", "}", "",
                  f"void art{N}ApplyColors(UWORD *pPalette) {{"]
            # Sprite colour index 1..3 -> slots base..base+2 (index 0 = transparent)
            c += [f"\tpPalette[{base + i}] = 0x{col:03X};" for i, col in enumerate(a.sprite_colors)]
            c += ["}", ""]
        elif a.kind == "bitmap":
            wpx = a.words * 16
            h += [f"#define ART_{U}_BITMAP_W {wpx}"]
            if getattr(a, "own_palette", None):
                n = 1 << a.depth
                h += [f"/** This bitmap's own palette ({n} entries; [0] is the transparent/background",
                      f" *  slot - leave it to the copper/background). Load it with copper MOVEs",
                      f" *  where this band starts. */",
                      f"extern const UWORD g_pArt{N}Palette[{n}];"]
                c += [f"const UWORD g_pArt{N}Palette[{n}] = {{",
                      "	" + ", ".join(f"0x{a.own_palette.get(i, 0):03X}" for i in range(n)), "};", ""]
            if getattr(a, "loop_w", None):
                h += [f"#define ART_{U}_LOOP_W {a.loop_w}  // scroll offset wraps at this width"]
            h += [f"/** {a.depth} planes, interleaved; frames (tiles) stacked vertically,",
                  f" *  frame f starts at row f * ART_{U}_H. Transparent pixels are colour 0. */",
                  f"tBitMap *art{N}Create(void);", ""]
            c += [f"tBitMap *art{N}Create(void) {{",
                  f"	tBitMap *pBm = bitmapCreate(",
                  f"		{wpx}, ART_{U}_H * ART_{U}_FRAMES, {a.depth}, BMF_CLEAR | BMF_INTERLEAVED",
                  f"	);",
                  f"	const UWORD *pSrc = s_pArt{N};",
                  f"	for(UWORD y = 0; y < ART_{U}_H * ART_{U}_FRAMES; ++y) {{",
                  f"		for(UBYTE p = 0; p < {a.depth}; ++p) {{",
                  f"			UWORD *pDst = (UWORD *)(pBm->Planes[p] + y * pBm->BytesPerRow);",
                  f"			for(UBYTE w = 0; w < {a.words}; ++w) {{",
                  f"				pDst[w] = *pSrc++;",
                  f"			}}",
                  f"		}}",
                  f"	}}",
                  f"	return pBm;", "}", ""]
        else:
            wpx = a.words * 16
            h += [f"#define ART_{U}_BITMAP_W {wpx}",
                  f"/** All frames stacked vertically, {a.depth} planes, interleaved (for bobInit/",
                  f" *  bobSetFrame with bobCalcFrameAddress(pBm, uwFrame * ART_{U}_H)). */",
                  f"tBitMap *art{N}Create(void);",
                  f"/** Matching mask: same size and depth, mask bits repeated in every plane. */",
                  f"tBitMap *art{N}CreateMask(void);", ""]
            for fn, is_mask in (("Create", False), ("CreateMask", True)):
                c += [f"tBitMap *art{N}{fn}(void) {{",
                      f"\ttBitMap *pBm = bitmapCreate(",
                      f"\t\t{wpx}, ART_{U}_H * ART_{U}_FRAMES, {a.depth}, BMF_CLEAR | BMF_INTERLEAVED",
                      f"\t);",
                      f"\tconst UWORD *pSrc = s_pArt{N};",
                      f"\tfor(UWORD y = 0; y < ART_{U}_H * ART_{U}_FRAMES; ++y) {{",
                      f"\t\tfor(UBYTE p = 0; p < {a.depth}; ++p) {{",
                      f"\t\t\tUWORD *pDst = (UWORD *)(pBm->Planes[p] + y * pBm->BytesPerRow);",
                      f"\t\t\tfor(UBYTE w = 0; w < {a.words}; ++w) {{",
                      (f"\t\t\t\tpDst[w] = pSrc[{a.depth} * {a.words} + w];" if is_mask
                       else f"\t\t\t\tpDst[w] = pSrc[p * {a.words} + w];"),
                      f"\t\t\t}}",
                      f"\t\t}}",
                      f"\t\tpSrc += {a.depth + 1} * {a.words};",
                      f"\t}}",
                      f"\treturn pBm;", "}", ""]
    h += ["#endif // _AGK_ART_H_", ""]
    new_h, new_c = "\n".join(h), "\n".join(c)
    # Only touch files whose content changed, so make doesn't rebuild needlessly
    for fname, text in (("art.h", new_h), ("art.c", new_c)):
        path = os.path.join(out_dir, fname)
        old = None
        if os.path.exists(path):
            with open(path) as f:
                old = f.read()
        if old != text:
            with open(path, "w") as f:
                f.write(text)


# ------------------------------------------------------------------ export

LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"


def export_text(png_path, txt_path, max_colors=15, frame_width=None, frame_height=None):
    """PNG -> editable text art: colours rounded to 12-bit (reduced to
    max_colors if needed), one letter per colour ordered dark to light,
    '.' = transparent. Returns (colour count, warnings)."""
    w, h, frames = load_png_art(png_path, frame_width, frame_height)
    counts = {}
    for fr in frames:
        for row in fr:
            for c in row:
                if c is not None:
                    counts[c] = counts.get(c, 0) + 1
    warnings = []
    keep = list(counts)
    if len(keep) > max_colors:
        warnings.append(f"{len(keep)} colours reduced to {max_colors}")
        keep = _reduce(counts, max_colors)
    remap = {c: min(keep, key=lambda k: dist(c, k)) for c in counts}
    keep.sort(key=lambda c: sum(to_rgb8(c)))
    letter = {c: LETTERS[i] for i, c in enumerate(keep)}
    out = [f"# Exported from {os.path.basename(png_path)} by agk art-export. Edit freely:",
           "# one character per pixel, '.' = transparent; add 'frame' sections to animate.",
           "colors", "  .  transparent"]
    out += [f"  {letter[c]}  0x{c:03X}" for c in keep]
    for fr in frames:
        out.append("frame")
        out += ["".join("." if c is None else letter[remap[c]] for c in row) for row in fr]
    with open(txt_path, "w") as f:
        f.write("\n".join(out) + "\n")
    return len(keep), warnings
