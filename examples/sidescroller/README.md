# sidescroller: dual-playfield parallax platformer (A500, OCS, PAL)

## The hero

A 32×32, 15-colour hero made from **4 hardware sprites**: two columns of
attached pairs on channels 0–3.
- **Base art:** AI-generated with `agk art-gen hero "..." --size 32x32` for $0.025.
- **Text art:** turned into editable text art with `agk art-export hero`.
- **Animation, by hand** in `art/hero.txt`:
  - idle with breathing
  - a 4-frame walk: the legs are sheared from the hip into strides and passing poses, with a 1-pixel body bob
  - jump with knees tucked and arms up
  - fall
- **Facing left:** `mirror = true` adds the left-facing copies.
- **Frame choice:** `logicHeroFrame()` picks the frame from the game state, and it's unit-tested.
- **Testing:** `tests/hero.agk` checks every animation state on all profiles.

A side-scrolling platformer with two layers of parallax background. It uses
the OCS **dual playfield** mode, set up with a raw copper list, in the style of
Shadow of the Beast and Agony. ACE has no dual-playfield support, so this game
uses no ACE buffer manager. The copper list sets every bitplane register itself.

```
 y   0 ┌──────────────────────────────┐  COLOR00 sky gradient (34 colours, 1 per 4 lines)
     │   PF2 blank                   │
  39 ├──────────────────────────────┤  PF2 = mountains, scroll = camera/4, colours 9-15
     │   mountains (96 lines)        │  PF1 = level (whole screen), scroll = camera
 135 ├──────────────────────────────┤  1-line gap: PF2 blank, hills palette loads
 136 ├──────────────────────────────┤  PF2 = hills, scroll = camera/2, colours 9-15
     │   hills (80 lines)            │  COLOR00 = pale haze gradient behind the hills
 216 ├──────────────────────────────┤  PF2 blank, COLOR00 = pit colour (0x102)
     │   ground (PF1)                │
 255 └──────────────────────────────┘
```

- **PF1 (front)**: the level, 80×16 tiles of 16×16 px, drawn once at startup into a
  1280×256×3 bitmap (`src/logic.c` holds the map, `art/tiles.txt` the tiles).
- **PF2 (back)**: the `mountains` and `hills` PNGs, 3 planes each, 384 px loops
  (704 px bitmaps including the 320 px `wrap_x` copy).
- **Player**: hardware sprite 0, with a 4-frame walk cycle (`art/player.txt`), in front of both playfields.

Controls: left/right to run (2 px/frame). Fire or up jumps (you must release
before jumping again). Stone slabs are one-way platforms: you jump up through
them and land on top. If you fall into a pit, you respawn at the start.

## Register setup

Set at the top of every frame, in the vertical blank, before any WAIT:

| register | value | why |
|---|---|---|
| BPLCON0 | `0x6600` | BPU=6, **DBLPF** (bit 10), COLOR (bit 9). ACE's `viewLoad()` writes `0x6200` from the vport's bpp=6. The copper overrides it every frame. |
| BPLCON2 | `0x0024` | PF2PRI=0 puts PF1 in front of PF2. PF1P=PF2P=4 puts all sprites in front of both playfields. |
| DDFSTRT | `0x30` | One fetch word earlier than the normal 0x38, needed for fine scroll. This costs sprite 7's DMA. |
| DDFSTOP | `0xD0` | The usual lores value. |
| BPL1MOD | `438` | PF1 (odd planes): interleaved 1280 px × 3 planes = 480 bytes/row, minus a 42-byte fetch (40 + 2 for the extra word). |
| BPL2MOD | `-42` | PF2 (even planes) outside the bands: each line re-reads the same 42 zero bytes. |
| BPLCON1 | `pf1Delay \| pf2Delay << 4` | PF1 fine scroll in bits 0-3, PF2 in bits 4-7. Rewritten at every band switch because one register holds both. |
| BPL1/3/5PT | level + `((cam-1)>>4)*2` | Coarse scroll, in words. At cam=0 this is -2 bytes: the extra word is fetched before the window opens, so it never shows. |
| BPL2/4/6PT | blank row | Changed by the copper at each band. |
| COLOR00 | sky[0] | Top of the gradient. The copper changes COLOR00 all the way down, so it must be reset every frame. |
| COLOR01-07 | tiles palette | PF1 colours. Colour 0 is transparent. |

