"""agk map: levels made in Tiled (https://www.mapeditor.org), as C tables.

    maps/level1.tmj   a Tiled map saved as JSON (File > Export As, or set the
                      project's map format to JSON); its tileset as JSON too
                      (.tsj), or embedded in the map
    -> build/maps/maps.h, maps.c      (built with the game, like art/ and sound/)
       build/maps/preview/level1.png  the map drawn with its tileset, objects outlined

maps.h is plain C (stdint, no Amiga headers), so src/logic.c can use it and
`agk unit` tests it on the host. For each map NAME (the file's name):

    #define MAP_NAME_W 80, MAP_NAME_H 16, MAP_NAME_TILE 16      size in tiles, tile size in px
    extern const uint8_t g_pMapName[MAP_NAME_W * MAP_NAME_H];   the first tile layer, column by
                                                                column: tile at (x, y) is [x * H + y]
    static inline uint8_t mapNameAt(int16_t tx, int16_t ty);   bounds-checked: outside = empty
    extern const uint8_t g_pMapNameLAYER[...];                  other tile layers, by their names
    extern const tMapObject g_pMapNameObjects[MAP_NAME_OBJECTS];   every object layer's objects:
        { szName, ubType (MAP_OBJ_CLASS, from the object's class), uwX, uwY, uwW, uwH (px),
          pProps[MAP_PROP_NAME] (its number and bool custom properties, 0 if unset) }

Tiles are the tileset's indices (its first tile = 0), which is what ACE's
tile buffer wants: `pTileData[x][y] = g_pMapName[x * MAP_NAME_H + y]`. Empty
cells (no tile) become the map's custom property `empty` (an int, default 0),
so make the tileset's first tile a blank one. Tile properties that are
booleans (solid, oneway, hurt...) become bits:

    #define MAP_TILE_SOLID (1 << 0) ...                         one bit per property name
    extern const uint8_t g_pTilesSETFlags[count];               per tileset SET: the tile's bits

The tileset's image is a grid; the art pipeline wants the same tiles stacked
in one column (ACE's format), which `frame_width`/`frame_height` do:

    [tiles]                      # art/art.toml
    source = "../maps/tiles.png"
    kind = "bitmap"
    frame_width = 16
    frame_height = 16
    palette = "auto"             # or the game palette

Flipped or rotated tiles can't be drawn by the hardware: they're used unflipped
(with a warning). Infinite maps and compressed tile data other than zlib/gzip
aren't read: save a fixed-size map with CSV or base64 encoding.
"""
import base64
import gzip
import json
import os
import re
import zlib

from .image import Image, load_png_rgba

FLIP_MASK = 0xE0000000 | 0x10000000


class MapError(Exception):
    pass


def ident(text, upper=False):
    """'Tile Layer 1' -> 'TileLayer1' (or 'TILE_LAYER_1')"""
    words = re.findall(r"[A-Za-z0-9]+", text)
    if not words:
        raise MapError(f"'{text}' has no letters or digits to name it in C")
    if upper:
        return "_".join(w.upper() for w in words)
    return "".join(w[:1].upper() + w[1:] for w in words)


def _props(obj):
    """Tiled's properties list -> {name: value}"""
    return {p["name"]: p.get("value") for p in obj.get("properties", []) or []}


def _layer_data(layer):
    enc = layer.get("encoding", "csv")
    if "chunks" in layer:
        raise MapError(f"layer '{layer['name']}' is an infinite map: set a fixed size (Map > Map Properties)")
    if enc == "csv":
        return list(layer["data"])
    raw = base64.b64decode(layer["data"])
    comp = layer.get("compression", "")
    if comp == "zlib":
        raw = zlib.decompress(raw)
    elif comp == "gzip":
        raw = gzip.decompress(raw)
    elif comp:
        raise MapError(f"layer '{layer['name']}' is compressed with {comp}: use zlib, gzip or none")
    return [int.from_bytes(raw[i:i + 4], "little") for i in range(0, len(raw), 4)]


