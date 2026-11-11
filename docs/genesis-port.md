# Porting a Mega Drive game to the Amiga

The Mega Drive and the Amiga share a CPU. A Mega Drive game's 68000 code can
run on an Amiga as it is; what can't come along is the machine around it: the
VDP's tile planes and sprites, the FM chip, and the memory map. This is the
kit's pipeline for bringing a game across, built on Sonic the Hedgehog 2 as
the hard case. You bring the ROM; the kit holds only tools.

Status: stages 1 to 3 work (the game's code, rewritten for the Amiga's memory
map, runs frame for frame against the original on the host), and stage 4 has
a hardware layer: `agk port DIR --amiga` builds a bootable floppy, and Sonic 2
boots on an A1200 with fast RAM and plays Emerald Hill with the planes as an
AGA dual playfield the copper scrolls, 15 colours a playfield, and its music
and effects on Paula's four channels. It needs an accelerated A1200 to hold
its speed; a stock one is the work ahead, an A500 version a reduced art pass
after that.

Thunder Force IV is the second game through it: 7997 of 8000 frames match
the original on the host, and on the Amiga it shows its intro, title and
stage select and plays its first stage with its status bar (the window
plane), without sound yet (its sound driver isn't Sonic 2's).

## The stages

```
ROM ──1──> analysis run ──2──> agk port ──3──> checked on the host ──4──> Amiga
          (the original,       (the code        (the port, with the      (the port, with
           instrumented)        rewritten)       Mega Drive's chips       the Amiga's
                                                 behind the layer)        behind it)
```

### 1. The analysis run: the original, instrumented

```sh
agk recomp --machine md --no-translate --rom "Sonic 2.md" -o s2   # a Mega Drive runner for the host
s2/game --play --analysis s2                         # play it; every run adds to the record
s2/game --frames 4000 --input keys.txt --analysis s2 # or scripted
```

The runner is a Mega Drive for the host (VDP, pads, the Z80 with the game's
sound driver; the sound chips are logged, not heard), there to measure the
game, not to be the product. `--analysis DIR` runs
everything through the interpreter, where the pc is exact, and records:

| file | what |
|---|---|
| `coverage.bin` | every instruction executed |
| `entries.bin` | where every computed `jmp`/`jsr` went (jump tables, object routines) |
| `romread.bin` | every byte of the ROM that was read; minus the executed instructions, that is the game's *data*, and no disassembly may claim it as code |
| `hwsites.txt` | every instruction that touched the hardware: pc, address, read or write, width |

The more of the game is played, the more of it the port has. Arrows/WASD are
the d-pad, Z/X/C the A/B/C buttons, Return is Start.

```sh
s2/game --frames 4000 --input keys.txt --sound-rip s2   # the game's sound, for stage 4
```

`--sound-rip DIR` is the analysis of the sound. After the run, the Z80
driver as it stood before the game's first request is given every request
the game could make (each value 1..255 at each address of the Z80's RAM the
game wrote requests to), alone, and what it tells the YM2612 and the PSG is
logged (`sound.rip`), a second time over music that is playing (what a
request does to the music says what it is). `z80rom.bin` records the ROM
bytes the driver read. It takes a minute or two.

### 2. `agk port`: the code rewritten

```sh
agk port s2 -o s2/port
```

Where things are is the problem. The work RAM is at `$FF0000`, reached mostly
through sign-extended 16-bit addresses; the hardware is at `$A00000` and
`$C00000`; on an Amiga those are the Kickstart ROM and expansion space. So
every instruction that names them is rewritten. The port's memory:

| where | what |
|---|---|
| `0 .. romsize` | the ROM at its own addresses. Data never moves, so every pointer and table in it stays valid. The old *code* in it is dead: each address a table or a vector can still send the CPU to holds a jump to the new code, and the rest is poisoned (`ILLEGAL`) |
| `ram` | the game's 64K, at a multiple of `$10000`, so a pointer's low word is what it was (games keep pointers as words) |
| `ram + $10000` = D | a copy of the ROM's first 32K. A word pointer sign-extended into an address register gets D added: `$8000..$FFFF` lands in RAM, `$0000..$7FFF` here |
| `shim` | the hardware layer's page: a cell, and the entry points the rewritten code calls |
| `code` | the rewritten code |

The rules, all plain 68000 and none disturbing the condition codes:

