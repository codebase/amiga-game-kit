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
directory are unioned, so sessions add up. What the coverage missed still
runs (the interpreter), just slower, until the next recompile.

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
   the emulator session didn't. Computed jumps (jump tables, `jsr (a0)`)
   into unseen code go to the interpreter, which records the address for
   the next recompile; `--no-follow` turns the following off.
3. **The interpreter.** A 68000 interpreter in the runtime
   (`m68k_interp.c`) executes whatever the translation doesn't have:
   code the game loads from disk later, jump-table targets the emulator
   never took, anything. It runs straight from memory with the same
   registers, flags and helpers as the translated code, one instruction at
   a time, until the pc lands on a translated address, and records where it
   had to step in (`needed.txt`) so the next `agk recomp` translates that
   too. Every dispatch also checks that the opcode word at the address is
   still the one that was translated; a game that loads new code over old
   gets the interpreter for it. The native build therefore never stops at
   untranslated code. `RECOMP_INTERP=1` interprets everything: the
   interpreter and the translator are checked against each other that way
   (identical traces over 1.5 million instructions of RoboCop, interrupts
   included).
4. **Translation.** Each instruction becomes a labelled C statement using
   `recomp/runtime/m68k.h`'s helpers: registers, flags, memory access that
   traps the chips. Branches are `goto`s; calls push the return address on
   the game's own stack; `rts`, `rte`, `jmp (an)` and exceptions go through
   a table from address to label. An immediate operand in an extension
   word is read from the code at run time (games patch constants into
   their instructions); one in the opcode word (moveq, addq, a shift
   count) is a constant. Each instruction is charged a fixed cost as it
   executes; the chips' events are looked at and interrupts taken at
   control flow (before a backward branch, a dbcc, a call).
5. **The runtime** (`recomp/runtime/`): the custom registers, DMACON /
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
6. **The start state** (`start.h`): the registers, the custom and CIA
   registers and the drive position from the dump; `memory.bin` is the RAM.
7. **Compile** with the host `cc`. With SDL2 installed (`brew install
   sdl2`, `apt install libsdl2-dev`) the binary has a window; without it
   it's headless.

The native program:

```
game --play --adf GAME.adf         a window: arrows/WASD + space/ctrl are the joystick, every
                                   key reaches the Amiga keyboard, a click grabs the mouse and
                                   F11 lets it go, F12 toggles full speed, Esc quits
game --frames N --png out.png      headless: run N frames, write the last picture
     --dump chip.bin               ... and the chip RAM at the end
     --wav out.wav                 ... and the sound of the run
     --pngs DIR                    every frame as DIR/fNNNNN.png
     --speed N                     a 68000 N times faster (2, 4, 8, 16, 32), as the game sees it
     --input keys.txt              scripted input: lines of "FRAME right left up down fire",
                                   plus "mouse DX DY" and "click" (the left button)
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
- **Who writes this?** When a variable moves in the emulator and not
  natively, the scenario step `watch ADDR NAME` logs the pc of every CPU
  write to it in the emulator, and `-DRECOMP_WATCH=ADDR` does the same in
  the native build (exact under `RECOMP_INTERP=1`). `--diff --diff-pc`
  compares the two paths rather than the register values, for when a
  counter is legitimately one tick apart. The native start state is taken
  from the snapshot itself (`end.vasnap`), so both sides of a diff begin
  identically; `--no-resnap` keeps the directory's dumps.
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

## The second game: RoboCop 3

Ocean's RoboCop 3 (1992, Digital Image Design) is the opposite kind of
game: a 3D polygon engine, CPU-bound, drawing with the blitter's line mode,
code in slow RAM, driven with the mouse, on three disks. Getting it to run
natively took, in order: the blitter's line mode; a dispatch table for
$C00000; anchoring the disassembly at every executed address (the game pads
its jump-table handlers with `****`, which objdump happily decodes as
instructions, sliding out of step and swallowing the real ones); the native
build recording the addresses it needed in `needed.txt` for the next
`agk recomp`; `move usp` being a long; the mouse and the keyboard; a copper
WAIT for the end of a line taking effect on the next one.

And then the one that took longest: the car drove (position, speed and
heading all evolved exactly as in the emulator) but the view never moved.
RoboCop 3's engine patches its rotation-matrix coefficients straight into
the code every frame (`move.w d6,(17,a0,d3.w)` writing the immediates of
`muls #coef,dN` at `$7A7E0`), so a translation that froze the immediates
rendered the world through a dead camera. Immediates that live in an
extension word are now read from the code at run time (`rd16(addr+2)`
rather than a constant; the opcode word itself is checked at dispatch), and
the view follows the car. Memory-vs-memory diffs, the emulator-side write
watch and `--diff` found it in that order; the lesson for the tooling is
that a divergence of *memory* with identical register streams means the
code bytes themselves changed.

Then the point of it: at the A500's speed the game renders a frame every
13 video frames. `--speed N` makes the 68000 N times faster as the game
sees it, and because the game syncs to the vertical blank it renders more
often: every frame at 16x. But only part of the game's simulation is
time-based: it reads the VBLs elapsed since the last frame (`$29472`,
counted by the VBL handler, consumed at `$1742A` into `$18200` and the
movement step at `$20C00`) for movement, while acceleration and the
traffic step once per rendered frame, so above about 4x the world runs
away (the car covers two to three times the distance at 16x). 4x, about
12 fps, is where it stays honest. A real 50 fps needs the per-frame parts
scaled by that delta too: a hand edit of a few routines, which is what the
tools are for next.

## What's next

- The blitter's line mode.
- A cycle model close enough that timing-seeded games replay identically.
- Hooks for improvements: the renderer and the vertical-blank wait are
  known addresses (`agk inspect` found them), so a 60 fps or high-resolution
  build replaces those routines and leaves the game logic alone.
