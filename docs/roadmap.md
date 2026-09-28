# Roadmap: what building VOIDRUNNER asked of the kit

Each item came from real friction while making `examples/voidrunner` (a
50 fps shmup) and fitting Retro Diffusion art into it. Status is kept here as
the work lands; the commit that finishes an item names it.

| # | item | why | status |
|---|---|---|---|
| 1 | `agk lint`: per-frame code that calls libgcc maths (`__divsi3`, `__modsi3`, `__mulsi3`, ...) | On a 68000 these are slow library calls. VOIDRUNNER hand-replaced them; nothing caught a stray `%`. | done: `agk lint`, a summary after `agk build`, `agk_lint` over MCP. It found a 32-bit multiply in VOIDRUNNER's boss sweep. |
| 2 | `agk art`: declared colours keep their slot even when unused; pin with `@slot` | An unused colour shifted the ship's last three out of the starfield's slots 29-31, silently. | done: pins (`W 0xFFF @31`, or `slots = {31 = "0xFFF"}` for PNGs) and a warning when an unused listed colour moves the ones after it. Kept backward compatible: every example's art converts byte-identically. |
| 3 | Scenario motion checks: `expect-scroll` | The backdrop jumped 16 px every 64 frames and every test passed: screenshots can't see motion. | done: `expect-scroll X0 Y0 X1 Y1 FRAMES DX [DY]`. Put back in VOIDRUNNER, the old bug fails it with "frame 13: -17, frame 17: +15"; the game now has `tests/scroll.agk`. |
| 4 | `agk profile`: where the frame goes (CPU functions, blitter, DMA) | Finding the 105% frame took awk over the serial log. | done: the emulator samples the PC and the bus every raster line and knows each frame's working time; `agk profile SCENARIO` reports load, bus use, functions and the heaviest frames, `--disasm FUNC` per instruction, `agk_profile` over MCP. First finding: VOIDRUNNER's heaviest frames are CPU-bound (blitter ~8% of the bus), not blitter-bound as assumed. |
| 5 | `agk art-fit`: fit any PNG (Retro Diffusion `--free-colors`) to the game's palette | Every game needs it; VOIDRUNNER wrote `art/tools/from_rd.py`. | done, in `agk art-export` (it already made text art from PNGs): `--palette`, `--pin 0xRGB@REG`, `--dither`, `--trim`; reductions leave room for pins. It reproduces VOIDRUNNER's hand-made Retro Diffusion conversions; animation frames (the Warden's flash) stay game code. `art-gen --free-colors` points to it. |
| 6 | `agk record` for READMEs: `--gif-start`, `--gif-size`, `--gif-colors` | The docs GIF was made by hand with ffmpeg. | planned |
| 7 | `runtime/`: blit queue, double-buffered copper lists, HUD split, sprite starfield | Each game rebuilt them from scratch. | planned |
| 8 | Perf chart per test run (load over time, with the game's state) | Seeing where the peak is, at a glance. | done with #4: `load.png`, one column per game frame (blue CPU-bound, orange blitter-bound, red over budget). |
| 9 | Scenarios that start from a checkpoint (a saved state mid-level) | `perf` plays the whole level to reach the boss. | planned |
| 10 | Warn when two sprite channels need the same colour registers with different colours | Sprites share colour registers in pairs. | planned |