- **RAM operands** (`$FFxxxx`, however written) become `ram + xxxx`.
- **A hardware operand** makes the instruction work on the layer's cell, with a
  call before it (a read) or after it (a write) that moves the cell from or to
  the hardware address: `move.w d0,(a6)` with a6 at the VDP's data port
  becomes `move.w d0,cell; pea (a6); jsr w16; addq.l #4,sp`. Which instructions
  those are comes from `hwsites.txt`, from absolute operands, and from
  following the registers: where a hardware address is loaded into an address
  register (`lea $C00000,a6`) it is traced through the branches, into the
  routines called with it and back out, so code the analysis run never
  executed (a counter that didn't change, a level it didn't reach) is
  rewritten too. Sonic 2: 310 such instructions beyond the 373 the run saw.
- **`x(pc)` data** becomes the absolute address (the data stayed put);
  **`x(pc,dn)`** goes through a scratch address register saved on the stack.
- **`movea.w ea,An`** is followed by `adda.l #D,An`; `cmpa.w` is bracketed by it.
- **Table jumps** (`jmp T(pc,d0.w)`) compute the old address and go there: its
  jump takes them on to the new code.

What is code is decided conservatively: what ran, what its branches reach,
and jump-table entries, but only where the whole path from them decodes as
68000 code and doesn't overlap bytes the game read as data.

`port.txt` in the analysis directory holds hints for what the rules can't
know: `entry HEX`, `data HEX`, `raw HEX` (leave the instruction alone),
`pre`/`post HEX asm`, `replace HEX asm; asm`, `reloc HEX` (a RAM pointer in
the ROM's data), `fixa HEX aN` (after this instruction aN may hold a Mega
Drive RAM address: bring it over). The last kind is written for you:

### 3. Checked on the host

`agk port` also builds `s2/port/game`: the ported image run with the Mega
Drive's chips *behind the hardware layer*. Nothing else of the Mega Drive
exists there, so:

- the same input must give the same frames as the original, pixel for pixel
  (`game --input keys.txt --frames N --pngs DIR` in both, then compare);
- any access to the Mega Drive's own addresses is reported with the pc, and
  with the instruction that loaded the offending pointer
  (`RECOMP_PORT_FAULTS=s2/port/faults.txt`); `agk port s2 -o s2/port --learn`
  turns that report into `fixa` hints and rebuilds. A few rounds converge;
- entering old code anywhere but a known entry stops the run and says which
  `entry` line to add.

Only the game's own instructions cost time in this build (the glue the port
adds is free), so it stays in step with the original and the frames can be
compared one to one.
The chips' events are looked at where the original's control flow
was, not at the port's own calls, so a loop that polls the vertical blank
leaves on the same turn as the original's (a game that seeds its random
numbers from the HV counter, as Thunder Force IV does, shows any slip at
once).

When they part, these find where: `RECOMP_PORT_STOP=HEX` ends the run when
the pc gets there and prints how it got there, the registers and the stack;
`RECOMP_DUMP_AT=HEX` with `--dump FILE` writes 64K from that address (the
game's RAM: `FF0000` in the original, the port's `RAM` in the port) to compare
the two after the same number of frames; `PORT_DEBUG_WALK=1 agk port ...`
says why a routine a table names wasn't taken as code.

#### What a second game needed (Thunder Force IV)

Sonic 2 alone had hidden these; the port handles them now:

- **A stack from zero.** `suba.l sp,sp` (and a reset vector of 0) means the
  top of RAM on a 24-bit bus: the port loads the top of the moved RAM.
- **`stop`.** The game waits for its interrupt with `stop #$2500`: the mask is
  lowered as `move #..,sr`'s is, and on the Amiga an interrupt of the layer's
  own (the copper's poll, a blank the game has turned off) puts it back to
  sleep, drawing meanwhile.
- **Calls with their data after them.** `bsr print` followed by the string,
  which the routine reads through its return address: the call keeps the old
  return address, and the code after the string is an entry in the old space.
- **`rte` from a subroutine.** Routines that push the status register and
  leave with `rte` build the 68000's six-byte frame, which a 68020 refuses:
  every `rte` becomes `move (sp)+,sr; rts`, and the Amiga's interrupt entry
  builds the game's frames to match.
- **Tables of routines.** `lea table(pc),an; movea.l (an,dn.w),am; jsr (am)`
  and a routine's address stored with `move.l #routine,somewhere` lead to
  code the analysis run may never have reached (game modes, object types):
  both are followed. Thunder Force IV went from 14,000 to 42,000 instructions
  ported.
- **Old bytes that never ran are left alone.** What the disassembly reached
  but the analysis never executed may be data: it is ported, but its old
  bytes are neither made into jumps nor poisoned.
- **The region.** `$A10001` answers what the cartridge's header asks for (a
  US game is told an NTSC console, or it stops with a message).
