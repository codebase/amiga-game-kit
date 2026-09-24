# Technique: the copper (per-scanline colour changes)

The **copper** is the display co-processor. It runs a list of instructions in
step with the video beam:
- **WAIT** holds until the beam reaches a given line and position.
- **MOVE** writes a custom-chip register.

If you WAIT for a line and then MOVE into a colour register, the colours change
partway down the screen. That's how a 4-colour screen shows a 58-colour sky, how
a status bar gets its own palette, and how raster bars move. None of it costs
CPU time while the screen is drawn.

| effect | what the copper does | cost |
|---|---|---|
| gradient background | a new COLOR00 every few lines | ~1% of a frame (copper DMA), zero CPU once built |
| palette split (HUD vs playfield) | new COLOR01..n at the split line, old ones again at the top of the frame | a few instructions |
| animated bar, moving gradient | rewrite the MOVE values every frame | CPU per changed instruction (see below) |

This directory is a complete, tested game. It has the template's player sprite
and walls, plus:
- a 24-line HUD band with its own palette
- a split at y=24: COLOR01 and COLOR02 change from HUD gold and brown to wall blue
- a sunset sky with 58 background colours on a 2-bitplane screen
- a 16-line shaded raster bar bouncing through the sky, crossing beam line 255

Run it:

```sh
agk test techniques/copper     # 12/12 on a500, a500-ks31, a500-aros
agk run techniques/copper -s "wait 100" -s "screenshot s"
```

## The recipe (ACE)

All of this is in `src/main.c`. `#define COPPER_RAW 1` (the default) builds the
list in **raw mode**. `0` builds the same picture in **block mode**. Both
produce identical pixels inside the 320×256 game area. Pick raw mode for
anything that changes every frame (see *What it costs*).

### Coordinates

- Copper WAITs use **beam lines**, not game y: beam line = game y + `pView->ubPosY` (0x2C = 44 on PAL). Game y 0..255 is beam line 44..299.
- `copSetWait()`/`copBlockCreate()` X is the horizontal beam position in colour clocks. **X = 0** fires during the horizontal blank, so the whole line gets the new colour.
- The `ubPosY` field is set by `viewCreate()`. Read it from there rather than hard-coding 44.

### Raw mode (fast; you manage the list)

1. **Size the list and create the view in raw mode.** Count every instruction:
   - the sprite manager's 16 MOVEs (2 per channel, all 8 channels)
   - `simpleBufferGetRawCopperlistInstructionCount(bpp)`, which is `6 + 2*bpp`
   - your own instructions

   ACE appends the final `WAIT(0xFF,0xFF)` itself.
   ```c
   s_pView = viewCreate(0,
       TAG_VIEW_GLOBAL_PALETTE, 1,
       TAG_VIEW_COPLIST_MODE, VIEW_COPLIST_MODE_RAW,
       TAG_VIEW_COPLIST_RAW_COUNT, COP_RAW_COUNT,
       TAG_DONE);
   ```
2. **Give each manager its offset in the list.** List order must follow beam order. The sprite pointers have no WAIT, so they go first, in the vertical blank. The simple buffer's own WAIT is at beam line `ubPosY - 1`, so your per-line slots go after it.
   ```c
   s_pBuffer = simpleBufferCreate(0, TAG_SIMPLEBUFFER_VPORT, s_pVPort,
       TAG_SIMPLEBUFFER_BITMAP_FLAGS, BMF_CLEAR,
       TAG_SIMPLEBUFFER_COPLIST_OFFSET, COP_SIMPLEBUFFER_POS,  // 19 here
       TAG_DONE);
   spriteManagerCreate(s_pView, COP_SPRITES_POS, 0);          // 0 here
   ```
3. **Write your instructions into both buffers once**, at startup. The list is double-buffered: `pCopList->pBackBfr` and `pCopList->pFrontBfr`, each a `tCopBfr` with `tCopCmd *pList`.
   ```c
   void copSetWait(tCopWaitCmd *pWaitCmd, UBYTE ubX, UBYTE ubY);    // ubY = beam line & 0xFF
   void copSetMove(tCopMoveCmd *pMoveCmd, volatile void *pReg, UWORD uwValue);
   static inline void copSetMoveVal(tCopMoveCmd *pMoveCmd, UWORD uwValue); // value only

   copSetMove(&pList[i++].sMove, &g_pCustom->color[0], HUD_COLOR0);  // top of frame
   ...
   if(uwBeamY > 0xFF && !isPast255) {             // once, before line 256
       copSetWait(&pList[i++].sWait, 0xDF, 0xFF);
       isPast255 = 1;
   }
   copSetWait(&pList[i++].sWait, 0, uwBeamY & 0xFF);
   copSetMove(&pList[i++].sMove, &g_pCustom->color[0], uwColor);
   ```
   Keep a pointer to each MOVE's value word: `(UWORD *)&pList[idx] + 1`.