The fine-scroll delay is `(16 - (x & 15)) & 15`. The pointer offset is
`((x - 1) >> 4) * 2` bytes. Both are in `logicScrollDelay()` and
`logicScrollByteOffset()`, and the unit tests check them. The same formula
works for both playfields, and for each band with its own `x`.

In dual-playfield mode, PF2 pixel values 1-7 appear as **colours 9-15**, and 8
is transparent. The plane mapping is bitmap plane 0 → BPL2, plane 1 → BPL4,
plane 2 → BPL6. The `g_pArt*Palette[1..7]` arrays generated for the bands go
into `COLOR09..15` through copper MOVEs. The sprite uses colours 17-19, which
`viewLoad()` loads once from the vport palette.

## Copper list layout (raw mode, 180 of 200 slots used)

```
[0..15]    sprite pointers (ACE sprite manager, raw offset 0)
[16..42]   top of frame: BPLCON0/2, DDFSTRT/STOP, BPL1MOD, BPL2MOD=-42, BPLCON1*,
           PF1 pointers* (6), PF2 -> blank row (6), COLOR00, COLOR01-07
           then in beam order:
WAIT y,0       COLOR00 = sky[y/4]                       y = 4, 8, ... 132
after y=36:    COLOR09-15 = mountains palette            (PF2 still blank: timing doesn't matter)
WAIT 38,0xD8   PF2 pointers* (6), BPLCON1*, BPL2MOD=222  -> mountains from line 39
WAIT 134,0xD8  PF2 -> blank (6), BPL2MOD=-42, COLOR00=haze[0]   -> line 135 is a gap
WAIT 135,0     COLOR09-15 = hills palette                (PF2 blank on this line)
WAIT 135,0xD8  PF2 pointers* (6), BPLCON1*, BPL2MOD=222  -> hills from line 136
WAIT y,0       COLOR00 = haze[i]                         y = 144, 152, ... 208
WAIT 255,0xDF  (line-255 wrap: game y 212 is beam line 256)
WAIT 215,0xD8  BPL2MOD=-42, PF2 -> blank (6)             -> PF2 off from line 216
WAIT 216,0     COLOR00 = pit colour
spare slots    WAIT(0xDF,0xFF), never fires after the wrap (never zeros)
```
(`*` = rewritten every frame. `y` here is the game line. The actual WAIT uses beam line = y + 44.)

Every frame, `copperUpdate()` rewrites 21 value words in the **back** buffer:
three BPLCON1 values and three sets of 6 pointer words. Every value is
recomputed from the camera, so the double-buffered list needs no per-buffer
bookkeeping. `copProcessBlocks()` just swaps the buffers.

Band offsets are `logicBandOffset(cam, shift, ART_*_LOOP_W)`: `(cam >> shift)`
reduced mod 384 by subtraction, with no divide. The fetch never runs past the
704 px bitmap: at offset 383, word 23 plus 21 words ends exactly at pixel 704.
A unit test checks this.

## Gotchas hit

1. **A band switch has to fit in the horizontal blank.** The first version
   switched mountains to hills on consecutive lines: 6 pointer MOVEs, BPLCON1,
   then 7 palette MOVEs and the haze COLOR00, all after `WAIT(135, 0xD8)`. The
   last MOVEs landed about 7 px into the visible part of line 136. The
   screenshot showed old COLOR00 (orange) in x=0..6 of that line, and the last
   hills colours would have been late in the same way. My estimate from colour clocks (0xD8 to the window
   start at about 0x40, 4 clocks per MOVE) was about 18 MOVEs. The measured
   budget is closer to 14, probably because the copper loses slots to the
   6-plane fetch that starts at DDFSTRT 0x30. Treat it as "about 8 MOVEs
   safe, 15 not". The fix is a **one-line gap** at y=135. The mountains move up one
   line to 39..134, which costs nothing because their top rows are
   transparent. PF2 goes to the blank row, and the hills palette loads during
   the gap line, where PF2 can't show it. Every remaining switch is at most
   8 MOVEs.
2. **Write the pointers after the line's last fetch.** WAIT at x=0xD8
   (DDFSTOP 0xD0 + one 8-clock fetch unit). The modulo has already been added
   for that line, so the new pointer applies unchanged to the next line.
   BPL2MOD for the band goes in the same group. The hardware adds the modulo
   at the end of each line, so a changed modulo only affects the following line.
