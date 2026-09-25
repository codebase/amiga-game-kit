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
Three mushroom enemies patrol the ground: land on one to squash it, touch one
any other way and you respawn at the start (see
[Enemies & sprite multiplexing](#enemies--sprite-multiplexing)). M turns the
music on and off.

## Sound

`sound/sound.toml` + `sound/theme.mml`, converted by `agk sound` (previews in
`build/sound/preview/`):
- **Music:** "Dawn Run", 16 bars in C major at 140 BPM (27 s loop): lead
  (pulse 25%), bass (square), fast-arpeggio chords (pulse 12%) and drums on
  channel D. 12 KB of MOD, 5 KB of samples.
- **Effects** (synthesized, 16.5 kHz): jump (rising square blip), stomp
  (falling "boing"), hit (buzzing saw slide down), fall (whistle falling into
  a pit, played once the feet drop below the ground line). They take Paula
  channel 3, so the drums drop out for a moment. 24 KB of chip RAM.
- **Frame timing:** ptplayer runs on a CIA-B timer (56 ticks/s at 140 BPM, not
  locked to the frame) and a tick can take ~15-30 raster lines. The frame used
  to start at the very end of the display (line 300), 12 lines before the
  vertical blank; a tick landing there pushed the start past the blank, one
  frame late (about one "dropped" frame every 2 s, even standing still). The
  frame now starts `FRAME_START_LINES` (32) lines earlier, over the dirt where
  no sprite is drawn, and the copper swap waits for the blank so every machine
  switches to the new list on the same frame (otherwise the faster AROS
  profile showed some frames one frame earlier).
- `tests/sound.agk` checks the theme is audible, M gives silence, and the jump
  and fall effects are heard on their own (Paula's output recorded by the
  harness).

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
[0..15]    sprite pointers (ACE sprite manager, raw offset 0); [8..11] = SPR4PT/SPR5PT
           are ours: they point at the enemy sprite chains (see Enemies)
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
9. **Sprite 7 is lost** with DDFSTRT 0x30. Channels 0-3 (hero) and 4-5
   (enemies) are used, 6 is free, and no garbage appears in any screenshot.

## Enemies & sprite multiplexing

Seven 16×16 mushroom critters (`art/enemy.txt`, 15 colours, sharing the
hero's palette 17-31). All of them are drawn with **one attached sprite pair,
channels 4+5**, by vertical sprite multiplexing: each frame the game writes a
sprite DMA chain per channel that shows several sprites, one below the other.

| id | floor | patrol (sprite x) | screen y |
|---|---|---|---|
| 0 | first slab, row 7 | 238..290 | 96 |
| 1 | ground between pit 1 and pit 2 | 398..738 | 192 |
| 2 | low slab, row 10 | 478..546 | 144 |
| 3 | high slab, row 4 | 718..754 | 48 |
| 4 | middle slab, row 7 | 910..962 | 96 |
| 5 | ground after pit 3 | 1038..1264 | 192 |
| 6 | last low slab, row 10 | 1150..1202 | 144 |

### Rules (`src/logic.c`, host-tested)

- **Walk:** 0.5 px/frame (`ENEMY_SPEED_FIX` = 8/16 px, sub-pixel `xFix`). They
  turn at the level ends, at solid tiles, and where the tile under the leading
  foot column is empty (pit or platform edge). They never fall. The level
  never changes, so `logicEnemyPlace()` walks the rule once to get `xMin/xMax`,
  and the per-frame move is a compare, not three tile lookups.
- **Animation:** walk pose A/B by distance walked (`(x >> 2) & 1`, every 4 px);
  base frames face left, `+ART_ENEMY_MIRROR` (3) when walking right; frame 2
  is squashed.
- **Stomp:** the hero is falling (`vy > 0`) and his feet (bottom row) are in
  the top 8 rows of the enemy's box (`STOMP_WINDOW`, the cap). The max fall
  speed is 7 px/frame, so a falling hero can't skip past the window. The enemy
  shows the squashed frame for 25 frames (0.5 s), with no collision, then it's
  gone. The hero bounces with `vy = -72` (a ~25 px hop), or a full jump if
  jump is held.
- **Hit:** any other overlap (side, from below, rising into it, standing
  while it walks into you) respawns the hero at the start like a pit
  (`deaths++`, `hits++`).
- **Respawn keeps the level as it is:** squashed enemies stay gone, the others
  keep walking. No ground enemy patrols the first stretch (x < 352), so the
  respawn point is always safe (a unit test checks the spawn positions).
- Collision boxes: hero columns 3..12 × 16 rows; enemy columns 2..13, rows
  3..15.

### The sprite chain

A hardware sprite channel is a DMA list in chip RAM. After a sprite's last
line the DMA reads two more words: the next sprite's control words, or `0,0`
to end the channel for this frame.

```
chain (per channel)     words
  POS, CTL                2     enemy A (top-most)
  DATA, DATB  x 16       32
  POS, CTL                2     enemy B (starts >= 1 line below A's VSTOP)
  DATA, DATB  x 16       32
  ...
  0, 0                    2     end
```

| word | bits |
|---|---|
| SPRxPOS | 15-8: VSTART bits 7-0 · 7-0: HSTART bits 8-1 |
| SPRxCTL | 15-8: VSTOP bits 7-0 · 7: **ATTACH** (odd channel only) · 2: VSTART bit 8 · 1: VSTOP bit 8 · 0: HSTART bit 0 |

`VSTART = 0x2C + y`, `VSTOP = VSTART + 16`, `HSTART = 0x80 + screen x` (the
same mapping as ACE's `spriteProcess()`). Channel 4 gets part 0 of the art
(colour bits 0-1), channel 5 gets part 1 (bits 2-3) with the same POS and CTL
plus ATTACH, so the pair shows 15 colours from 17-31. `artEnemyCreate(frame,
part)` returns a ready single-sprite bitmap (row 0 = header, rows 1-16 =
`DATA, DATB`); the game keeps those bitmaps and copies their 16 data rows
(one longword per row) into the chains.

**The overlap rule.** A sprite is shown on lines VSTART..VSTOP-1; on line
VSTOP the DMA fetches the next control words, so the next sprite on the channel
must start at VSTOP + 1 or later: `y_next >= y + 17` (`ENEMY_MUX_GAP`).
`logicEnemyPlan()` walks the enemies top to bottom (`pOrderY`, sorted once:
they never change height), keeps those with any column on screen (screen x
-15..319) that aren't gone, and greedily accepts each one that starts at least
17 lines below the last accepted one. On a conflict it alternates: even frames
keep the upper enemy, odd frames the lower one, so both flicker at 25 Hz
instead of one vanishing (`skip=N` in the serial line counts them). The level
is designed so this doesn't happen in play: the four floors are at y = 48, 96,
144, 192 (48 lines apart), and the only same-height pairs (ids 1/5, 0/4,
2/6) are 300+ px apart. Unit tests cover the sort, the alternation, the
exact 17-line boundary, and "shown list is always sorted and 17 apart" for
every layout.

**Double buffering.** The copper list is double-buffered, so the chains are
too: 2 buffers × 2 channels × 240 words (7 enemies × 34 + 2). Copper buffer
A's SPR4PT/SPR5PT MOVEs point at chain A and buffer B's at chain B. They're
written once at startup, and each frame the game fills the chain that belongs
to the **back** copper buffer (told apart by comparing `pBackBfr` with the
pointer saved at startup). The chain being displayed is never touched.

**ACE's sprite manager** (`src/ace/managers/sprite.c`, raw mode):
`spriteManagerCreate()` writes MOVEs with a blank sprite for all 8 channels
into both copper buffers (`spriteDisableInCopRawMode`), and after that only
`spriteProcessChannel(n)` rewrites a channel's two MOVEs, and only for
channels that have a sprite from `spriteAdd()`. So channels 4/5, never added,
are ours: `enemyChainsCreate()` overwrites their MOVE values (list slots
`COP_SPRITES_POS + 8..11`) after `spriteManagerCreate()`.

**Cheap per frame.** A chain slot's 16 data rows are rewritten only when its
frame changes (walk pose every 8 frames, a different enemy moving into the
slot, squashing), tracked per buffer in `s_pChainFrame`. Otherwise only the 4
control words change. The `0,0` terminator overwrites only the next slot's
control words, so that slot's cached rows stay valid.

### Serial state for tests

- The state line gains `stomps=N`, plus `spr=N skip=N` (enemies in this
  frame's chain / left out by overlap) on frames where those change, on the
  heartbeat and in the first frames.
- `AGK stomp id=1 x=443 y=192` when an enemy is squashed (its position), and
  `AGK hit id=1` when an enemy kills the hero, both before that frame's state
  line.
- `AGK t0` at game frame 10: the test sync point (see gotcha 11).

### Costs (A500, Kick 1.3)

| | load | maxload |
|---|---|---|
| running, **no** serial state line, before enemies | 9% | 12% |
| running, **no** serial state line, with enemies (chain of 1-3) | 13% | 15-17% (22% on a stomp frame) |
| standing, with the state line only on heartbeats | 11-12% | 34% (heartbeat) |
| `perf.agk` (whole level, 2 stomps, up to 3 multiplexed), state line every frame | 33-37% | **46%** |

- The enemies cost about 4% of a frame: `logicEnemyPlan()` about 3 raster lines, the chain
  about 2-5 lines (more on frames that copy data rows: 64 longwords per
  enemy), movement and collision about 2 lines.
- `stomps=` adds ~1.3% to every state line, and `spr`/`skip` add ~2.8% on the
  frames that print them.
- Sprite DMA is free for the CPU: dedicated slots, 2 words per line per channel
  on the 16 lines of each enemy. Pointing channels 4/5 back at the blank
  sprite changed nothing in the measurement.
- Chip RAM: chains 1.9 KB, enemy bitmaps 0.9 KB. No copper slots added.
- `perf.agk`'s `expect-max-load` went from 40 to 50. Most of the peak is
  test output (the 14-16 value state line plus event lines on the same frame);
  the game work itself peaks at 17-22%. Frames are never dropped.

### Enemy gotchas

10. **My first version cost +12% of a frame** (35-40% load, 50% max), not the few %
    I estimated. The 68000 is slow at everything: a 7-iteration loop with a sort costs ~1% of a frame.
    Measuring with raster-line marks per phase (a scratch build printing
    `getRayPos()` deltas after `agkPerfEnd()`) showed where the time went. What
    brought it down: patrol bounds computed once, the y-order sorted once, data rows copied only when
    the frame changes, `spr`/`skip` printed only when they change, and event
    lines via `agkState()` (one host transfer instead of seven `agkPrint`s).
11. **Scenarios start at different game frames per profile.** The harness
    applied the first input at game frame 7 on Kickstart 1.3/3.1 and 6 on
    AROS. One early run on a500 was also 6, with a heavier build. That never
    mattered for the hero, who stands still until the first input. The
    enemies walk from frame 0, so a 1-frame offset moves them 0.5 px and the
    goldens differed between profiles. The fix is in the game: it prints `AGK t0` at game frame 10, and
    **every scenario starts with `wait-serial "AGK t0"`**, which resumes at
    frame 11 on every profile. After that all 19 screenshots are identical
    on all three profiles.
12. **Test routes against moving enemies were designed on the host.** A tiny
    scratch replayer (scenario `press`/`wait` lines → `logicUpdate()`,
    10 idle frames first) searched the stomp timings. Each stomp in
    `farright.agk`/`perf.agk` works over a window: the hop before enemy 1
    works for `press right` 12..18 (15 shipped) and the chase of enemy 5 for
    18..38+ (26 shipped). So small timing changes don't break the route.
13. **Existing routes vs enemies:** `farright.agk` used to run straight
    through. Enemy 1 (ground, between pits 1 and 2) killed the hero at frame
    ~209, and enemy 5 stands at the level end (x 1264). The route now stomps
    both. `perf.agk` now runs that route (the whole level, two stomps, up to
    three enemies in the chain) and then runs back left. The old perf route
    never left the first screen. `boot`, `parallax`, `jump`, `hero` and
    `mouse` keep their routes. Enemy 0 shows up on their first screen, so their
    goldens were re-recorded. The `expect-color` points don't touch it (it
    stands on the slab, y 96..111, and the checked slab pixels are at y 112).

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
- These numbers are from before the enemies. With the enemies, see [Costs](#costs-a500-kick-13) above:
  `tests/perf.agk` now runs the whole level with enemies, requires no dropped frames and
  `expect-max-load 60` (measured peak 54% with music: the ptplayer tick adds up to
  ~15% to the frame it lands in; 46% before sound).
- Chip RAM: level 120 KB + mountains 24.8 KB + hills 20.6 KB + tiles, copper and sprites ≈ 170 KB.

## Testing

```sh
agk unit    # 30 host tests: jump arc (54 px, 34 frames), one-way landing, falling off an edge,
            # pits, level edges, camera clamp, parallax = cam/4, cam/2 with wrap, scroll registers,
            # smooth gradients, hero frames; enemies: spawns on floors / away from the start,
            # 0.5 px/frame, turning at slab edges, pits and level ends, walk/mirror/squash frames,
            # stomp + bounce + 25-frame squash, jump-held bounce, the stomp window boundary
            # (feet 7 rows in = stomp, 8 = hit), side hit + respawn, rising into one = hit,
            # squashed = harmless, multiplex plan (sort, culling, alternation, 17-line rule)
agk test    # 11 scenarios x 3 profiles (a500, a500-ks31, a500-aros), identical pixels on all
```

| scenario | proves |
|---|---|
| `boot.agk` | the sky gradient (4 lines), PF2 transparency showing sky inside the mountains band, PF1 transparency showing the hills (between grass blades), both transparent showing haze, the gap line, the level in front, and the sprite in front |
| `parallax.agk` | the **parallax**: one vertical colour edge per layer (left pixel and right pixel) at camera 0 and camera 160. Mountains edge 66\|67 → 26\|27 (−40 = cam/4), hills 115\|116 → 35\|36 (−80 = cam/2), level slab edge 239\|240 → 79\|80 (−160). The old positions no longer hold the feature. Pit colour below the bands. |
| `jump.agk` | jump in progress (through the one-way slab), landing on the slab (`y=144 ground=1`), walking off its end and falling to the ground |
| `farright.agk` | jumps all three pits, stomps enemies 1 and 5, and reaches the end: `x=1264 cam=960 mtn=240 hills=96` (hills wrapped: 480 − 384), `stomps=2`, no deaths |
| `perf.agk` | the whole level (farright route + back left): no dropped frames, maxload ≤ 50% |
| `enemies.agk` | enemy 0 visible (red cap `0xD43`) and walking: 20 px further left 40 frames later, with the old spot showing sky again; after turning at the slab end it's at walk1's x again but in the mirrored frame (the cap's white spot moved from x 266 to 269) |
| `multiplex.agk` | three enemies at y 96 / 144 / 192 on screen in the same frame, all on channels 4+5: `spr=3 skip=0`, a cap colour at each |
| `stomp.agk` | drop onto enemy 1: `AGK stomp id=1`, that frame's line has `vy=-72 ground=0 stomps=1` (bounce), the squashed frame's red rows at y 203-204, gone (hills) 30 frames later, no deaths |
| `hit.agk` | run into enemy 1: `AGK hit id=1`, the next line is back at `x=32 y=192 cam=0` with `deaths=1 stomps=0`, hero drawn at the start |

Every scenario starts with `wait-serial "AGK t0"` (gotcha 11).
**Negative checks:** leaving out the ATTACH bit makes all three `multiplex.agk`
colour checks fail (the caps come out as 0x322), and ending the chain after its
first sprite makes the other two fail.

**Negative check:** making the mountains scroll at cam/8 instead of cam/4
makes `parallax.agk` fail on exactly the mountains pixels (and the `mtn=` serial).

I also checked every screenshot pixel by pixel, except the 92 sprite pixels,
against a reference render composed from the art files, the level map and the
gradient formulas: cameras 0, 52, 160 and 960 (with the hills wrap) all match
exactly. So the band starts (39/136/216), the fine scroll and the palettes
are right to the pixel, not just at the tested points.
