"""agk profile: where each frame's time goes.

The patched emulator records, at the end of every raster line (15,625 a
second), where the CPU is (its program counter) and who used the bus in
that line: the CPU, the blitter, bitplane, copper and sprite DMA, the rest,
or nobody. It also knows when the game is working on a frame: from
agkPerfBegin (the tick) to agkPerfEnd. None of it changes the emulation.

The harness places the program counters in the game's code with the linker
map the build writes (build/NAME.map: every object file's address, every
global) and the objects' disassembly (their static functions too), anchored
by the runtime's g_szAgkPerfKey, whose address the emulator notes from the
first perf report. So the game must use agk/perf.h and run 50+ frames.

Code inlined into a bigger function counts as that function (-O3 inlines a
lot of statics): --disasm FUNC shows the samples per instruction.
"""
import bisect
import os
import re
import struct
import subprocess

LINES = 313                      # PAL lines per video frame
KINDS = ["free", "CPU", "blitter", "bitplanes", "copper", "sprites", "other", "blocked"]
WORKING, TICK, NEW_FRAME = 1 << 24, 1 << 25, 1 << 26
ANCHOR = "g_szAgkPerfKey"


class ProfileError(Exception):
    pass


# ------------------------------------------------------------------ reading

def load(path):
    """profile.bin -> (anchor address, [(pc | flags, (8 bus counts))] per line)"""
    data = open(path, "rb").read()
    if data[:4] != b"AGKP":
        raise ProfileError(f"{path} isn't a profile (no AGKP header)")
    version, anchor = struct.unpack_from("<II", data, 4)
    if version != 2:
        raise ProfileError(f"{path}: profile version {version}, expected 2 - rebuild the emulator (tools/setup)")
    words = struct.unpack_from(f"<{(len(data) - 12) // 4}I", data, 12)
    lines = []
    for i in range(0, len(words) - 2, 3):
        a, b = words[i + 1], words[i + 2]
        lines.append((words[i], (a & 255, a >> 8 & 255, a >> 16 & 255, a >> 24,
                                 b & 255, b >> 8 & 255, b >> 16 & 255, b >> 24)))
    return anchor, lines


def parse_map(text):
    """GNU ld map -> ({global: address}, [(base, size, object path)]) for .text"""
    symbols, objects = {}, []
    in_text = False
    for line in text.splitlines():
        if line.startswith(".text"):
            in_text = True
            continue
        if in_text and re.match(r"^\.\w", line):
            break
        if not in_text:
            continue
        m = re.match(r"^ \.text\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)", line)
        if m:
            objects.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
            continue
        m = re.match(r"^\s+0x([0-9a-f]+)\s+([A-Za-z_]\w*)\s*$", line)
        if m:
            symbols[m.group(2)] = int(m.group(1), 16)
    return symbols, objects


def parse_functions(dis):
    """objdump -d of several objects -> {object path: [(offset, name)]}"""
    out, obj = {}, None
    for line in dis.splitlines():
        m = re.match(r"^(\S+):\s+file format", line)
        if m:
            obj = m.group(1)
            out[obj] = []
            continue
        m = re.match(r"^([0-9a-f]+) <([^>+]+)>:$", line)
        if m and obj:
            name = m.group(2)
            out[obj].append((int(m.group(1), 16), name[1:] if name.startswith("_") else name))
    return out


class Symbols:
    """offset in .text -> (function, object)"""

    def __init__(self, symbols, objects, functions):
        entries = {}
        for base, size, path in objects:
            src = _source(path)
            entries.setdefault(base, (f"({src})", src))
            for off, name in functions.get(path, []):
                entries[base + off] = (name, src)
        owner = [(b, s, _source(p)) for b, s, p in objects]
        for name, addr in symbols.items():
            if addr not in entries or entries[addr][0].startswith("("):   # beats a placeholder
                src = next((o for b, s, o in owner if b <= addr < b + s), "")
                entries[addr] = (name, src)
        self.addrs = sorted(entries)
        self.names = [entries[a] for a in self.addrs]
        self.size = max((b + s for b, s, _ in objects), default=0)
        self.symbols = symbols
        self.objects = objects

    def object_of(self, off):
        """-> (base, path) of the object whose .text holds this offset"""
        for b, s, p in self.objects:
            if b <= off < b + s:
                return b, p
        return None, None

    def lookup(self, off):
        if not 0 <= off < self.size:
            return None
        i = bisect.bisect_right(self.addrs, off) - 1
        return self.names[i] if i >= 0 else None

    def start(self, name):
        for a, (n, _) in zip(self.addrs, self.names):
            if n == name:
                return a
        return None