3. **"Blank" PF2 uses a negative modulo**: 42 zero bytes in chip RAM with
   BPL2MOD = -42. The hills bitmap is only 80 rows high. Without the switch at
   line 216, PF2 would keep fetching past its end and show garbage in the pits.
4. **BPLCON1 is shared.** Each band's BPLCON1 MOVE must carry PF1's delay as
   well as the band's own.
5. **COLOR00 is also "transparent" for both playfields**, so under each band it
   shows through PF1 *and* PF2 gaps. Behind the hills the list switches COLOR00
   to a pale haze gradient, because orange dawn sky between trees below a
   snowfield looked wrong. Below 216 it's a dark pit colour.
6. **The line-255 wrap**: the band-off WAIT is at beam line 259. As in the
   copper technique, the writer (`cwWait`) inserts `WAIT(0xDF, 0xFF)` once
   before the first WAIT past line 255.
7. **The display lags the serial log by one frame.** In `jump.agk`, a sync on
   `y=150` shows the sprite at y=153 in the screenshot. The tests sync on
   serial and then assert the lagged (deterministic) pixel.
8. **ACE `viewLoad()`** writes BPLCON0/BPLCON2 without DBLPF, but the copper
   list overrides both from the first frame. There's no scroll or simple
   buffer manager, so nothing else touches the bitplane registers.
9. **Sprite 7 is lost** with DDFSTRT 0x30. Only sprite 0 is used, and no
   garbage appears in any screenshot.

## Measured costs (A500, Kick 1.3, `agk/perf.h`, % of a 1/50 s frame)

| situation | load | maxload |
|---|---|---|
| standing still (logic, sprite, 21 copper words; no state line) | 6% | 7% |
| running and jumping, **no** serial state line | 7% | 9% |
| running and jumping with the 11-value `AGK x=... y=...` line every frame (as shipped) | 23% | **27%** |
| same, BPLCON0 changed to 3 or 4 planes (for comparison) | 21-22% | 24% |

- The copper effect costs almost nothing in CPU time: 21 word writes per frame plus ~180 instructions of copper DMA.
- 6-plane DMA hardly matters here, because the frame's work runs from line 300 through the vertical blank.
- The largest cost is the **serial state line**: about 1.4% of a frame per `agkState()` value. The heartbeat frames (every 50) show the same spike when standing still (22% maxload).
- `tests/perf.agk` runs, jumps pits, uses platforms and falls into a pit. It requires no dropped frames and `expect-max-load 40`.
- Chip RAM: level 120 KB + mountains 24.8 KB + hills 20.6 KB + tiles, copper and sprites ≈ 170 KB.

## Testing

```sh
agk unit    # 15 host tests: jump arc (54 px, 34 frames), one-way landing, falling off an edge,
            # pits, level edges, camera clamp, parallax = cam/4, cam/2 with wrap, scroll registers,
            # smooth gradients
agk test    # 5 scenarios x 3 profiles (a500, a500-ks31, a500-aros), identical pixels on all
```

| scenario | proves |
|---|---|
| `boot.agk` | the sky gradient (4 lines), PF2 transparency showing sky inside the mountains band, PF1 transparency showing the hills (between grass blades), both transparent showing haze, the gap line, the level in front, and the sprite in front |
| `parallax.agk` | the **parallax**: one vertical colour edge per layer (left pixel and right pixel) at camera 0 and camera 160. Mountains edge 66\|67 → 26\|27 (−40 = cam/4), hills 115\|116 → 35\|36 (−80 = cam/2), level slab edge 239\|240 → 79\|80 (−160). The old positions no longer hold the feature. Pit colour below the bands. |
| `jump.agk` | jump in progress (through the one-way slab), landing on the slab (`y=144 ground=1`), walking off its end and falling to the ground |
| `farright.agk` | jumps all three pits and reaches the end: `x=1264 cam=960 mtn=240 hills=96` (hills wrapped: 480 − 384) |
| `perf.agk` | no dropped frames, maxload ≤ 40% |

**Negative check:** making the mountains scroll at cam/8 instead of cam/4
makes `parallax.agk` fail on exactly the mountains pixels (and the `mtn=` serial).

I also checked every screenshot pixel by pixel, except the 92 sprite pixels,
against a reference render composed from the art files, the level map and the
gradient formulas: cameras 0, 52, 160 and 960 (with the hills wrap) all match
exactly. So the band starts (39/136/216), the fine scroll and the palettes
are right to the pixel, not just at the tested points.
