# Amiga Game Kit (AGK)

A kit for building Commodore Amiga games with AI coding agents (Claude Code,
Codex, Cursor, ...). Agents are only as good as their feedback loop, so AGK
gives them a **deterministic, headless build → run → observe → test** cycle:
screenshots, frame-exact joystick input, serial debug output, memory and
register dumps, and golden-image tests. It's built on proven Amiga tooling:
[ACE](https://github.com/AmigaPorts/ACE), bebbo's GCC and
[vAmiga](https://github.com/dirkwhoffmann/vAmiga).

```
$ agk test examples/hello
PASS boot [a500] 0.3s, boot 1.5s (cached next time)
PASS clamp [a500] 0.8s
PASS move [a500] 0.4s
...
9/9 passed
```

## Quick start

```sh
tools/setup                    # fetch pinned ACE + vAmiga, apply patches, build emulator, pull toolchain
export PATH="$PWD/tools:$PATH"
agk doctor                     # check Docker, emulator, ROMs, profiles
agk build examples/hello       # C + ACE -> build/hello.adf (bootable floppy), compiled in Docker
agk run examples/hello -s "press right 20" -s "screenshot moved"
agk test examples/hello        # run tests/*.agk on every profile, compare with golden images
```

Requirements: Docker, git, cmake, Python 3.11+, and a C++20 compiler (on macOS:
`brew install llvm`, because Apple's clang 15 is too old for vAmiga).

## How a run works

1. **Boot once.** AGK powers on the emulated machine, inserts the ADF and runs
   until the game prints its ready text (`AGK ready`) on the serial port. It
   saves a snapshot on the next frame boundary. Snapshots are cached per build
   and profile, so later runs skip the floppy boot and start in about 0.3 s.
2. **Play the scenario.** Every step (`press right 20`, `wait 5`,
   `screenshot moved`, ...) is timed in video frames and lands on a frame
   boundary. The same scenario gives byte-identical results on every run,
   whether it started from the cached snapshot or from a fresh boot.
3. **Collect results.** You get PNG screenshots, the full serial log, memory
   dumps and register views in `build/agk/<profile>/<scenario>/`. `agk test`
   also compares screenshots against `tests/golden/` and writes a
   `*.diff.png` that highlights changed pixels in red.

The scenario language is documented in `agk help-scenario`.

## Games talk to the harness over serial

A game reports what it's doing by writing lines to the serial port. See
`examples/hello/src/dbg.c`: about 30 lines that poll SERDAT directly, with no
OS needed. Conventions:

- `AGK ready`: print this once the first real frame is on screen. The harness waits for it.
- `AGK <key>=<value> ...`: state that tests can assert on with `expect-serial`.

## Profiles

| profile | machine | ROM |
|---|---|---|
| `a500` (default) | A500, OCS, 512K chip + 512K slow | Kickstart 1.3 |
| `a500-ks31` | A500, ECS, 1MB | Kickstart 3.1 |
| `a500-aros` | A500, OCS, 1MB + 2MB fast | AROS (free, no Kickstart needed) |
| `a1200` | A1200, AGA, 2MB (experimental) | Kickstart 3.1 |

**Kickstart ROMs are copyrighted and are never committed, bundled or downloaded
by AGK.** Put your own licensed ROMs in `roms/` (gitignored), or point
`AGK_ROMS` at another directory. Files are matched by SHA-1, so any file name
works. The `a500-aros` profile needs no Kickstart, which makes it a good fit
for CI.

## Layout

```
tools/        setup, build, agk (CLI entry point)
harness/agk/  the agk CLI: scenarios, emulator driver, images, profiles
patches/      local patches to vAmiga and ACE (all intended for upstream)
examples/     example games; each has agk.toml, src/, tests/*.agk, tests/golden/
docs/         design notes and results
roms/         your Kickstart ROMs (gitignored)
third_party/  pinned ACE + vAmiga checkouts (gitignored, created by tools/setup)
```

## License

MIT. Third-party components keep their own licences:
- ACE: MPL-2.0.
- vAmiga core (`Core/`, including VAHeadless): MPL-2.0.
- Moira CPU core: MIT.

Only vAmiga's macOS GUI app is GPL-3.0, and we don't use it.
