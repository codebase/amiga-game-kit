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
   gets the interpreter for it. Self-modifying code is caught as it
   happens: a write that lands in translated code (`code_written`, through
   a 16-byte bitmap in the memory helpers) compares each instruction word
   it changes with the translation. A patched *immediate* is read live
   anyway; a rewritten *opcode* marks the block it is in (from one branch
   target to the next) dirty, the block's label bows out to dispatch, and
   the interpreter runs the block as it now is until the words are restored.
   The address goes to `patched.txt` next to `memory.bin`, and the next
   `agk recomp` gives that instruction a guard (`if (rd16(addr) != opcode)
   goto dispatch`) so the block stays translated. The native build therefore
   never stops at untranslated code and never runs stale code. `RECOMP_INTERP=1` interprets everything: the
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
RECOMP_MOUSE_DIV=N                 host pixels per Amiga mouse count (default 4: a trackpad at 50 fps)
game --play --adf GAME.adf         a window: arrows/WASD + space/ctrl are the joystick, every
                                   key reaches the Amiga keyboard, a click grabs the mouse and
                                   F11 lets it go, F12 toggles full speed, Esc quits
game --frames N --png out.png      headless: run N frames, write the last picture
     --dump chip.bin               ... and the chip RAM at the end
     --wav out.wav                 ... and the sound of the run
     --pngs DIR                    every frame as DIR/fNNNNN.png
     --speed N                     a 68000 N times faster (2, 4, 8, 16, 32), as the game sees it
     --input keys.txt              scripted input: lines of "FRAME right left up down fire click rclick"
                                   (the state from that frame on), plus "mouse DX DY" (moved by that
                                   much), "key HH" (an Amiga keycode tapped), "disk N" (the Nth --adf in)
     --record keys.txt             (with --play) write what was played in that form: replaying it
                                   headless is the same run, frame for frame, so a glitch someone saw
                                   can be reproduced, dumped and diffed without them
     --trace file                  the registers after every frame
     --timeout SEC                 report where the game is if it's still running after SEC
RECOMP_DISK_TRACE=1                say what the drive does (steps, reads)
RECOMP_WORD_TRACE=hex[,hex...]     those words of memory after every frame (a camera angle, a speed)
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
- **Recordings.** `--record keys.txt` with `--play` writes what was played
  as `--input` lines. A replay is the same run frame for frame, so a glitch
  a player saw once can be reproduced headless, traced
  (`RECOMP_WORD_TRACE`, the `--itrace` build with `RECOMP_ITRACE_FROM` and
  `RECOMP_ITRACE_PEEK=PC:ADDR:N`) and bisected (`RECOMP_INTERP_FROM=N`
  hands frame N on to the interpreter; `RECOMP_NO_HOOKS_FROM=N` with
  `RECOMP_NO_HOOK=name` switches hooks off from there, for hooks that
  honour them).
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

The same engine had a second, rarer trick that took a recording of the
player's own session to catch (`--record`, replayed headless frame for
frame): when a matrix coefficient is 0, 1 or a power of two, its code
generator (`$7AA6A`) replaces the whole `muls #coef,dN` with an `ext.l` /
`asl.l` pair. A translation that only re-read the immediate multiplied by
the second word of the new instruction, and for a few frames at a time one
row of the matrix was garbage: the road flipped up into the sky. That is
what the dirty-block detection above is for; with `patched.txt` fed back,
the nine rewritten instructions carry their own opcode checks.

Then the point of it: at the A500's speed the game renders a frame every
13 video frames. `--speed N` makes the 68000 N times faster as the game
sees it, and because the game syncs to the vertical blank it renders more
often: every frame at 16x. But only part of the game's simulation is
time-based: it reads the VBLs elapsed since the last frame (`$29472`,
counted by the VBL handler, consumed at `$1742A` into `$18200` and the
movement step at `$20C00`) for movement, while some steering, drag and
the other car's driving step once per rendered frame, so above about 4x
the world runs away. A real 50 fps needs the per-frame parts scaled by
that delta too, which is what hooks are for.

