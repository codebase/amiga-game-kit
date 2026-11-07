# Make your first game

Start small: one screen, one enemy, one way to win. Get it playable before
adding more art and levels.

## Install the kit

You'll need Docker running, Git, CMake, Python 3.11+, a host C compiler and a
C++20 compiler. On macOS, install Homebrew LLVM if Apple's compiler is too old.

```sh
git clone https://github.com/codebase/amiga-game-kit.git
cd amiga-game-kit
tools/setup
export PATH="$PWD/tools:$PATH"
agk doctor
agk new ~/games/mygame
cd ~/games/mygame
```

No Kickstart ROM? Set these two lines in the new game's `agk.toml` to use the
free AROS profile installed by setup:

```toml
profile = "a500-aros"
test_profiles = ["a500-aros"]
```

If you own a supported Kickstart ROM, put it in the kit's `roms/` directory
and choose the matching profile instead. `agk doctor` shows what's available.

## Give your coding agent a game to make

Open `~/games/mygame` in your coding agent. The project includes `AGENTS.md`
and an MCP configuration; enable that server if your client supports it.
The same tools work from the command line.

Try this prompt:

> Read AGENTS.md and make a one-screen Commodore Amiga arena shooter. You're a
> little combat robot defending a reactor from drones. Give it chunky pixel art,
> a copper-gradient background, animated enemies and punchy sound effects.
> Use joystick movement and fire, with a score, three lives and restart after
> death. Keep it within the selected machine profile. Build it, inspect gameplay
> screenshots, and test movement, collisions, restart and frame timing before
> calling it done.

## Build, test, play

```sh
agk unit
agk build
agk test --update              # inspect these first baseline screenshots
agk test                       # compare subsequent runs with the baseline
agk run -s "press right 20" -s "screenshot moved"
agk play
```

`agk play` needs [FS-UAE](https://fs-uae.net/). On macOS:
`brew install --cask fs-uae-emulator`. It configures arrow keys and Space as a
joystick. The bootable game disk is `build/mygame.adf`.

Only update screenshot baselines when the new pictures are intentional. Ask
your agent to compare repeated runs and a fresh boot (`agk run --fresh`) too.

For a bigger machine and more colors, see the [A1200 / AGA guide](aga.md).
For working examples, browse the [demos](play-demos.md) and
[small technique projects](../techniques/README.md).
