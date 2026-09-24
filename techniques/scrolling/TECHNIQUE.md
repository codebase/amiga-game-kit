# Technique: tile-map scrolling (ACE tile buffer)

A level much wider than the screen, scrolled smoothly by 1 px at a time. The
**tile buffer** keeps a bitmap only a little bigger than the screen. Then, each
frame:
- the **copper** points the display at the right place in that bitmap (coarse
  scroll, in 16 px steps)
- **BPLCON1** delays the picture by 0–15 px (fine scroll)
- the **blitter** draws only the tile column that is about to scroll into view,
  one tile per frame, off-screen

Nothing on screen gets redrawn, so a full-screen scroll costs a few percent of
the frame.

| | redraw the screen yourself (simple buffer) | ACE tile buffer |
|---|---|---|
| chip RAM | a bitmap as big as the whole level (960×256×4bpp = 123 KB here), or a full redraw every frame | screen + 1–2 tile margins (97 KB per buffer here, whatever the level width) |
| per-frame work | blit the whole screen | copper pointer update + 1 tile (a column burst every 16 px) |
| level size | limited by RAM | 1 byte per tile (`pTileData`, fast RAM) + 1 bitmap row per screen width |

Use it for any level that is wider (or taller) than the screen and built from
16×16 or 32×32 tiles.

This directory is a complete, tested game. The level is 60×16 tiles
(960×256 px, three screens wide), written as strings in `src/logic.c` with
tiles drawn procedurally at startup, so it needs no asset files. The player is
a hardware sprite that walks along the ground, 1 px/frame, or 2 px/frame with
fire held. The camera keeps the player centred and stops at the level edges.
Run it:

```sh
agk test      # 12/12 on a500, a500-ks31, a500-aros
agk run -s "press right+fire 200" -s "wait 3" -s "screenshot s"
```

## The recipe (ACE)

All of this is in `src/main.c`. Rules like camera, player and level queries
live in `src/logic.c`, with unit tests.

1. **View and viewport** as usual. The tile buffer creates the scroll buffer
   manager and the camera manager itself.
   ```c
   s_pView = viewCreate(0, TAG_VIEW_GLOBAL_PALETTE, 1, TAG_DONE);
   s_pVPort = vPortCreate(0, TAG_VPORT_VIEW, s_pView, TAG_VPORT_BPP, 4, TAG_DONE);
   ```
2. **The tileset** is one bitmap with all tiles in **one column**: width =
   tile size, tile *i* at y = *i* × 16. Give it the **same depth and the same
   interleaving as the buffer**. Then ACE blits a whole tile, all planes, with
   one blit. Mixing interleaved and non-interleaved bitmaps works, but ACE falls
   back to one blit per plane and logs a warning.
   ```c
   s_pTileset = bitmapCreate(16, 16 * TILE_COUNT, 4, BMF_CLEAR | BMF_INTERLEAVED);
   blitRect(s_pTileset, x, tile * 16 + y, w, h, colour);   // draw tiles procedurally
   ```
3. **Create the tile buffer** before `systemUnuse()`:
   ```c
   s_pTileBuffer = tileBufferCreate(0,
       TAG_TILEBUFFER_VPORT, s_pVPort,
       TAG_TILEBUFFER_BITMAP_FLAGS, BMF_CLEAR | BMF_INTERLEAVED,
       TAG_TILEBUFFER_BOUND_TILE_X, 60,        // level size in tiles
       TAG_TILEBUFFER_BOUND_TILE_Y, 16,
       TAG_TILEBUFFER_TILE_SHIFT, 4,           // 16 px tiles
       TAG_TILEBUFFER_TILESET, s_pTileset,
       TAG_TILEBUFFER_IS_DBLBUF, 1,
       TAG_TILEBUFFER_REDRAW_QUEUE_LENGTH, 8,  // mandatory, non-zero (see gotchas)
       TAG_TILEBUFFER_MAX_TILESET_SIZE, TILE_COUNT,
       TAG_DONE);
   ```