- Three hints were still written by hand (`port.txt`): a `cmpa.w` on a
  register that holds a number, not a pointer (`raw`); a `jmp (pc,d3.w)` that
  with d3 = 0 runs its own extension word (`replace`); the first RAM clear,
  from address 0 down (`pre`/`post`).

On the Amiga an exception nothing handles is reported on the serial port
(vector, pc, registers, stack) before the border stripes: vector `010` at a
ROM address is old code no known path leads to.

### 4. The Amiga's hardware layer

```sh
agk port s2 -o s2/port --amiga          # s2/port/amiga/game.adf
agk inspect s2/port/amiga/game.adf -p a1200-fast --boot 100 -s "wait 6000" -s "screenshot title"
```

`recomp/mdamiga` is the Amiga side: `main.c` loads the image (deflated, it
fits one floppy) into fast RAM and takes the machine over; `start.S` has the
entry points the rewritten code calls and the vertical blank, which hands the
game its own interrupt; `layer.c` is the hardware layer (`layer_soft.c` is the
first one, kept for reference: the VDP drawn by the CPU into six bitplanes,
exact and slow); `sound.c` plays the sound. Cursor keys or the joystick,
Z/X/C or fire, Return for Start.

How `layer.c` works, and what it has learned:

- **Chip RAM is the budget, not the CPU.** The CPU gets a few thousand
  accesses to chip RAM a frame whatever its speed (the eight bitplanes take
  half the bus). So each plane is kept in fast RAM first (a shadow: the 64x32
  cells, plane A's with the sprites drawn in), and a cell is copied to a
  bitmap only if it is on screen and differs, a word or a long at a time.
  Cells off screen cost nothing until they scroll in.
- **Nothing is shown half done.** Each plane has two bitmaps; the frame is
  made in the pair that isn't on screen, and one copper list switches the
  bitmaps, their scroll and the 32 colours together at the next vertical
  blank.
- **Colours** are given out as tiles are drawn: a colour a playfield's tiles
  use gets one of its 15 slots, the nearest slot when they have run out, and
  the grouping is redone (colours as far apart as possible) when that fit is
  poor. Colours the game cycles get slots of their own.
- **The game's interrupt handlers may now catch the main loop** between
  setting a VRAM address and writing there (on the Mega Drive the loop was
  always done in time; here the layer takes time too): the VDP's address is
  put back after each handler.
- **The game runs from fast RAM.** The layout puts the game's RAM and the
  rewritten code after the ROM, which on an Amiga is chip RAM: half speed
  with eight bitplanes on. So `agk port` does the port a second time for a
  layout a megabyte on and lists every long that moved (in the code and in
  the ROM image); the loader puts the RAM, the ROM's 32K copy and the code in
  fast RAM at a multiple of 64K, adds the difference at each, and redoes the
  header checksum. Only the ROM's data and the layer's page stay in chip RAM.
- **The layer works to a deadline.** The game counts frames by its vertical
  interrupt, so that must come every blank: the layer stops at a raster line
  (with a minimum slice when the game's interrupt left it late), goes on with
  the same picture after the next blank, and shows it when it is whole (or
  after eight blanks, as far as it got). A slow machine gets fewer pictures,
  not a slower game. Colours aren't regrouped while a fade is moving: every
  step of it would be a screenful of cells.
- **The game first, the picture in its waits.** `agk port` finds the loops
  where the game waits for an interrupt (a test of RAM and a branch back to
  it: Sonic 2 has two) and lists them in `port.json` (`idle`) and
  `port_layout.h` (`PORT_IDLES`). The copper raises an interrupt every
  sixteen lines; when it lands in one of those loops the layer draws until the
  blank is near, so the game's logic is never held up by drawing. Without
  such loops (or when none is reached for six blanks) the layer draws after
  the game's vertical interrupt as before. While the game's vertical interrupt
  runs the VDP's status reads "in blank", however late the Amiga delivered it:
  Sonic 2's handler otherwise waits a whole frame for the flag.
- **Only what changed.** DMA to video memory is compared and copied in longs,
  the scroll geometry is redone only when the scroll table, the vertical
  scroll or its registers changed, tiles go from chunky to planar through a
  table per palette line, and a sprite cell that lies whole on the screen is
  drawn a tile row at a time.
- **Bitplane pointers carry.** Skipping the high word of a pointer in the
  copper list is only right while the running pointer is in the same 64K.

**Sound** (`harness/agk/mdsound.py`, `sound.c`). An FM chip can't be emulated
on an A1200 beside the game, so the driver is run on the host instead (the
sound rip above) and its output is turned into what Paula plays:

- every key-on in the log is a note, with its FM patch and its pitch and
  level frame by frame (vibrato, slides and fades come along as they are);
- each patch is rendered once per octave it is used in, by an FM synthesiser
  in the build, to an 8-bit sample: the attack, then one period looped. The
  slow part of the envelope is left out of the sample and played as volume.
  PSG squares and noise are tiny loops; drum samples are the game's own,
  cut out of the DAC writes;
- music has eight voices of the Amiga's mixer (the Mega Drive has ten or
  eleven, rarely all sounding): when more notes sound at once the least
  important gives way, decided in the build. Music that goes round is found
  from the notes repeating;
- the Amiga gets a stream: per sixtieth of a second, per voice, "start
  sample N, here (left, right, both), this period, this volume";
- `sound.c` mixes twelve voices (eight for music, two effects of two) into
  Paula's four channels as two fourteen-bit sides, 232 samples a tick at
  13.9 kHz. Paula's own interrupt at the end of each buffer is the tick, so
  the music keeps its beat when the game doesn't. Samples stay in fast RAM;
- the game's requests (a byte written to the Z80's RAM) are answered from a
  table: music, an effect, a jingle (the music waits), stop, fade. What the
  game reads back is the idle driver's RAM.

