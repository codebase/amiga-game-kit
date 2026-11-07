# Recompiling a game to run natively

`agk recomp` turns a 68000 game that `agk inspect` has watched into a native
program: the game's code becomes C, and a small runtime stands in for the
chips. The result runs on the Mac (or Linux) in a window, with the keyboard
as the joystick, from the user's own disk image. It is the first step of the
road to a port that runs faster than the Amiga did and can be improved: once
the game is C, a routine can be rewritten by hand, the renderer can be
replaced, the frame rate can be raised.

The repo ships the translator and the runtime only. Every byte of the game
comes from the disk image the user supplies, the same way Doom source ports
need the user's WAD. Nothing generated from a game (the C, the memory image,
the coverage) belongs in the repo.

The worked example is Ocean's **RoboCop** (1989): its first level runs
natively, pixel-identical to vAmiga for the first thousand frames, and the
whole loop (game over, the title, the level reloaded from disk) works.

## 1. Watch the game

```sh
agk inspect robocop.adf --from game.vasnap -f play.agk --coverage -o start
```

`--coverage` records every instruction address the CPU executes (a bit per
address, so a long play session costs nothing), and at the end of the steps
dumps what a native build needs:

| file | what |
|---|---|
| `chip.bin`, `slow.bin` | the 512 KB of chip RAM and the slow RAM at $C00000 |
| `coverage.bin` | every instruction address executed |
| `cpu.txt` | D0-D7, A0-A7, PC, SR, USP, SSP |
| `chip.regs.txt` | the display, DMA and interrupt registers, the copper and bitplane pointers, both CIAs, the drive's head position |
| `end.vasnap` | the snapshot the dump was taken at (add `snapshot end` to the steps) |