4. **Every frame, edit only the back buffer**, and only what changed. Then call `copProcessBlocks()`, which in raw mode just swaps the buffers:
   ```c
   UBYTE ubBfr = (pCopList->pBackBfr == s_pCopBfrA) ? 0 : 1; // which one is back?
   if(s_pBarInBfr[ubBfr] != wBarY) {
       paintBar(s_pLineVal[ubBfr], s_pBarInBfr[ubBfr], wBarY); // restore old lines, draw new
       s_pBarInBfr[ubBfr] = wBarY;
   }
   ...
   copProcessBlocks();
   vPortWaitForEnd(s_pVPort);
   ```

### Block mode (simple; ACE manages the list)

```c
tCopBlock *copBlockCreate(tCopList *pCopList, UWORD uwMaxCmds, UWORD uwWaitX, UWORD uwWaitY);
void copMove(tCopList *pCopList, tCopBlock *pBlock, volatile void *pReg, UWORD uwValue);
void copBlockWait(tCopList *pCopList, tCopBlock *pBlock, UWORD uwX, UWORD uwY);

tCopBlock *pHud = copBlockCreate(pList, 3, 0, 0);             // WAIT 0,0 = vblank
copMove(pList, pHud, &g_pCustom->color[0], HUD_COLOR0);
...
tCopBlock *pLine = copBlockCreate(pList, 1, 0, beamY);         // full beam line, ACE wraps >255
copMove(pList, pLine, &g_pCustom->color[0], uwColor);
```

To change a value later without appending, write it in place and tell ACE:
```c
pLine->pCmds[0].sMove.bfValue = uwNewColor;    // or through a saved UWORD*
s_pView->pCopList->ubStatus |= STATUS_UPDATE;  // otherwise ACE never re-merges
```
`copProcessBlocks()` then merges all blocks into the back buffer. Because of
the double buffering, it does this for two frames after each change.

## Gotchas

- **ACE has no copper guide.** `docs/programming/` has nothing on the copper. The real sources are `include/ace/managers/copper.h`, `src/ace/managers/copper.c` and `showcase/src/test/copper.c`. The showcase covers both modes but has no sprites and never shows how to mark a value-only edit as dirty (see below).
- **ACE loads the palette once, in `viewLoad()`, by CPU.** A colour the copper changes stays changed. At the bottom of the frame COLOR00 still holds the last sky colour. So the copper list must set the HUD colours again **at the top of every frame**: raw MOVEs before the first WAIT, or a block at WAIT 0,0.
- **WAIT has only 8 bits of Y.** Lines 256+ (game y ≥ 212 on PAL) need one `WAIT(0xDF, 0xFF)`, then WAITs with `y & 0xFF`. In raw mode that's your job; forget it and every WAIT after line 255 fires at once. Block mode inserts it for you, but:
- **Stock ACE put a block at exactly beam line 255 one line late.**
  - `copUpdateFromBlocks()` emitted only the `WAIT(0xDF,0xFF)` wrap guard for Y == 0xFF, so the MOVEs landed at the end of line 255, and game line 211 showed line 210's colour.
  - `tests/copper.agk` caught it when the block-mode and raw-mode screenshots differed.
  - **AGK's ACE patch** (`patches/ace-agk.patch`) now emits `WAIT(x, 0xFF)` for that line. It was verified both ways: block mode fails the line-211 check without the patch and passes with it.
  - With stock ACE, create that block at `x=0xDF, y=254` instead.