## Improving a game: hooks

`agk recomp DIR --hook ADDR=file.c:function` compiles `file.c` into the
build and calls `function()` just before the instruction at `ADDR` runs.
The function sees everything the translated code sees (`cpu`, `mem`,
`rd16`/`wr16`, the flag helpers, from `m68k.h`); it returns 0 to let the
original instruction run or 1 to replace it, having set `cpu.pc` to where
execution continues. That's the whole mechanism: a port improves a game by
replacing its routines one address at a time, and the differential tester
checks the rest still matches.

The worked example is `examples/recomp-hooks/robocop3.c`. RoboCop 3's
control routine runs from the VBL interrupt, so the throttle (a countdown of
6 blanks), braking and the drag at full steering lock are already paced by
real time. But the main loop steps other things once per *rendered* frame:
the off-road drag (`$2446C` calls `$2070E` three times), the steering
(`$20208`, the wheel angle into the heading), the pursued car's movement
(`$259BE`), and that car's driver (throttle `$25830`, a quarter-way speed
convergence `$257FA`, a quarter-way turn `$25876`/`$25898`). At 50 fps
those run twelve times too often: the player's car turned like a kart and
the quarry sped off and cornered like a toy. The hooks scale each step by
the frame's share of the A500's 12 VBLs per frame, the game's own timer
(`$18200`), carrying the remainder; the heading ones keep a full-precision
angle because the game rounds to 64ths right after. One more fixes a unit
conversion: a collision with the car swaps speeds between the player's
per-VBL units and the car's per-frame units using the measured VBLs per
frame as the ratio, which the A500 kept near 12 and which is 1 at 50 fps
(the formula even divides by it minus 2); the hook supplies 12. A hook that
scales a step the game already paces by the blank makes things *worse*:
the first version had a "coasting friction" hook on `$205D4`, which is the
brake, and a `frame_sync()` hook at the start of each frame, which burned a
blank the game then waited out again, halving the frame rate to 25. Whether
a step is per frame or per blank is read off the call counts in an
instruction trace (`--itrace` with `RECOMP_ITRACE_FROM`), not guessed.

Finding the per-frame steps took the memory diffs and the write watch
above: compare the variables at a fixed frame across `--speed` settings,
and whatever drifts is per-frame logic. A hook is a hypothesis; the
memory and picture checks, and a replay of a real session, are what
confirm it.

```sh
agk recomp cov -o native --adf robocop3-disk2.adf \
    --hook 2071A=examples/recomp-hooks/robocop3.c:drag \
    --hook 20208=examples/recomp-hooks/robocop3.c:steering \
    --hook 259BE=examples/recomp-hooks/robocop3.c:traffic \
    --hook 25830=examples/recomp-hooks/robocop3.c:car_accel \
    --hook 257FA=examples/recomp-hooks/robocop3.c:car_converge \
    --hook 25876=examples/recomp-hooks/robocop3.c:car_turn_r \
    --hook 25898=examples/recomp-hooks/robocop3.c:car_turn_l \
    --hook 22250=examples/recomp-hooks/robocop3.c:crash_units_a \
    --hook 222A2=examples/recomp-hooks/robocop3.c:crash_units_b
native/game --play --speed 16 --adf disk1.adf --adf disk2.adf --adf disk3.adf --adf-in 2
```

## What's next

- A sharper picture for the 3D games: RoboCop 3 fills its polygons with the
  CPU (the hot span filler at `$7A800`), so a higher-resolution view means
  hooking that filler with a rasteriser fed from the game's vertex lists.
- A cycle model close enough that timing-seeded games replay identically.
- Hooks for improvements: the renderer and the vertical-blank wait are
  known addresses (`agk inspect` found them), so a 60 fps or high-resolution
  build replaces those routines and leaves the game logic alone.
