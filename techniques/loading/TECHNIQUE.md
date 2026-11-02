# Technique: levels as packed files on the disk

A game bigger than its executable: levels, pictures and tables on the floppy
as packed files, loaded when they're needed. The executable holds no level.

This directory is a complete, tested game: two "levels", each a 40 KB
16-colour picture in the display's own format, packed to 7 KB and 18 KB,
loaded straight into the display bitmap. Fire loads the next one.

```sh
agk test techniques/loading      # 12/12 on a500, a500-ks31, a500-aros
agk help-disk                    # the reference
```

## The recipe

1. **Put the data in `disk/`**, in the form the game wants in memory. Here
   `tools/make_pictures.py` writes `disk/level1.pln` and `level2.pln`: 4
   bitplanes, interleaved, 40960 bytes, exactly what a `BMF_INTERLEAVED`
   simple buffer holds. No conversion at load time.
2. **The build packs it.** Every file in `disk/` becomes `build/disk/NAME.pk`
   (`agk build` prints `disk level1.pln.pk: 40960 -> 7028 bytes`), and
   `tools/build` writes them to the floppy's root. `disk/` counts as source:
   change a file and the next `agk test` rebuilds the ADF.
3. **Load it with one call** (`agk/pack.h`), into the buffer it belongs in:
   ```c
   ULONG ulSize = agkLoadFile("level1.pln.pk", pBitmap->Planes[0], PICTURE_BYTES);
   agkPerfSkip();                       // a load takes longer than a frame; don't count it as dropped frames
   ```
   It opens the file through AmigaDOS (ACE's `diskFileOpen`, which switches
   the OS back on for the read), reads the packed bytes into a temporary
   buffer, unpacks into the destination, and returns the unpacked size, or 0
   after an `AGK ERR` line that says what went wrong (not found, not packed,
   too big for the buffer).
4. **Show something while it loads.** A floppy takes seconds: this game
   clears the screen and writes "LOADING LEVEL 2" with `agk/text.h` first.
5. **Test it.** `tests/boot.agk` expects the picture and the serial line the
   game prints (`AGK loaded level1.pln.pk`, then `size=40960 frames=N`);
   `tests/next.agk` loads level 2 and back. A missing file fails the test
   with the `AGK ERR` line in the report.

## The format

`harness/agk/pack.py` packs, `runtime/src/unpack.c` unpacks: LZSS with a
4 KB window. Tokens are a flag byte for the next 8 items, each a literal byte
or a two-byte match (12 bits of distance, 4 bits of length, 3 to 18). The
unpacker is 40 lines of plain C with no Amiga headers, and the harness runs it
on the host against the packer every time (`harness/tests/test_pack.py`), so
the two can't drift apart. It unpacks at about 1 MB/s on a 7 MHz 68000: the
40 KB picture takes 2 frames; the floppy takes the rest.

What packs: planar pictures with flat areas and dithers (2-6x), tile maps,
tables. What doesn't: sampled sound and noise (keep those raw, as `sound/`
does; a flag bit per byte is all they'd lose).

## What it costs

Loading a level from the floppy, measured in frames (50 a second):

| | packed | Kickstart 1.3 | Kickstart 3.1 | AROS |
|---|---|---|---|---|
| level 1 (40 KB, a planet: flat areas) | 7 KB | 2.1 s | 1.4 s | 0.9 s |
| level 2 (40 KB, layered rock: busier) | 18 KB | 3.0 s | 2.1 s | 2.1 s |

Most of it is AmigaDOS and the drive, not the unpacking: Kickstart 1.3's
filesystem (OFS) reads 488 bytes per block with the head stepping between
them. Packing halves the time because it halves the blocks.

Once loaded, the game idles at 2-3% of a frame (`tests/perf.agk`). Chip RAM:
the temporary buffer for the packed file is freed after the load; the
executable is 154 KB on disk, levels included it would be 234 KB and growing.

## Why not a trackloader

Demos read raw MFM tracks with the disk DMA themselves: no AmigaDOS, no
filesystem, about 10 KB in half a second, and the whole floppy's 880 KB free
for data. It costs you everything AmigaDOS gives: the game only runs from
that floppy (no hard disk, no WHDLoad, no running from the Shell), the OS
can't be switched back on for anything, and the loader is a few hundred lines
of careful assembly per disk format. For a game that's less than a floppy and
loads a level in a few seconds, files are the better trade; the time you save
is better spent on the game.

If you ever need it, keep this layer: the game calls `agkLoadFile(name)`
either way, and the loader behind it can change.

## Gotchas

- **Don't count the load as dropped frames.** `agkPerfBegin` sees the next
  vertical blank seconds after the last one. `agkPerfSkip()` after the load
  tells it the gap was on purpose; without it `expect-no-dropped-frames` fails.
- **AmigaDOS names:** 30 characters, so `disk/` names are 27 at most (plus
  `.pk`); the build refuses longer ones. Case doesn't matter to AmigaDOS.
- **The OS comes back during the read.** ACE's `diskFileOpen(..., 1)` handles
  the switch; don't hold the blitter or depend on your copper list in between.
  Load between frames, not in the middle of drawing one.
- **The destination must be big enough.** The file's header holds the unpacked
  size; `agkLoadFile` checks it against the buffer you give it. The build's
  `disk NAME: raw -> packed` line tells you the raw size to allow for.

## Files

| path | what |
|---|---|
| `disk/level1.pln`, `disk/level2.pln` | the pictures, as the display reads them (`tools/make_pictures.py`) |
| `src/main.c` | loading into the display bitmap, the LOADING text, the timing line |
| `src/logic.c` | which level, and fire asks for the next |
| `tests/next.agk` | level 2 loads and level 1 comes back, on every profile |