- **Raw lists are double-buffered, so the back buffer is two frames old.** Anything you change incrementally must be tracked per buffer. Here that's `s_pBarInBfr[2]`, told apart by comparing `pBackBfr` with a pointer saved at startup. If you track it once, the bar leaves stale lines behind every other frame. Write static instructions into **both** buffers at startup; the sprite manager and simple buffer do the same.
- **Block mode, value-only edits:** writing `pCmds[i].sMove.bfValue` directly sets no dirty flag. The showcase only works because it also calls `copBlockWait()`, which sets the flag. Set `pCopList->ubStatus |= STATUS_UPDATE` yourself.
- **Block mode rebuilds the whole list when anything changes.** The partial-update path in `copUpdateFromBlocks()` is commented out as "broken". Cost scales with the **total** number of blocks, not the changed ones: about 0.35% of a frame per block per frame here. Static blocks are free once merged.
- **Raw list order must be beam order.** ACE's sprite MOVEs have no WAIT, so they must come first. The simple buffer WAITs at `ubPosY - 1`. Put your top-of-frame MOVEs between the two, and your line slots after both. `TAG_VIEW_COPLIST_RAW_COUNT` must cover everything. Fill spare slots with a harmless WAIT, never leave zeros. A zero is a MOVE to register 0 (BLTDDAT), which is copper-protected, and the copper stops there for the rest of the frame. ACE's raw buffers are allocated cleared.
- **Don't call `logic.c` once per line in the frame loop.** A cross-file call per line costs about 300 cycles on the 68000. Rewriting 232 lines that way cost 41% of a frame. Build colour tables at startup from `logic.c` and copy words per frame.
- **COLOR00 is also the border colour, and colour 0 is "background".** The sky and bar fill the overscan border, and the bar passes behind walls and sprites. To put a bar in front, change a colour the walls use, or use more bitplanes.
- **The picture lags one frame.** The list you edit in frame N is displayed from frame N+1. Tests compute expected colours from `logicBarY(frame)` and pick the frame shown in the screenshot.
- **Debugging:** `copDumpBfr()` and `copDumpBlocks()` print nothing unless `ACE_DEBUG` is on. In the emulator, `regs NAME copper` shows what the copper actually ran.

## What it costs

Measured with `agk/perf.h` on an A500 (Kick 1.3), as the average % of a 1/50 s
frame. The scenario idles for 200 frames, then moves the player for 200. The
picture on screen is identical in every row except the template.

| what | idle | moving |
|---|---|---|
| template (sprite player, input, logic) | 4% | 10% |
| + static copper list: HUD split + 232-line gradient (~500 instructions), **raw or block** | 5% | 12% |
| + animated bar, **raw**, only changed lines (32 word writes/frame) — *shipped* | 7% | 15% |
| same, raw, rewrite all 232 lines from a table | 12% | 19% |
| same, raw, rewrite all 232 lines calling `logicLineColor()` per line | 46% | 55% |
| same, **block mode**, one block per line (233 blocks re-merged every frame) | 86% | 93%, **drops frames** |

What this means in practice:
- **A static copper effect is nearly free** in either mode: about 1% for 500 instructions of copper DMA. Use block mode for splits and gradients that never change; it's simpler.
- **For anything animated, use raw mode and write only what changed.** In block mode every change re-merges every block. With 233 blocks that's the whole frame. With the handful of blocks that `viewProcessManagers` and double-buffering create, it's the ~6% the BOB technique measured.
- Budget roughly **0.03% of a frame per rewritten raw instruction** (table copy) and **0.35% per block** in a block list that changes every frame.
- `tests/perf.agk` caps `expect-max-load` at 30. The shipped raw version peaks at 15–16%. Block mode fails that test, which is intended.

## Testing copper effects

Goldens only prove "same as last time". `tests/copper.agk` proves the effects
with `expect-color`. Column x=8 has no walls or sprite, so it shows COLOR00:
- **gradient**: six lines in different bands have the colours `logicBuildSkyBands()` gives, including game y 210/212 on either side of the line-255 wrap
- **palette split**: the same register shows different colours above and below y=24. COLOR00 is black vs sky, COLOR01 is HUD fill 0xFC0 vs wall fill 0x468, COLOR02 is 0xA50 vs 0x9BD
- **animation**: the bar's centre colour 0xEFF is at y=56 in `early` and at y=205 in `late`. y=56 shows sky again in `late`
- **the wrap**: the bar's lines 210–213 each have their own colour across beam line 255/256, with none a line late
- **per-frame reset**: the HUD colours are still right after 100 frames of copper changes

Unit tests in `tests/unit/test_logic.c` cover:
- all 58 sky bands are distinct and smooth (one channel step between neighbours)
- the bar is symmetric, stays in the sky, moves at most 2 lines per frame, and repeats every 256 frames
- `logicLineColor()` layers the bar over the bands

To compare modes yourself:
1. Copy the project and set `#define COPPER_RAW 0`.
2. Run `agk test`. `copper`, `boot` and `walls` pass: the game area is identical, and only the `late` golden differs, by the 12 border pixels above. `perf` fails, which is the point.

## Further reading

- ACE: `include/ace/managers/copper.h` (API), `src/ace/managers/copper.c` (merge, 255 wrap), `showcase/src/test/copper.c` (both modes), `src/ace/managers/viewport/simplebuffer.c` and `src/ace/managers/sprite.c` (their raw-mode offsets).
- Hardware: the *Amiga Hardware Reference Manual*, chapter 2 "Coprocessor Hardware" (WAIT/MOVE/SKIP, the line-255 wrap).
