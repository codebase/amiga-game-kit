"""agk map: Tiled JSON -> C tables (synthetic maps, no emulator)."""
import base64
import json
import os
import tempfile
import unittest
import zlib

from agk import tilemap
from agk.image import Image, load_png_rgba


def tileset(d, name="tiles", tiles=None):
    png = os.path.join(d, "maps", f"{name}.png")
    # 2 columns x 2 rows of 2x2 tiles: red, green / blue, white
    cols = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]
    px = bytearray()
    for y in range(4):
        for x in range(4):
            px += bytes(cols[(y // 2) * 2 + x // 2])
    Image(bytes(px), 4, 4).save_png(png)
    data = {"name": name, "tilewidth": 2, "tileheight": 2, "tilecount": 4, "columns": 2,
            "image": f"{name}.png", "tiles": tiles or []}
    with open(os.path.join(d, "maps", f"{name}.tsj"), "w") as f:
        json.dump(data, f)


def project(layers, objects=(), props=(), tiles=None, encoding=None):
    d = tempfile.mkdtemp()
    os.makedirs(os.path.join(d, "maps"))
    tileset(d, tiles=tiles)
    ls = []
    for name, data in layers:
        layer = {"type": "tilelayer", "name": name, "width": 3, "height": 2}
        if encoding == "zlib":
            raw = b"".join(v.to_bytes(4, "little") for v in data)
            layer.update(encoding="base64", compression="zlib", data=base64.b64encode(zlib.compress(raw)).decode())
        else:
            layer["data"] = data
        ls.append(layer)
    if objects:
        ls.append({"type": "objectgroup", "name": "things", "objects": list(objects)})
    tmj = {"orientation": "orthogonal", "width": 3, "height": 2, "tilewidth": 2, "tileheight": 2,
           "tilesets": [{"firstgid": 1, "source": "tiles.tsj"}], "layers": ls,
           "properties": [{"name": k, "type": "int", "value": v} for k, v in props]}
    with open(os.path.join(d, "maps", "level1.tmj"), "w") as f:
        json.dump(tmj, f)
    return d


class TilemapTests(unittest.TestCase):
    def test_tiles_are_column_major_and_empty_is_configurable(self):
        # row 0: tiles 1 2 3 (gids), row 1: 4 _ 1  -> local ids 0 1 2 / 3 empty 0
        d = project([("ground", [1, 2, 3, 4, 0, 1])], props=[("empty", 9)])
        (m,) = tilemap.build(d)
        self.assertEqual(m["layers"][0][1], [0, 3, 1, 9, 2, 0])      # [x * H + y]
        h = open(os.path.join(d, "build", "maps", "maps.h")).read()
        self.assertIn("#define MAP_LEVEL1_W 3", h)
        self.assertIn("#define MAP_LEVEL1_EMPTY 9", h)
        self.assertIn("mapLevel1At(int16_t tx, int16_t ty)", h)
        self.assertIn("const uint8_t g_pMapLevel1[MAP_LEVEL1_W * MAP_LEVEL1_H]", h)
        self.assertTrue(os.path.exists(os.path.join(d, "build", "maps", "preview", "level1.png")))
        # the preview: tile 0 (red) at (0,0), tile 3 (white) at (0,1)
        w, hh, px = load_png_rgba(os.path.join(d, "build", "maps", "preview", "level1.png"))
        self.assertEqual((px[0][:3], px[2 * w][:3]), ((255, 0, 0), (255, 255, 255)))

    def test_flags_objects_layers_and_zlib(self):
        tiles = [{"id": 1, "properties": [{"name": "solid", "type": "bool", "value": True}]},
                 {"id": 2, "properties": [{"name": "oneway", "type": "bool", "value": True},
                                          {"name": "solid", "type": "bool", "value": False}]}]
        objs = [{"name": "e1", "class": "enemy", "x": 4.0, "y": 2.0, "width": 0, "height": 0,
                 "properties": [{"name": "param", "type": "int", "value": -1}]},
                {"name": "exit", "type": "exit zone", "x": 0, "y": 0, "width": 2, "height": 2}]
        d = project([("Tile Layer 1", [1, 1, 1, 2, 2, 2]), ("back", [3, 3, 3, 3, 3, 3])],
                    objects=objs, tiles=tiles, encoding="zlib")
        (m,) = tilemap.build(d)
        h = open(os.path.join(d, "build", "maps", "maps.h")).read()
        c = open(os.path.join(d, "build", "maps", "maps.c")).read()
        self.assertIn("#define MAP_TILE_ONEWAY (1 << 0)", h)
        self.assertIn("#define MAP_TILE_SOLID (1 << 1)", h)
        self.assertIn("const uint8_t g_pTilesTilesFlags[4] = {\n\t0, 2, 1, 0", c)
        self.assertIn("#define MAP_OBJ_ENEMY 1", h)
        self.assertIn("#define MAP_OBJ_EXIT_ZONE 2", h)
        self.assertIn("#define MAP_PROPS 1", h)
        self.assertIn("#define MAP_PROP_PARAM 0", h)
        self.assertIn('{"e1", MAP_OBJ_ENEMY, 4, 2, 0, 0, {-1}}', c)
        self.assertIn('{"exit", MAP_OBJ_EXIT_ZONE, 0, 0, 2, 2, {0}}', c)
        self.assertIn("g_pMapLevel1Back[", h)                       # the second layer, by name
        self.assertEqual(m["layers"][1][1], [2] * 6)

    def test_flipped_tiles_warn_and_errors_say_what_to_do(self):
        d = project([("g", [1 | 0x80000000, 1, 1, 1, 1, 1])])
        (m,) = tilemap.build(d)
        self.assertIn("1 flipped/rotated tiles", m["warnings"][0])
        self.assertEqual(m["layers"][0][1][0], 0)
        d = project([("g", [1, 2, 3, 4, 5, 6])])
        with self.assertRaisesRegex(tilemap.MapError, "tile id 5, past the end of tileset 'tiles' \\(4 tiles\\)"):
            tilemap.build(d)
        with self.assertRaisesRegex(tilemap.MapError, "no letters"):
            tilemap.ident("***")
        self.assertEqual(tilemap.ident("Tile Layer 1"), "TileLayer1")


if __name__ == "__main__":
    unittest.main()