def load_tilesets(tmj, map_dir):
    """-> [{firstgid, name, tilecount, columns, image (abs path or None), tilewidth,
            tileheight, margin, spacing, tiles: {id: props}}], highest firstgid first"""
    out = []
    for ts in tmj.get("tilesets", []):
        base = map_dir
        if "source" in ts:
            src = os.path.join(map_dir, ts["source"])
            if not src.endswith((".tsj", ".json")):
                raise MapError(f"tileset {ts['source']} isn't JSON: in Tiled, save it as .tsj "
                               f"(Edit Tileset > File > Export As, JSON)")
            if not os.path.exists(src):
                raise MapError(f"tileset {src} not found")
            data = json.load(open(src))
            base = os.path.dirname(src)
        else:
            data = ts
        image = os.path.normpath(os.path.join(base, data["image"])) if data.get("image") else None
        out.append({"firstgid": ts["firstgid"], "name": data.get("name", "tiles"),
                    "tilecount": data.get("tilecount", 0), "columns": data.get("columns", 0),
                    "image": image, "tilewidth": data.get("tilewidth", tmj["tilewidth"]),
                    "tileheight": data.get("tileheight", tmj["tileheight"]),
                    "margin": data.get("margin", 0), "spacing": data.get("spacing", 0),
                    "tiles": {t["id"]: _props(t) for t in data.get("tiles", [])}})
    return sorted(out, key=lambda t: -t["firstgid"])


def convert(path):
    """A .tmj -> dict: name, w, h, tile, empty, layers [(name, [column-major ids], tileset)],
    objects [(name, class, x, y, w, h, param)], tilesets, warnings"""
    tmj = json.load(open(path))
    if tmj.get("orientation", "orthogonal") != "orthogonal":
        raise MapError(f"{path}: only orthogonal maps (not {tmj['orientation']})")
    if tmj.get("infinite"):
        raise MapError(f"{path}: an infinite map; set a fixed size (Map > Map Properties)")
    name = os.path.splitext(os.path.basename(path))[0]
    w, h, tw, th = tmj["width"], tmj["height"], tmj["tilewidth"], tmj["tileheight"]
    if tw != th or tw < 2 or tw & (tw - 1):
        raise MapError(f"{path}: tiles are {tw}x{th}; ACE's tile buffer wants square tiles of a power of "
                       f"two (16 or 32 px, usually)")
    empty = int(_props(tmj).get("empty", 0))
    tilesets = load_tilesets(tmj, os.path.dirname(os.path.abspath(path)))
    warnings, layers, objects = [], [], []

    def walk(items):
        for layer in items:
            if layer["type"] == "group":
                walk(layer.get("layers", []))
            elif layer["type"] == "tilelayer":
                layers.append(layer)
            elif layer["type"] == "objectgroup":
                for o in layer.get("objects", []):
                    objects.append(o)
    walk(tmj.get("layers", []))
    if not layers:
        raise MapError(f"{path}: no tile layer")
    out_layers = []
    for layer in layers:
        data = _layer_data(layer)
        if len(data) != w * h:
            raise MapError(f"{path}: layer '{layer['name']}' has {len(data)} cells, not {w}x{h}")
        flipped = sum(1 for g in data if g & FLIP_MASK)
        if flipped:
            warnings.append(f"layer '{layer['name']}': {flipped} flipped/rotated tiles used unflipped "
                            f"(the hardware can't flip tiles; draw flipped copies in the tileset)")
        used = set()
        ids = [0] * (w * h)
        for i, gid in enumerate(data):
            gid &= ~FLIP_MASK
            x, y = i % w, i // w
            if gid == 0:
                ids[x * h + y] = empty
                continue
            ts = next((t for t in tilesets if t["firstgid"] <= gid), None)
            if ts is None:
                raise MapError(f"{path}: layer '{layer['name']}' uses tile id {gid}, which no tileset has")
            if ts["tilecount"] and gid - ts["firstgid"] >= ts["tilecount"]:
                raise MapError(f"{path}: layer '{layer['name']}' uses tile id {gid}, past the end of tileset "
                               f"'{ts['name']}' ({ts['tilecount']} tiles)")
            used.add(ts["name"])
            ids[x * h + y] = gid - ts["firstgid"]
        if len(used) > 1:
            raise MapError(f"{path}: layer '{layer['name']}' mixes tilesets {sorted(used)}; a tile buffer "
                           f"draws from one tileset (put them in one image)")
        out_layers.append((layer["name"], ids, next(iter(used)) if used else None))
    out_objects = []
    for o in objects:
        cls = o.get("class") or o.get("type") or ""
        props = {}
        for k, v in _props(o).items():
            if isinstance(v, bool):
                props[k] = int(v)
            elif isinstance(v, (int, float)):
                if not -32768 <= v <= 32767 or v != int(v):
                    raise MapError(f"{path}: object '{o.get('name')}': property {k} = {v} isn't a whole number "
                                   f"from -32768 to 32767")
                props[k] = int(v)
        out_objects.append((o.get("name", ""), cls, int(round(o["x"])), int(round(o["y"])),
                            int(round(o.get("width", 0))), int(round(o.get("height", 0))), props))
    return {"name": name, "w": w, "h": h, "tile": tw, "empty": empty, "layers": out_layers,
            "objects": out_objects, "tilesets": tilesets, "warnings": warnings, "path": path}


