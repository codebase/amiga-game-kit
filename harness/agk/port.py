"""agk port: a Mega Drive game's 68000 code rewritten to run on an Amiga.

The CPU is the same, so the code itself can stay; what can't is where things
are. On the Mega Drive the work RAM is at $FF0000 (reached mostly through
sign-extended 16-bit addresses, $FFFF8000..$FFFFFFFF) and the hardware at
$A00000..$C0001F; on an Amiga those addresses are the Kickstart ROM and
expansion space. So the game's code is rewritten, instruction by instruction,
from the disassembly:

  memory map of the port (everything below `top` is the Amiga's RAM)

    0 .. romsize          the ROM, at its own addresses: data never moves, so
                          every pointer and table in it stays valid. The old
                          code in it is dead; each place a table or a vector
                          can still send the CPU has a jump to the new code
    ram .. ram+$10000     the game's work RAM. ram is a multiple of $10000, so
                          a pointer's low word is what it was on the Mega Drive
                          (games store pointers as words)
    ram+$10000 (= D) ..   a copy of the ROM's first 32K: a word pointer the game
                          sign-extends into an address register gets D added,
                          which lands in RAM when the word was $8000..$FFFF and
                          here when it was a ROM address
    shim                  the hardware layer's page: a cell the rewritten
                          instructions exchange values through, and the entry
                          points they call (read/write 8/16/32 bits at a
                          hardware address). On the Amiga these are the port's
                          VDP, pad and sound code; in the kit's verifier they
                          are the emulated Mega Drive chips
    code ..               the rewritten code

  rewriting rules (all 68000; the condition codes are never disturbed)

    $FFxxxx operands      -> ram + xxxx
    a hardware operand    -> the instruction works on the shim's cell, with a
                          call before it (a read) or after it (a write) that
                          moves the cell from or to the hardware address. Which
                          instructions those are comes from the analysis run
                          (hwsites.txt) and from absolute operands
    x(pc) data            -> the absolute address (the data stayed where it was)
    x(pc,dn) data, jumps  -> through a scratch address register kept on the stack
    movea.w ea,An         -> followed by adda.l #D,An (see D above)
    cmpa.w ea,An          -> An brought back by D around it

What the analysis run didn't execute and the branches don't reach is left as
it was: `agk port` says how much code it has, and the verifier reports the
first time anything reaches for the old RAM or hardware addresses, or enters
old code somewhere unexpected, with the pc.
"""
import collections
import json
import os
import re
import struct
import subprocess

from . import recomp


class PortError(Exception):
    pass


POISON = 0x4AFC                           # ILLEGAL: old code no known path enters
SHIM_ENTRIES = 0x100                      # the entry points start here in the shim page, 8 bytes apart
SHIM_NAMES = ("r8", "r16", "r32", "w8", "w16", "w32", "fixa0", "fixa1", "fixa2", "fixa3", "fixa4", "fixa5", "fixa6")

BRANCH = re.compile(r"b(ra|sr|hi|ls|cc|cs|ne|eq|vc|vs|pl|mi|ge|lt|gt|le)([swl]?)$")
DBCC = re.compile(r"db(f|t|ra|hi|ls|cc|cs|ne|eq|vc|vs|pl|mi|ge|lt|gt|le)$")
ABS = re.compile(r"0x([0-9a-f]+)$")
IMM = re.compile(r"#(-?\d+)$")
PCREL = re.compile(r"%pc@\(0x([0-9a-f]+)\)$")
PCIDX = re.compile(r"%pc@\(0x([0-9a-f]+),((?:d|a)[0-7]|sp):([wl])(?::[1248])?\)$")
AREG = re.compile(r"\b(a[0-7]|sp|fp)\b")
GARBAGE = re.compile(r"\)@\(|:[248]\)|@\(0x[0-9a-f]+\)@|\[|^@\(")   # (68020 addressing: what data looks like when read as code)
NO_FALL = ("rts", "rte", "rtr", "jmp", "illegal", ".short", ".long")
SIZES = {"b": 8, "w": 16, "l": 32}


def layout(romsize):
    base = (romsize + 0xFFFF) & ~0xFFFF
    return {"romsize": romsize, "ram": base, "D": base + 0x10000, "mirror": base + 0x10000,
            "shim": base + 0x18000, "cell": base + 0x18000, "code": base + 0x20000}


def is_ram(a):
    a &= 0xFFFFFFFF
    return a >= 0xFFE00000 or 0xE00000 <= a <= 0xFFFFFF


def is_hw(a):
    a &= 0xFFFFFFFF
    return (a >> 24) in (0, 0xFF) and 0xA00000 <= (a & 0xFFFFFF) < 0xE00000


def base_size(mn):
    """'movew' -> ('move', 16); mnemonics without a size -> (mn, None)"""
    b, bits = recomp.mnemonic_size(mn)
    return b, bits


def read_bits(path):
    """the addresses set in a coverage-style bitmap"""
    out = set()
    if not os.path.exists(path):
        return out
    for i, b in enumerate(open(path, "rb").read()):
        if b:
            for k in range(8):
                if b >> k & 1:
                    out.add(i * 8 + k)
    return out


def read_hwsites(path):
    """hwsites.txt -> {pc: {'r', 'w'}}"""
    out = collections.defaultdict(set)
    if os.path.exists(path):
        for line in open(path):
            p = line.split()
            if len(p) == 4:
                out[int(p[0], 16)].add(p[1])
    return out


def read_hints(path):
    """port.txt: `entry HEX`, `raw HEX`, `pre HEX asm`, `post HEX asm`, `replace HEX asm; asm`,
    `reloc HEX` (the long in the ROM at HEX is a RAM pointer), `data HEX` (not code, whatever reached it)"""
    h = {"entry": set(), "raw": set(), "pre": collections.defaultdict(list), "post": collections.defaultdict(list),
         "replace": {}, "reloc": set(), "data": set(), "fixa": collections.defaultdict(set)}
    if os.path.exists(path):
        for n, line in enumerate(open(path), 1):
            line = line.split(";;")[0].strip()
            if not line or line.startswith("#"):
                continue
            p = line.split(None, 2)
            try:
                kind, addr = p[0], int(p[1], 16)
            except (IndexError, ValueError):
                raise PortError(f"{path}:{n}: expected `KIND HEXADDR [text]`, got {line!r}")
            if kind in ("entry", "raw", "reloc", "data"):
                h[kind].add(addr)
            elif kind in ("pre", "post"):
                h[kind][addr].append(p[2])
            elif kind == "fixa":          # after this instruction the register may hold a Mega Drive RAM address: bring it over
                if not re.fullmatch(r"a[0-6]", p[2].strip()):
                    raise PortError(f"{path}:{n}: fixa takes a0..a6")
                h["fixa"][addr].add(p[2].strip())
            elif kind == "replace":
                h["replace"][addr] = [s.strip() for s in p[2].split(";")]
            else:
                raise PortError(f"{path}:{n}: unknown hint {kind!r} (entry, raw, pre, post, replace, reloc, data, fixa)")
    return h


# ----------------------------------------------------------- what is code

def gas_operand(o):
    """objdump prints an indexed operand's displacement in bare hex (a0@(1e,d2:w), 64-bit when negative);
    the assembler wants a number"""
    m = re.fullmatch(r"((?:a[0-7]|sp|fp)@\()([0-9a-f]+)(,.*)", o)
    if not m:
        return o
    v = int(m.group(2), 16) & 0xFF
    return f"{m.group(1)}{v - 256 if v > 127 else v}{m.group(3)}"


def direct_target(mn, ops):
    """the address a branch, a dbcc or a jmp/jsr to a fixed place goes to, or None"""
    if BRANCH.match(mn) or DBCC.match(mn):
        m = ABS.match(ops[-1])
        return int(m.group(1), 16) if m else None
    if mn in ("jmp", "jsr"):
        m = ABS.match(ops[0]) or PCREL.match(ops[0])
        return int(m.group(1), 16) if m else None
    return None


def falls_through(mn):
    return not (mn in NO_FALL or mn.startswith("bra"))