def _source(path):
    # CMakeFiles/game.dir/src/main.c.obj -> src/main.c; ace/.../bob.c.obj -> ACE bob.c;
    # /opt/.../libnix.a(foo.o) -> libnix foo.o
    m = re.search(r"CMakeFiles/[^/]+\.dir/(.+)\.obj$", path)
    if m and not path.startswith(("ace/", "agk/")):
        return m.group(1)
    if path.startswith("ace/"):
        return "ACE " + os.path.basename(path)[:-4]
    m = re.search(r"([^/]+)\.a\(([^)]+)\)$", path)
    if m:
        return f"{m.group(1)} {m.group(2).replace('.obj', '')}"
    return os.path.basename(path)


def object_file(build, path):
    """A map's object path -> the object file in the build tree, or None.
    Archive members (agk/libagk.a(perf.c.obj)) are found next to the archive."""
    if path.endswith(".obj") and os.path.exists(os.path.join(build, path)):
        return path
    m = re.match(r"^(.*)/lib[^/]+\.a\(([^)]+\.obj)\)$", path)
    if m:
        for root, _, files in os.walk(os.path.join(build, m.group(1), "CMakeFiles")):
            if m.group(2) in files:
                return os.path.relpath(os.path.join(root, m.group(2)), build)
    return None


def symbols_for(proj, image):
    """The build's map + disassembly of the objects that are in the build directory"""
    build = os.path.join(proj["dir"], "build")
    mp = os.path.join(build, f"{proj['name']}.map")
    if not os.path.exists(mp):
        raise ProfileError(f"no linker map ({mp}): rebuild with this kit's tools/build (agk build)")
    symbols, objects = parse_map(open(mp).read())
    if ANCHOR not in symbols:
        raise ProfileError(f"{ANCHOR} isn't in {mp}: the game must link the kit's runtime (agk/perf.h)")
    files = {p: object_file(build, p) for _, _, p in objects}
    objs = sorted({f for f in files.values() if f})
    functions = {}
    if objs:
        r = subprocess.run(["docker", "run", "--rm", "-v", f"{build}:/b", "-w", "/b", image,
                            "m68k-amigaos-objdump", "-d", *objs], capture_output=True, text=True, errors="replace")
        if r.returncode != 0:
            raise ProfileError(f"objdump failed: {r.stderr.strip()[:500]}")
        by_file = parse_functions(r.stdout)
        functions = {p: by_file.get(f, []) for p, f in files.items() if f}
    return Symbols(symbols, objects, functions)


# ----------------------------------------------------------------- analysis

def place(pc, base, syms):
    """-> (function, source) for a PC"""
    if pc >= 0xF80000:
        return ("Kickstart ROM", "")
    hit = syms.lookup(pc - base)
    return hit or ("outside the game (OS, interrupts)", "")


def analyse(anchor, lines, syms):
    if not anchor:
        raise ProfileError("no perf report during the run: the game must call agkPerfBegin()/agkPerfEnd() "
                           "(agk/perf.h) every frame, and the scenario must run 50+ frames")
    base = anchor - syms.symbols[ANCHOR]
    frames = []          # game frames: {"start": line index, "work": lines, "bus": [...], "funcs": {}}
    cur = None
    all_funcs, work_funcs = {}, {}
    work_bus, all_bus = [0] * 8, [0] * 8
    for i, (v, bus) in enumerate(lines):
        pc, flags = v & 0xFFFFFF, v & ~0xFFFFFF
        if flags & TICK:
            cur = {"start": i, "work": 0, "bus": [0] * 8, "funcs": {}}
            frames.append(cur)
        fn = place(pc, base, syms)
        all_funcs[fn] = all_funcs.get(fn, 0) + 1
        for k in range(8):
            all_bus[k] += bus[k]
        if flags & WORKING and cur is not None:
            cur["work"] += 1
            cur["funcs"][fn] = cur["funcs"].get(fn, 0) + 1
            work_funcs[fn] = work_funcs.get(fn, 0) + 1
            for k in range(8):
                cur["bus"][k] += bus[k]
                work_bus[k] += bus[k]
    # the last frame may be cut off by the end of the run
    if frames and frames[-1]["work"] and lines and lines[-1][0] & WORKING:
        frames.pop()
    return {"base": base, "lines": len(lines), "frames": frames, "all_funcs": all_funcs,
            "work_funcs": work_funcs, "work_bus": work_bus, "all_bus": all_bus}


def _pct(n, total):
    return 100.0 * n / total if total else 0.0


