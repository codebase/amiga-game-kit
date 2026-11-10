# Porting a Mega Drive game to the Amiga

The Mega Drive and the Amiga share a CPU. A Mega Drive game's 68000 code can
run on an Amiga as it is; what can't come along is the machine around it: the
VDP's tile planes and sprites, the FM chip, and the memory map. This is the
kit's pipeline for bringing a game across, built on Sonic the Hedgehog 2 as
the hard case. You bring the ROM; the kit holds only tools.

Status: stages 1 to 3 work (the game's code, rewritten for the Amiga's memory
map, runs frame for frame against the original on the host). Stage 4, the
Amiga's own hardware layer, is next. Target order: A1200 (AGA) first, an A500
version afterwards as a reduced art pass.

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

The runner is a Mega Drive for the host (VDP, pads, the Z80's bus; no sound
yet), there to measure the game, not to be the product. `--analysis DIR` runs
everything through the interpreter, where the pc is exact, and records:

| file | what |
|---|---|
| `coverage.bin` | every instruction executed |
| `entries.bin` | where every computed `jmp`/`jsr` went (jump tables, object routines) |
| `romread.bin` | every byte of the ROM that was read; minus the executed instructions, that is the game's *data*, and no disassembly may claim it as code |
| `hwsites.txt` | every instruction that touched the hardware: pc, address, read or write, width |

The more of the game is played, the more of it the port has. Arrows/WASD are
the d-pad, Z/X/C the A/B/C buttons, Return is Start.

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
  those are comes from `hwsites.txt` and from absolute operands.
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

### 4. The Amiga's hardware layer (next)

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
| YM2612 FM + PSG, driven by a Z80 | Paula: the driver's music and sound requests answered by a player whose instruments are rendered from the FM patches at build time |

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