def table_targets(by_addr, order, rom, known, data=frozenset()):
    """Jump tables found in the code -> (the addresses they lead to, the bytes the tables occupy):
    `move.w T(pc,dn),dm` then `jmp/jsr T(pc,dm)`: word offsets from T, up to the nearest thing they point at;
    `jmp/jsr T(pc,dn)` onto a row of bra.w or jmp instructions: each of them;
    `movea.l T(pc,dn),am` then `jmp/jsr (am)`: longs, while they are addresses in the ROM."""
    out, tbytes = set(), set()
    romsize = len(rom)
    for i, a in enumerate(order):
        _, mn, ops = by_addr[a]
        if mn in ("jmp", "jsr") and PCIDX.match(ops[0]):
            m = PCIDX.match(ops[0])
            t, idx = int(m.group(1), 16), m.group(2)
            op = struct.unpack(">H", rom[t:t + 2])[0]
            if op in (0x6000, 0x4EF9, 0x4EF8, 0x4EFA):     # a row of jumps
                step = 6 if op == 0x4EF9 else 4
                k = 0
                while struct.unpack(">H", rom[t + step * k:t + step * k + 2])[0] == op and k < 256:
                    out.add(t + step * k)
                    k += 1
                continue
            prev = [by_addr[x] for x in order[max(0, i - 2):i]]
            if not any(m2 == "movew" and PCIDX.match(o2[0]) and int(PCIDX.match(o2[0]).group(1), 16) == t and o2[1] == idx
                       for _, m2, o2 in prev):
                continue
            limit, k, got = romsize, 0, []
            while t + 2 * k + 1 < min(limit, romsize) and k < 1024:
                if k and (t + 2 * k) in known:
                    break
                tgt = t + struct.unpack(">h", rom[t + 2 * k:t + 2 * k + 2])[0]
                if tgt & 1 or not 0x200 <= tgt < romsize:
                    break
                if tgt > t:
                    limit = min(limit, tgt)
                got.append(tgt)
                k += 1
            out |= set(got)
            tbytes |= set(range(t, t + 2 * k))
        elif mn == "moveal" and PCIDX.match(ops[0]) and re.fullmatch(r"a[0-6]", ops[1]):
            nxt = [by_addr[x] for x in order[i + 1:i + 4]]
            if not any(m2 in ("jmp", "jsr") and o2[0] == ops[1] + "@" for _, m2, o2 in nxt):
                continue
            t = int(PCIDX.match(ops[0]).group(1), 16)
            # (the index often starts at 4: object numbers from 1; look a few entries either side)
            for start in (t, t + 4):
                k = 0
                while start + 4 * k + 3 < romsize and k < 1024:
                    if (start + 4 * k) in known:
                        break
                    tgt = struct.unpack(">I", rom[start + 4 * k:start + 4 * k + 4])[0]
                    if tgt & 1 or not 0x200 <= tgt < romsize:
                        break
                    out.add(tgt)
                    k += 1
                tbytes |= set(range(start, start + 4 * k))
        elif mn == "moveal" and re.fullmatch(r"a[0-6]", ops[1]) and re.fullmatch(r"(a[0-6])@\((-?\d+|0x[0-9a-f]+),d[0-7]:[wl](:1)?\)", ops[0]):
            # `lea T,an` then `movea.l (d,an,dm),ak` then `jmp/jsr (ak)`: longs from T + d, as above
            base = ops[0][:2]
            m = re.search(r"\((-?\d+|0x[0-9a-f]+),", ops[0])
            d = int(m.group(1), 0)
            nxt = [by_addr[x] for x in order[i + 1:i + 4]]
            if not any(m2 in ("jmp", "jsr") and o2[0] == ops[1] + "@" for _, m2, o2 in nxt):
                continue
            prev = [by_addr[x] for x in order[max(0, i - 3):i]]
            ts = [int((ABS.match(o2[0]) or PCREL.match(o2[0])).group(1), 16) for _, m2, o2 in prev
                  if m2 == "lea" and len(o2) == 2 and o2[1] == base and (ABS.match(o2[0]) or PCREL.match(o2[0]))]
            if not ts or not 0x200 <= ts[-1] + d < romsize:
                continue
            start, k = ts[-1] + d, 0
            while start + 4 * k + 3 < romsize and k < 1024:
                if k and (start + 4 * k) in known:
                    break
                tgt = struct.unpack(">I", rom[start + 4 * k:start + 4 * k + 4])[0]
                if tgt & 1 or not 0x200 <= tgt < romsize:
                    break
                out.add(tgt)
                k += 1
            tbytes |= set(range(start, start + 4 * k))
    return out, tbytes


