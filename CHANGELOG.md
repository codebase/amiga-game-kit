# Changelog

## v0.1.0 (2026-11-06)

The first tagged release. Everything below was built between 24 September
and 6 November 2026; `docs/roadmap.md` has the reasoning behind each item.

**The kit**
- `agk new`, `build`, `run`, `test`, `unit`, `play`, `doctor`: a deterministic,
  headless build-run-observe-test loop on a patched vAmiga, with golden
  screenshots shared by Kickstart 1.3, 3.1, AROS and A1200 profiles, boot
  snapshots, tick-exact input, and a host-channel serial log.
- The scenario language: input, waits, screenshots, memory and register
  dumps, colour and audio checks, `expect-scroll` (motion), `checkpoint` /
  `start-at` (tests that begin mid-level), `expect-no-dropped-frames`,
  `expect-max-load`, `expect-min-chip-free`.
- `agk profile`: the emulator samples the program counter and the bus every
  raster line; the report names functions, the heaviest frames, and draws
  the load. `agk lint`: per-frame code that calls the 68000 maths library.
- Art: text art and PNGs to sprites, attached sprites, BOBs and bitmaps with
  previews; colour pins; Retro Diffusion generation (`art-gen`) and fitting
  (`art-export --palette`, `--pin`); AGA RGB24.
- Sound: effects synthesis and MML music with sampled instruments to
  ProTracker modules; spectrograms and audio checks in tests.
- Levels: Tiled maps to C tables (`agk map`); packed data files on the disk
  (`disk/`, `agkLoadFile`).
- The runtime (`agk/`): the frame meter and memory report, the host serial
  channel, a blit queue, a copper writer, text lines with a font, a sprite
  starfield, the file loader.
- `agk record` (MP4 with sound, GIF) and `agk release` (zip + Aminet readme).
- `agk inspect` and `agk rip`: any disk image booted, scripted, sampled by
  address, snapshotted, dumped and ripped (`docs/reading-games.md`).
- An MCP server, a project template with its `AGENTS.md`, and CI on the free
  AROS profile.

**Games and techniques**
- `examples/voidrunner` (a horizontal shoot-'em-up, level 1), `racer`
  (a copper road racer), `sidescroller` (a dual-playfield platformer),
  `ironwraith` (an A1200 AGA mech demo), `hello`.
- `techniques/`: bobs, scrolling, copper, sprites, hotpath (asm checked
  against C), multiplex (24 bullets from 6 sprite channels), loading.
