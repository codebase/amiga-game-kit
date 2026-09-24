# Technique: BOBs (blitter objects)

A **BOB** is a moving image the **blitter** draws into the playfield bitmap,
restoring the background behind it every frame. Use one when a hardware sprite
won't do:

| | hardware sprite | BOB |
|---|---|---|
| width | 16 px (wider = several sprites side by side) | any (a multiple of 16 is cheapest) |
| colours | 3 + transparent (15 when two are attached) | as many as the playfield (2^bpp) |
| count per line | 8 channels | limited by blitter time |
| cost | nearly free (DMA) | blitter time each frame, plus a background restore |
| drawing | on top of or behind the playfield | part of the playfield |

This directory is a complete, tested game. The enemy is a 32×16, 3-colour BOB
patrolling between the walls, and the player is a sprite. Run it:

```sh
agk test techniques/bobs      # 18/18 on a500, a500-ks31, a500-aros
agk run techniques/bobs -s "wait 60" -s "screenshot s"
```

## The recipe (ACE)

All of this is in `src/main.c`.

1. **Display: interleaved and double-buffered.** ACE's bob manager expects both.
   ```c
   s_pBuffer = simpleBufferCreate(0,
       TAG_SIMPLEBUFFER_VPORT, s_pVPort,
       TAG_SIMPLEBUFFER_BITMAP_FLAGS, BMF_CLEAR | BMF_INTERLEAVED,
       TAG_SIMPLEBUFFER_IS_DBLBUF, 1,
       TAG_DONE);
   ```
2. **Draw the static background into both buffers.** Otherwise it flickers every other frame.
3. **Create the bitmaps.**
   - The **frame** bitmap is the BOB's width × height, **the same depth as the display**, interleaved.
   - The **mask** bitmap has the same width, height and depth, also interleaved. Its bits are **repeated in every plane**: a 1 means "draw this pixel", a 0 means "transparent".
   - Why the same depth: the mask shares the frame's source modulo in the blit, so a 1-plane mask would be read wrong. ACE's docs don't say this.
4. **Set up the manager**, once, after the buffer:
   ```c
   bobManagerCreate(s_pBuffer->pFront, s_pBuffer->pBack, s_pBuffer->pBack->Rows);
   bobInit(&s_sEnemy, ENEMY_W, ENEMY_H, 1,           // 1 = is undrawable (restore bg)
       bobCalcFrameAddress(s_pEnemyBm, 0), bobCalcFrameAddress(s_pEnemyMask, 0),
       x, y);
   bobReallocateBuffers();                            // after all bobInit calls
   ```
5. **Every frame, in this order:**
   ```c
   bobBegin(s_pBuffer->pBack);   // restores the background from last time in this buffer
   s_sEnemy.sPos.uwX = x; s_sEnemy.sPos.uwY = y;
   bobPush(&s_sEnemy);           // queue drawing (one call per visible BOB)
   bobEnd();
   viewProcessManagers(s_pView); // REQUIRED with double buffering: shows pBack, swaps
   copProcessBlocks();
   vPortWaitForEnd(s_pVPort);
   ```

## Gotchas

- **ACE's `docs/programming/using_bobs.md` shows `bobBegin()` with no argument.** The real signature is `bobBegin(tBitMap *pBuffer)`, and it has to be the current back buffer.
- **Don't blit into the back buffer between `bobBegin` and `bobEnd`.** Other drawing goes before `bobBegin` (for BOBs to cover) or after `bobEnd`.
- **Without `viewProcessManagers()` a double buffer never swaps.** You'd keep drawing into a buffer nobody sees. The single-buffered template doesn't call it.
- **The picture lags the logic by one frame.** You draw into the back buffer, and it's shown on the next frame. When a test takes a screenshot, the serial log may already report the next position.
- **Build options** change the API:
  - `ACE_BOB_PRISTINE_BUFFER` makes BOBs restore from a clean copy of the background, and `bobManagerCreate()` then takes an extra argument.
  - `ACE_BOB_WRAP_Y` is off by default.
  - Set them in `CMakeLists.txt` before `add_subdirectory(ace)`.
- **Changing a static background:** with double buffering you must change it in *both* buffers, and after `bobEnd`. Otherwise a BOB restore can bring back stale pixels.

## What it costs

Measured with `agk/perf.h` on an A500 (Kick 1.3), average % of a 1/50 s frame:

| what | frame load |
|---|---|
| sprite player, input, logic (single buffer) | ~5% |
| + double buffering (`viewProcessManagers` rebuilds copper blocks) | +6% |
| + one 32×16 3-plane BOB (undraw + draw) | +5% |
| + a 7-field status line (only on frames that print) | +9% on that frame |

What this means in practice:
- The double-buffer swap costs more than the BOB itself. ACE's copper **block** mode regenerates copper lists every frame. Raw copper-list mode (`VIEW_COPLIST_MODE_RAW`, see ACE's `showcase/src/test/copper.c`) avoids that and is the first thing to try when the frame budget gets tight.
- BOB cost grows with **area × depth**. Budget roughly 1–2% of a frame per 32×16 3-plane BOB, and prefer 16-pixel-aligned widths.
- Keep `tests/perf.agk` (`expect-no-dropped-frames`, `expect-max-load`) and read `maxload` after adding BOBs.

## Testing BOBs

A golden image only proves "same as last time". Prove the drawing is correct
with `expect-color` (see `tests/patrol.agk`):
- **body pixels** show the BOB's colours
- **transparent corners** show the background, which proves the mask works
- **points the BOB has left** show the background (no trail)
- **wall pixels the BOB crossed** are restored exactly

## Further reading

- ACE: `third_party/ACE/include/ace/managers/bob.h` (the real API, with doc comments), `docs/programming/using_bobs.md` (concepts; note the `bobBegin` mistake above), `blits_with_mask.md`, `blit_undraw.md`.
- Open-source A500 games that draw many BOBs, as reading material only. They have no licence, so learn from them but don't copy code: the jotd666 arcade ports (e.g. github.com/jotd666/pacman500) and Knightmare (github.com/djh0ffman/KnightmareAmiga). See `docs/references.md`.
