# Phase 1: the agk harness

**Date:** 2026-09-24.

Phase 1 turned the spike into tools an agent can use without a human. This page
records what we built, what we found along the way, and which of our patches
should go upstream.

## What exists now

**Commands**

| command | what it does |
|---|---|
| `agk new` | Scaffolds a game from `templates/game`. The template splits portable rules (`logic.c`) from the Amiga side (`main.c`), and ships tests, goldens, AGENTS.md and `.mcp.json`. |
| `agk unit` | Compiles the rules for the host with ASan/UBSan and runs `tests/unit/*.c` in milliseconds. |
| `agk build` | Cross-compiles in Docker. Diagnostics from the project's own files come first; the full log goes to `build/build.log`. |
| `agk run` / `agk test` | Deterministic headless runs driven by the scenario language: input, `wait`, `wait-serial`, screenshots, `expect-serial`, `expect-color`, `regs`, `dump-mem`. Screenshots are compared against goldens. Both rebuild first if the sources changed. |
| `agk doctor` | Checks the setup. ROMs are found by SHA-1. |
| `tools/agk-mcp` | The same tools over MCP. Screenshots come back as images. |
| `tools/selftest` | Runs everything end to end. Also runs in CI on the free AROS profile. |

**Runtime:** `agk/debug.h` provides the serial channel to the harness. Output goes through an interrupt-driven ring buffer, so it costs almost no frame time.

**Timings** (macOS, arm64):
- A cold boot to `AGK ready` takes 1–1.5 s.
- A scenario starting from the cached boot snapshot takes about 0.3–0.8 s.
- The full self-test (harness tests, 9 hello tests, template round trip) takes about 25 s.

**Determinism:** every screenshot is byte-identical:
- across repeated runs, and across 4 runs in parallel
- between a cached-snapshot run and `agk run --fresh`
- across all four profiles (Kick 1.3, Kick 3.1, AROS, A1200/AGA) for 12-bit content
- between emulators built on macOS (clang) and Linux (gcc)

## Agent evaluations

We gave a fresh subagent only the project's AGENTS.md and `agk`, plus a real
feature task. Its friction log became the to-do list.

**Eval 1: collectible coins**
- It succeeded: rules, drawing, 6 unit tests and 2 emulator tests, 12/12 passing on three ROMs.
- It needed 1 build and 5 test runs.

Fixed from its report:
- `wait-serial "score=1"` broke, because RetroShell parses `key=value`. Texts are now hex-encoded.
- Errors pointed at lines of the generated script. They now point at the `.agk` line.
- The template shipped without goldens, so the first `agk test` failed. Baseline goldens now ship with it.
- Tests could run against a stale ADF. They now rebuild automatically.
- There was no way to assert what is actually drawn. `expect-color` was added.
- Doc gaps: golden paths, wait-serial being literal text and lagging one frame, single vs double buffering, and `agk help`.

**Eval 2: blitter enemy (BOB)**
- It succeeded: a masked, double-buffered 3-colour BOB, plus lives and game over. 15/15 passed, and `expect-color` proved there were no trails.
- Nearly all the effort went into reading ACE's source to find undocumented details:
  - the mask format
  - `bobBegin(pBuffer)`, where ACE's guide shows no argument
  - `viewProcessManagers()`, which double buffering needs
  - which build options are in effect
- Timing was the other cost: a screenshot lags the logic by a frame.

What we did about it:
- The agent's code is now `techniques/bobs`. It comes with a `TECHNIQUE.md` covering the recipe, the gotchas and measured costs.
- ACE's options are documented in the template's CMakeLists.
- `agk run tests/x.agk` now works.
- Ad hoc runs write to `build/agk-run/`.
- `expect-color` failures name the nearest matching pixel.
- The regex rules and the timing rules are documented.

The same run exposed a flaky test (see "tick sync" below), which led to the
biggest harness change in this phase.

### Tick sync, the host channel and the perf meter

- **Perf meter.** `agk/perf.h` reports the frame load (average and worst %) and dropped frames every 50 frames. Tests check it with `expect-no-dropped-frames` and `expect-max-load`.
  - Its first finding was our own debug output. Serial printing cost up to 20% of a frame; an interrupt-driven ring buffer halved that.
  - That's still too slow, because each character costs a level-1 interrupt on a 68000.
- **Host channel.** The default debug channel now hands strings straight to the emulator: three writes to the NOOP register, which do nothing on real hardware. A frame that prints status now costs 9% instead of 25%, against 4% for the game alone. The serial channel remains for debugging on real hardware.
- **Tick sync.**
  - *The bug:* the BOB game uses about 24% of each frame, so its work straddles video line 0, where the harness used to apply input and check serial. Whether a line printed before or after line 0 depended on CPU speed, and CPU speed differs per profile because of RAM layout. So `hit.agk` passed on Kick 1.3 and failed on Kick 3.1 and AROS.
  - *The fix:* `agkPerfBegin()` now marks the start of each game frame (a NOOP "tick"). With `sync = "ticks"`, vAmiga runs pending script commands synchronously at that tick, right before the game reads input.
  - *Result:* waits count game frames, `wait-serial` no longer lags by a frame, and the BOB game renders identically on all profiles with no per-profile goldens.
