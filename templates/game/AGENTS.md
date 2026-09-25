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
agk play        # let the human play it in FS-UAE (arrow keys + Space)
agk help        # all commands;  agk help-scenario  # the test/run step language
```

`agk run` and `agk test` rebuild automatically when sources are newer than the
ADF. The template ships passing tests and goldens, so run `agk test` once
**before** you change anything: that's your baseline.

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
- `expect-serial "x=224"`: a regex over the whole serial log
- `expect-color NAME 44 44 0xFC0`: the pixel at game (44,44) in screenshot NAME must be Amiga colour $FC0. Use this to prove something is or isn't drawn; a golden only proves "same as last time".
- `wait-serial "score=1"`: literal text, not a regex. It resumes one frame after the text arrives, so the game has usually run one more frame with the old input.
- `regs NAME cpu copper`
- `dump-mem NAME 0x0 0x80000`

Time is in game frames: 50 per second, starting when the game prints `AGK ready`.
`agkPerfBegin()` at the top of each frame marks the frames, and `sync = "ticks"`
in agk.toml tells the harness to count them. Keep that call first in
`genericProcess()`. Runs are deterministic, so the same scenario gives the
same pixels on every profile, every time.

### Golden images
`tests/golden/<test>/<shot>.png` are the approved screenshots, shared by all
profiles. Colours are exact: Amiga colour `0xRGB` appears as
(R×17, G×17, B×17).
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

## Art: sprites and BOBs from files

Graphics live in `art/`, not in C:
- `art/palette.txt` is the game palette.
- `art/art.toml` lists the assets.
- `art/player.txt` is the player as text art, one character per pixel.

PNGs work too, from any tool or AI generator.

- **`agk art`** converts everything. It prints what it did and any rule problems, and writes previews to `build/art/preview/`. **Look at the preview** after every art change: it's exactly what the Amiga will show.
- `agk build` and `agk test` regenerate art automatically. C code includes `art.h` and calls, e.g., `artPlayerCreate(frame)`, `artPlayerApplyColors(palette)` and `artPaletteApply(palette)`.
- The rules are enforced for you:
  - sprites are 16 px wide, 3 colours plus transparent, and sprites sharing a channel pair share colours
  - BOB colours must be in `palette.txt`
- `agk help-art` has the formats. `{{kit}}/techniques/sprites` shows animated sprites and BOB frames end to end.
- **Bigger, richer sprites:** `attached = true` makes 15-colour sprites up to 64 px wide from channel pairs, and `mirror = true` adds left-facing frames. See `{{kit}}/examples/sidescroller` (a 32×32 hero, 8 frames).
- **AI art + hand animation:** `agk art-export NAME` turns a PNG into editable text art, so you can generate a base with AI and draw the animation frames yourself.
- **AI art:** `agk art-gen NAME "description" --size 32x16` generates pixel art in the game's palette with Retro Diffusion, adds it to `art/`, converts it and shows the preview. It needs a key (`RD_API_KEY` or `~/.config/agk/credentials`) and costs about $0.02 per image; `--dry-run` checks the price for free. Always look at the preview: AI art needs a human-quality eye, and you can refine it by exporting to text art or regenerating with a `--seed`. AI backgrounds often have see-through holes and floating fragments: `agk art-clean SRC -o OUT --fill-holes --despeckle 60` fixes most of them.

## Telling the harness what happens: serial debug

`#include <agk/debug.h>`, then:
- `agkState("score", s); agkState("lives", l); agkEnd();` prints `AGK score=10 lives=3`, which tests match with `expect-serial "score=10"`.
- `agkPrint("AGK level 2\n")` prints free text; `wait-serial "level 2"` syncs a test on it.
- `agkReady()` has already been called for you: the template calls it once the first frame is on screen. Keep that behaviour if you restructure startup.

Printing is cheap but not free. The default "host" channel hands each line
straight to the emulator, but formatting it still takes CPU time: about 1–2%
of a frame per value on a plain screen. With many bitplanes (e.g. a 6-plane
dual playfield) the display DMA slows the CPU, and a 10-value line cost about
12% of a frame (`examples/sidescroller`). So:
- print when something changes, plus a heartbeat every N frames
- keep lines short
- check `maxload` in the perf lines For real hardware, `AGK_DEBUG_CHANNEL serial` (see
CMakeLists.txt) uses the serial port, where every character does cost time.

## Performance: measure, don't guess

`main.c` wraps each frame in `agkPerfBegin()` … `agkPerfEnd()`. Every 50
frames this prints `AGK perf frames=50 dropped=0 load=9 maxload=11`:
- **load** and **maxload** are the average and worst % of the 1/50 s frame spent on your code.
- **dropped** counts frames that missed the vertical blank. That's visible as stutter or half speed.

`tests/perf.agk` guards this with `expect-no-dropped-frames` and
`expect-max-load 60`. Keep it passing:
- After adding something expensive, look at `maxload`.
- If a change pushes it up a lot, the usual fixes are:
  - move work out of per-frame code (precompute and use lookup tables)
  - avoid `int`/`long` multiply and divide
  - draw only what changed
- Measure again after each fix.

Baseline: the template uses about 4% idle and about 9% while moving.

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
- The template's `simpleBufferCreate` is **single-buffered**, so `pBack == pFront`. Draw or erase static things once. If you add double buffering (`TAG_SIMPLEBUFFER_IS_DBLBUF`), every change must be made in both buffers, or the old image flickers back every other frame.
- The copper changes registers at chosen scanlines: colour bars, palette splits, scroll.
  - ACE **block** mode (`copBlockCreate`/`copMove`) re-merges every block whenever anything changes. That's fine for a few static blocks, but a per-line effect updated every frame costs about 90% of a frame.
  - Use **raw** mode for anything big or animated; `{{kit}}/techniques/copper` shows how.
- ACE copper lists are double-buffered, so a change appears a frame or two later. A screenshot can show the state from up to 2 frames before the serial log.
- ACE build options (BOB wrapping, scroll buffer margins, ACE_DEBUG…) go in `agk.toml` `[cmake]`. `CMakeLists.txt` lists them.

**OS**
- After `systemUnuse()` the game owns the hardware: don't call AmigaOS (DOS, Intuition, graphics.library).
- Load files before `systemUnuse()`, or wrap them in `systemUse()` … `systemUnuse()`.
- Kickstart 1.3 is the baseline. Don't use OS functions newer than V34.

## Techniques: copy what's proven

`{{kit}}/techniques/` has small, complete games. Each one is tested on every
profile and comes with a `TECHNIQUE.md` covering the recipe, the gotchas and
measured frame cost:
- `bobs`: blitter objects, i.e. masked, double-buffered, background restore, no trails. Read it before drawing anything with the blitter; ACE's own BOB guide has a wrong signature.
- `scrolling`: tile-map scrolling with ACE's tile buffer, a camera that follows the player, and measured costs per depth and speed. ACE's `tilebuffer.md` has several errors; this guide lists them.
- `copper`: a sky gradient (58 colours on a 4-colour screen), a HUD palette split and a moving raster bar, in raw and block mode with measured costs.
- `sprites`: the art pipeline end to end. Text art becomes an animated sprite, and a PNG sheet becomes an animated BOB.

`{{kit}}/docs/references.md` lists open-source Amiga games and what each is
good for studying, with licences.

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
