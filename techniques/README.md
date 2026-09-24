# Techniques

Each directory here is a small, complete AGK game that demonstrates one
technique and is **tested on every profile**. Each also has a `TECHNIQUE.md`
covering:
- what the technique is and when it pays off
- the exact recipe
- the gotchas
- **measured frame cost**

Agents: copy the pattern from here rather than from memory. It's known to work.

| technique | what it shows | status |
|---|---|---|
| [bobs](bobs/TECHNIQUE.md) | Blitter objects: masked, double-buffered, background restore, no trails | ✅ tested, 18/18 |
| [scrolling](scrolling/TECHNIQUE.md) | Tile-map scrolling, 3 screens wide, camera follows the player; pixel-exact at odd offsets (ACE tilebuffer). Found and fixed an ACE fast-RAM bug | ✅ tested, 12/12 |
| [copper](copper/TECHNIQUE.md) | Sky gradient (58 colours on 4-colour screen), HUD palette split, moving raster bar; raw vs block mode (2–3% vs ~90% of a frame). Found and fixed an ACE line-255 bug | ✅ tested, 12/12 |
| [sprites](sprites/TECHNIQUE.md) | The art pipeline: text art → animated hardware sprite, PNG sheet → animated BOB; rules enforced, previews | ✅ tested, 12/12 |
| copper → sprite tricks | Horizontal sprite reuse (R-Type 2 style repeating backgrounds), vertical multiplexing (many bullets from few channels), sprite HUD, sprite parallax layer. See [codetapper's breakdowns](https://codetapper.com/amiga/sprite-tricks/) | planned |
| hot paths | C first, then a proven hot spot in asm, checked against the C version | planned |
| loading | Trackloader + packed data instead of AmigaDOS | planned |

Run one: `agk test techniques/<name>`. Measure one: every technique reports
`AGK perf` lines. See `runtime/include/agk/perf.h`.