- **What the BOB example costs:** the sprite game alone uses ~5% of a frame, double buffering adds ~6% (ACE's copper block mode rebuilds lists every frame), and one 32×16 BOB adds ~5%.

## Findings

### vAmiga
These are fixed by `patches/vamiga-agk.patch`, where not noted otherwise.

1. **No frame-exact waits.** `wait` took whole seconds, and its `unit` argument was ignored.
   - Added `wait N frames`, which wakes at a frame boundary.
   - With boundary alignment, holding an input for N frames gives exactly N input samples. Without it, the count could be N or N+1.
2. **No way to wait for the game.** Added `waitserial TEXT [frames]`, which polls once per frame. On timeout it prints the tail of the serial log.
3. **`screenshot save` exits the emulator.** Added `agk screenshot PATH`, which doesn't, plus `agk serial`, `agk snapsave` and `agk snapload`. The shell had no snapshot commands.
4. **Warp-mode frame skipping made screenshots stale.** `DENISE_FRAME_SKIPPING` defaults to 16, so a screenshot could be up to 16 frames old.
   - Worse, changing the option didn't reset the pending skip counter, and that counter isn't part of snapshots.
   - Fixed by clamping the counter when the option changes.
5. **Snapshots don't include video buffers.** The first frame after a restore is blank. The harness runs one full frame before scenario time 0.
6. **`regression setup` loses the RGB palette.** It selects the RGB palette and then applies the machine scheme, which resets the monitor to CRT emulation (gamma 2.8 → 2.2 plus adjustments).
   - The harness sets `monitor set PALETTE RGB` explicitly.
   - OCS/ECS colours then come out as n×16 and AGA as n×17. The harness canonicalises to n×17, which makes 12-bit colours identical on every chipset.
7. **No way to hear what the game plays.** Added `agk audio start`, `agk audio mark NAME` and `agk audio save PATH`: while recording, every sample Paula produces is kept at a fixed 44.1 kHz (the adaptive host sample rate is off), independent of the host audio buffer. A mark first brings Paula's lazy synthesis up to the current cycle. The sub-sample position isn't part of snapshots, so `start` resets it: recordings are bit-identical between a fresh boot and a restored snapshot.
8. **Small quirks:**
   - `snapload` requires a `.vasnap` extension.
   - The console treats `\r` as "clear line", which wiped the serial tail.
   - The `waitserial` pause prints a harmless `std::exception` line.

### ACE
Fixed by `patches/ace-agk.patch`.

1. **Kickstart 1.3 is detected as NTSC under vAmiga.**
   - After boot, exec reports `VBlankFrequency=60`, `PowerSupplyFrequency=50`, and graphics has `DisplayFlags & PAL` clear, while Agnus reports PAL.
   - ACE's `systemIsPal()` trusts `VBlankFrequency`, so it used the NTSC display window (`0x22`), which drew everything 10 lines off.
   - Fix: on Kickstart below V36, ask Agnus instead (VPOSR bit 12). All profiles now render identically.
   - **Open question:** does real PAL hardware with 1.3 report 60 here, or is this a vAmiga gap? Either way, ACE shouldn't depend on it.
2. **Level-1 interrupts are ignored.** `int1Handler` didn't dispatch TBE (serial transmit), and didn't even acknowledge it. We added dispatch that acknowledges before calling the handler, so the next character's interrupt isn't lost.

### Found by the harness in our own code
- **Serial debug output dropped frames.** Busy-waiting output cost ~4 ms per status line, enough to miss the vertical blank, so the game ran at 25 fps while moving. This showed up as "held 10 frames, moved 7". The fix is the interrupt-driven ring buffer (`agkDebugAsync`).
- **`ready` came too early.** On AROS the first screenshot differed because the sprite wasn't on screen yet: ACE double-buffers copper lists. The convention is now that `AGK ready` means the first frame is actually displayed.
- **Pointer-type bug.** A compiler warning caught a byte-vs-word bug in sprite bitmap row addressing (`Planes[]` is a byte pointer).

## Upstream candidates

| patch | project | rationale |
|---|---|---|
| `wait N frames` | vAmiga | Useful to vAmigaTS and anyone scripting RetroShell. The `unit` argument was already reserved. |
| `waitserial` | vAmiga | Lets test programs signal readiness instead of relying on fixed delays. |
| non-exiting screenshot, snapshot commands | vAmiga | Scripting completeness. |
| frame-skip counter reset | vAmiga | Plain bug fix. |
| KS1.x PAL via Agnus | ACE | Fixes a real 10-line offset on 1.3 (at least under vAmiga). |
| TBE dispatch in `int1Handler` | ACE | Makes the serial transmit interrupt usable. Needs review from ACE maintainers for OS-mode interaction. |
| NOOP host channel + ticks | vAmiga | A zero-cost debug and sync channel for test harnesses. It's AGK-specific, so it would need design discussion before going upstream. |

We haven't submitted any of these. They're outward-facing, so the project owner decides.

## Next

- Knowledge: agent-oriented guides for BOBs and double buffering, scrolling, audio (ptplayer) and asset conversion. Eval 2's friction log will shape these.
- Performance visibility: report raster time or dropped frames per frame, e.g. `agk perf`, or a runtime helper that measures VPOSR at the end of the frame.
- Interactive MCP sessions (a persistent emulator over vAmiga's RPC server) for step-and-look debugging.
- Asset pipeline: PNG to bitmap and palette via ACE's converters, wired into the template's CMake.
