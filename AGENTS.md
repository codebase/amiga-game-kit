# Working on the Amiga Game Kit itself

This repo is the kit: the build tooling, the headless test harness, the runtime,
templates and docs. Games are separate projects created with `agk new`, and each
has its own AGENTS.md (from `templates/game/AGENTS.md`).

## Verify every change

```sh
tools/selftest              # everything: harness unit tests, hello tests, template round-trip (~25s)
tools/selftest a500-aros    # the same on one profile (no Kickstart needed)
PYTHONPATH=harness python3 -m unittest discover -s harness/tests   # harness only, instant
```

Determinism is the product. If a change makes two identical runs differ, or
makes a cached-snapshot run differ from `agk run --fresh`, it's a bug even if
all tests pass. Check both.

## Map

| path | what |
|---|---|
| `harness/agk/cli.py` | `agk` commands (doctor, build, run, test, unit, new) |
| `harness/agk/scenario.py` | scenario language → RetroShell lines (its docstring is the user-facing reference) |
| `harness/agk/runner.py` | boot-snapshot cache, driving VAHeadless, collecting artifacts |
| `harness/agk/image.py` | PNG in/out, playfield crop, diff |
| `harness/agk/profiles.py` | machine profiles, ROM detection by SHA-1 |
| `harness/agk/art.py` / `rd.py` | art pipeline (`agk art`, `art-clean`, `art-export`) / Retro Diffusion (`art-gen`, `art-animate`) |
| `harness/agk/sound.py` | sound pipeline: effect synthesis, MML → ProTracker MOD, previews, spectrograms (`agk sound`) |
| `harness/agk/tilemap.py` | `agk map`: Tiled `.tmj` → `build/maps/maps.h` (tile tables, property bits, objects) |
| `harness/agk/release.py` | `agk release`: the zip and the Aminet readme |
| `harness/agk/inspect.py` | `agk inspect`: any ADF booted and scripted, the sampled program counter by address, disassembly of a dump |
| `harness/agk/rip.py` | `agk rip`: copper lists, the screen and bitmaps out of a chip-RAM dump |
| `harness/agk/pack.py` | `disk/` → packed files on the ADF (the LZ packer; `runtime/src/unpack.c` reads them) |
| `harness/agk/motion.py` | `expect-scroll`: how far a region moved between consecutive frames |
| `harness/agk/profile.py` | `agk profile`: places the emulator's per-line PC and bus samples in the game (linker map + disassembly) |
| `harness/agk/lint.py` | `agk lint`: per-frame functions that call libgcc maths (from `objdump -dr` of the game's objects) |
| `harness/agk/port.py` | `agk port`: a Mega Drive game's code rewritten for the Amiga's memory map (its docstring is the reference; `docs/genesis-port.md` the pipeline) |
| `harness/agk/mcp.py` | MCP stdio server (stdlib only) |
| `runtime/` | C library every game links: `agk/debug.h` (the serial channel), `perf.h` (frame meter, memory report), `blitq.h` (blit queue), `copw.h` (copper writer), `text.h` (text lines, the kit font: `tools/font8.py`), `stars.h` (sprite starfield), `pack.h` (packed disk files: load + unpack) |
| `templates/game/` | what `agk new` copies; `{{name}}` / `{{kit}}` are substituted |
| `patches/` | our changes to vAmiga and ACE, applied by `tools/setup` |
| `third_party/` | pinned checkouts (gitignored); edit through patches, not in place |

## Changing vAmiga or ACE

1. Edit inside `third_party/vAmiga` or `third_party/ACE`.
2. Rebuild: `cmake --build third_party/build-vamiga -j` (the ACE side rebuilds with `agk build`).
3. Regenerate the patch: `git -C third_party/vAmiga diff > patches/vamiga-agk.patch` (same for `ace-agk.patch`).
4. Keep patches small and upstreamable, and mention them in `docs/`.

## Rules

- **Never commit, bundle or download Kickstart ROMs.** `roms/` is gitignored. AROS (from vAmiga's sources) is the free CI profile.
- **Harness Python is stdlib-only** (Python 3.11+), so `tools/setup` never needs pip.
- **Error messages are for agents.** Say what failed and what to do next, and include the evidence (the serial tail, the changed game-coordinate region).
