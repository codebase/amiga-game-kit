"""Reading a game that isn't ours: any ADF, booted and scripted like a kit
game, with the sampled program counter placed by address instead of by
symbol, and a disassembler on the memory dump.

    agk inspect GAME.adf --boot 45 -s "press fire" -s "wait 500" -s "screenshot title" \\
        -s "snapshot title" -s "dump-chip ram"
    agk inspect GAME.adf --from build/inspect/title.vasnap -s "wait 1000" --sample
    agk inspect GAME.adf --from ... --sample --disasm 0x21D00

A kit game prints 'AGK ready'; a foreign one prints nothing, so --boot waits
that many seconds after power-on instead (the boot is cached, like a kit
game's). Screenshots, snapshots ('snapshot NAME' in the steps) and memory
dumps ('dump-chip NAME': the 512 KB of chip RAM) land in the output
directory; --sample records the program counter on every raster line and
reports where the time goes by 256-byte region and by frame; --disasm ADDR
disassembles the dump there (objdump; IRA for whole executables).
"""
import os
import subprocess
from collections import Counter

from . import profile as prof

CHIP = 0x80000


def hot_regions(lines, top=16, grain=256):
    """-> [(address, share)] of the sampled program counter, outside the ROM"""
    pcs = [v & 0xFFFFFF for v, _ in lines]
    n = len(pcs) or 1
    rom = sum(1 for p in pcs if p >= 0xF80000)
    bins = Counter(p // grain * grain for p in pcs if p < 0xF80000)
    lo = min((p for p in pcs if p < 0xF80000), default=0)
    hi = max((p for p in pcs if p < 0xF80000), default=0)
    return {"rom": rom / n, "regions": [(a, c / n) for a, c in bins.most_common(top)], "span": (lo, hi)}


def frame_sequence(lines, frame=-2, grain=256):
    """The order in which code regions ran in one video frame (default: the
    last whole one): [(address, raster lines)]"""
    starts = [i for i, (v, _) in enumerate(lines) if v & prof.NEW_FRAME]
    if len(starts) < 2:
        return []
    i0 = starts[frame - 1] if frame < 0 else starts[frame]
    i1 = starts[frame] if frame < 0 else starts[frame + 1]
    seq = []
    for v, _ in lines[i0:i1]:
        a = (v & 0xFFFFFF) // grain * grain
        if seq and seq[-1][0] == a:
            seq[-1][1] += 1
        else:
            seq.append([a, 1])
    return [tuple(s) for s in seq]


def bus_share(lines):
    bus = [0] * 8
    for _, b in lines:
        for k in range(8):
            bus[k] += b[k]
    t = sum(bus) or 1
    return [(k, v / t) for k, v in zip(prof.KINDS, bus)]


def report(lines):
    out = [f"{len(lines)} line samples, {len(lines) // 313} frames"]
    h = hot_regions(lines)
    out.append(f"program counter: {100 * h['rom']:.1f}% in the Kickstart ROM; "
               f"the game's code spans ${h['span'][0]:06X}..${h['span'][1]:06X}")
    out.append("bus: " + ", ".join(f"{k} {100 * v:.0f}%" for k, v in bus_share(lines) if v))
    if h["rom"] > 0.5:
        out.append("  (mostly in the ROM: the OS, not the game - still loading or on a menu? "
                   "raise --boot, or script the way in and sample from a snapshot)")
    out.append("")
    out.append("hot 256-byte regions (share of all samples; --disasm ADDR reads one):")
    for a, s in h["regions"]:
        out.append(f"  ${a:06X}  {100 * s:5.1f}%")
    seq = frame_sequence(lines)
    if seq:
        out.append("")
        out.append("one frame, the regions in order (raster lines each), the loop's shape:")
        shown = " ".join(f"${a:05X}x{n}" for a, n in seq[:60])
        out.append("  " + shown + (" ..." if len(seq) > 60 else ""))
    return out


def disassemble(dump, addr, length, image, workdir):
    """objdump on a memory dump, as 68000 code at its real addresses"""
    rel = os.path.relpath(dump, workdir)
    r = subprocess.run(["docker", "run", "--rm", "-v", f"{workdir}:/w", "-w", "/w", image,
                        "m68k-amigaos-objdump", "-b", "binary", "-m", "m68k:68000", "-D",
                        f"--start-address={addr:#x}", f"--stop-address={addr + length:#x}", rel],
                       capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        raise RuntimeError(f"objdump failed: {r.stderr.strip()[:300]}")
    out = []
    for line in r.stdout.splitlines():
        if line and line[0] == " " and ":" in line:
            a, _, rest = line.partition(":")
            parts = rest.split("\t")
            out.append(f"${int(a, 16):06X}  {parts[-1].strip() if len(parts) > 1 else rest.strip()}")
    return out