Take it at a point where the game has the machine to itself (a title or a
level: 0% in the ROM in `--sample`'s report) and play as much of the game as
you can under the emulator first: the more the coverage saw, the less the
translator has to guess. Several coverage files (`coverage*.bin`) in the
directory are unioned, so sessions add up.

## 2. Recompile

```sh
agk recomp start -o native --adf robocop.adf
native/game --play --adf robocop.adf
```

What happens:

1. **Disassembly from coverage.** objdump (in the kit's Docker toolchain)
   disassembles the covered address ranges. Coverage says exactly where
   instructions start, so data mixed into the code is never mistaken for
   code.
2. **Following branches.** Constant branch and call targets that the
   coverage never reached are disassembled too, from each target until a
   known instruction, an unconditional jump or data, repeatedly until nothing
   new turns up. This is how the game survives the player doing something
   the emulator session didn't. Only computed jumps (jump tables, `jsr (a0)`)
   into unseen code still stop the program, with a message saying which
   address; `--no-follow` turns the following off.
3. **Translation.** Each instruction becomes a labelled C statement using
   `recomp/runtime/m68k.h`'s helpers: registers, flags, memory access that
   traps the chips. Branches are `goto`s; calls push the return address on
   the game's own stack; `rts`, `rte`, `jmp (an)` and exceptions go through
   a table from address to label. Interrupts are checked at backward
   branches and calls, as the timing model (a fixed cost per instruction)
   ticks past the vertical blank or a CIA timer.
4. **The runtime** (`recomp/runtime/`): the custom registers, DMACON /
   INTENA / INTREQ, the copper interpreted line by line at the end of each
   frame, the bitplanes fetched as the registers say (DDF, modulos, fine
   scroll, dual playfields), sprites, a software blitter (area mode with
   masks, shifts, minterms, descending and fill), both CIAs with their
   timers, the index pulse and the joystick, and a drive: the ADF is
   MFM-encoded on the fly, the disk turns (reads start where the head is,
   at the next sync word), DSKBLK arrives when the DMA would have finished.
   Loaders that step the head, wait for the index and sort sectors work as
   they do on the machine. Paula: the four DMA channels from chip RAM at
   their periods and volumes, the reload-and-interrupt at the end of a
   sample that music drivers rely on, the A500's output filter and the LED
   filter, mixed to stereo (`--wav` records it headless).
5. **The start state** (`start.h`): the registers, the custom and CIA
   registers and the drive position from the dump; `memory.bin` is the RAM.
6. **Compile** with the host `cc`. With SDL2 installed (`brew install
   sdl2`, `apt install libsdl2-dev`) the binary has a window; without it
   it's headless.

The native program:

```
game --play --adf GAME.adf         a window: arrows/WASD + space/ctrl are the joystick,
                                   F toggles full speed, Esc quits
game --frames N --png out.png      headless: run N frames, write the last picture
     --dump chip.bin               ... and the chip RAM at the end
     --wav out.wav                 ... and the sound of the run
     --input keys.txt              scripted input: lines of "FRAME right left up down fire"
     --trace file                  the registers after every frame
     --timeout SEC                 report where the game is if it's still running after SEC
RECOMP_DISK_TRACE=1                say what the drive does (steps, reads)
```

## 3. Check it against the emulator

Determinism is the product here too. Three comparisons, cheapest first:

- **Pictures.** `game --frames 300 --png n300.png` against a `screenshot`
  at the same frame in `agk inspect --from start/end.vasnap`. RoboCop's
  level 1 matches pixel for pixel at frames 300 and 1000.
- **Instruction streams.** `agk recomp start --diff 400000 --adf robocop.adf`
  traces 400,000 instructions in vAmiga (from `end.vasnap`) and in a
  traced native build, and prints the first instruction where the registers
  differ, with context. Interrupts land at different instructions because
  the native timing is approximate, so the main program (interrupt level 0)
  is compared and the handlers' instructions are skipped. RoboCop's first
  divergence is a frame counter the VBL handler had incremented once more
  in one of the two: a timing artefact, not a translation bug.
- **Replays.** The same inputs in both: `--diff N --input keys.txt` plays
  the keys file in the emulator and natively (or, by hand, a scenario of
  `hold`/`release`/`wait` lines against `--input`, with `dump-chip` and
  `--dump` at the same frame). Snapshots taken mid-play (`snapshot end` in
  the steps) move the start of the comparison to just before the trouble. Where the game's behaviour
  depends on timing (RoboCop seeds its enemies from it), the two runs drift
  within a few hundred frames; the data a loader brought in still has to be
  identical, which is how the disk model's bugs were found.

## What the RoboCop port found, in order

Every one of these was a runtime bug the differential checks caught, and
each is the kind of thing the next game will hit too:

- the copper's bitplane pointers were reloaded on every MOVE, not only on
  MOVEs to BPLxPT (the picture repeated down the screen);
- the drive's status lines (`/RDY`, `/TK0`) are read through CIA-A, the
  joystick's fire button is bit 7 (port 2), not 6;
- the motor latches `/MTR` on the falling edge of `/SEL`;
- the loader waits for the index pulse (CIA-B FLAG) before seeking;
- the disk turns: a read starts where the head is, so the loader's "wait for
  the sector before the gap, then read the track" trick works;
- the head position at the snapshot matters: the loader seeks relative to it;
- DSKBLK must arrive after the loader has cleared INTREQ, not before;
- a 32-bit read of a CIA register (RoboCop seeds its randomness with
  `move.l` from CIA-B's TOD) must reach the CIA, and CIA-B's TOD counts
  raster lines, not frames: with either wrong the enemies stop coming;
- objdump wraps instructions longer than three words onto a second line, so
  their length (and the next instruction's address) was 2 bytes short;
- the fall-through of the last covered instruction of a range is code too:
  without it a `beq` fell straight into the next label in the file.

## What's next

- The blitter's line mode.
- A cycle model close enough that timing-seeded games replay identically.
- Hooks for improvements: the renderer and the vertical-blank wait are
  known addresses (`agk inspect` found them), so a 60 fps or high-resolution
  build replaces those routines and leaves the game logic alone.
