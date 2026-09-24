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
| scrolling | Tile-map scrolling that blits only the newly exposed edge (ACE tilebuffer) | planned |
| copper | Per-line colour changes, gradients, palette splits | planned |
| sprite multiplexing | Reusing the 8 sprite channels further down the screen | planned |
| hot paths | C first, then a proven hot spot in asm, checked against the C version | planned |
| loading | Trackloader + packed data instead of AmigaDOS | planned |

Run one: `agk test techniques/<name>`. Measure one: every technique reports
`AGK perf` lines. See `runtime/include/agk/perf.h`.