The ROM bytes only the driver read (the music data and samples) are blanked
in the Amiga's image. The sound data goes on the game's disk if it fits,
else on a second one (`game2.adf`, volume `PORT2`: in DF1:, or swapped in
when the loader asks).

What it costs: the mixing takes processor time a stock A1200 doesn't have
beside the game (a four-channel player straight on Paula's hardware, with no
mixing, is the route there: the first version of `sound.c`, in the history),
and an FM sound that is a snapshot per octave rather than the chip. Tempo
changes (speed shoes) and the driver's pause aren't followed yet.

The window plane is a bar of up to 64 lines at the top or the bottom (a
status bar): a small bitmap of its own that the copper points plane A's
bitplanes at on those lines.

Not in it yet: the interlaced two-player mode (the attract mode's second
demo), a window that isn't such a bar, water palettes, hardware sprites. On a stock
A1200 with fast RAM (`a1200-fast`), sound off, Emerald Hill's logic runs at
about 85 to 90% of full speed (70% while scrolling fast) with eight to twelve
pictures a second, and cells can be seen stale while it scrolls; the mixer is
more than that machine has, so it has no sound there yet. The original code
runs unchanged against an emulated VDP, which a 14 MHz 68020 pays for: a port
written by hand for the Amiga will be faster. `a1200-accel` is the
profile it is developed on.

The plan it follows:

The same image, with the layer's entry points filled by Amiga code instead
of the emulated chips. The game keeps talking to "a VDP"; the layer keeps a
shadow of VRAM, the registers, the palette and the sprite table, and turns
what changes into Amiga work:

| Mega Drive | A1200 (AGA) |
|---|---|
| plane A, plane B (scrolling 64x32-cell tile maps, 16 colours of 4 palettes per tile) | dual playfield, 4+4 bitplanes: 16 colours each; a wrapping bitmap per plane, hardware scrolled; a changed cell is one blit from a cache of the tiles in planar form |
| per-line horizontal scroll (parallax) | the copper: `BPLCON1` and the bitplane pointers per band |
| 80 sprites of up to 32x32, from tiles | hardware sprites for the player (AGA: 16 colours of their own); the rest as masked blits into the foreground playfield, with the tiles under them restored |
| 61 colours on screen | 16 + 16 + the sprites': each zone's palettes quantised at build time; this is where the picture gives |
| `VInt` / `HInt` at levels 6 and 4 | the vertical blank is level 3 on an Amiga, so the game's interrupt masks are rewritten with it |
| YM2612 FM + PSG, driven by a Z80 | Paula: the driver is run on the host and its notes become a four-channel stream; instruments are rendered from the FM patches at build time |

An A500 version follows the same layer with 3+3 bitplanes, a smaller window
and fewer blits per frame; that one is an art pass as much as a port.

## Limits worth knowing

- The port has the code the analysis saw or could reach. Code only a table
  the scanner didn't recognise leads to stays as it was, and the checked
  build reports it the first time it runs; playing more of the game under
  `--analysis` is the cure.
- Self-modifying code and code copied to RAM aren't handled (Sonic 2 has
  neither).
- The image is about 1.3 MB for Sonic 2 before any Amiga code or bitmaps:
  the ROM stays whole. A 2 MB A1200 is tight; trimming the ROM's art (which
  the port converts at build time anyway) is part of stage 4.
