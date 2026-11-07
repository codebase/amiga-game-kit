# Reading a game that isn't yours

The harness that tests the kit's games also runs any Amiga disk image. With
three commands it boots a game, scripts the way into it, samples where the
CPU spends its time, dumps memory, and pulls the picture and the graphics
out of the dump. Nothing in the game needs the kit; the game doesn't know
it's being watched. This is the groundwork for understanding a game well
enough to port it.

The example below is Ocean's **RoboCop** (1989, Peter Johnson), from a disk
image the user supplied. The repo holds no byte of it: everything here is
what the commands print.

## 1. Boot it and get in

```sh
agk inspect robocop.adf --boot 45 -s "press fire 2" -s "wait 500" -s "screenshot title"
```

A kit game prints `AGK ready` when it's up; a foreign one prints nothing, so
`--boot` is how many seconds to wait after power-on before the steps start
(the boot is cached, so the second run of the same disk starts there in a
second). The steps are the scenario language (`agk help-scenario`): waits,
input, screenshots. Screenshots every few seconds find the way through a
cracktro, a logo and a title. RoboCop takes 45 s to reach its title, and fire
starts a game about a minute later.

Two steps exist for this work only:
- `snapshot NAME` saves the whole machine to `NAME.vasnap`; `--from NAME.vasnap`
  starts a later run there instead of booting. Save one at the title and one
  in the game: every experiment after that starts in a second.
- `dump-chip NAME` writes the 512 KB of chip RAM to `NAME.bin` and the display
  registers (the palette, BPLCON0, DIWSTRT, the modulos) to `NAME.regs.txt`.

```sh
agk inspect robocop.adf --from agk-inspect/robocop/game.vasnap \
    -s "hold right" -s "wait 400" -s "release" -s "screenshot walk" -s "dump-chip ram" --sample
```

## 2. Where the time goes

`--sample` records the program counter on every raster line (15,625 a
second) and who used the bus, and reports it by address, since a foreign
game has no symbols:

```
128274 line samples, 409 frames
program counter: 0.0% in the Kickstart ROM; the game's code spans $01D108..$0231D8
bus: free 15%, CPU 24%, blitter 37%, bitplanes 22%, other 2%

hot 256-byte regions (share of all samples; --disasm ADDR reads one):
  $021E00   54.0%
  $022600   18.9%
  $021D00   14.8%
  $022C00    5.2%
```

Read: the game has switched the OS off (0% in the ROM); its code sits
between $1D108 and $231D8, about 25 KB of it hot; it spends 37% of the bus on
the blitter. Over a minute of sampling, the hot regions told the whole story:

| address | what it turned out to be |
|---|---|
| `$21D00`-`$21E00` | the renderer: for every column, 8 tiles of 16x16 blitted from a table indexed by a map byte; the whole screen redrawn every frame (no hardware scrolling), which is why the original is choppy |
| `$22600` | the vertical-blank wait and a 32 KB screen copy |
| `$26D00` | a bit-stream decruncher (the data files' format: Andrew Bond's Flexicrunch, named in the disk's strings) |
| `$200` | a raw MFM track loader, the game's own, used even on this AmigaDOS disk |

The "one frame" line shows the loop's shape: the order the regions ran in
and how many raster lines each took, so the frame's structure is visible
before any disassembly.

## 3. Read the code

```sh
agk inspect robocop.adf --disasm 0x21D54:0x60
```

disassembles the dump at that address (objdump, 68000). The renderer's core:

```
$021D54  movew #38,0xdff066      ; BLTDMOD 38: 16 px tiles on a 40-byte row
$021D5C  movew #38,0xdff060      ; BLTCMOD
$021D64  movew #-2,0xdff064      ; BLTAMOD -2: the tile's rows are 2 bytes apart
$021D74  movel #65535,d0
$021D7A  asll d7,d0
$021D7C  movew d0,0xdff046       ; BLTALWM from a shift
$021D84  moveb a5@+,d6           ; the next map byte...
$021D86  moveal a4@(0,d6:w),a1   ; ...is an index into a table of tile bitmaps
$021DA4  movew #7,d1             ; 8 tiles down a column
$021DB2  addal #640,a0           ; 16 rows x 40 bytes to the next tile row
```

For a whole executable, IRA (in the toolchain image) turns a hunk file into
reassemblable source with labels; for pseudocode, Ghidra with the Amiga
hunk loader (`lab313ru/ghidra_amiga_ldr`). Both are the next step up from
`--disasm` once a routine is worth reading in full.

## 4. Get the graphics

```sh
agk rip ram.bin --copper              # the copper lists in the dump
agk rip ram.bin --screen -o screen.png
```

`--copper` finds the copper lists in the dump (runs of MOVEs to the custom
registers) and shows what each sets up: bitplane pointers, modulos, colours.
`--screen` interprets the best one line by line the way the hardware does
(pointers advancing by the fetch width plus the modulo, pointer and colour
changes at their WAIT lines, DDFSTRT's early fetch and BPLCON1's fine
scroll, dual playfields) with the registers from `ram.regs.txt`, and writes
the picture the display was showing. RoboCop's comes out pixel-identical to
the screenshot, apart from the people: they're in the other frame buffer or
are sprites, which a memory dump can't show. That is the point, though: the
background without the objects is the first asset.

A bitmap at a known address, or a run of tiles, is the other form:

```sh
agk rip ram.bin 0x70000 320x200x4 -o field.png --palette @0x1C000
agk rip ram.bin 0x30000 16x16x4 --count 64 -o tiles.png
```

The renderer above says where RoboCop's tiles are (the table `a4` points
at) and how they're laid out (16x16, 2 bytes a row, planes together), so a
`--count` sheet of them is a session's work from here, and the art pipeline
reads such a sheet back with `frame_width`/`frame_height`.

## What this is and isn't

It's a workbench: boot, script, sample, dump, disassemble, rip, all
headless and repeatable from snapshots, with the same emulator that proves
the kit's own games deterministic. It isn't a decompiler: understanding a
game's rules means reading its code, and a port means rewriting them. The
kit's shape fits that end: rules in portable C with host tests, assets
loaded from the user's own disk at runtime, no copyrighted bytes in the
repo. Which brings up the obvious: a game's code and art belong to whoever
holds its rights. Reading them to learn is one thing; redistributing them
is another, and a port written from a disassembly sits between. Know which
you're doing.
