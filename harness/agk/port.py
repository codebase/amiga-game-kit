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
GARBAGE = re.compile(r"\)@\(|:[248]\)|@\(0x[0-9a-f]+\)@|\[")
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
                    return None
                n, mn, ops = by_addr[a]
                if a not in covered:
                    if mn in (".short", ".long", "illegal") or any(GARBAGE.search(o) for o in ops):
                        return None
                    if any(b in data or owner.get(b, a) != a for b in range(a, a + n)):
                        return None
                seen.add(a)
                t = direct_target(mn, ops)
                if t is not None and not is_ram(t):
                    todo.append(t)
                if falls_through(mn):
                    todo.append(a + n)
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
        self.hints = hints
        self.unsafe = set()               # instructions that can't run from their old address
        self.old_targets = collections.defaultdict(set)   # addr -> old addresses it may transfer to if run in place
        self.calls = set()                # instructions that push a return address
        self.unknown_targets = set()
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
        for a in order:
            n, mn, ops = self.code[a]
            out.append(f"{self.label(a)}:")
            try:
                out += self.instr(a)
            except PortError as e:
                errors.append(f"${a:06X} {mn} {','.join(ops)}: {e}")
                out += [f"\t.short {POISON:#x}"]
                self.unsafe.add(a)
            if falls_through(mn) and a + n not in self.code:
                out.append(f"\tjmp {a + n:#x}")   # into code the port doesn't have: it runs where it was
                self.unknown_targets.add(a + n)
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

def patch_old(rom, code, rw, entries, new_addr, lay, log=print):
    """the ROM image with the old code made into jumps to the new: 6 bytes at every entry that has
    the room, the original instructions where a short block can run in place, a branch to an
    island for the rest; everything else poisoned -> (image, stats)"""
    img = bytearray(rom)
    order = sorted(code)
    nxt_instr = {a: a + code[a][0] for a in order}
    entries = set(entries)
    # routines: what calls reach and what follows an unconditional transfer are entries too (a table may name them)
    for a in order:
        n, mn, ops = code[a]
        t = direct_target(mn, ops)
        if t in code and (mn == "jsr" or mn.startswith("bsr")):
            entries.add(t)
        if not falls_through(mn) and a + n in code:
            entries.add(a + n)
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

def port(rom_path, analysis, outdir, image, log=print):
    """-> (the port's memory image path, its layout and counts)"""
    os.makedirs(outdir, exist_ok=True)
    hints = read_hints(os.path.join(analysis, "port.txt"))
    code, entries, rom, covered = find_code(rom_path, analysis, image, hints, log)
    lay = layout(len(rom))
    hw = read_hwsites(os.path.join(analysis, "hwsites.txt"))
    hw = {pc: v for pc, v in hw.items() if pc in code}
    rw = Rewriter(code, lay, hw, hints)
    text, errors = rw.source()
    if errors:
        raise PortError("instructions the rewriter can't do yet (a `replace ADDR asm; asm` line in "
                        f"{os.path.join(analysis, 'port.txt')} can stand in):\n  " + "\n  ".join(errors[:40]))
    blob, relocs, syms = assemble(text, outdir, image)
    for off in relocs:
        v = struct.unpack(">I", blob[off:off + 4])[0] + lay["code"]
        blob[off:off + 4] = struct.pack(">I", v)
    new_addr = {a: lay["code"] + syms[rw.label(a)] for a in code}
    img, stats, failed, plan, inplace = patch_old(rom, code, rw, entries, new_addr, lay, log)
    for a in hints["reloc"]:
        v = struct.unpack(">I", img[a:a + 4])[0]
        img[a:a + 4] = struct.pack(">I", lay["ram"] + (v & 0xFFFF))
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
    info = {"layout": lay, "instructions": len(code), "code_bytes": len(blob), "stats": dict(stats),
            "rewrites": dict(rw.notes), "unknown_targets": sorted(rw.unknown_targets),
            "entries_without_room": failed, "entries": plan,
            "sp": lay["ram"] + (struct.unpack(">I", rom[0:4])[0] & 0xFFFF), "pc": struct.unpack(">I", rom[4:8])[0]}
    with open(os.path.join(outdir, "port.json"), "w") as f:
        json.dump(info, f)
    log(f"rewritten: {len(code)} instructions -> {len(blob)} bytes of code at ${lay['code']:06X}; RAM at ${lay['ram']:06X}, "
        f"the hardware layer's page at ${lay['shim']:06X}, top ${top:06X}")
    log("  " + ", ".join(f"{v} {k}" for k, v in sorted(rw.notes.items())))
    log("  old code: " + ", ".join(f"{v} {k}" for k, v in sorted(stats.items())))
    if rw.unknown_targets:
        log(f"  {len(rw.unknown_targets)} branch targets in code the port doesn't have (it runs where it was, unported): "
            + " ".join(f"${t:06X}" for t in sorted(rw.unknown_targets)[:8]) + (" ..." if len(rw.unknown_targets) > 8 else ""))
    if failed:
        log(f"  {len(failed)} entries too cramped for a jump and with no room nearby (they're poisoned; a `replace` or more coverage): "
            + " ".join(f"${t:06X}" for t in failed[:12]))
    return path, info
