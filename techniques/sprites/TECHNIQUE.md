# Technique: sprites and art from files (the AGK art pipeline)

Draw your graphics as **text files or PNGs** in `art/`. AGK turns them into
Amiga hardware sprites and BOBs, **checks them against the hardware's rules**,
and writes previews that show exactly what the Amiga will display. Your C code
calls the generated functions and never touches bit patterns.

This directory is a complete, tested game:
- The player is a hardware sprite with a 4-frame walk cycle, drawn as text in `art/player.txt`.
- The enemy is a 2-frame BOB drawn from a PNG sheet, `art/enemy.png`.

```sh
agk art techniques/sprites     # convert + previews in build/art/preview/
agk test techniques/sprites    # 12/12 on a500, a500-ks31, a500-aros
```

## Where art comes from

| source | good for | how |
|---|---|---|
| **Text art** (`.txt`) | Anything an agent draws or edits: small sprites, animation frames, precise pixel fixes. It shows in diffs and is exact. | Write it (see below) |
| **PNG from a pixel-art tool** | Human-made art: Aseprite (there are [Amiga palette scripts](https://sphair.itch.io/aseprite-script-plt-export)), GrafX2 | Save RGBA or indexed PNG |
| **PNG from an AI generator** | Quickly drafting enemies, items and tiles | e.g. [Retro Diffusion](https://retrodiffusion.ai/) or [PixelLab](https://www.pixellab.ai/). Ask for the game's palette and size, then let `agk art` enforce the rules |
| **Code** (procedural) | Tiles, particles, gradients | See `techniques/scrolling` |

Whatever the source, the rules below are enforced and the preview shows the result.

## The files

```
art/palette.txt   game palette:   INDEX  0xRGB  name   (depth 3 -> indices 0-7)
art/art.toml      one [table] per asset: source, kind, channel / frame_width
art/player.txt    text art
art/enemy.png     PNG sheet (frames side by side)
```

Text art is one character per pixel, with frames separated by `frame` lines:

```
colors
  .  transparent
  W  0xFFF
  O  0xFA0
  K  0x000
frame
......KKKK......
.....KOOOOK.....
...
frame
...
```

`agk help-art` has the full reference.

## The rules `agk art` enforces

| kind | rules | if broken |
|---|---|---|
| `sprite` | 16 px wide at most; 3 colours + transparent; channels 0/1, 2/3, 4/5 and 6/7 each share one set of colours (slots 17–19, 21–23, 25–27, 29–31) | Too wide is an **error** (use a BOB or two channels). More than 3 colours are **reduced** with a warning, so check the preview. Different colours on a shared pair is an **error** |
| `bob` | Every colour must be in `palette.txt` within the display depth; the width is padded to 16 | Off-palette colours map to the **nearest** entry, with a warning naming it |

PNG colours are rounded to the Amiga's 12-bit colours, and alpha below 128
counts as transparent.

## Using the generated code

`build/art/art.h` has one block per asset:

```c
#include "art.h"

artPaletteApply(s_pVPort->pPalette);        // palette.txt -> colours 0..7
artPlayerApplyColors(s_pVPort->pPalette);   // this sprite's 3 colours -> 17-19

// Sprite: one bitmap per frame (the sprite manager writes control words
// into the bitmap it shows, so frames can't share one)
for(UBYTE i = 0; i < ART_PLAYER_FRAMES; ++i) s_pFrames[i] = artPlayerCreate(i);
s_pPlayer = spriteAdd(ART_PLAYER_CHANNEL, s_pFrames[0]);
spriteSetBitmap(s_pPlayer, s_pFrames[f]);   // animate

// BOB: all frames stacked vertically in one bitmap, plus a matching mask
s_pEnemyBm = artEnemyCreate();
s_pEnemyMask = artEnemyCreateMask();
bobInit(&s_sEnemy, ART_ENEMY_BITMAP_W, ART_ENEMY_H, 1,
    bobCalcFrameAddress(s_pEnemyBm, 0), bobCalcFrameAddress(s_pEnemyMask, 0), x, y);
bobSetFrame(&s_sEnemy,                        // animate
    bobCalcFrameAddress(s_pEnemyBm, f * ART_ENEMY_H),
    bobCalcFrameAddress(s_pEnemyMask, f * ART_ENEMY_H));
```

`agk build` (and `agk run`/`agk test`) regenerate the art automatically. Edit
`art/`, never `build/art/`.

## Workflow for agents

1. Edit `art/*.txt`, or drop in a PNG.
2. Run **`agk art`** and read its output. It prints each asset's size, frames and colours, plus any warning (e.g. *"colour 0xC22 isn't in palette.txt; used the nearest, 0xD22"*).
3. **Look at `build/art/preview/<name>.png`.** It's 4× zoom, with transparency as a grey checkerboard and frames separated by magenta, in exactly the colours the Amiga will show.
4. Then `agk test`, using `expect-color` on pixels that differ between frames to prove the right frame is on screen. See `tests/animation.agk` and `tests/enemy.agk`.

## Gotchas

- **A sprite's colours are per channel pair, not per sprite.** Two sprites on channels 0 and 1 must use the same 3 colours; `agk art` refuses otherwise. Put differently coloured sprites on different pairs (0, 2, 4, 6).
- **Sprite colours go in slots 17–19 (channel 0/1), not 16–18.** Slot 16 is transparent. `artXApplyColors()` does this for you.
- **Sprite frames need separate bitmaps**, because ACE's sprite manager writes position and control words into the bitmap it displays. `artXCreate(frame)` makes one per frame.
- **BOB frames share one bitmap.** Pick a frame with `bobSetFrame` and `bobCalcFrameAddress(bm, frame * H)`.
- **The screen lags the state by a frame or two** (double buffering). Place `expect-color` checks using the position the serial log reported for the frame that's displayed.
- **A PNG's off-palette colours map to the nearest palette entry.** That's fine for drafts; for final art, use the exact palette colours so the warnings go away.

## What it costs

- Converting happens at build time. At run time `artXCreate()` copies data into chip RAM once, at startup, so frame cost is zero.
- Each sprite frame is (H + 2) × 4 bytes of chip RAM. Each BOB frame set is `BITMAP_W/8 × H × frames × (depth + 1 for the mask)` bytes.
- This game (a sprite player plus a double-buffered BOB) runs at about 24% frame load, the same as `techniques/bobs`, as expected.