4. **Fill the map.** `pTileData` is allocated by `tileBufferCreate` and indexed
   **`[x][y]`** (column-major). Each entry is a `UBYTE` tile index by default
   (`ACE_TILEBUFFER_TILE_TYPE`).
   ```c
   for(x...) for(y...) s_pTileBuffer->pTileData[x][y] = levelTileAt(x, y);
   cameraSetCoord(s_pTileBuffer->pCamera, startX, 0);
   ```
   The camera's bounds are already set to the level size by `tileBufferCreate`.
   Don't call `cameraReset` unless you know why (ACE's guide does; its
   `isDblBfr` argument must match the buffer).
5. **Full draw once**, after `systemUnuse()` (it drives the blitter directly)
   and before `viewLoad()`:
   ```c
   systemUnuse();
   tileBufferRedrawAll(s_pTileBuffer);   // needs AGK's ACE patch with fast RAM, see gotchas
   viewLoad(s_pView);
   ```
6. **Every frame:** move the camera, then process tile → scroll → camera:
   ```c
   cameraSetCoord(s_pTileBuffer->pCamera, state.cameraX, 0);  // or cameraMoveBy()
   tileBufferProcess(s_pTileBuffer);                 // blit margin tiles into pBack
   scrollBufferProcess(s_pTileBuffer->pScroll);      // copper -> pBack, BPLCON1, swap
   cameraProcess(s_pTileBuffer->pCamera);            // remember pos for this buffer
   // ... sprites ...
   copProcessBlocks();
   vPortWaitForEnd(s_pVPort);
   ```
   `viewProcessManagers(s_pView)` does the same three calls in the order
   scroll → tile → camera, which also works (measured identical, same
   pictures). The order above is the one in ACE's showcase and only ever blits
   into the buffer that isn't on screen. Call one or the other, **never both**:
   calling both processes the camera twice per frame.
7. **Sprites** are in screen coordinates: `sprite->wX = playerX - cameraX`.

## Gotchas

These are things ACE's docs don't say, found in ACE's source or by testing.

- **`tileBufferRedrawAll()` corrupted tiles on machines with fast RAM, in stock
  ACE.** Found while building this example.
  - Non-debug builds skipped `blitWait()` between tiles (`TILEBUFFER_REDRAW_HOG`),
    assuming the blitter stalls the CPU.
  - That's only true when the code runs from chip or slow RAM, and ACE forces
    that placement only for Bartman's GCC.
  - On the `a500-aros` profile (fast RAM) a tile row came out in the wrong
    colour, and the shared golden caught it.
  - **AGK's ACE patch** (`patches/ace-agk.patch`) now skips the waits only in
    that chip-RAM case, so the plain call is correct everywhere.
  - If you use stock ACE with fast RAM, blit the visible tiles again with
    `blitCopyAligned()` after the redraw.
- **The display is 2 frames behind the logic.** The copper list is
  double-buffered and the screenshot shows the last finished frame. A
  screenshot taken while scrolling shows the camera from 2 frames earlier.
  This is the same with single or double buffering. `tests/moving.agk` relies
  on it, and tick sync makes it exact.
- **`TAG_TILEBUFFER_REDRAW_QUEUE_LENGTH` is mandatory**: with 0,
  `tileBufferCreate` returns NULL.
  - ACE's guide calls it a trade-off between fast scrolling and CPU power.
    **It isn't.** The queue only holds tiles you change with
    `tileBufferSetTile()` or `tileBufferInvalidateTile()`.
  - `tileBufferProcess()` never drains it. You must call
    `tileBufferQueueProcess()` yourself, and it redraws **one** tile per call.
  - Each change is queued twice (once per buffer), so the usable capacity is
    half the length. Not exercised in this game.
- **Don't combine non-interleaved buffers with double buffering.**
  `tileBufferRedrawAll()` copies back → front as `BytesPerRow × Rows` bytes,
  which is all planes only for interleaved bitmaps. ACE's own
  `showcase/src/test/buffer_reuse.c` says the same. Use `BMF_INTERLEAVED` for
  both the buffer and the tileset.
