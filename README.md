# Amiga Game Kit (AGK)

A kit for building Commodore Amiga games with AI coding agents (Claude Code,
Codex, Cursor, ...). The core idea: agents are only as good as their feedback
loop, so AGK gives them a **deterministic, headless build → run → observe**
cycle — screenshots, frame-exact input, serial debug output — on top of proven
Amiga tooling ([ACE](https://github.com/AmigaPorts/ACE), bebbo's GCC,
[vAmiga](https://github.com/dirkwhoffmann/vAmiga)).

**Status: Phase 0 spike.** See [docs/spike-results.md](docs/spike-results.md).

## Quick start

```sh
tools/setup                     # fetch pinned ACE + vAmiga, build VAHeadless, pull toolchain image
tools/build examples/hello      # C + ACE -> Amiga executable -> bootable OFS ADF (in Docker)
python3 harness/agk_run.py \
  --adf examples/hello/build/hello.adf \
  --boot 45 \
  --step "1f:joystick2 pull right" --step "20f:joystick2 release x" \
  --name demo                   # -> out/demo.png, out/demo.serial.txt, JSON summary
```

Requirements: Docker, git, cmake, Python 3, and a C++20 compiler (on macOS:
`brew install llvm`).

## Kickstart ROMs

ROMs are copyrighted and are **never** committed, bundled or downloaded by AGK.
Put your own licensed ROMs in `roms/` (gitignored); the harness defaults to
`roms/kick13-34005-a500.rom`. The AROS replacement ROM (bundled with vAmiga's
sources) works for CI when the machine has fast RAM — see the spike results.

## Layout

```
tools/       build + setup scripts
harness/     headless emulator driver (agk_run.py), raw->PNG, vAmiga patches
examples/    example games (hello: copper bars + joystick sprite + serial log)
docs/        design notes and spike results
roms/        your Kickstart ROMs (gitignored)
third_party/ pinned ACE + vAmiga checkouts (gitignored, created by tools/setup)
```

## License

MIT. Third-party components keep their own licenses: ACE is MPL-2.0; the
vAmiga core we build (`Core/`, incl. VAHeadless) is MPL-2.0 with the Moira CPU
core under MIT (only vAmiga's macOS GUI app is GPL-3.0, and we don't use it).