# ----------------------------------------------------------------- C output

def tile_flag_names(maps):
    names = set()
    for m in maps:
        for ts in m["tilesets"]:
            for props in ts["tiles"].values():
                names |= {k for k, v in props.items() if isinstance(v, bool)}
    return sorted(names)


def object_classes(maps):
    return sorted({cls for m in maps for _, cls, *_ in m["objects"] if cls})


MAX_PROPS = 8


def object_props(maps):
    names = sorted({k for m in maps for *_, props in m["objects"] for k in props})
    if len(names) > MAX_PROPS:
        raise MapError(f"objects use {len(names)} different number properties ({', '.join(names)}); "
                       f"at most {MAX_PROPS}")
    return names


def generate(maps):
    """-> (maps.h, maps.c) text"""
    flags = tile_flag_names(maps)
    classes = object_classes(maps)
    props = object_props(maps)
    wide = any(max(ids, default=0) > 255 for m in maps for _, ids, _ in m["layers"])
    tile_t = "uint16_t" if wide else "uint8_t"
    h = ["// Generated by agk map from maps/*.tmj - don't edit; edit the maps in Tiled (agk help-map)",
         "#ifndef _AGK_MAPS_H_", "#define _AGK_MAPS_H_", "", "#include <stdint.h>", ""]
    if wide:
        h.append("// A tileset with more than 256 tiles: build with -DACE_TILEBUFFER_TILE_TYPE=UWORD")
    h += [f"#define MAP_PROPS {max(1, len(props))}"]
    h += [f"#define MAP_PROP_{ident(p, True)} {i}" for i, p in enumerate(props)]
    h += ["typedef struct {", "\tconst char *szName;", "\tuint8_t ubType;              // MAP_OBJ_*",
          "\tuint16_t uwX, uwY, uwW, uwH;  // px; a point has w = h = 0",
          "\tint16_t pProps[MAP_PROPS];    // the object's number properties, by MAP_PROP_* (0 if unset)",
          "} tMapObject;", "", "#define MAP_OBJ_NONE 0"]
    h += [f"#define MAP_OBJ_{ident(c, True)} {i + 1}" for i, c in enumerate(classes)]
    h.append("")
    h += [f"#define MAP_TILE_{ident(f, True)} (1 << {i})" for i, f in enumerate(flags)]
    c = ['#include "maps.h"', ""]
    seen_ts = {}
    for m in maps:
        for ts in m["tilesets"]:
            if ts["name"] in seen_ts or not ts["tiles"]:
                continue
            seen_ts[ts["name"]] = True
            n = max(ts["tilecount"], max(ts["tiles"]) + 1)
            cname = f"g_pTiles{ident(ts['name'])}Flags"
            h.append(f"extern const uint8_t {cname}[{n}];   // tileset \"{ts['name']}\": MAP_TILE_* bits per tile")
            vals = []
            for i in range(n):
                p = ts["tiles"].get(i, {})
                vals.append(sum(1 << k for k, f in enumerate(flags) if p.get(f) is True))
            c += [f"const uint8_t {cname}[{n}] = {{", "\t" + ", ".join(map(str, vals)), "};", ""]
    for m in maps:
        N, U = ident(m["name"]), ident(m["name"], True)
        h += ["", f"// {os.path.basename(m['path'])}: {m['w']}x{m['h']} tiles of {m['tile']} px",
              f"#define MAP_{U}_W {m['w']}", f"#define MAP_{U}_H {m['h']}", f"#define MAP_{U}_TILE {m['tile']}",
              f"#define MAP_{U}_EMPTY {m['empty']}"]
        for i, (lname, ids, ts) in enumerate(m["layers"]):
            arr = f"g_pMap{N}" + ("" if i == 0 else ident(lname))
            h.append(f"extern const {tile_t} {arr}[MAP_{U}_W * MAP_{U}_H];   // layer \"{lname}\""
                     + (f", tileset \"{ts}\"" if ts else "") + ", [x * H + y]")
            rows = []
            for x in range(m["w"]):
                rows.append("\t" + ", ".join(str(v) for v in ids[x * m["h"]:(x + 1) * m["h"]]) + ",")
            c += [f"const {tile_t} {arr}[MAP_{U}_W * MAP_{U}_H] = {{", *rows, "};", ""]
            if i == 0:
                h += [f"static inline {tile_t} map{N}At(int16_t tx, int16_t ty) {{",
                      f"\tif(tx < 0 || tx >= MAP_{U}_W || ty < 0 || ty >= MAP_{U}_H) return MAP_{U}_EMPTY;",
                      f"\treturn {arr}[tx * MAP_{U}_H + ty];", "}"]
        h.append(f"#define MAP_{U}_OBJECTS {len(m['objects'])}")
        if m["objects"]:
            h.append(f"extern const tMapObject g_pMap{N}Objects[MAP_{U}_OBJECTS];")
            c.append(f"const tMapObject g_pMap{N}Objects[MAP_{U}_OBJECTS] = {{")
            for oname, cls, x, y, w, hh, op in m["objects"]:
                t = f"MAP_OBJ_{ident(cls, True)}" if cls else "MAP_OBJ_NONE"
                vals = ", ".join(str(op.get(p, 0)) for p in props) or "0"
                c.append(f"\t{{{json.dumps(oname)}, {t}, {x}, {y}, {w}, {hh}, {{{vals}}}}},")
            c += ["};", ""]
    h += ["", "#endif"]
    return "\n".join(h) + "\n", "\n".join(c) + "\n"