- **The level must be at least as large as the viewport** in both axes. The
  camera's max position is `UWORD` (level − viewport), so a smaller level
  underflows. Here it's 256 px high on a 256-line screen, so `maxY = 0`.
- **ACE build options:**
  - `ACE_SCROLLBUFFER_ENABLE_SCROLL_Y OFF` (horizontal-only) **doesn't
    compile** in this ACE: `tilebuffer.c` uses `pMarginY` in the X path.
  - `ACE_SCROLLBUFFER_POT_BITMAP_HEIGHT` (default ON) rounds the buffer height
    up to a power of two: 16 × (16 + 4) = 320 → **512** lines, plus 2 (see
    below). Turning it off saves 37% chip RAM for about +1% load. See the
    table.
  - **Set options in `agk.toml` under `[cmake]`** (e.g.
    `ACE_SCROLLBUFFER_POT_BITMAP_HEIGHT = false`), or one-off with
    `agk build -D NAME=VALUE`. Both become `-D` flags. A plain `set(X OFF)` in
    CMakeLists is ignored on the first configure of a fresh build directory:
    ACE declares its options with `set(... CACHE)` under policy CMP0126=OLD.
- **The buffer height grows by 1 row per screen of level width.** ACE uses the
  "scroll trick": coarse X scroll moves the bitplane pointer, which creeps down
  one row per 320 px. So `h = 512 + 960/320 − 1 = 514`. Level width is nearly
  free.
- **Fine scroll moves DDFSTRT from `$38` to `$30`** (checked with
  `regs NAME agnus`). On OCS/ECS the extra fetch takes the DMA slots of
  **sprite 7**, which then can't be used. Channels 0–6 are fine.
- **Single buffering saves chip RAM, not CPU.** ACE keeps two redraw states
  and alternates between them even with `IS_DBLBUF 0`, so every margin tile is
  still drawn twice. It measured identical, and the pictures were identical
  too. Use double buffering once you add BOBs.
- **ACE's `docs/programming/tilebuffer.md` example has bugs:**
  - `joyCheck(JOY1_UP || keyCheck(KEY_UP))` (misplaced parenthesis)
  - `vTAG_VPORT_HEIGHT`
  - `oyClose()`
  - the tile-draw callback is "to be documented"

  `cameraGetYDiff()` returns the X difference (ACE bug), so use
  `cameraGetDeltaY()`.
- **Scroll speed limit:**
  - 8 px/frame gives clean pictures (checked by rendering the level on the host and comparing whole screenshots).
  - **16 px/frame shows wrong tiles** over thousands of pixels on the side
    being scrolled into view.
  - The margin is one tile, and with double buffering each buffer moves twice
    the per-frame delta between its updates.
  - Stay at or below half a tile per frame, or raise
    `ACE_SCROLLBUFFER_X_MARGIN_SIZE` (untested).

## What it costs

Measured with `agk/perf.h` on `a500` (Kick 1.3, 68000 at 7 MHz) with
`tests/perf.agk`, summarised by `tools/perfsum.py`. Numbers are average /
worst % of a 1/50 s frame. Chip RAM is for the scroll bitmap(s), 384 px wide.

| configuration | idle | scrolling 1 px/f | scrolling 2 px/f | buffer chip RAM |
|---|---|---|---|---|
| **this game**: 4 bpp, double-buffered | 11 / 15 | 14 / 23 | 15 / 24 | 2 × 98,688 = 193 KB |
| no status prints (heartbeat off) | 11 / 11 | 14 / 19 | 15 / 20 | same |
| 4 px/frame | 11 / 15 | 14 / 25 | 17 / 25 (at 4 px) | same |
| 8 px/frame | 11 / 15 | 14 / 19 | 20 / 21 (at 8 px) | same |
| single-buffered (`IS_DBLBUF 0`) | 11 / 15 | 14 / 23 | 15 / 24 | 98,688 = 96 KB |
| 5 bpp (32 colours) | 12 / 16 | 15 / 26 | 16 / 27 | 2 × 123,360 = 241 KB |
| 2 bpp (4 colours) | 10 / 14 | 12 / 21 | 13 / 22 | 2 × 49,344 = 96 KB |
| `POT_BITMAP_HEIGHT OFF` (h 514 → 322) | 12 / 15 | 15 / 24 | 15 / 25 | 2 × 61,824 = 121 KB |
| `viewProcessManagers()` instead of the 3 calls | 12 / 16 | 14 / 21 | 15 / 25 | same |
| no scroll processing at all (static screen) | 4 / 8 | – | – | – |