def _bus_text(bus, top=4):
    total = sum(bus)
    parts = sorted(((b, k) for k, b in zip(KINDS, bus) if k != "free"), reverse=True)[:top]
    return ", ".join(f"{k} {_pct(b, total):.0f}%" for b, k in parts if b) + f", free {_pct(bus[0], total):.0f}%"


def report(a, top=12, worst=5):
    frames = a["frames"]
    out = []
    if not frames:
        return ["no game frames seen (no ticks): does the game call agkPerfBegin() every frame?"]
    loads = [_pct(f["work"], LINES) for f in frames]
    out.append(f"{len(frames)} game frames, {a['lines']} line samples")
    w = max(range(len(frames)), key=lambda i: loads[i])
    out.append(f"load (share of a 1/50 s frame spent working): average {sum(loads) / len(loads):.0f}%, "
               f"worst {loads[w]:.0f}% (game frame {w})")
    out.append(f"bus while working: {_bus_text(a['work_bus'], 6)}")
    out.append(f"bus overall:       {_bus_text(a['all_bus'], 6)}")
    total = sum(a["work_funcs"].values())
    out.append("")
    out.append("where the working time goes:")
    for (name, src), n in sorted(a["work_funcs"].items(), key=lambda x: -x[1])[:top]:
        out.append(f"  {_pct(n, total):5.1f}%  {name}  {src}")
    out.append("")
    out.append(f"the {worst} heaviest frames:")
    for i in sorted(range(len(frames)), key=lambda i: -loads[i])[:worst]:
        f = frames[i]
        fn_total = sum(f["funcs"].values())
        fns = ", ".join(f"{n} {_pct(c, fn_total):.0f}%" for (n, _), c in
                        sorted(f["funcs"].items(), key=lambda x: -x[1])[:4])
        out.append(f"  game frame {i}: {loads[i]:.0f}% | {_bus_text(f['bus'], 3)} | {fns}")
    return out


# -------------------------------------------------------------------- chart

def chart(a, path, height=160):
    """Load per game frame (1 px column each, or the worst of several), a line
    at 100%. The column's colour: mostly blitter (orange), mostly CPU (blue)."""
    from .image import Image
    frames = a["frames"]
    if not frames:
        return
    per = max(1, (len(frames) + 1599) // 1600)
    cols = [max(frames[i:i + per], key=lambda f: f["work"]) for i in range(0, len(frames), per)]
    w, h = len(cols), height
    top = 125                      # the chart goes to 125%
    px = bytearray([16, 16, 24] * w * h)
    y100 = h - 1 - int((h - 1) * 100 / top)
    for x, f in enumerate(cols):
        load = min(top, _pct(f["work"], LINES))
        bus = f["bus"]
        colour = (255, 150, 40) if bus[2] > bus[1] else (80, 150, 255)
        if load > 100:
            colour = (255, 60, 60)
        for y in range(h - 1 - int((h - 1) * load / top), h):
            px[(y * w + x) * 3:(y * w + x) * 3 + 3] = bytes(colour)
        px[(y100 * w + x) * 3:(y100 * w + x) * 3 + 3] = bytes((230, 230, 230))
    Image(bytes(px), w, h).save_png(path)


# ------------------------------------------------------------ disassembly

def annotate(a_lines, anchor, syms, name, disasm_text):
    """Samples per instruction of one function: objdump -d text of its object -> lines"""
    start = syms.start(name)
    if start is None:
        raise ProfileError(f"no function '{name}' in the map or the objects")
    i = syms.addrs.index(start)
    end = syms.addrs[i + 1] if i + 1 < len(syms.addrs) else syms.size
    base = anchor - syms.symbols[ANCHOR]
    counts, idle = {}, 0
    for v, _ in a_lines:
        off = (v & 0xFFFFFF) - base
        if start <= off < end:
            if v & WORKING:
                counts[off] = counts.get(off, 0) + 1
            else:
                idle += 1
    total = sum(counts.values())
    obj_base, _ = syms.object_of(start)
    out = [f"{name}: {total} samples while the game works on a frame (the % below)"
           + (f", {idle} more outside that (waiting for the next frame, interrupts)" if idle else "")]
    inside = False
    for line in disasm_text.splitlines():
        m = re.match(r"^([0-9a-f]+) <([^>]+)>:$", line)
        if m:
            fn = m.group(2)
            inside = (fn[1:] if fn.startswith("_") else fn) == name
            continue
        m = re.match(r"^\s+([0-9a-f]+):\t(.*)$", line)
        if inside and m:
            off = obj_base + int(m.group(1), 16)
            n = counts.get(off, 0)
            out.append(f"{_pct(n, total):5.1f}% {n:6d}  {line.strip()}" if n else f"{'':13}  {line.strip()}")
    return out
