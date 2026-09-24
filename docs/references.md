# References: open-source Amiga games and demos

Real code from people who shipped fast Amiga games. Use it to learn how a
technique is done in practice. **Check the licence before reusing anything:**
- **None** means all rights reserved. Read and learn, but don't copy code.
- **Permissive** (MIT, BSD, Unlicense) means you may reuse it with attribution.

Source list: [grovdata/Amiga_Sources](https://github.com/grovdata/Amiga_Sources/blob/master/software.md).
Licences were checked with the GitHub API on 2026-09-24.

## Games

| project | platform / language | licence | worth studying for |
|---|---|---|---|
| [jotd666 arcade ports](https://github.com/jotd666) (pacman500, galaxian500, scramble500, xevious, gyruss, ~25 more) | A500 OCS, 68k asm | none | Stock-A500 arcade speed: BOB/sprite mixing, sprite multiplexing, 50 fps game loops, many small moving objects |
| [Knightmare](https://github.com/djh0ffman/KnightmareAmiga) | A500, 68k asm | none | A complete vertical shooter: scrolling, enemies, bullets, and tuning for the frame budget |
| [Stunt Car Racer (Framerate Unleashed)](https://github.com/Vesuri/stuntcarracer) | 68k asm | none | Speeding up a 3D engine; hot-path optimisation |
| [Alien Breed 3D II](https://github.com/mheyer32/alienbreed3d2) | AGA, 68k asm | none | Texture mapping and chunky rendering on AGA |
| [Gloom](https://github.com/earok/GloomAmiga) | 68k asm | unclear ("NOASSERTION") | Fast 3D on 020+ |
| [Blocky Skies](https://github.com/alpine9000/blockyskies) | A500, 68k asm | **BSD-2-Clause** | A small complete game you can learn from *and reuse* |

## Demos

| project | language | licence | worth studying for |
|---|---|---|---|
| [Planet Rocklobster](https://github.com/AxisOxy/Planet-Rocklobster) (Oxyron) | 68k asm | **Unlicense** | Copper, blitter and effect tricks at the edge of OCS; reusable |
| [System Zoetrope](https://github.com/astrofra/system-zoetrope-amiga-demo) (Mandarine) | **C** | **MIT** | Demo effects in C that stay system-friendly: close to AGK's style, and reusable |

## Technique write-ups (articles, no code)

These are copyrighted articles: link to them and learn from them, and describe
what you take in your own words.

| resource | what it explains |
|---|---|
| [codetapper: Amiga Sprite Tricks](https://codetapper.com/amiga/sprite-tricks/) | How 14 shipped games bent the sprite and copper hardware, with a per-game breakdown. See the list below. |
| [Lou's Pseudo 3d Page](http://www.extentofthejam.com/pseudo/) | Road-rendering techniques for racing games (Lotus and Outrun style): per-line road segments, curves, hills, sprite scaling. |

The codetapper breakdowns, by technique:
- **Horizontal sprite reuse:** the copper repositions a sprite later on the same line, so 8 sprites cover a screen-wide repeating pattern. Examples: R-Type 2 (18 sprites across 288 px), Jim Power, Risky Woods.
- **Sprites as a parallax layer** behind or between playfields: Videokid, Brian the Lion, Agony (dual playfield + sprites = 3 layers).
- **Vertical multiplexing:** chained control words let a few channels show many objects down the screen, e.g. Saint Dragon (4 sprites → 20+ bullets).
- **Sprite HUD / score panel,** leaving the playfield alone: Rod-Land (all 8 sprites as a 16-colour panel), Parasol Stars (with beam racing).
- **Sprites animating faster than the bitmap:** Stunt Car Racer's tyres. Shadow of the Beast plays priority tricks between playfields.

AGK's planned `techniques/` for these: `copper` (the basis for all of them), then
horizontal sprite reuse, vertical multiplexing, and a sprite HUD.

## How to use this with AGK

1. Find the technique in `techniques/` first. Those examples are tested, measured, and written for ACE and AGK.
2. When you need more (for example, how a shipped game multiplexes 30 sprites), read the relevant project above.
3. Re-implement the idea in your own code, and prove it with `agk test` and the perf meter.
