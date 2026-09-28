# A1200 / AGA games

`examples/ironwraith` is a complete eight-bitplane AGA demo. The existing ACE and
vAmiga AGA implementations support its 256-entry RGB24 palette, FMODE=3 fetches,
attached sprites and blitter objects; this work needed no additional emulator or
ACE patches. The A1200 profile remains experimental and real hardware validation
is still outstanding.

## Project setup

In `agk.toml`:

```toml
profile = "a1200"
test_profiles = ["a1200"]
[cmake]
M68K_CPU = "68020"
ACE_USE_AGA_FEATURES = true
```

In `art/art.toml`:

```toml
[art]
chipset = "aga"
depth = 8
```

`art/palette.txt` now accepts indices 0–255 and `0xRRGGBB` colors. PNG input keeps
all eight bits of each channel. Text art also uses RGB24 when AGA is selected;
write six digits for clarity. The generated palette functions accept `ULONG *`:

```c
artPaletteApply((ULONG *)port->pPalette);
```

Per-bitmap palettes, sprite palettes, previews, nearest-color mapping,
`art-export` and Retro Diffusion palette input all honor the selected chipset.
The default remains OCS RGB12, with the original `UWORD *` palette API and output.
`art-clean` fade colors retain their existing RGB12 syntax.

Create both an AGA view **and an explicitly AGA first viewport**:

```c
view = viewCreate(0, TAG_VIEW_USES_AGA, 1,
                    TAG_VIEW_GLOBAL_PALETTE, 1, TAG_DONE);
port = vPortCreate(0, TAG_VPORT_VIEW, view, TAG_VPORT_USES_AGA, 1,
                     TAG_VPORT_BPP, 8, TAG_VPORT_FMODE, 3, TAG_DONE);
```

FMODE 3 selects 64-bit bitplane fetches while retaining 16-pixel sprite fetches.
At width 320 the interleaved plane stride is 40 bytes, divisible by eight. The
raw simple-buffer copper list needs `6 + 2 * depth` commands: 22 at eight planes,
or 38 including the sprite manager's 16 pointer commands. Keep those offsets
separate. AGA palettes are stored as ULONGs by ACE despite its viewport field
being declared as a UWORD pointer.

BPLCON0's eight-plane encoding is `0x0210`; palette banks and low component nibbles
are selected through BPLCON3. ACE writes both halves when loading the palette.
The original hardware notes are preserved in
[AGA.guide](https://github.com/rkrajnc/minimig-mist/blob/master/doc/amiga/aga/AGA.guide).

## Palette and display tests

The A1200 harness preserves RGB24 output. Use the new exact assertion:

```text
screenshot arena
expect-rgb arena 0 0 0x080E18
expect-rgb arena 160 130 0x123456 near 2
```

The last value is illustrative: use an observed, intended asset color at that
coordinate. Existing `expect-color ... 0xRGB` assertions remain nibble checks.
`expect-rgb` checks every bit and reports the actual six-digit value on failure.
Its optional `near` radius has the same square neighborhood semantics as
`expect-color`. Goldens for AGA projects should be recorded on AGA.

Run identical scenarios twice with the snapshot cache and once with `--fresh`;
the screenshots, state log and audio must agree. Palette correctness alone does
not prove DMA timing. Inspect moving sprites, busy boss scenes and result screens,
and keep the performance checks enabled.

`tools/selftest a500-aros` now skips examples whose declared test targets do not
include that profile, so adding an AGA-only example does not break the free A500
CI sweep. Without arguments, selftest still tests every project's declared
profiles, including the A1200 demo (which needs a user-provided ROM).