def find_code(rom_path, analysis, image, hints, log=print):
    """the instructions that are certainly code: executed, or reached from executed code by branches,
    calls, jump tables and the analysis run's computed jumps -> ({addr: (bytes, mn, ops)}, entries)"""
    rom = open(rom_path, "rb").read()
    images = [(0, rom_path, len(rom))]
    cov = sorted(os.path.join(analysis, f) for f in os.listdir(analysis) if f.startswith("coverage") and f.endswith(".bin"))
    ranges, covered = recomp.coverage_ranges(cov) if cov else ([], set())
    if not covered:
        raise PortError(f"no coverage in {analysis}: run the game there first (game --play --analysis {analysis})")
    covered = {a for a in covered if a < len(rom)}
    ranges = [(lo, min(hi, len(rom))) for lo, hi in ranges if lo < len(rom)]
    dyn = {a for a in read_bits(os.path.join(analysis, "entries.bin")) if a < len(rom) and not a & 1}
    vectors = {struct.unpack(">I", rom[v:v + 4])[0] for v in range(4, 0x100, 4)}
    vectors = {v for v in vectors if 0x200 <= v < len(rom) and not v & 1}
    truth = set(dyn) | vectors | hints["entry"]
    instrs = recomp.disassemble(images, ranges, image, analysis, covered)
    # two executed instructions can share bytes (Thunder Force IV's boot enters two bytes before its
    # reset path's first instruction, and the reset enters at it): the decode keeps one, the other is added
    have = {i[0] for i in instrs}
    for a in sorted(covered - have):
        instrs += [i for i in recomp.disassemble(images, [(a, a + 1)], image, analysis) if i[0] == a]
    instrs.sort()
    # what the game read of its ROM as data: every byte read that isn't part of an executed instruction
    by0 = {a: 2 * w for a, w, mn, ops in instrs}
    read = set()
    rr = os.path.join(analysis, "romread.bin")
    if os.path.exists(rr):
        tags = open(rr, "rb").read()[:len(rom)]
        count = collections.Counter(tags)
        bulk = {t for t, n in count.items() if t not in (0, 255) and n > len(rom) // 4}   # a checksum loop: it reads code too
        read = {i for i, t in enumerate(tags) if t and t not in bulk}
    for a in covered:
        for b in range(a, a + by0.get(a, 2)):
            read.discard(b)
    data = read | hints["data"]
    candidates, tables = set(), set()
    code = set()
    # routines that read their data from after the call (a string to print) and return past it: known
    # from the calls that ran, where what follows the call never executed
    data_callees = set()
    for a, w, mn, ops in instrs:
        if a in covered and (mn == "jsr" or mn.startswith("bsr")) and (a + 2 * w) not in covered:
            t = direct_target(mn, [gas_operand(o) for o in recomp.split_operands(ops)] if ops else [])
            if t is not None:
                data_callees.add(t)
    for _ in range(16):
        instrs, added = recomp.follow_branches(images, instrs, image, analysis, anchors=sorted(truth | candidates))
        by_addr = {a: (2 * w, mn, [gas_operand(o) for o in recomp.split_operands(ops)] if ops else []) for a, w, mn, ops in instrs}

        def walk(start, accepted, owner):
            """the instructions reached from start that aren't accepted yet, or None if the path can't be
            code: an undecodable or 68020-only instruction, bytes the game reads as data, or out of step
            with accepted code"""
            seen, todo = set(), [start]
            while todo:
                a = todo.pop()
                if a in accepted or a in seen:
                    continue
                if a not in by_addr or a & 1 or a >= len(rom):
                    if os.environ.get("PORT_DEBUG_WALK"): print(f"walk {start:06X}: {a:06X} not disassembled")
                    return None
                n, mn, ops = by_addr[a]
                if a not in covered:
                    if mn in (".short", ".long", "illegal") or any(GARBAGE.search(o) for o in ops):
                        if os.environ.get("PORT_DEBUG_WALK"): print(f"walk {start:06X}: {a:06X} {mn} {ops} isn't an instruction")
                        return None
                    if any(b in data or owner.get(b, a) != a for b in range(a, a + n)):
                        if os.environ.get("PORT_DEBUG_WALK"): print(f"walk {start:06X}: {a:06X} {mn} is data the game reads, or overlaps code")
                        return None
                seen.add(a)
                t = direct_target(mn, ops)
                if t is not None and not is_ram(t):
                    todo.append(t)
                if falls_through(mn) and not (a not in covered and t in data_callees):
                    todo.append(a + n)    # (not after a call to a routine that takes its data from after the call)
            return seen

        code, owner = set(), {}
        def accept(seen):
            for a in seen:
                code.add(a)
                for b in range(a, a + by_addr[a][0]):
                    owner[b] = a
        # what ran is code, and what it leads to (a path from executed code can still fail where the
        # disassembly ran into data after a branch that is never taken: it is cut there)
        for a in sorted(covered | truth):
            if a in by_addr:
                accept({a})
        for a in sorted(covered | truth):
            if a in by_addr:
                n, mn, ops = by_addr[a]
                nxt = ([direct_target(mn, ops)] if direct_target(mn, ops) is not None else []) + ([a + n] if falls_through(mn) else [])
                for t in nxt:
                    seen = walk(t, code, owner)
                    if seen:
                        accept(seen)
        rejected = 0
        for c in sorted(candidates):
            seen = walk(c, code, owner)
            if seen is None:
                rejected += 1
            else:
                accept(seen)
        order = sorted(code)
        found, tbytes = table_targets(by_addr, order, rom, code, data)
        # a routine's address stored in memory (move.l #routine,somewhere: an interrupt's handler for
        # this screen, an object's code) is code too, whether or not the analysis run came that way
        for a in order:
            n, mn, ops = by_addr[a]
            if mn == "movel" and len(ops) == 2 and IMM.match(ops[0]) and not re.fullmatch(r"[ad][0-7]|sp|fp", ops[1]):
                v = int(IMM.match(ops[0]).group(1)) & 0xFFFFFFFF
                if 0x200 <= v < len(rom) and not v & 1 and v not in data and v not in code:
                    found.add(v)
        found = found - truth - candidates
        tables = candidates & code
        if not found and tbytes <= data:
            break
        candidates |= found
        data |= tbytes
    anchors = (truth | candidates)
    log(f"  ({rejected} jump-table candidates turned out to be data)")
    log(f"code: {len(code)} instructions ({len(covered)} executed, the rest reached through branches, "
        f"{len(dyn)} computed jumps seen and {len(tables)} jump-table entries)")
    return {a: by_addr[a] for a in code}, anchors & code, rom, covered


# ------------------------------------------- hardware through a register

MEMBASE = re.compile(r"(a[0-7]|sp|fp)@")
REGNAME = {"fp": 6, "sp": 7, **{f"a{i}": i for i in range(8)}}
POOL = False
NOT, HW = 1, 2                            # an address register's value: not a hardware address / one (3: either)


def _reglist(text):
    """'d0-d7/a0-fp' -> the address registers in it (0..7)"""
    out = set()
    for part in text.split("/"):
        ends = part.split("-")
        if any(e not in REGNAME and not re.fullmatch(r"d[0-7]", e) for e in ends):
            return None
        if all(e in REGNAME for e in ends):
            out |= set(range(REGNAME[ends[0]], REGNAME[ends[-1]] + 1))
        elif len(ends) == 2 and ends[1] in REGNAME:       # d5-a3
            out |= set(range(0, REGNAME[ends[1]] + 1))
    return out


def hw_reach(code, entries, traced, vectors=()):
    """Which instructions may reach hardware through an address register, found without running
    them: where a hardware address is loaded into a register (lea $C00000,a6) it is followed
    through the branches, into the routines called with it and back out of them. The analysis run
    only knows the instructions it executed; a path it never played (a counter that never changed)
    would otherwise write to the Mega Drive's addresses on the Amiga, where nothing answers.

    -> {addr: {operand index: HW or HW|NOT}}. An operand marked "either" is wrapped too: the layer's
    entry points serve an ordinary address as well, only slower.

    The state is, per address register, NOT / HW / both, and what was last pushed of it (so a
    routine that saves a register, uses it for the VDP and restores it gives back what it got).
    Pointers loaded from memory or computed in data registers count as NOT; a traced site says HW
    for its register whatever the analysis thought. Routines reached only through computed jumps
    start from what all the computed jumps and calls had, joined."""
    order = sorted(code)
    nxt = {a: a + code[a][0] for a in order}
    info = {}                              # a -> (base, bits, ops normalised)
    for a in order:
        n, mn, ops = code[a]
        base, bits = base_size(mn)
        info[a] = (base, bits)
    def kind(a):
        """-> ('flow', [succ...]) | ('call', target, after) | ('icall', after) | ('ijmp',) | ('ret',)"""
        n, mn, ops = code[a]
        if BRANCH.match(mn):
            t = int(ABS.match(ops[0]).group(1), 16)
            cc = BRANCH.match(mn).group(1)
            if cc == "sr":
                return ("call", t, a + n)
            return ("flow", [t] if cc == "ra" else [t, a + n])
        if DBCC.match(mn):
            return ("flow", [int(ABS.match(ops[1]).group(1), 16), a + n])
        if mn in ("jmp", "jsr"):
            t = direct_target(mn, ops)
            if t is None:
                return ("icall", a + n) if mn == "jsr" else ("ijmp",)
            return ("call", t, a + n) if mn == "jsr" else ("flow", [t])
        if mn in ("rts", "rte", "rtr"):
            return ("ret",)
        if not falls_through(mn):
            return ("ijmp",)
        return ("flow", [a + n])
    kinds = {a: kind(a) for a in order}

    def writes(a):
        """the address registers whose kind of value the instruction at a sets (not through a pop)"""
        n, mn, ops = code[a]
        base, bits = info[a]
        if base in ("lea", "movea", "link") and ops and ops[-1] in REGNAME:
            if base == "movea" and ops[0] in ("sp@+",):
                return set()
            return {REGNAME[ops[-1] if base != "link" else ops[0]]} if base != "link" else {REGNAME[ops[0]]}
        if base == "unlk":
            return {REGNAME[ops[0]]}
        if base == "exg":
            return {REGNAME[o] for o in ops if o in REGNAME}
        if base == "movem" and len(ops) == 2 and "@" in ops[0] and not ops[0].startswith("sp@"):
            return _reglist(ops[1]) or set()
        return set()

    # the routines: what each call target reaches without calling, and the registers it sets
    targets = {k[1] for k in kinds.values() if k[0] == "call" and k[1] in code}
    body = {}
    for f in targets:
        seen, todo = set(), [f]
        while todo:
            a = todo.pop()
            if a in seen or a not in code:
                continue
            seen.add(a)
            k = kinds[a]
            if k[0] == "flow":
                todo += k[1]
            elif k[0] in ("call", "icall"):
                todo.append(k[-1])
        body[f] = seen
    popped = {f: set() for f in targets}
    mod = {f: set() for f in targets}
    for f in targets:
        for a in body[f]:
            n, mn, ops = code[a]
            base, bits = info[a]
            mod[f] |= writes(a)
            if len(ops) == 2 and ops[0] == "sp@+":
                if base == "movem":
                    popped[f] |= _reglist(ops[1]) or set()
                elif ops[1] in REGNAME:
                    popped[f].add(REGNAME[ops[1]])
    changed = True
    while changed:                         # and what the routines it calls set
        changed = False
        for f in targets:
            for a in body[f]:
                k = kinds[a]
                if k[0] == "call" and k[1] in mod and not mod[k[1]] - popped[k[1]] <= mod[f]:
                    mod[f] |= mod[k[1]] - popped[k[1]]
                    changed = True
    net = {f: (mod[f] - popped[f]) - {7} for f in targets}
    rets = collections.defaultdict(set)    # a return -> the routines it ends
    for f in targets:
        for a in body[f]:
            if kinds[a][0] == "ret":
                rets[a].add(f)
    callers = collections.defaultdict(set)
    for a, k in kinds.items():
        if k[0] == "call":
            callers[k[1]].add(a)

    BOT = (0,) * 16                        # registers 0..7, then what was pushed of each
    START = (NOT,) * 8 + (0,) * 8
    join = lambda x, y: tuple(p | q for p, q in zip(x, y))
    IN = collections.defaultdict(lambda: BOT)
    EXIT = collections.defaultdict(lambda: BOT)
    pool = BOT
    work = []
    def flow(a, st):
        if a in code:
            j = join(IN[a], st)
            if j != IN[a]:
                IN[a] = j
                work.append(a)
    def step(a, st):
        """the state after the instruction at a (calls aside)"""
        n, mn, ops = code[a]
        base, bits = info[a]
        r, saved = list(st[:8]), list(st[8:])
        val = lambda o: (r[REGNAME[o]] if o in REGNAME else None)
        if base == "lea" and ops[-1] in REGNAME:
            m = ABS.match(ops[0])
            mb = MEMBASE.match(ops[0])
            r[REGNAME[ops[1]]] = (HW if is_hw(int(m.group(1), 16)) else NOT) if m else r[REGNAME[mb.group(1)]] if mb else NOT
        elif base == "movea" and ops[-1] in REGNAME:
            d = REGNAME[ops[1]]
            m = IMM.match(ops[0])
            if ops[0] == "sp@+":
                r[d] = saved[d] or r[d]
            elif bits == 32 and m:
                r[d] = HW if is_hw(int(m.group(1))) else NOT
            elif bits == 32 and ops[0] in REGNAME:
                r[d] = r[REGNAME[ops[0]]]
            else:
                r[d] = NOT
        elif base == "move" and len(ops) == 2 and ops[1] == "sp@-" and ops[0] in REGNAME:
            saved[REGNAME[ops[0]]] = r[REGNAME[ops[0]]]
        elif base == "pea" and MEMBASE.match(ops[0]) and ops[0].endswith("@"):
            saved[REGNAME[MEMBASE.match(ops[0]).group(1)]] = r[REGNAME[MEMBASE.match(ops[0]).group(1)]]
        elif base == "movem" and len(ops) == 2:
            if ops[1] == "sp@-":
                for x in _reglist(ops[0]) or ():
                    saved[x] = r[x]
            elif ops[0] == "sp@+":
                for x in _reglist(ops[1]) or ():
                    r[x] = saved[x] or r[x]
            elif "@" in ops[0] or ABS.match(ops[0]):
                for x in _reglist(ops[1]) or ():
                    r[x] = NOT
        elif base == "exg":
            a0, a1 = (REGNAME.get(o) for o in ops)
            v0, v1 = (r[a0] if a0 is not None else NOT), (r[a1] if a1 is not None else NOT)
            if a0 is not None: r[a0] = v1
            if a1 is not None: r[a1] = v0
        elif base in ("link", "unlk"):
            r[REGNAME[ops[0]]] = NOT
        r[7] = NOT
        return tuple(r) + tuple(saved)
    def forced(a, st):
        """a traced site: its register held a hardware address"""
        if a in traced:
            n, mn, ops = code[a]
            cand = [MEMBASE.match(o).group(1) for o in ops if MEMBASE.match(o)]
            if len(cand) == 1 and cand[0] != "sp":
                r = list(st)
                r[REGNAME[cand[0]]] |= HW
                return tuple(r)
        return st
    def run():
        nonlocal pool
        while work:
            a = work.pop()
            st = forced(a, IN[a])
            k = kinds[a]
            if k[0] == "flow":
                out = step(a, st)
                for t in k[1]:
                    flow(t, out)
            elif k[0] == "call":
                f = k[1]
                if f not in code:
                    flow(k[2], st)
                    continue
                flow(f, st[:8] + (0,) * 8)
                ex = EXIT[f]
                if any(ex[x] == 0 for x in net[f]):
                    continue                # (it hasn't returned yet)
                flow(k[2], tuple(ex[x] if x in net[f] else st[x] for x in range(8)) + st[8:])
            elif k[0] in ("icall", "ijmp"):
                if POOL:
                    j = join(pool, st[:8] + (0,) * 8)
                    if j != pool:
                        pool = j
                        for e in entries:
                            flow(e, pool)
                if k[0] == "icall":
                    flow(k[1], st)
            elif k[0] == "ret":
                for f in rets[a]:
                    j = join(EXIT[f], st)
                    if j != EXIT[f]:
                        EXIT[f] = j
                        work.extend(c for c in callers[f] if IN[c] != BOT)
    for v in sorted(vectors):
        flow(v, START)
    run()
    for a in order:                        # what nothing known leads to: as if entered with nothing special
        if IN[a] == BOT:
            flow(a, START)
            run()
    out = {}
    for a in order:
        n, mn, ops = code[a]
        base, bits = info[a]
        if base in ("lea", "pea", "jmp", "jsr") or BRANCH.match(mn) or DBCC.match(mn):
            continue
        st = forced(a, IN[a])
        for i, o in enumerate(ops):
            m = MEMBASE.match(o)
            if m and m.group(1) != "sp" and st[REGNAME[m.group(1)]] & HW:
                out.setdefault(a, {})[i] = st[REGNAME[m.group(1)]]
    return out


# -------------------------------------------------------------- rewriting

INVERT = {"hi": "ls", "ls": "hi", "cc": "cs", "cs": "cc", "ne": "eq", "eq": "ne", "vc": "vs", "vs": "vc",
          "pl": "mi", "mi": "pl", "ge": "lt", "lt": "ge", "gt": "le", "le": "gt"}
READ_LAST = ("tst", "cmp", "cmpi", "cmpa", "cmpm", "btst")
WRITE_LAST = ("move", "clr", "st", "sf") + tuple("s" + c for c in INVERT)


class Rewriter:
    def __init__(self, code, lay, hwsites, hints):
        self.code = code
        self.lay = lay
        self.hw = hwsites
        self.reach = {}                   # hw_reach's: {addr: {operand: HW or HW|NOT}}
        self.hints = hints
        self.unsafe = set()               # instructions that can't run from their old address
        self.old_targets = collections.defaultdict(set)   # addr -> old addresses it may transfer to if run in place
        self.calls = set()                # instructions that push a return address
        self.unknown_targets = set()
        self.data_calls = set()           # calls followed by their data, not by code
        self.notes = collections.Counter()

    def label(self, a):
        return f"X_{a:06x}"

    def ram(self, a):
        return self.lay["ram"] + (a & 0xFFFF)

    def target(self, t):
        if is_ram(t):
            return f"{self.ram(t):#x}"
        if t in self.code:
            return self.label(t)
        self.unknown_targets.add(t)
        return f"{t:#x}"

    def shim(self, name):
        return self.lay["shim"] + SHIM_ENTRIES + 8 * SHIM_NAMES.index(name)

    def scratch(self, ops):
        used = set()
        for o in ops:
            used |= set(AREG.findall(o))
        for r in ("a0", "a1", "a2", "a3", "a4", "a5", "a6"):
            if r not in used:
                return r
        raise PortError("no free address register")

    def hw_wrap(self, a, mn, ops, idx, how, body):
        """the lines for an instruction whose operand idx is at a hardware address: it works on the cell"""
        base, bits = base_size(mn)
        if bits is None:
            if base in ("btst", "bset", "bclr", "bchg") or base in WRITE_LAST:
                bits = 8
            else:
                raise PortError(f"hardware operand of {mn}: size unknown")
        if base in ("movem", "movep"):
            raise PortError(f"{mn} on a hardware address isn't handled yet")
        e = ops[idx]
        pre, post, ea = [], [], e
        m = re.fullmatch(r"(a[0-7]|sp)@\+", e)
        step = 2 if bits == 8 and m and m.group(1) in ("a7", "sp") else bits // 8
        if m:
            ea = m.group(1) + "@"
            post.append(f"\taddql #{step},{m.group(1)}")
        m = re.fullmatch(r"(a[0-7]|sp)@-", e)
        if m:
            ea = m.group(1) + "@"
            pre.append(f"\tsubql #{step},{m.group(1)}")
        if re.search(r"\b(sp|a7)\b", ea):
            raise PortError(f"a hardware operand through the stack pointer ({e})")
        cell = f"{self.lay['cell']:#x}"
        new_ops = list(ops)
        new_ops[idx] = cell
        call = lambda name: [f"\tpea {ea}", f"\tjsr {self.shim(name):#x}", "\taddql #4,sp"]
        lines = list(pre)
        if "r" in how:
            lines += call(f"r{bits}")
        lines += body(new_ops)
        if "w" in how:
            lines += call(f"w{bits}")
        return lines + post

    def access(self, mn, ops, idx):
        """how an instruction uses its operand idx: 'r', 'w' or 'rw'"""
        base, _ = base_size(mn)
        if len(ops) == 2 and idx == 0:
            return "r"
        if base in READ_LAST:
            return "r"
        if base in WRITE_LAST or base == "movea":
            return "w"
        return "rw"

    def instr(self, a):
        """-> the new code's lines for the instruction at a (without its label)"""
        n, mn, ops = self.code[a]
        H = self.hints
        sub = lambda t: t.replace("{D}", f"{self.lay['D']:#x}").replace("{RAM}", f"{self.lay['ram']:#x}")   # (hint text may name the layout)
        H = {**H, "pre": {a: [sub(t) for t in H["pre"].get(a, [])]}, "post": {a: [sub(t) for t in H["post"].get(a, [])]},
             "replace": {a: [sub(t) for t in H["replace"][a]]} if a in H["replace"] else {}}
        if a in H["replace"]:
            self.unsafe.add(a)
            return ["\t" + s for s in H["replace"][a]]
        lines = self.instr_(a, n, mn, ops)
        fix = [f"\tjsr {self.shim('fix' + r):#x}" for r in sorted(H["fixa"].get(a, ()))]
        if fix and not falls_through(mn):
            raise PortError("a fixa hint on an instruction nothing follows")
        if H["pre"].get(a) or H["post"].get(a) or fix:
            self.unsafe.add(a)
            self.notes["pointers brought over where they are loaded (fixa)"] += len(fix)
        return ["\t" + s for s in H["pre"].get(a, [])] + lines + fix + ["\t" + s for s in H["post"].get(a, [])]

    def call_with_data(self, a, n, t):
        """a call that data follows (a string printed by the routine, which reads it through its return
        address and returns past it): the return address is the old one, where the data is, and the
        routine comes back into old code, which has a jump at the instruction after the data"""
        self.unsafe.add(a)
        self.data_calls.add(a)
        self.notes["calls with their data after them (the old return address is kept)"] += 1
        return [f"\tpea {a + n:#x}", f"\tjbra {self.target(t)}"]

    def instr_(self, a, n, mn, ops):
        text = lambda o=ops: [f"\t{mn} {','.join(o)}" if o else f"\t{mn}"]
        if a in self.hints["raw"]:
            return text()
        base, bits = base_size(mn)
        D = self.lay["D"]
        # ---- control flow
        m = BRANCH.match(mn)
        if m:
            t = int(ABS.match(ops[0]).group(1), 16)
            self.old_targets[a].add(t)
            cc = m.group(1)
            if cc == "sr":
                self.calls.add(a)
                if a + n not in self.code:
                    return self.call_with_data(a, n, t)
            return [f"\tjb{cc} {self.target(t)}"]
        if DBCC.match(mn):
            t = int(ABS.match(ops[1]).group(1), 16)
            self.old_targets[a].add(t)
            if t not in self.code:
                raise PortError(f"{mn} to ${t:06X}, which isn't known code")
            return [f"\t{mn} {ops[0]},{self.label(t)}"]
        if mn in ("jmp", "jsr"):
            if mn == "jsr":
                self.calls.add(a)
            t = direct_target(mn, ops)
            if t is not None:
                self.old_targets[a].add(t)
                if mn == "jsr" and a + n not in self.code:
                    return self.call_with_data(a, n, t)
                return [f"\t{'jbsr' if mn == 'jsr' else 'jbra'} {self.target(t)}"]
            m = PCIDX.match(ops[0])
            if m:                         # a table jump: the target is table + index, an old address (its trampoline takes it on)
                t, idx, sz = int(m.group(1), 16), m.group(2), m.group(3)
                s = "a1" if idx == "a0" else "a0"
                ret = f"R_{a:06x}"
                seq = ([f"\tpea %pc@({ret})"] if mn == "jsr" else []) + [
                    "\tsubql #4,sp", f"\tmoveml {s},sp@-", f"\tlea {t:#x},{s}", f"\tlea {s}@(0,{idx}:{sz}),{s}",
                    f"\tmoveml {s},sp@(4)", f"\tmoveal sp@+,{s}", "\trts"] + ([f"{ret}:"] if mn == "jsr" else [])
                self.notes["table jumps"] += 1
                return seq
            return text()
        # ---- rte: the game's frames are the 68000's (a status word and an address: some routines push
        # their own and are called with jsr); a 68020 wants a format word after them, so the port
        # returns the 68000's way and the Amiga's interrupt entry builds its frames to match
        if mn == "rte":
            self.unsafe.add(a)
            self.notes["rte as the 68000 did it"] += 1
            return ["\tmovew sp@+,%sr", "\trts"]
        # ---- `suba.l sp,sp`: a stack from address 0 down, which on the Mega Drive is the top of its RAM
        if base == "suba" and bits == 32 and ops == ["sp", "sp"]:
            self.unsafe.add(a)
            self.notes["stack pointers cleared (the top of RAM)"] += 1
            return [f"\tmoveal #{self.lay['ram'] + 0x10000:#x},sp"]
        # ---- the interrupt mask: the Mega Drive's vertical interrupt is level 6 and its horizontal one 4;
        # the port delivers both at level 3 (the Amiga's vertical blank), so "allow them" is mask 2
        if mn == "stop" and len(ops) == 1 and IMM.match(ops[0]):      # (the wait for an interrupt, with its mask)
            v = int(IMM.match(ops[0]).group(1)) & 0xFFFF
            if 3 <= (v >> 8 & 7) <= 5:
                self.unsafe.add(a)
                self.notes["interrupt masks lowered to the Amiga's"] += 1
                return [f"\tstop #{(v & ~0x700) | 0x200}"]
        if len(ops) == 2 and ops[1] == "%sr" and base == "move" and IMM.match(ops[0]):
            v = int(IMM.match(ops[0]).group(1)) & 0xFFFF
            ipl = v >> 8 & 7
            if 3 <= ipl <= 5:
                self.unsafe.add(a)
                self.notes["interrupt masks lowered to the Amiga's"] += 1
                return [f"\tmovew #{(v & ~0x700) | 0x200},%sr"]
            if ipl == 6:
                raise PortError("an interrupt mask of 6 (the horizontal interrupt alone): not mapped yet")
        # ---- operands
        new = list(ops)
        pre, post = [], []
        hw_idx, safe = None, True
        for i, o in enumerate(ops):
            m = IMM.match(o)
            if m:
                v = int(m.group(1)) & 0xFFFFFFFF
                if i == 0 and len(ops) == 2 and re.fullmatch(r"a[0-7]|sp", ops[1]):
                    if base == "movea" and bits == 32 and is_ram(v) and v != 0xFFFFFFFF:
                        new[i] = f"#{self.ram(v):#x}"; safe = False
                    elif base in ("movea", "cmpa") and bits == 16 and v >= 0xFFFF8000:
                        mn = base + "l"; new[i] = f"#{self.ram(v):#x}"; safe = False
                    elif base == "cmpa" and bits == 32 and is_ram(v) and v != 0xFFFFFFFF:
                        new[i] = f"#{self.ram(v):#x}"; safe = False
                elif i == 0 and base == "move" and bits == 32 and (0xE00000 <= v <= 0xFFFFFF or 0xFFFF0000 <= v <= 0xFFFFFF00):
                    new[i] = f"#{self.ram(v):#x}"; safe = False   # (a `raw` hint if it was a number after all)
                    self.notes["move.l #$FFxxxx taken as a RAM pointer"] += 1
                continue
            m = ABS.match(o)
            if m:
                v = int(m.group(1), 16)
                if is_ram(v):
                    new[i] = f"{self.ram(v):#x}"; safe = False
                elif is_hw(v) and base not in ("lea", "pea"):
                    hw_idx = i; safe = False
                continue
            m = PCREL.match(o)
            if m:
                new[i] = f"{int(m.group(1), 16):#x}"
                continue
            m = PCIDX.match(o)
            if m:
                if base == "pea" or any(re.search(r"\b(sp|a7)\b", x) for x in ops):
                    raise PortError("an indexed pc-relative operand next to the stack pointer")
                s = self.scratch(ops)
                pre += [f"\tmoveml {s},sp@-", f"\tlea {int(m.group(1), 16):#x},{s}"]
                post += [f"\tmoveal sp@+,{s}"]
                new[i] = f"{s}@(0,{m.group(2)}:{m.group(3)})"
                self.notes["indexed pc-relative data"] += 1
                continue
        # a word sign-extended into an address register is a pointer: bring it into the port's space
        if base == "movea" and bits == 16 and not IMM.match(ops[0]):
            post.append(f"\taddal #{D:#x},{ops[1]}"); safe = False
            self.notes["movea.w + D"] += 1
        if base == "cmpa" and bits == 16 and not IMM.match(ops[0]):
            if AREG.search(ops[0]) and ops[1] in AREG.findall(ops[0]):
                raise PortError("cmpa.w of a register with something reached through itself")
            pre.insert(0, f"\tsubal #{D:#x},{ops[1]}"); post.append(f"\taddal #{D:#x},{ops[1]}"); safe = False
            self.notes["cmpa.w around D"] += 1
        body = lambda o: pre + [f"\t{mn} {','.join(o)}"] + post
        # ---- a hardware operand: absolute (above), or one the analysis run saw this instruction touch
        how = None
        if hw_idx is not None:
            how = self.access(mn, ops, hw_idx)
        elif a in self.hw:
            cand = [i for i, o in enumerate(ops) if "@" in o and not o.startswith("%pc")]
            seen = self.hw[a]
            if not cand:
                raise PortError("the analysis saw it touch hardware, but no operand is a memory one")
            if len(cand) == 1:
                hw_idx = cand[0]
            elif seen == {"w"}:
                hw_idx = cand[-1]
            elif seen == {"r"}:
                hw_idx = cand[0]
            elif len(cand) == 2 and base == "move":
                # both ends (a decompressor copying within the Z80's RAM): the source into the cell, then the cell out
                self.unsafe.add(a)
                self.notes["hardware accesses"] += 2
                inner = lambda o: self.hw_wrap(a, mn, o, 1, "w", body)
                return self.hw_wrap(a, mn, new, 0, "r", inner)
            else:
                raise PortError("reads and writes hardware through two operands")
            how = self.access(mn, ops, hw_idx)
            self.notes["hardware through a register"] += 1
        elif a in self.reach:
            # not seen by the analysis run, but the register may hold a hardware address here
            cand = sorted(self.reach[a])
            sure = all(v == HW for v in self.reach[a].values())
            try:
                if len(cand) == 2 and base == "move":
                    inner = lambda o: self.hw_wrap(a, mn, o, cand[1], "w", body)
                    lines = self.hw_wrap(a, mn, new, cand[0], "r", inner)
                elif len(cand) == 1:
                    lines = self.hw_wrap(a, mn, new, cand[0], self.access(mn, ops, cand[0]), body)
                else:
                    raise PortError("two operands")
            except PortError:
                self.notes["possible hardware accesses left alone (movem, two operands)"] += 1
                lines = None
            if lines:
                self.unsafe.add(a)
                self.notes["hardware accesses found by following the registers" if sure else
                           "accesses that may be hardware or memory, by following the registers"] += 1
                return lines
        if how:
            self.unsafe.add(a)
            self.notes["hardware accesses"] += 1
            return self.hw_wrap(a, mn, new, hw_idx, how, body)
        if not safe:
            self.unsafe.add(a)
        return body(new)

    def source(self):
        out = ["\t.text"]
        order = sorted(self.code)
        errors = []
        for i, a in enumerate(order):
            n, mn, ops = self.code[a]
            out.append(f"{self.label(a)}:")
            try:
                out += self.instr(a)
            except PortError as e:
                errors.append(f"${a:06X} {mn} {','.join(ops)}: {e}")
                out += [f"\t.short {POISON:#x}"]
                self.unsafe.add(a)
            if falls_through(mn) and a + n not in self.code and a not in self.data_calls:
                out.append(f"\tjmp {a + n:#x}")   # into code the port doesn't have: it runs where it was
                self.unknown_targets.add(a + n)
            elif falls_through(mn) and i + 1 < len(order) and order[i + 1] != a + n and a not in self.data_calls:
                out.append(f"\tjbra {self.label(a + n)}")   # (the next one here shares this one's bytes: another way in)
        return "\n".join(out) + "\n", errors


# ------------------------------------------------------------- assembling

def assemble(src_text, workdir, image):
    """the new code assembled and linked as an Amiga hunk file -> (bytes, [reloc32 offsets], {label: offset})"""
    with open(os.path.join(workdir, "port.s"), "w") as f:
        f.write(src_text)
    script = ("m68k-amigaos-as -m68000 --register-prefix-optional port.s -o port.o 2> port.as.log || { head -30 port.as.log; exit 1; }; "
              "m68k-amigaos-ld port.o -o port.hunk 2> port.ld.log || { head -30 port.ld.log; exit 1; }; "
              "m68k-amigaos-nm port.o > port.nm")
    r = subprocess.run(["docker", "run", "--rm", "-v", f"{workdir}:/w", "-w", "/w", image, "sh", "-c", script],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise PortError("the rewritten code didn't assemble:\n" + (r.stdout + r.stderr)[-3000:])
    data = open(os.path.join(workdir, "port.hunk"), "rb").read()
    code, relocs = parse_hunk(data)
    syms = {}
    for line in open(os.path.join(workdir, "port.nm")):
        p = line.split()
        if len(p) == 3 and p[2].startswith(("X_", "R_")):
            syms[p[2]] = int(p[0], 16)
    return code, relocs, syms


def parse_hunk(data):
    """an Amiga load file with one code hunk -> (its bytes, the offsets of the longs to relocate)"""
    w = lambda o: struct.unpack(">I", data[o:o + 4])[0]
    if w(0) != 0x3F3:
        raise PortError("not an Amiga hunk file")
    o = 8                                 # (no resident library names)
    nh = w(o); first = w(o + 4); last = w(o + 8)
    o += 12 + 4 * (last - first + 1)
    code, relocs, hunk = None, [], 0
    while o < len(data):
        t = w(o) & 0x3FFFFFFF
        o += 4
        if t in (0x3E9, 0x3EA):           # code, data
            n = w(o) * 4
            if hunk == 0:
                code = bytearray(data[o + 4:o + 4 + n])
            elif n:
                raise PortError("the rewritten code has more than one hunk")
            o += 4 + n
        elif t == 0x3EB:                  # bss
            o += 4
        elif t == 0x3EC:                  # reloc32
            while True:
                n = w(o); o += 4
                if not n:
                    break
                target = w(o); o += 4
                offs = struct.unpack(f">{n}I", data[o:o + 4 * n]); o += 4 * n
                if hunk == 0 and target == 0:
                    relocs += offs
                elif n:
                    raise PortError("a relocation between hunks")
        elif t == 0x3F0:                  # symbols
            while True:
                n = w(o); o += 4
                if not n:
                    break
                o += 4 * n + 4
        elif t == 0x3F1:                  # debug
            o += 4 + w(o) * 4
        elif t == 0x3F2:
            hunk += 1
        else:
            raise PortError(f"hunk type {t:#x} not understood")
    if code is None:
        raise PortError("no code hunk")
    return code, relocs


# ---------------------------------------------------------- the old space

def patch_old(rom, code, rw, entries, new_addr, lay, log=print, covered=None):
    """the ROM image with the old code made into jumps to the new: 6 bytes at every entry that has
    the room, the original instructions where a short block can run in place, a branch to an
    island for the rest; everything else poisoned -> (image, stats)"""
    img = bytearray(rom)
    order = sorted(code)
    nxt_instr = {a: a + code[a][0] for a in order}
    entries = set(entries)
    # (what the disassembly reached but the analysis run never executed may yet be data the game reads:
    # the old bytes of it are left as they are, neither made into jumps nor poisoned)
    ran = (lambda x: x in covered) if covered is not None else (lambda x: True)
    # routines: what calls reach and what follows an unconditional transfer are entries too (a table may name them)
    for a in order:
        n, mn, ops = code[a]
        t = direct_target(mn, ops)
        if t in code and (mn == "jsr" or mn.startswith("bsr")) and ran(t):
            entries.add(t)
        if not falls_through(mn) and a + n in code and ran(a + n):
            entries.add(a + n)
    follows = {a + code[a][0] for a in order}
    entries |= {a for a in order if a not in follows and ran(a)}   # code after data: a call with its data after it returns here
    while True:
        plan, grew = {}, False
        es = sorted(entries)
        for i, e in enumerate(es):
            limit = es[i + 1] if i + 1 < len(es) else len(rom)
            end = e
            block = []
            while end in code and end < limit:
                block.append(end)
                end = nxt_instr[end]
            room = end - e                # contiguous known code up to the next entry
            if room >= 6:
                plan[e] = ("jmp", room, block)
            elif all(b not in rw.unsafe for b in block):
                plan[e] = ("inplace", room, block)
                need = set()
                for b in block:
                    need |= {t for t in rw.old_targets.get(b, ()) if t in code}
                    if b in rw.calls and nxt_instr[b] in code:
                        need.add(nxt_instr[b])
                if end in code:
                    need.add(end)
                if need - entries:
                    entries |= need
                    grew = True
            else:
                plan[e] = ("island", room, block)
        if not grew:
            break
    # dead bytes: known code that no plan keeps
    kept = set()
    for e, (kind, room, block) in plan.items():
        if kind == "jmp":
            kept |= set(range(e, e + 6))
        else:
            kept |= set(range(e, e + room))
    dead = []
    for a in order:
        if not ran(a):
            continue
        for b in range(a, a + code[a][0], 2):
            if b not in kept:
                img[b:b + 2] = struct.pack(">H", POISON)
                dead.append(b)
    dead_set = set(dead)
    stats = collections.Counter()
    for e, (kind, room, block) in plan.items():
        if kind == "jmp":
            img[e:e + 6] = b"\x4E\xF9" + struct.pack(">I", new_addr[e])
            stats["entries with a jump"] += 1
        elif kind == "inplace":
            stats["short blocks run in place"] += 1
    islands_failed = []
    for e, (kind, room, block) in sorted(plan.items()):
        if kind != "island":
            continue
        reach = 126 if room == 2 else 32000
        spot = None
        for b in sorted(dead_set, key=lambda b: abs(b - e)):
            if abs(b - e) > reach:
                break
            if all(b + k in dead_set for k in (0, 2, 4)):
                spot = b
                break
        if spot is None:
            islands_failed.append(e)
            continue
        dead_set -= {spot, spot + 2, spot + 4}
        img[spot:spot + 6] = b"\x4E\xF9" + struct.pack(">I", new_addr[e])
        if room == 2:
            img[e:e + 2] = bytes([0x60, (spot - (e + 2)) & 0xFF])
        else:
            img[e:e + 4] = b"\x60\x00" + struct.pack(">h", spot - (e + 2))
            for b in range(e + 4, e + room, 2):
                img[b:b + 2] = struct.pack(">H", POISON)
        stats["entries through an island"] += 1
    stats["entries"] = len(plan)
    inplace = [b for e, (kind, room, block) in plan.items() if kind == "inplace" for b in block]
    return img, stats, islands_failed, sorted(plan), inplace


def learn(analysis, outdir, log=print):
    """the checking build's report (faults.txt in outdir) -> `fixa` lines added to port.txt: each pointer
    into the Mega Drive's RAM that was followed is brought over at the instruction that loaded it"""
    faults = os.path.join(outdir, "faults.txt")
    nm = os.path.join(outdir, "port.nm")
    info = json.load(open(os.path.join(outdir, "port.json")))
    base = info["layout"]["code"]
    syms = sorted((int(l.split()[0], 16) + base, int(l.split()[2][2:], 16)) for l in open(nm)
                  if len(l.split()) == 3 and l.split()[2].startswith("X_"))
    import bisect
    def old(pc):
        if pc < info["layout"]["romsize"]:
            return pc
        i = bisect.bisect_right(syms, (pc, 1 << 30)) - 1
        return syms[i][1] if i >= 0 else None
    have = read_hints(os.path.join(analysis, "port.txt"))
    added, manual = [], []
    for line in open(faults):
        p = line.split()
        if p[1] != "m":
            manual.append(line.strip())
            continue
        origins = [x.split("=") for x in p[3:]]
        if not origins:
            manual.append(line.strip())
        for reg, pc in origins:
            o = old(int(pc, 16))
            if o is None or reg == "a7":
                manual.append(line.strip())
            elif reg not in have["fixa"].get(o, ()) and (o, reg) not in added:
                added.append((o, reg))
    if added:
        with open(os.path.join(analysis, "port.txt"), "a") as f:
            for o, reg in added:
                f.write(f"fixa {o:06X} {reg}\n")
    log(f"learned {len(added)} place(s) where a pointer has to be brought over (fixa lines in port.txt)"
        + (f"; {len(manual)} report(s) need a look: " + "; ".join(manual[:6]) if manual else ""))
    return len(added)


# ------------------------------------------------------------------- port

def idle_loops(code):
    """-> [(a, b)]: a test of the game's RAM at a and a branch back to it at b when it fails: nothing in
    the loop changes what it tests, so it is the game waiting for an interrupt (the vertical blank's,
    mostly: its frame is done). The Amiga's layer does its drawing while the game is in one."""
    out = []
    for a in sorted(code):
        n, mn, ops = code[a]
        base, _ = base_size(mn)
        if base not in ("tst", "cmp", "cmpi", "btst") or a + n not in code:
            continue
        mem = [o for o in ops if not IMM.match(o)]
        if not mem or not all(ABS.match(o) and is_ram(int(ABS.match(o).group(1), 16)) for o in mem):
            continue
        _, mn2, ops2 = code[a + n]
        m = BRANCH.match(mn2)
        if m and m.group(1) not in ("ra", "sr") and int(ABS.match(ops2[0]).group(1), 16) == a:
            out.append((a, a + n))
    return out


def port(rom_path, analysis, outdir, image, log=print):
    """-> (the port's memory image path, its layout and counts)"""
    os.makedirs(outdir, exist_ok=True)
    hints = read_hints(os.path.join(analysis, "port.txt"))
    code, entries, rom, covered = find_code(rom_path, analysis, image, hints, log)
    lay = layout(len(rom))
    hw = read_hwsites(os.path.join(analysis, "hwsites.txt"))
    hw = {pc: v for pc, v in hw.items() if pc in code}
    rw = Rewriter(code, lay, hw, hints)
    vectors = {struct.unpack(">I", rom[v:v + 4])[0] for v in range(4, 0x100, 4)}
    # (an instruction the analysis run executed without it touching hardware, and that only may: left alone)
    rw.reach = {a: v for a, v in hw_reach(code, entries, hw, vectors & set(code)).items()
                if a not in hints["raw"] and not (a in covered and a not in hw and any(x != HW for x in v.values()))}
    text, errors = rw.source()
    if errors:
        raise PortError("instructions the rewriter can't do yet (a `replace ADDR asm; asm` line in "
                        f"{os.path.join(analysis, 'port.txt')} can stand in):\n  " + "\n  ".join(errors[:40]))
    blob, relocs, syms = assemble(text, outdir, image)
    for off in relocs:
        v = struct.unpack(">I", blob[off:off + 4])[0] + lay["code"]
        blob[off:off + 4] = struct.pack(">I", v)
    new_addr = {a: lay["code"] + syms[rw.label(a)] for a in code}
    img, stats, failed, plan, inplace = patch_old(rom, code, rw, entries, new_addr, lay, log, covered)
    for a in hints["reloc"]:
        v = struct.unpack(">I", img[a:a + 4])[0]
        img[a:a + 4] = struct.pack(">I", lay["ram"] + (v & 0xFFFF))
    # Where the image holds an address of the game's RAM, of the ROM's copy above it or of the new code, so a
    # loader may put those somewhere else, together (the Amiga's puts them in fast RAM: chip RAM is half the
    # speed with eight bitplanes on). Found by doing the port once more for a layout a megabyte on and seeing
    # which longs moved by that. The layer's page stays where it is.
    shift = 0x100000
    lay2 = dict(lay)
    for key in ("ram", "D", "mirror", "code"):
        lay2[key] += shift
    rw2 = Rewriter(code, lay2, hw, hints)
    rw2.reach = rw.reach
    text2, errors2 = rw2.source()
    os.makedirs(os.path.join(outdir, "alt"), exist_ok=True)
    blob2, relocs2, syms2 = assemble(text2, os.path.join(outdir, "alt"), image)
    for off in relocs2:
        v = struct.unpack(">I", blob2[off:off + 4])[0] + lay2["code"]
        blob2[off:off + 4] = struct.pack(">I", v)
    img2 = patch_old(rom, code, rw2, entries, {a: lay2["code"] + syms2[rw2.label(a)] for a in code}, lay2, lambda *_: None, covered)[0]
    for a in hints["reloc"]:
        v = struct.unpack(">I", img2[a:a + 4])[0]
        img2[a:a + 4] = struct.pack(">I", lay2["ram"] + (v & 0xFFFF))
    if errors2 or len(blob2) != len(blob) or len(img2) != len(img):
        raise PortError("the port doesn't come out the same shape for another layout: it can't be told where its addresses are")

    def moved(a, b, what):
        out = []
        i = 0
        while i < len(a):
            if a[i] != b[i]:
                at = next((k for k in (i - 1, i) if k >= 0 and
                           (struct.unpack(">I", b[k:k + 4])[0] - struct.unpack(">I", a[k:k + 4])[0]) & 0xFFFFFFFF == shift), None)
                if at is None:
                    raise PortError(f"{what} at ${i:06X} depends on the layout but isn't a whole address: the port can't be moved")
                out.append(at)
                i = at + 4
            else:
                i += 1
        return out
    lay["relocs"] = {"code": moved(blob, blob2, "the new code"), "rom": moved(img, img2, "the ROM image")}
    # the header's checksum (the words from $200 to the ROM's end, which games verify at startup) for the image as patched
    end = min(struct.unpack(">I", rom[0x1A4:0x1A8])[0] + 1, len(img)) & ~1
    if end > 0x200:
        img[0x18E:0x190] = struct.pack(">H", sum(struct.unpack(f">{(end - 0x200) // 2}H", img[0x200:end])) & 0xFFFF)
    top = (lay["code"] + len(blob) + 0xFFF) & ~0xFFF
    mem = bytearray(top)
    mem[0:len(img)] = img
    mem[lay["mirror"]:lay["mirror"] + 0x8000] = img[0:0x8000]
    mem[lay["code"]:lay["code"] + len(blob)] = blob
    lay["top"] = top
    path = os.path.join(outdir, "port.bin")
    with open(path, "wb") as f:
        f.write(mem)
    # where the game's own instructions start (the checking build charges time for those only):
    # every rewritten instruction's label, and the old code that still runs in place
    starts = bytearray(top >> 4)
    for a in list(new_addr.values()) + inplace:
        starts[a >> 4] |= 1 << ((a >> 1) & 7)
    with open(os.path.join(outdir, "port.starts"), "wb") as f:
        f.write(starts)
    # and which of the rewritten ones were control flow: the checking build looks at the chips' events
    # where the original did, not at the calls the port added
    flows = bytearray(top >> 4)
    for o, na in new_addr.items():
        mn = code[o][1]
        if BRANCH.match(mn) or DBCC.match(mn) or mn in ("jmp", "jsr", "nop", "stop", "rte", "rts", "rtr", "trap"):
            flows[na >> 4] |= 1 << ((na >> 1) & 7)
    with open(os.path.join(outdir, "port.flows"), "wb") as f:
        f.write(flows)
    info = {"layout": lay, "instructions": len(code), "code_bytes": len(blob), "stats": dict(stats),
            "rewrites": dict(rw.notes), "unknown_targets": sorted(rw.unknown_targets),
            "entries_without_room": failed, "entries": plan,
            "idle": [[new_addr[a], new_addr[b]] for a, b in idle_loops(code)],
            "sp": lay["ram"] + ((struct.unpack(">I", rom[0:4])[0] - 1 & 0xFFFF) + 1),   # (a stack from 0 is from the top of RAM)
             "pc": struct.unpack(">I", rom[4:8])[0]}
    with open(os.path.join(outdir, "port.json"), "w") as f:
        json.dump(info, f)
    log(f"rewritten: {len(code)} instructions -> {len(blob)} bytes of code at ${lay['code']:06X}; RAM at ${lay['ram']:06X}, "
        f"the hardware layer's page at ${lay['shim']:06X}, top ${top:06X}")
    log("  " + ", ".join(f"{v} {k}" for k, v in sorted(rw.notes.items())))
    log("  old code: " + ", ".join(f"{v} {k}" for k, v in sorted(stats.items())))
    log(f"  {len(info['idle'])} loop(s) where the game waits for an interrupt")
    if rw.unknown_targets:
        log(f"  {len(rw.unknown_targets)} branch targets in code the port doesn't have (it runs where it was, unported): "
            + " ".join(f"${t:06X}" for t in sorted(rw.unknown_targets)[:8]) + (" ..." if len(rw.unknown_targets) > 8 else ""))
    if failed:
        log(f"  {len(failed)} entries too cramped for a jump and with no room nearby (they're poisoned; a `replace` or more coverage): "
            + " ".join(f"${t:06X}" for t in failed[:12]))
    return path, info


# ------------------------------------------------------------- the Amiga

def sound_holes(analysis, romsize):
    """the ROM's bytes only the sound driver read (z80rom.bin, from the sound rip) and the 68000 never did:
    the music and the samples, which the Amiga plays from its own data -> [(start, length)], the runs of 2K and more"""
    zpath, rpath = os.path.join(analysis, "z80rom.bin"), os.path.join(analysis, "romread.bin")
    if not os.path.exists(zpath) or not os.path.exists(rpath):
        return []
    zbits = open(zpath, "rb").read()
    tags = open(rpath, "rb").read()[:romsize]
    count = collections.Counter(tags)
    bulk = {t for t, n in count.items() if t not in (0, 255) and n > romsize // 4}
    covered = read_bits(os.path.join(analysis, "coverage.bin"))
    code = bytearray(romsize)
    for a in covered:
        if a < romsize - 10:
            code[a:a + 10] = b"\1" * 10
    runs = []
    a = 0
    n = min(romsize, len(tags), len(zbits) * 8)
    while a < n:
        if zbits[a >> 3] >> (a & 7) & 1 and (tags[a] == 0 or tags[a] in bulk) and not code[a]:
            b = a
            while b < n and zbits[b >> 3] >> (b & 7) & 1 and (tags[b] == 0 or tags[b] in bulk) and not code[b]:
                b += 1
            lo, hi = (a + 1) & ~1, b & ~1
            if hi - lo >= 2048:
                runs.append((lo, hi - lo))
            a = b
        else:
            a += 1
    return runs


def build_amiga(outdir, kit_root, image, analysis=None, log=print):
    """the port as an Amiga program: OUT/amiga/game (the loader, the hardware layer and the sound player,
    recomp/mdamiga), game.z (the image, deflated) and a bootable game.adf -> the ADF's path. With a sound rip
    in the analysis directory (game --sound-rip DIR), sound.z too (agk/mdsound.py): on the same disk if it
    fits, else on game2.adf (volume PORT2: the second drive, or swapped in when asked for)."""
    import shutil
    import zlib
    info = json.load(open(os.path.join(outdir, "port.json")))
    lay = info["layout"]
    mem = open(os.path.join(outdir, "port.bin"), "rb").read()
    rom = bytearray(mem[:lay["romsize"]])
    screen = (lay["top"] + 0xFFFF) & ~0xFFFF
    copper = screen + 0x80000
    sound = None
    if analysis and os.path.exists(os.path.join(analysis, "sound.idx")):
        from . import mdsound
        try:
            snd = mdsound.build(analysis, log)
            holes = sound_holes(analysis, lay["romsize"])
            for a, n in holes:
                rom[a:a + n] = bytes(n)   # (nothing reads them now; they pack to nothing, and are room for samples)
            end = min(struct.unpack(">I", rom[0x1A4:0x1A8])[0] + 1, len(rom)) & ~1
            if holes and 0x200 < end:     # (the header's checksum again, for the ROM without them)
                rom[0x18E:0x190] = struct.pack(">H", sum(struct.unpack(f">{(end - 0x200) // 2}H", rom[0x200:end])) & 0xFFFF)
            top = (lay["top"] + 1) & ~1
            regions = [(copper + 0xB000, 0x200000 - copper - 0xB000), (top, screen - top)] + holes   # (after the copper lists and the window's bitmap)
            sound = mdsound.pack(snd, [r for r in regions if r[1] >= 2048], mdsound.aliases(analysis, snd["addrs"]), log)
        except mdsound.SoundError as e:
            raise PortError(f"the sound: {e}")
    elif analysis:
        log("no sound rip in the analysis directory: the port will be silent (DIR/game --frames N --input keys.txt --sound-rip DIR makes one)")
    # after the ROM and the code, where addresses of the code are: counts, then offsets in the code and in the ROM
    rl = lay.get("relocs", {"code": [], "rom": []})
    table = struct.pack(f">II{len(rl['code'])}I{len(rl['rom'])}I", len(rl["code"]), len(rl["rom"]), *rl["code"], *rl["rom"])
    payload = bytes(rom) + mem[lay["code"]:lay["top"]] + table
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    z = c.compress(payload) + c.flush()
    adir = os.path.join(outdir, "amiga")
    os.makedirs(adir, exist_ok=True)
    snd_disk, sz = 0, b""
    for name in ("sound.z", "game2.adf"):
        if os.path.exists(os.path.join(adir, name)):
            os.remove(os.path.join(adir, name))
    if sound:
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        sz = c.compress(sound) + c.flush()
        with open(os.path.join(adir, "sound.z"), "wb") as f:
            f.write(sz)
        snd_disk = 1 if len(z) + len(sz) < 820000 else 2
    with open(os.path.join(adir, "game.z"), "wb") as f:
        f.write(z)
    src = os.path.join(kit_root, "recomp", "mdamiga")
    for name in os.listdir(src):
        shutil.copy(os.path.join(src, name), adir)
    defs = {"ROMSIZE": lay["romsize"], "RAM": lay["ram"], "D": lay["D"], "SHIM": lay["shim"], "CODE": lay["code"],
            "CODESIZE": lay["top"] - lay["code"], "TOP": lay["top"], "SP": info["sp"], "PC": info["pc"],
            "SCREEN0": screen, "COPPER": copper, "ZSIZE": len(z),   # (four bitmaps of 128K, then two copper lists)
            "IMGSIZE": len(payload), "SNDDISK": snd_disk, "SNDZSIZE": len(sz), "SNDSIZE": len(sound or b"")}
    # the console the game is told it runs on ($A10001): a game sold in one region may refuse another
    # (Thunder Force IV stops with a message), so it is the header's: Europe if listed (the Amiga is
    # PAL), else overseas at 60 Hz, else Japan
    region = bytes(rom[0x1F0:0x200])
    defs["VERSION"] = 0xE0 if b"E" in region or not region.strip() else 0xA0 if b"U" in region else 0x20
    if defs["COPPER"] + 0xB000 > 0x200000:
        raise PortError("the port's image and its screen don't fit in 2 MB of chip RAM")
    with open(os.path.join(adir, "port_layout.h"), "w") as f:
        f.write("/* where agk port put things (port.json) */\n")
        for k, v in defs.items():
            f.write(f"#define PORT_{k} {v:#x}\n")
        # (the loops the game waits for an interrupt in, first and last instruction: the layer draws meanwhile)
        f.write("#define PORT_IDLES " + "".join(f"{a:#x},{b:#x}," for a, b in info.get("idle", [])) + "\n")
    script = ("m68k-amigaos-gcc -m68020 -O2 -fomit-frame-pointer -Wall -Wl,-Map,game.map -o game main.c layer.c sound.c inflate.c start.S > build.log 2>&1 "
              "|| { tail -30 build.log; exit 1; }; m68k-amigaos-nm -n game > game.nm; m68k-amigaos-strip game; "
              "printf 'game\\n' > startup-sequence; rm -f game.adf; "
              "xdftool game.adf format PORT ffs + boot install + write game game + write game.z game.z + makedir s "
              "+ write startup-sequence s/startup-sequence 2>&1 | tail -5"
              + ("; xdftool game.adf write sound.z sound.z 2>&1 | tail -5" if snd_disk == 1 else
                 "; rm -f game2.adf; xdftool game2.adf format PORT2 ffs + write sound.z sound.z 2>&1 | tail -5" if snd_disk == 2 else ""))
    r = subprocess.run(["docker", "run", "--rm", "-v", f"{adir}:/w", "-w", "/w", image, "sh", "-c", script],
                       capture_output=True, text=True)
    adf = os.path.join(adir, "game.adf")
    if r.returncode != 0 or not os.path.exists(adf):
        raise PortError("the Amiga build failed:\n" + (r.stdout + r.stderr)[-3000:])
    log(f"Amiga: {os.path.getsize(os.path.join(adir, 'game'))} bytes of program, {len(z)} of image (deflated); "
        f"screen at ${screen:06X}. Needs a 68020, AGA, 2 MB chip and fast RAM"
        + (f"; {len(sz)} bytes of sound on a second disk, game2.adf (in DF1:, or swapped in when it asks for PORT2)" if snd_disk == 2 else
           f"; {len(sz)} bytes of sound" if snd_disk else ""))
    return adf
