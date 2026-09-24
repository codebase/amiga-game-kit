# {{name}}: Amiga game (AGK project)

A Commodore Amiga game written in C with the [ACE](https://github.com/AmigaPorts/ACE)
engine. The target is an **A500: 68000 at 7 MHz, OCS chipset, 512K chip RAM,
PAL 50 Hz, Kickstart 1.3**. It's built and tested with the Amiga Game Kit (`agk`).

## The loop: always verify, never guess

```sh
agk unit        # game rules only, on the host, in milliseconds - run after editing src/logic.c
agk build       # cross-compile in Docker -> build/{{name}}.adf (a bootable floppy)
agk test        # boot on every profile, play tests/*.agk, compare screenshots to goldens
agk run -s "press right 20" -s "screenshot moved"   # try something ad hoc
```

After a change you can see, **look at the screenshot**. `agk run` and `agk test`
print the path of `<name>.screen.png`, a 320×256 image in game coordinates
(pixel x,y in the PNG = x,y in the game). Read it and check that it shows what
you intended. A passing build proves nothing about what appears on screen.

The same commands are available as MCP tools (`agk_build`, `agk_run`,
`agk_test`, `agk_unit`) through `.mcp.json`. `agk_run` and `agk_test` return
the screenshots as images. If `agk` isn't on your PATH, it's `{{kit}}/tools/agk`.

`agk help-scenario` prints the scenario language. Common steps:
- `press right 20` holds for 20 frames
- `press up+fire`
- `hold left` … `release`
- `wait 10`
- `screenshot NAME`
- `expect-serial "x=224"`
- `wait-serial "level 2"`
- `regs NAME cpu copper`
- `dump-mem NAME 0x0 0x80000`

Time is in frames: 50 per second, starting when the game prints `AGK ready`.
Runs are deterministic, so the same scenario gives the same pixels every time.

### Golden images
`tests/golden/<test>/<shot>.png` are the approved screenshots.
- If a test fails with `DIFFERENT`, open the `*.diff.png` it names. Changed pixels are red, and the message gives the changed area in game coordinates.
- Only if the change is intended, run `agk test --update`, and say which goldens you updated and why.
- Never update goldens just to make a failing test pass.

## Project layout

| file | what goes there |
|---|---|
| `src/logic.c/.h` | **Game rules**: movement, collisions, scoring, state. Portable C only: no ACE or Amiga headers, no hardware access. Tested on the host by `agk unit`. |
| `src/main.c` | Amiga side: display setup (ACE view/viewport/buffer), sprites, blitter drawing, joystick, calling `logicUpdate` once per frame. |
| `tests/unit/*.c` | Host tests for the rules (`CHECK(...)` macro, `main()` returns non-zero on failure). |
| `tests/*.agk` | Emulator tests: input, screenshots, serial expectations. |
| `agk.toml` | name, profiles, boot text, `unit_sources`. |

Put new rules in `logic.c` and write a unit test first. Keep `main.c` a thin
layer that maps state to hardware.

## Telling the harness what happens: serial debug

`#include <agk/debug.h>`, then:
- `agkState("score", s); agkState("lives", l); agkEnd();` prints `AGK score=10 lives=3`, which tests match with `expect-serial "score=10"`.
- `agkPrint("AGK level 2\n")` prints free text; `wait-serial "level 2"` syncs a test on it.
- `agkReady()` has already been called for you: the template calls it once the first frame is on screen. Keep that behaviour if you restructure startup.

Print when something **changes**, not every frame. Each character busy-waits
about 87 µs.

## Amiga facts that bite

**CPU (68000, 7 MHz)**
- No 32-bit multiply or divide instruction. `int` is 32-bit here, so `a * b` or `a / b` on `int`/`long` calls a slow libgcc routine. Use `int16_t`/`WORD`, shifts, and lookup tables in per-frame code.
- Word and long accesses must be even-aligned, or you get an address error (a crash, a "Guru"). Don't cast odd `UBYTE*` offsets to `UWORD*`.
- No FPU. Use fixed point instead (see ACE `docs/programming/fixed_point.md`).
- The frame budget is 1/50 s ≈ 140,000 CPU cycles, and bitplane and blitter DMA steal some of them.

**Memory**
- Chip RAM (512K) is the only memory the custom chips can see. Bitmaps, sprites, copper lists and audio samples must live there; ACE's `bitmapCreate` allocates chip RAM for you unless you pass `BMF_FASTMEM`.
- The rest of RAM is "slow" RAM at `$C00000`: fine for code and data, not for graphics.
- `tBitMap->Planes[i]` is a **byte** pointer. Cast before indexing words: `(UWORD *)bm->Planes[0] + row * (bm->BytesPerRow / 2)`.

**Display (PAL lores)**
- The playfield is 320×256 pixels.
- Colours are 12-bit `0xRGB` (4 bits per channel). You get 2^bpp palette entries, up to 32 at 5 bitplanes.
- Hardware sprites (`spriteAdd`) are 16 px wide, 3 colours plus transparent. Channels 0/1 use palette entries 17–19, 2/3 use 21–23, and so on.
- A sprite bitmap is 2-bitplane, interleaved, with an extra empty line at the top and bottom for the hardware control words.
- Moving objects wider than 16 px or with more colours are BOBs, drawn with the blitter (ACE `bob` manager, `docs/programming/using_bobs.md`).
- The copper changes registers at chosen scanlines: colour bars, palette splits, scroll. ACE: `copBlockCreate` / `copMove`.
- ACE copper lists are double-buffered, so a change appears a frame or two later.

**OS**
- After `systemUnuse()` the game owns the hardware: don't call AmigaOS (DOS, Intuition, graphics.library).
- Load files before `systemUnuse()`, or wrap them in `systemUse()` … `systemUnuse()`.
- Kickstart 1.3 is the baseline. Don't use OS functions newer than V34.

## ACE documentation

The kit's ACE checkout is at `{{kit}}/third_party/ACE`:
- `docs/programming/*.md` has guides for views, sprites, blits, BOBs, tile buffers, fonts, palettes and audio.
- `showcase/src/test/*.c` has working examples of each subsystem. Copy their patterns.
- `include/ace/**/*.h` has the API with doc comments.

## When something goes wrong

- **Build error:** `agk build` shows errors in your files first. The full log is in `build/build.log`.
- **Boot timeout** (`waitserial: timeout waiting for 'AGK ready'`): the game crashed or hung before its first frame. The message includes the serial output so far, so print progress markers with `agkPrint` to narrow down where it stops.
- **Wrong picture:** check the `.screen.png`, then check your state lines in `serial.txt`, then use `regs NAME copper denise agnus` to see what the hardware was actually told.
- **Works on one profile but not another:** that's a real compatibility bug (OS version, memory layout, PAL detection). Don't paper over it with per-profile goldens unless the difference is intended.