What this means in practice:
- **Scrolling itself is cheap:** +3–4% average over idle, +5–9% in the worst
  frame. The worst frame is the one where the next tile column is due and ACE
  blits the rest of the column at once.
- **The fixed cost is the managers:** 11% idle against 4% without them. That is
  mostly the copper **block** being rebuilt every frame, because the scroll
  manager rewrites its bitplane pointers and BPLCON1. ACE's raw copper mode
  (`TAG_TILEBUFFER_COPLIST_OFFSET_START/_BREAK`) avoids that. Not measured here.
- **Speed costs little:** 2 → 4 → 8 px/frame is 15 → 17 → 20% average.
- **Depth costs little CPU but a lot of RAM:** 1 bpp is about ±1% load and
  about ±25 KB per buffer.
- **The cheapest RAM win is `POT_BITMAP_HEIGHT OFF`:** −74 KB for about +1%.
- **A 3-field `agkState` line costs about 4% of the frame it's printed in.**
  That's why this game prints only when the player stops, plus a heartbeat
  every 50 frames.
- About 80% of the frame is left for the game. Keep `tests/perf.agk`
  (`expect-no-dropped-frames`, `expect-max-load 60`) passing.

## Testing scrolling

A golden image only proves "same as last time". Prove the scroll offset is
exact with `expect-color` on tile edges:
- **Marker tiles with a 1-px edge.** The signs have a white left column. At
  camera *c*, a tile at world *x* must show white at screen *x − c*, sky at
  *x − c − 1* and the board colour at *x − c + 1*. Compute *x − c* in a unit
  test (`worldToScreenX`) so the numbers in the `.agk` file are checked twice.
- **Odd offsets.** 2 px/frame only ever reaches even fine-scroll values. The
  `mid` shot walks 1 extra frame to camera 281 (fine scroll 9).
- **Both screen edges** (x = 0 and x = 319) show real tiles. Garbage from
  undrawn margins shows up there first.
- **Level edges:**
  - the camera clamps at 640, and the last level pixel is on the last screen
    column
  - scrolling all the way back redraws the start view exactly
    (`back_to_start`)
- **While moving** (`tests/moving.agk`): screenshots taken mid-scroll, with
  the 2-frame display lag worked out.
- **Whole-picture check** (how this example was validated): render the level
  on the host exactly as `main.c` draws it, and find the camera offset at which
  each `.screen.png` matches. A clean scroll leaves only the 16×16 player
  sprite mismatched. `agk`'s `Image.load_png()` (stdlib) reads the screenshots.

## Further reading

- ACE:
  - `include/ace/managers/viewport/tilebuffer.h`, `scrollbuffer.h`, `camera.h`
    (the API)
  - `src/ace/managers/viewport/tilebuffer.c` (how margins and the redraw
    state work)
  - `showcase/src/test/scroll_tile_buffer.c` (tile buffer + BOBs, bpp
    switching)
  - `showcase/src/test/buffer_reuse.c` (`tileBufferQueueProcess`, sharing chip
    RAM)
  - `docs/programming/tilebuffer.md` (asset pipeline with Tiled; note the bugs
    above)
- With BOBs on a tile buffer:
  `bobManagerCreate(pScroll->pFront, pScroll->pBack, pScroll->uwBmAvailHeight)`,
  and turn on `ACE_BOB_WRAP_Y` (the default) because the buffer wraps
  vertically. See `techniques/bobs` and the showcase above.