# ------------------------------------------------------------------ preview

def preview(m, path):
    """The map drawn with its tileset image(s), objects outlined in magenta."""
    tw = m["tile"]
    W, H = m["w"] * tw, m["h"] * tw
    px = bytearray(W * H * 3)
    images = {}
    for ts in m["tilesets"]:
        if ts["image"] and os.path.exists(ts["image"]):
            images[ts["name"]] = (ts, load_png_rgba(ts["image"]))
    for lname, ids, tsname in m["layers"]:
        if tsname not in images:
            continue
        ts, (iw, ih, ipx) = images[tsname]
        cols = ts["columns"] or max(1, (iw - 2 * ts["margin"] + ts["spacing"]) // (tw + ts["spacing"]))
        for x in range(m["w"]):
            for y in range(m["h"]):
                t = ids[x * m["h"] + y]
                sx = ts["margin"] + (t % cols) * (tw + ts["spacing"])
                sy = ts["margin"] + (t // cols) * (tw + ts["spacing"])
                for yy in range(tw):
                    for xx in range(tw):
                        ix, iy = sx + xx, sy + yy
                        if ix >= iw or iy >= ih:
                            continue
                        r, g, b, a = ipx[iy * iw + ix]
                        if a >= 128:
                            o = ((y * tw + yy) * W + x * tw + xx) * 3
                            px[o:o + 3] = bytes((r, g, b))
    for _, _, x, y, w, h, _props_ in m["objects"]:
        w, h = max(w, 2), max(h, 2)
        for xx in range(x, min(W, x + w)):
            for yy in (y, y + h - 1):
                if 0 <= yy < H:
                    px[(yy * W + xx) * 3:(yy * W + xx) * 3 + 3] = b"\xff\x00\xff"
        for yy in range(y, min(H, y + h)):
            for xx in (x, x + w - 1):
                if 0 <= xx < W and 0 <= yy:
                    px[(yy * W + xx) * 3:(yy * W + xx) * 3 + 3] = b"\xff\x00\xff"
    Image(bytes(px), W, H).save_png(path)


# -------------------------------------------------------------------- build

def build(project_dir, out_dir=None):
    """maps/*.tmj -> build/maps/{maps.h, maps.c, preview/}. Returns [maps] or None (no maps/)."""
    maps_dir = os.path.join(project_dir, "maps")
    if not os.path.isdir(maps_dir):
        return None
    files = sorted(f for f in os.listdir(maps_dir) if f.endswith((".tmj", ".json")))
    if not files:
        return None
    maps = [convert(os.path.join(maps_dir, f)) for f in files]
    names = [m["name"] for m in maps]
    if len(set(ident(n) for n in names)) != len(names):
        raise MapError(f"maps/ names clash as C identifiers: {names}")
    out_dir = out_dir or os.path.join(project_dir, "build", "maps")
    os.makedirs(os.path.join(out_dir, "preview"), exist_ok=True)
    h, c = generate(maps)
    for fname, text in (("maps.h", h), ("maps.c", c)):
        p = os.path.join(out_dir, fname)
        if not os.path.exists(p) or open(p).read() != text:   # (keeps make from rebuilding)
            with open(p, "w") as f:
                f.write(text)
    for m in maps:
        preview(m, os.path.join(out_dir, "preview", m["name"] + ".png"))
        art_toml = os.path.join(project_dir, "art", "art.toml")
        art_text = open(art_toml).read() if os.path.exists(art_toml) else ""
        for ts in m["tilesets"]:
            if ts["image"] and os.path.basename(ts["image"]) not in art_text:
                m["warnings"].append(f"tileset image {os.path.relpath(ts['image'], project_dir)} isn't in "
                                     f"art/art.toml: add it as kind = \"bitmap\" with frame_width = "
                                     f"frame_height = {m['tile']} to draw the map (agk help-map)")
    return maps
