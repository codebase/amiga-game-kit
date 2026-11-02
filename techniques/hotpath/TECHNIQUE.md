# Technique: a hot path in assembly, checked against its C version

Write it in C first. Keep the C. When the profiler says one loop is the
frame's problem, write that loop in 68000 assembly, and keep running the C
version against it so the assembly is checked, not trusted.

This directory is a complete, tested game: 128 particles fall, bounce and
drift with the joystick's tilt. Every frame each particle's pixel is erased
and plotted again: that's the hot path. The rules (where the particles go)
are plain C in `src/logic.c`, tested on the host by `agk unit`.

```sh
agk test techniques/hotpath      # 15/15 on a500, a500-ks31, a500-aros
agk profile techniques/hotpath/tests/perf.agk
```

## The recipe

All of this is in `src/main.c`.

1. **The C version, written to be fast C.** No multiply (a row-offset table,
   `s_pRowOffs[y]`), no `%`, 16-bit values, pointers stepped instead of
   indexed:
   ```c
   static void plotC(UBYTE *pPlane, const UWORD *pRowOffs, const tParticle *pP, UWORD *pOld, UWORD uwCount) {
       for(; uwCount--; ++pP, pOld += 2) {
           UWORD x = pOld[0], y = pOld[1];
           pPlane[pRowOffs[y] + (x >> 3)] &= (UBYTE)~(0x80 >> (x & 7));   // erase
           x = (UWORD)(pP->x >> 2); y = (UWORD)(pP->y >> 2);
           pOld[0] = x; pOld[1] = y;
           pPlane[pRowOffs[y] + (x >> 3)] |= (UBYTE)(0x80 >> (x & 7));    // plot
       }
   }
   ```
   `agk lint` confirms it calls no library maths. `agk profile` shows what it costs.
2. **The same loop in assembly**, as GCC inline asm with the pointers in address
   registers and the count in a data register. What the compiler doesn't know:
   - `BCLR`/`BSET` with a register take the bit number modulo 8, so the mask
     `0x80 >> (x & 7)` is just `NOT.W x` and the bit instruction.
   - The row table is read with `(a1,d1.w)`: index by a register, no add.
   - `DBRA` counts the loop; `(a2)+` steps the pointers.
   ```c
   __asm__ volatile(
       "1:\n\t"
       "move.w (%1),%%d0\n\t"  "move.w 2(%1),%%d1\n\t"       // old x, y
       "add.w %%d1,%%d1\n\t"   "move.w (%4,%%d1.w),%%d1\n\t" // row offset
       "move.w %%d0,%%d2\n\t"  "lsr.w #3,%%d2\n\t"  "add.w %%d2,%%d1\n\t"
       "not.w %%d0\n\t"        "bclr %%d0,(%3,%%d1.w)\n\t"   // erase
       ...                                                    // the same for the new x, y, then bset
       "addq.l #8,%0\n\t"      "dbra %2,1b"
       : "+a"(pP), "+a"(pOld), "+d"(uwLast) : "a"(pPlane), "a"(pRowOffs)
       : "d0", "d1", "d2", "cc", "memory");
   ```
   The constraints are the contract: `"+a"` for pointers the loop moves,
   `"d"` for data, every scratch register in the clobber list, and
   `"memory"` because the loop writes through pointers.
3. **Check it, every 100 frames, in the game itself.** The C plotter runs on a
   cleared scratch plane with the same particles and the same "old" pixels; the
   asm plotter runs on the screen. Both results must be identical: every pixel
   on the screen is some particle's old one, and both erase then plot in the
   same order. The blitter compares the two planes (A XOR B with no D output:
   its zero flag, DMACONR bit 13, says whether they were equal), so the CPU
   doesn't scan 10 KB. A mismatch prints `AGK ERR hot: ...`, and `tests/check.agk`
   fails on any `ERR`.
4. **Time both** with the raster line (`getRayPos().bfPosY`) around each call,
   and print the lines: `AGK hot check=1 c_lines=99 asm_lines=63`. The test
   expects the line, and the numbers below come from it.
5. **Keep the frame budget.** The check frame runs both plotters. Its two 10 KB
   blits (clear the scratch plane, compare) run while the CPU does other work:
   the clear during the frame's logic, the compare during the next frame's
   logic, with the result read a frame later. Waiting for them cost 45% of the
   check frame.

## What it costs

Measured on an A500 (Kickstart 1.3), 128 particles, raster lines of a 313-line frame:

| | lines | of a frame | per particle |
|---|---|---|---|
| C plotter (erase + plot) | 96–99 | 31% | ~175 cycles |
| asm plotter | 61–64 | 20% | ~110 cycles |
| the rules (`logicUpdate`: move, clamp, bounce) | | ~27% | |
| a normal frame (rules, asm plot, double-buffer swap) | | 51% | |
| the check frame (+ the C plotter) | | 93% | |

GCC 6's code for this loop was already decent: the gain is 1.6x, from the bit
instructions and the indexed row lookup. Expect that order of improvement from
hand assembly over careful C; expect much more only where C hides a library
call (`agk lint`) or a stack spill (`agk profile --disasm FUNC` shows them as
`sp@(...)` operands with most of the samples).

## Gotchas

- **The screen tears with a single buffer.** The first version plotted into the
  displayed bitmap. The picture was fine to the eye, but `agk test` found that
  AROS (with fast RAM, so a faster CPU) and Kickstart 1.3 gave different
  screenshots: the beam met the plotting at a different line. Double-buffered,
  every profile gives the same pixels. Each buffer remembers where its particles
  were, because the erase clears the pixel from two frames ago in that buffer.
- **`frame % 100` is a library call.** `uint16_t % int` promotes to 32-bit and
  calls `__modsi3`. Count with a variable, or use `& 63`.
- **DMACONR's "BLTNZERO" bit is BZERO.** It reads 1 when the blit's result was
  all zeros. The NDK's name says the opposite. Read DMACONR twice after a blit
  on OCS: the first read can be stale.
- **Keep the C version compiled and called**, not in a comment. Compilers,
  flags and data layouts change; a check that runs every 100 frames catches
  the day the asm is wrong.

## Files

| path | what |
|---|---|
| `src/logic.c`, `src/logic.h` | the particles' rules (host-tested) |
| `src/main.c` | the C and asm plotters, the check, the timing |
| `tests/check.agk` | the check passed and the timing line was printed, on every profile |
| `tests/perf.agk` | no dropped frames; the check frame is the peak |
