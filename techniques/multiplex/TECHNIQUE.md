# Technique: sprite multiplexing (many bullets from few channels)

The Amiga has 8 hardware sprite channels, each 16 px wide and one sprite per
line. But a channel's data stream can hold any number of sprites one below
another: after a sprite's last line come the next sprite's control words, the
hardware reads them and carries on, until two zero words. So 6 channels show
24 bullets, as long as no more than 6 overlap vertically. No blitter, no
bitplanes: the bullets cost the CPU the writing of the chains only.

This directory is a complete, tested game: the player (a sprite on channel 0,
ACE's sprite manager) dodges 24 bullets raining from channels 2-7.

```sh
agk test techniques/multiplex    # 18/18 on a500, a500-ks31, a500-aros
agk unit techniques/multiplex    # the channel dealing, on the host
```

## The recipe

1. **The hardware's rule.** On one channel, sprite *n+1* must start at least
   one line below sprite *n*'s end (its VSTOP line, where the control words
   for the next one are fetched). `CHAIN_GAP 1` in `logic.h`.
2. **Deal the sprites onto chains** (`logicMultiplex`, portable C): sort them
   by y, then for each in order take the first channel whose last sprite ends
   early enough. Sprites that fit no channel are dropped for that frame and
   counted. The sort is an insertion sort of an order kept between frames:
   bullets move a few lines a frame, so last frame's order is nearly right.
   (Sorting from scratch every frame cost half the game's CPU time: the
   profiler, not a guess, found that.)
3. **Write each chain** into chip RAM (`chainsWrite`): per sprite the two
   control words, then DATA and DATB per line, then two zero words:
   ```c
   UWORD uwVStart = uwTop + pB->y, uwVStop = uwVStart + BULLET_H, uwHStart = uwLeft + pB->x;
   *p++ = (uwVStart << 8) | ((uwHStart >> 1) & 0xFF);
   *p++ = (uwVStop << 8) | (((uwVStart >> 8) & 1) << 2) | (((uwVStop >> 8) & 1) << 1) | (uwHStart & 1);
   // then BULLET_H x (DATA, DATB), copied as longs from the art's sprite bitmap
   ```
   `uwTop` is the view's `ubPosY` (the display's first line, 0x2C) and
   `uwLeft` is `ubPosX - 1`: sprite coordinates are beam positions, not
   playfield pixels. The art pipeline's sprite bitmap has the control words in
   row 0 and the lines after it, so the lines are copied once at startup.
4. **Double-buffer the chains** with the copper lists (one set per copper
   buffer) and point the back list's SPRxPT MOVEs at them: a raw copper list
   (`VIEW_COPLIST_MODE_RAW`) with the sprite manager's 16 pointer slots at
   `COP_SPRITES_POS`. The player sprite on channel 0 goes through ACE's
   `spriteProcess` as usual; channels 2-7's pointers are written by hand.
5. **Colours:** channels come in pairs that share three colour registers
   (2-3: 21-23, 4-5: 25-27, 6-7: 29-31). The bullet art sets 21-23; `main.c`
   copies them to the other pairs, so a bullet looks the same on any channel.
6. **Test the dealing on the host** (`tests/unit/test_logic.c`): every chain
   top to bottom with the gap, every bullet shown or counted as dropped, a
   stack of 7 at one height drops exactly 1 on 6 channels. On the Amiga,
   `tests/rain.agk` expects `shown=24` and no `dropped=` over 6 s.

## What it costs

Measured on an A500 (Kickstart 1.3), 24 bullets of 16x8 on 6 channels:

| what | of a frame |
|---|---|
| writing the chains (24 x 18 words, by the CPU) | ~10% |
| dealing (the nearly-sorted insertion sort, then the chains) | ~9% |
| the rules (24 bullets, the player, hits) | ~3% |
| the whole frame | 28% average, 31% worst |

The same 24 bullets as 16x8 BOBs would cost the blitter about 24 x (erase +
cookie-cut) a frame, roughly 30% of the bus on a 3-plane screen, and chip RAM
for their backgrounds. Sprites cost none of that. The limits are the width
(16 px per channel; attached pairs for 15 colours halve the channel count) and
the vertical crowding: with 6 channels, 7 bullets within 9 lines lose one.

## Gotchas

- **VSTART and VSTOP are beam lines**: add the view's `ubPosY`. A sprite
  placed at playfield y goes 44 lines too high without it.
- **The next sprite's VSTART must be below the previous VSTOP**, strictly. A
  chain that breaks the rule shows the second sprite with the first one's
  data, or nothing, and differently on different chipsets.
- **End the chain with two zero words**, or the channel reads on into
  whatever follows in memory.
- **Partly off-screen sprites** (y near the top or bottom edge) are clipped
  by the display window; this game keeps bullets inside it, so the chains
  never need clipping.
- **Horizontal reuse** (the same channel again on the same line, R-Type's
  repeating backgrounds) needs the copper to rewrite the sprite's position
  mid-line; that's the other sprite trick, and `examples/voidrunner`'s
  starfield is one (a star per line from one channel, by the copper).

## Files

| path | what |
|---|---|
| `src/logic.c`, `src/logic.h` | the bullets, the player, and `logicMultiplex` (host-tested) |
| `src/main.c` | the chains in chip RAM, the copper's sprite pointers |
| `art/bullet.txt` | the bullet, a 16x8 sprite |
| `tests/rain.agk` | 24 shown, none dropped, for 6 s on every profile |
