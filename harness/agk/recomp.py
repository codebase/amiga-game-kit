"""agk recomp: a 68000 game recompiled to C, to run natively.

Inputs, from an `agk inspect ... --coverage --sample` run on the game, at a
point where the game has taken the machine over (a title screen, a level):
    chip.bin        512 KB of chip RAM: the game's code and data as loaded
    slow.bin        the 512 KB at $C00000, if the game uses it (optional)
    coverage.bin    every instruction address executed (agk coverage)
    cpu.txt         the CPU registers at that moment (agk cpu)
    chip.regs.txt   the display registers (agk regs)
Output: out/game.c, the runtime, a memory image and a native binary.

How the code is translated: objdump disassembles the covered address ranges
(coverage says exactly where instructions start, so data in the code is
never mistaken for code), and each instruction becomes a labelled C
statement using recomp/runtime/m68k.h's helpers: registers, flags, memory
access that traps the chips. Branches are gotos; calls push the return
address on the game's own stack like the real thing; rts, rte, jsr (an),
jmp (an) and exceptions go through a dispatch table from address to label.
A jump to an address that was never executed under the emulator stops the
program and says so: run the game further under `agk inspect --coverage`
and recompile.
"""
import os
import re
import subprocess

SIZES = {"b": 8, "w": 16, "l": 32}
CONDS = {"t": "CC_T", "f": "CC_F", "hi": "CC_HI", "ls": "CC_LS", "cc": "CC_CC", "cs": "CC_CS", "ne": "CC_NE",
         "eq": "CC_EQ", "vc": "CC_VC", "vs": "CC_VS", "pl": "CC_PL", "mi": "CC_MI", "ge": "CC_GE", "lt": "CC_LT",
         "gt": "CC_GT", "le": "CC_LE", "hs": "CC_CC", "lo": "CC_CS"}


class RecompError(Exception):
    pass


# ----------------------------------------------------------------- input

def coverage_ranges(paths, lo=0, hi=0x1000000, gap=64):
    """The covered addresses (the union of the coverage files) -> [(start, end)]
    merged over gaps smaller than `gap`, and the set of addresses"""
    bits = bytearray(1 << 21)
    for path in paths:
        for i, b in enumerate(open(path, "rb").read()):
            if b:
                bits[i] |= b
    addrs = []
    for byte in range(lo >> 3, min(len(bits), hi >> 3)):
        b = bits[byte]
        if b:
            for i in range(8):
                if b >> i & 1:
                    addrs.append(byte * 8 + i)
    ranges = []
    for a in addrs:
        if ranges and a - ranges[-1][1] <= gap:
            ranges[-1][1] = a + 2
        else:
            ranges.append([a, a + 2])
    return [(s, e) for s, e in ranges], set(addrs)


OBJDUMP_LINE = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f ]+)\t(\S+)(?:\s+(.*))?$")
OBJDUMP_MORE = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f ]+?)\s*$")     # the rest of an instruction longer than 3 words


def parse_objdump(line, out):
    """append the instruction on an objdump line to out (or extend the last one
    with a continuation line); True if the line was one"""
    m = OBJDUMP_LINE.match(line)
    if m:
        out.append((int(m.group(1), 16), len(m.group(2).split()), m.group(3), (m.group(4) or "").strip()))
        return True
    m = OBJDUMP_MORE.match(line)
    if m and out and out[-1][0] + 2 * out[-1][1] == int(m.group(1), 16):
        addr, words, mn, ops = out[-1]
        out[-1] = (addr, words + len(m.group(2).split()), mn, ops)
        return True
    return False


def image_for(images, addr):
    """(base, path) of the memory image holding addr, or None"""
    for base, path, size in images:
        if base <= addr < base + size:
            return base, path
    return None


def objdump_cmd(images, workdir, addr, stop):
    """the objdump arguments disassembling [addr, stop) out of the right image"""
    found = image_for(images, addr)
    if not found:
        return None
    base, path = found
    rel = os.path.relpath(path, workdir)
    return ["m68k-amigaos-objdump", "-b", "binary", "-m", "m68k:68000", "-D", f"--adjust-vma={base:#x}",
            f"--start-address={addr:#x}", f"--stop-address={stop:#x}", rel]


def disassemble(images, ranges, image, workdir, covered=frozenset()):
    """objdump each range -> [(addr, nwords, mnemonic, operands)]; images are the
    memory dumps as [(base, path, size)] (chip RAM, slow RAM). Every address in
    `covered` (the instructions the emulator executed) ends up an instruction
    start: where a linear decode of a range slid out of step over data or
    padding between executed instructions, the decode restarts at the
    executed address and what it had made of those bytes is dropped."""
    out = []
    for s, e in ranges:
        cmd = objdump_cmd(images, workdir, s, e + 10)
        if not cmd:
            continue                      # code in memory we have no dump of (the ROM)
        r = subprocess.run(["docker", "run", "--rm", "-v", f"{workdir}:/w", "-w", "/w", image, *cmd],
                           capture_output=True, text=True, errors="replace")
        if r.returncode != 0:
            raise RecompError(f"objdump failed: {r.stderr.strip()[:300]}")
        got = []
        for line in r.stdout.splitlines():
            parse_objdump(line, got)
        out += [i for i in got if i[0] < e]
    for _ in range(8):
        starts = {a for a, *_ in out}
        missing = sorted(a for a in covered if a not in starts and image_for(images, a))
        if not missing:
            break
        ends = {}
        for a in missing:
            for s, e in ranges:
                if s <= a < e:
                    ends[a] = e
                    break
            else:
                ends[a] = a + 0x40
        cmds = [(a, objdump_cmd(images, workdir, a, ends[a] + 10)) for a in missing]
        script = " ; ".join(f"echo '@ {a:#x}' ; " + " ".join(cmd) for a, cmd in cmds if cmd)
        r = subprocess.run(["docker", "run", "--rm", "-v", f"{workdir}:/w", "-w", "/w", image, "sh", "-c", script],
                           capture_output=True, text=True, errors="replace")
        if r.returncode != 0:
            raise RecompError(f"objdump failed: {r.stderr.strip()[:300]}")
        new, run, anchor = [], [], None
        for line in r.stdout.splitlines() + ["@ end"]:
            if line.startswith("@ "):
                new += run
                run, anchor = [], (int(line[2:], 16) if line != "@ end" else None)
                continue
            if anchor is None or (run and run[-1][0] in starts and len(run) > 1):
                continue
            before = len(run)
            if parse_objdump(line, run) and len(run) > before and run[-1][0] in starts:
                run.pop()                 # back in step with the earlier decode
                anchor = None
        if not new:
            break
        seen = set()                      # (two anchors' runs can reach the same bytes)
        new = [i for i in new if not (i[0] in seen or seen.add(i[0]))]
        spans = [(a, a + 2 * w) for a, w, *_ in new]
        def overlaps(i):
            a, w = i[0], i[1]
            return any(a < e2 and s2 < a + 2 * w for s2, e2 in spans)
        out = sorted([i for i in out if not overlaps(i)] + new)
    return out


def disassemble_from(images, anchors, image, workdir, known, limit=0x800, trusted=frozenset()):
    """objdump from each anchor address (a branch target the coverage didn't
    reach) until a known instruction, an unconditional jump or data: the code
    the emulator never ran but the game can get to. One docker call for all
    the anchors. `known` maps every word of an existing instruction to that
    instruction's address; `trusted` are the instructions the coverage saw. An
    anchor's own instruction may displace earlier sweep instructions it overlaps
    (they decoded data or padding), never a trusted one. -> (new instructions
    in order, the displaced instructions' addresses)"""
    cmds = [(a, objdump_cmd(images, workdir, a, a + limit)) for a in anchors]
    script = " ; ".join(f"echo '@ {a:#x}' ; " + " ".join(cmd) for a, cmd in cmds if cmd)
    if not script:
        return []
    r = subprocess.run(["docker", "run", "--rm", "-v", f"{workdir}:/w", "-w", "/w", image, "sh", "-c", script],
                       capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        raise RecompError(f"objdump failed: {r.stderr.strip()[:300]}")
    out, run, stop, displaced = [], [], False, set()
    for line in r.stdout.splitlines() + ["@ end"]:
        if line.startswith("@ "):
            out += run
            run, stop = [], False
            continue
        if stop:
            continue
        before, prev_words = len(run), (run[-1][1] if run else 0)
        if not parse_objdump(line, run):
            continue
        addr, words, mn, ops = run[-1]
        grew = len(run) == before         # a continuation line: the last instruction is longer than it looked
        first = len(run) == 1             # the anchor's own instruction
        overlap = {known[w] for w in (addr + 2 * i for i in range(prev_words if grew else 0, words)) if w in known}
        if first and overlap and not (overlap & trusted) and addr not in overlap:
            displaced |= overlap          # the anchor wins over what a sweep made of these bytes
            for w, start in list(known.items()):
                if start in overlap:
                    del known[w]
            overlap = set()
        if overlap:
            run.pop()                     # back in known code (or straddling it: this run was misaligned)
            stop = True
            continue
        if mn in (".short", ".long", "illegal"):
            run.pop()                     # data: this target was a jump table entry or similar, leave it
            stop = True
            continue
        for i in range(words):
            known[addr + 2 * i] = addr
        if re.fullmatch(r"(bra|jmp|rts|rte|rtr|illegal|reset)[swl]?", mn):
            stop = True
    return out, displaced


def follow_branches(images, instrs, image, workdir, hi=0x1000000, rounds=12, anchors=()):
    """Add the code reachable by constant branches from `instrs` (repeatedly,
    until nothing new turns up), and from `anchors`: addresses a native run
    jumped to (needed.txt). -> (instrs sorted, the number added)"""
    known = {}                            # every word of every instruction -> the instruction's address
    for addr, words, *_ in instrs:
        for i in range(words):
            known[addr + 2 * i] = addr
    trusted = frozenset(a for a, *_ in instrs)
    added = 0
    extra = set(anchors)
    for _ in range(rounds):
        t = Translator(instrs, set())
        t.translate()
        targets = set(t.unknown) | extra
        extra = set()
        # a target inside an instruction means that instruction was mis-decoded (a
        # sweep that started in data, or padding that happens to decode): drop it
        bad = {known[a] for a in targets if a in known and known[a] != a and known[a] not in trusted}
        if bad:
            instrs = [i for i in instrs if i[0] not in bad]
            known = {w: a for w, a in known.items() if a not in bad}
        for addr, words, mn, _ in instrs:          # the fall-through of the last instruction of a range
            if not re.fullmatch(r"(bra|jmp|rts|rte|rtr|illegal|reset|\.short|\.long)[swl]?", mn):
                targets.add(addr + 2 * words)
        anchors = sorted(a for a in targets if a < hi and a % 2 == 0 and a not in known)
        if not anchors:
            break
        new, displaced = disassemble_from(images, anchors, image, workdir, known, trusted=trusted)
        if displaced:
            instrs = [i for i in instrs if i[0] not in displaced]
        if not new:
            break
        instrs = sorted(instrs + new)
        added += len(new)
    return instrs, added


# ------------------------------------------------------------- operands

REG_RE = re.compile(r"^(d[0-7]|a[0-7]|sp|fp)$")


def reg_name(t):
    if t == "sp":
        return "A[7]"
    if t == "fp":
        return "A[6]"
    return f"{t[0].upper()}[{t[1]}]"


def is_areg(t):
    return t in ("sp", "fp") or (len(t) == 2 and t[0] == "a")


def parse_disp(text, hexadecimal=False):
    """objdump's displacement: decimal in a0@(-56); hex (and 64-bit negative) in a0@(1e,d2:w)"""
    text = text.strip()
    if not hexadecimal and re.fullmatch(r"-?\d+", text):
        return int(text)
    v = int(text, 16)
    if v >= 1 << 63:
        v -= 1 << 64
    elif v >= 1 << 31:
        v -= 1 << 32
    return v


class Operand:
    """One operand as C: setup statements, the address (for memory), the read
    expression and a write statement. `bits` is the access size."""

    def __init__(self, text, bits, pc_next):
        self.text, self.bits = text, bits
        self.setup, self.addr, self.kind = [], None, None
        self.reg = None
        self.imm = None
        t = text.strip()
        m = re.fullmatch(r"#(-?\d+)", t)
        if m:
            self.kind, self.imm = "imm", int(m.group(1)) & 0xFFFFFFFF
            return
        if REG_RE.match(t):
            self.kind, self.reg = "areg" if is_areg(t) else "dreg", reg_name(t)
            return
        m = re.fullmatch(r"(d[0-7]|a[0-7]|sp|fp)@([+-]?)", t)
        if m:
            r = reg_name(m.group(1))
            step = 2 if (m.group(1) == "sp" and bits == 8) else bits // 8
            if m.group(2) == "+":
                self.setup.append(f"ea = {r}; {r} += {step};")
            elif m.group(2) == "-":
                self.setup.append(f"{r} -= {step}; ea = {r};")
            else:
                self.setup.append(f"ea = {r};")
            self.kind, self.addr = "mem", "ea"
            return
        m = re.fullmatch(r"(d[0-7]|a[0-7]|sp|fp)@\(([^,()]+)\)", t)
        if m:
            self.setup.append(f"ea = {reg_name(m.group(1))} + ({parse_disp(m.group(2))});")
            self.kind, self.addr = "mem", "ea"
            return
        m = re.fullmatch(r"(d[0-7]|a[0-7]|sp|fp)@\(([^,()]*),(d[0-7]|a[0-7]|sp|fp):([wl])\)", t)
        if m:
            idx = reg_name(m.group(3))
            idx = f"(u32)(s32)(s16){idx}" if m.group(4) == "w" else idx
            disp = parse_disp(m.group(2), hexadecimal=True) if m.group(2) else 0
            self.setup.append(f"ea = {reg_name(m.group(1))} + ({disp}) + {idx};")
            self.kind, self.addr = "mem", "ea"
            return
        m = re.fullmatch(r"%pc@\(([0-9a-fx]+)(?:\s*<[^>]*>)?\)", t)
        if m:
            self.setup.append(f"ea = {int(m.group(1), 16):#x}u;")
            self.kind, self.addr = "mem", "ea"
            return
        m = re.fullmatch(r"%pc@\(([0-9a-fx]+)(?:\s*<[^>]*>)?,(d[0-7]|a[0-7]|sp|fp):([wl])\)", t)
        if m:
            idx = reg_name(m.group(2))
            idx = f"(u32)(s32)(s16){idx}" if m.group(3) == "w" else idx
            # objdump's base already includes the displacement; the index is added
            self.setup.append(f"ea = {int(m.group(1), 16):#x}u + {idx};")
            self.kind, self.addr = "mem", "ea"
            return
        m = re.fullmatch(r"0x([0-9a-f]+)(?:\s*<[^>]*>)?", t)
        if m:
            self.setup.append(f"ea = {int(m.group(1), 16):#x}u;")
            self.kind, self.addr = "mem", "ea"
            return
        m = re.fullmatch(r"%(sr|ccr|usp)", t)
        if m:
            self.kind = m.group(1)
            return
        raise RecompError(f"operand not understood: {text!r}")

    def read(self):
        if self.kind == "imm":
            return f"{self.imm:#x}u"
        if self.kind == "dreg":
            return f"({self.reg} & MASK({self.bits}))" if self.bits != 32 else self.reg
        if self.kind == "areg":
            return f"({self.reg} & MASK({self.bits}))" if self.bits != 32 else self.reg
        if self.kind == "mem":
            return f"rd{self.bits}({self.addr})"
        if self.kind == "sr":
            return "get_sr()"
        if self.kind == "ccr":
            return "(get_sr() & 0xFF)"
        if self.kind == "usp":
            return "(cpu.s ? cpu.usp : A[7])"
        raise RecompError(f"can't read {self.text}")

    def write(self, value):
        if self.kind == "dreg":
            n = self.reg[2]
            return f"setd({n}, {value}, {self.bits});"
        if self.kind == "areg":
            return f"{self.reg} = {value};"           # (callers sign-extend word writes)
        if self.kind == "mem":
            return f"wr{self.bits}({self.addr}, {value});"
        if self.kind == "sr":
            return f"set_sr({value});"
        if self.kind == "ccr":
            return f"set_ccr({value});"
        if self.kind == "usp":
            return f"if (cpu.s) cpu.usp = {value}; else A[7] = {value};"
        raise RecompError(f"can't write {self.text}")


def reglist(text):
    """d0-d3/a0/a2-a4 -> ['D[0]', ...] in order d0..d7, a0..a7"""
    order = [f"d{i}" for i in range(8)] + [f"a{i}" for i in range(8)]
    out = set()
    for part in text.split("/"):
        if "-" in part:
            a, b = part.split("-")
            a = "a7" if a == "sp" else a
            b = "a7" if b == "sp" else b
            for r in order[order.index(a):order.index(b) + 1]:
                out.add(r)
        else:
            out.add("a7" if part in ("sp", "fp") and part == "sp" else ("a6" if part == "fp" else part))
    return [reg_name(r) for r in order if r in out]


# ----------------------------------------------------------- translate

def split_operands(text):
    """'a4@(0,d6:w),a1' -> ['a4@(0,d6:w)', 'a1'] (commas inside parentheses stay)"""
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur:
        out.append(cur)
    return [o.strip() for o in out]


BRANCH_RE = re.compile(r"^(bra|bsr|b(?:hi|ls|cc|cs|ne|eq|vc|vs|pl|mi|ge|lt|gt|le|hs|lo))[sw]?$")
DBCC_RE = re.compile(r"^db(f|t|ra|hi|ls|cc|cs|ne|eq|vc|vs|pl|mi|ge|lt|gt|le)$")
SCC_RE = re.compile(r"^s(t|f|hi|ls|cc|cs|ne|eq|vc|vs|pl|mi|ge|lt|gt|le)$")
NO_SIZE = {"rts", "rte", "rtr", "nop", "stop", "illegal", "trap", "swap", "exg", "lea", "pea", "jmp", "jsr",
           "link", "unlk", "reset", "moveq", "trapv", "bkpt", "tas", "chk", ".short", ".long"}


def mnemonic_size(mn):
    """'movew' -> ('move', 16); branches, dbcc, scc and the sizeless ones -> (mn, None)"""
    if mn in NO_SIZE or BRANCH_RE.match(mn) or DBCC_RE.match(mn) or SCC_RE.match(mn):
        return mn, None
    if mn[-1] in "bwl" and len(mn) > 2:
        return mn[:-1], SIZES[mn[-1]]
    return mn, None


class Translator:
    def __init__(self, instrs, covered):
        self.instrs = instrs
        self.covered = covered
        self.addrs = {a for a, *_ in instrs}
        self.lines = []
        self.pending_ticks = 0
        self.unknown = {}

    def label(self, addr):
        return f"L_{addr:06x}"

    def tick(self):
        if self.pending_ticks:
            self.lines.append(f"\tTICK({self.pending_ticks * 8});")
            self.pending_ticks = 0

    def goto(self, target, from_addr):
        """a jump to a constant address"""
        if target in self.addrs:
            return f"goto {self.label(target)};"
        self.unknown[target] = from_addr
        return f"{{ cpu.pc = {target:#x}u; goto dispatch; }}"

    def emit_irq_check(self, addr):
        self.lines.append(f"\tif (irq_pending > (int)cpu.ipl) {{ cpu.pc = take_interrupt(irq_pending, {addr:#x}u); goto dispatch; }}")
        self.lines.append(f"\tITRACE({addr:#x}u);")       # (traced once it will really execute)
        self.traced = True

    def translate(self):
        L = self.lines
        for addr, words, mn, ops in self.instrs:
            nxt = addr + 2 * words
            label_at = len(L)
            L.append(f"{self.label(addr)}:")
            if addr not in self.covered and mn in ("illegal", ".short", ".long"):
                L.append(f"\ttrace_die(\"data reached as code\", {addr:#x}u);")
                continue
            self.pending_ticks += 1
            self.traced = False
            try:
                self.instr(addr, nxt, mn, ops)
            except RecompError as e:
                raise RecompError(f"at ${addr:06X} '{mn} {ops}': {e}")
            if not self.traced:
                L.insert(label_at + 1, f"\tITRACE({addr:#x}u);")
            if nxt not in self.addrs and not re.fullmatch(r"(bra|jmp|rts|rte|rtr|illegal|reset|\.short|\.long)[swl]?", mn):
                self.unknown[nxt] = addr
                L.append(f"\t{{ cpu.pc = {nxt:#x}u; goto dispatch; }}   /* the next instruction was never seen */")
        return L

    def instr(self, addr, nxt, mn, ops):
        L = self.lines
        base, bits = mnemonic_size(mn)
        o = split_operands(ops) if ops else []

        def opnd(i, b=None):
            return Operand(o[i], b or bits or 16, nxt)

        def emit(op, *stmts):
            for s in op.setup:
                L.append("\t" + s)
            for s in stmts:
                L.append("\t" + s)

        # --- moves
        if base == "move" and o and (o[0] in ("%sr", "%ccr", "%usp") or o[1] in ("%sr", "%ccr", "%usp")):
            width = 32 if "%usp" in o else 16    # the USP is a whole address; SR and CCR are words
            src, dst = opnd(0, width), opnd(1, width)
            emit(src); emit(dst, dst.write(src.read()))
            return
        if base == "move":
            src, dst = opnd(0), opnd(1)
            emit(src, f"tmp = {src.read()};")
            emit(dst, dst.write("tmp"), f"flags_logic(tmp, {bits});")
            return
        if base == "movea":
            src, dst = opnd(0), opnd(1)
            emit(src, f"tmp = {src.read()};")
            emit(dst, dst.write(f"sext(tmp, {bits})"))
            return
        if base == "moveq":
            dst = opnd(1, 32)
            imm = Operand(o[0], 32, nxt).imm
            v = imm if imm < 128 else imm - 256 if imm < 256 else imm
            v &= 0xFFFFFFFF
            if v & 0x80 and v < 0x100:
                v = (v - 0x100) & 0xFFFFFFFF
            emit(dst, dst.write(f"{v:#x}u"), f"flags_logic({v:#x}u, 32);")
            return
        if base == "movem":
            if o[1].startswith(("d", "a", "sp", "fp")) and "@" not in o[1] or o[0].startswith(("d", "a")) and "@" not in o[0] and not o[0].endswith(("@", "+", "-", ")")):
                pass
            if "@" in o[0] or o[0].startswith("0x") or o[0].startswith("%pc"):
                # memory -> registers
                regs = reglist(o[1])
                m = re.fullmatch(r"(d[0-7]|a[0-7]|sp|fp)@\+", o[0])
                if m:
                    r = reg_name(m.group(1))
                    L.append(f"\tea = {r};")
                    for reg in regs:
                        L.append(f"\t{reg} = {'rd32(ea)' if bits == 32 else 'sext(rd16(ea), 16)'}; ea += {bits // 8};")
                    L.append(f"\t{r} = ea;")
                else:
                    op = Operand(o[0], bits, nxt)
                    emit(op)
                    for reg in regs:
                        L.append(f"\t{reg} = {'rd32(ea)' if bits == 32 else 'sext(rd16(ea), 16)'}; ea += {bits // 8};")
            else:
                regs = reglist(o[0])
                m = re.fullmatch(r"(d[0-7]|a[0-7]|sp|fp)@-", o[1])
                if m:
                    r = reg_name(m.group(1))
                    L.append(f"\tea = {r};")
                    for reg in reversed(regs):
                        L.append(f"\tea -= {bits // 8}; wr{bits}(ea, {reg});")
                    L.append(f"\t{r} = ea;")
                else:
                    op = Operand(o[1], bits, nxt)
                    emit(op)
                    for reg in regs:
                        L.append(f"\twr{bits}(ea, {reg}); ea += {bits // 8};")
            return
        if base == "lea":
            src, dst = opnd(0, 32), opnd(1, 32)
            emit(src); emit(dst, dst.write("ea"))
            return
        if base == "pea":
            src = opnd(0, 32)
            emit(src, "push32(ea);")
            return
        if base == "exg":
            a, b = opnd(0, 32), opnd(1, 32)
            L.append(f"\ttmp = {a.reg}; {a.reg} = {b.reg}; {b.reg} = tmp;")
            return
        if base == "swap":
            d = opnd(0, 32)
            L.append(f"\t{d.reg} = ({d.reg} << 16) | ({d.reg} >> 16); flags_logic({d.reg}, 32);")
            return
        if base == "ext":
            d = opnd(0, 32)
            if bits == 16:
                L.append(f"\tsetd({d.reg[2]}, sext({d.reg}, 8), 16); flags_logic({d.reg} & 0xFFFF, 16);")
            else:
                L.append(f"\t{d.reg} = sext({d.reg}, 16); flags_logic({d.reg}, 32);")
            return
        if base == "link":
            a = opnd(0, 32)
            imm = Operand(o[1], 16, nxt).imm
            disp = imm if imm < 0x8000 else imm - 0x10000
            L.append(f"\tpush32({a.reg}); {a.reg} = A[7]; A[7] += ({disp});")
            return
        if base == "unlk":
            a = opnd(0, 32)
            L.append(f"\tA[7] = {a.reg}; {a.reg} = pop32();")
            return
        # --- arithmetic
        if base in ("clr",):
            d = opnd(0)
            emit(d, d.write("0"), f"flags_logic(0, {bits});")
            return
        if base in ("tst",):
            d = opnd(0)
            emit(d, f"flags_logic({d.read()}, {bits});")
            return
        if base in ("neg", "negx", "not"):
            d = opnd(0)
            if base == "not":
                emit(d, f"tmp = ~{d.read()} & MASK({bits});", d.write("tmp"), f"flags_logic(tmp, {bits});")
            else:
                emit(d, f"tmp = op_{base}({d.read()}, {bits});", d.write("tmp"))
            return
        if base in ("add", "addi", "addq", "sub", "subi", "subq", "adda", "suba", "addx", "subx"):
            src, dst = opnd(0), opnd(1)
            if base in ("adda", "suba") or (dst.kind == "areg" and base in ("addq", "subq", "add", "sub")):
                op = "+" if base.startswith("add") else "-"
                emit(src, f"tmp = sext({src.read()}, {bits});")
                emit(dst, f"{dst.reg} = {dst.reg} {op} tmp;")
                return
            fn = {"add": "op_add", "addi": "op_add", "addq": "op_add", "sub": "op_sub", "subi": "op_sub", "subq": "op_sub",
                  "addx": "op_addx", "subx": "op_subx"}[base]
            emit(src, f"tmp = {src.read()};")
            emit(dst, f"tmp = {fn}(tmp, {dst.read()}, {bits});", dst.write("tmp"))
            return
        if base in ("cmp", "cmpi", "cmpa", "cmpm"):
            src, dst = opnd(0), opnd(1)
            if base == "cmpa" or dst.kind == "areg":
                emit(src, f"tmp = sext({src.read()}, {bits});")
                emit(dst, f"op_cmp(tmp, {dst.reg}, 32);")
            else:
                emit(src, f"tmp = {src.read()};")
                emit(dst, f"op_cmp(tmp, {dst.read()}, {bits});")
            return
        if base in ("and", "andi", "or", "ori", "eor", "eori"):
            src, dst = opnd(0), opnd(1)
            if dst.kind in ("sr", "ccr"):
                opc = {"and": "&", "andi": "&", "or": "|", "ori": "|", "eor": "^", "eori": "^"}[base]
                emit(src, f"tmp = {src.read()};")
                emit(dst, dst.write(f"({dst.read()} {opc} tmp)"))
                return
            opc = {"and": "&", "andi": "&", "or": "|", "ori": "|", "eor": "^", "eori": "^"}[base]
            emit(src, f"tmp = {src.read()};")
            emit(dst, f"tmp = ({dst.read()} {opc} tmp) & MASK({bits});", dst.write("tmp"), f"flags_logic(tmp, {bits});")
            return
        if base in ("mulu", "muls"):
            src, dst = opnd(0, 16), opnd(1, 32)
            emit(src, f"tmp = {src.read()};")
            emit(dst, f"{dst.reg} = op_{base}(tmp, {dst.reg});")
            return
        if base in ("divu", "divs"):
            src, dst = opnd(0, 16), opnd(1, 32)
            emit(src, f"tmp = {src.read()};")
            emit(dst, f"{dst.reg} = op_{base}(tmp, {dst.reg}, &ok); if (!ok) {{ cpu.pc = take_exception(5, {nxt:#x}u); goto dispatch; }}")
            return
        if base in ("asl", "asr", "lsl", "lsr", "rol", "ror", "roxl", "roxr"):
            if len(o) == 1:                       # memory, by one, word
                d = opnd(0, 16)
                emit(d, f"tmp = op_{base}({d.read()}, 1, 16);", d.write("tmp"))
                return
            src, dst = opnd(0, 32), opnd(1)
            count = f"{src.imm & 0xFF}u" if src.kind == "imm" else f"({src.reg} & 63)"
            if src.kind == "imm" and src.imm == 0:
                count = "8u"
            L.append(f"\ttmp = op_{base}({dst.read()}, {count}, {bits});")
            L.append("\t" + dst.write("tmp"))
            return
        if base in ("btst", "bchg", "bclr", "bset"):
            src, dst = Operand(o[0], 8, nxt), Operand(o[1], 8, nxt)
            if dst.kind == "dreg":
                dst.bits = 32
                bit = f"({src.imm & 31}u)" if src.kind == "imm" else f"({src.reg} & 31)"
                emit(src)
                L.append(f"\t{dst.reg} = op_{base}({dst.reg}, {bit});")
            else:
                bit = f"({src.imm & 7}u)" if src.kind == "imm" else f"({src.reg} & 7)"
                emit(src); emit(dst, f"tmp = op_{base}({dst.read()}, {bit});")
                if base != "btst":
                    L.append("\t" + dst.write("tmp"))
            return
        if base == "tas":
            d = opnd(0, 8)
            emit(d, f"tmp = {d.read()}; flags_logic(tmp, 8);", d.write("tmp | 0x80"))
            return
        if base == "chk":
            src, dst = opnd(0, 16), opnd(1, 16)
            emit(src, f"tmp = {src.read()};")
            L.append(f"\tif ((s16){dst.read()} < 0 || (s16){dst.read()} > (s16)tmp) {{ cpu.pc = take_exception(6, {nxt:#x}u); goto dispatch; }}")
            return
        # --- control
        if base in ("bra", "bras", "braw"):
            self.tick()
            target = int(o[0].split()[0], 16)
            if target <= addr:
                self.emit_irq_check(addr)
            L.append("\t" + self.goto(target, addr))
            return
        if base in ("bsr", "bsrs", "bsrw"):
            self.tick()
            target = int(o[0].split()[0], 16)
            self.emit_irq_check(addr)
            L.append(f"\tpush32({nxt:#x}u); " + self.goto(target, addr))
            return
        m = re.fullmatch(r"b(hi|ls|cc|cs|ne|eq|vc|vs|pl|mi|ge|lt|gt|le|hs|lo)[sw]?", mn)
        if m:
            self.tick()
            target = int(o[0].split()[0], 16)
            if target <= addr:
                self.emit_irq_check(addr)
            L.append(f"\tif ({CONDS[m.group(1)]}) " + self.goto(target, addr))
            return
        m = re.fullmatch(r"db(f|t|hi|ls|cc|cs|ne|eq|vc|vs|pl|mi|ge|lt|gt|le|ra)", mn)
        if m:
            self.tick()
            d = Operand(o[0], 16, nxt)
            target = int(o[1].split()[0], 16)
            cond = "CC_F" if m.group(1) in ("f", "ra") else CONDS[m.group(1)]
            self.emit_irq_check(addr)
            L.append(f"\tif (!({cond})) {{ tmp = ({d.reg} - 1) & 0xFFFF; {d.reg} = ({d.reg} & 0xFFFF0000u) | tmp; "
                     f"if (tmp != 0xFFFF) {self.goto(target, addr)} }}")
            return
        m = re.fullmatch(r"s(t|f|hi|ls|cc|cs|ne|eq|vc|vs|pl|mi|ge|lt|gt|le)", mn)
        if m and base not in ("sub", "swap"):
            d = Operand(o[0], 8, nxt)
            emit(d, d.write(f"({CONDS[m.group(1)]} ? 0xFF : 0)"))
            return
        if base == "jmp":
            self.tick()
            op = Operand(o[0], 32, nxt)
            emit(op)
            if op.text.startswith("0x") and "@" not in op.text:
                L.append("\t" + self.goto(int(op.text.split()[0], 16), addr))
            else:
                L.append("\tcpu.pc = ea; goto dispatch;")
            return
        if base == "jsr":
            self.tick()
            op = Operand(o[0], 32, nxt)
            emit(op)
            self.emit_irq_check(addr)
            if op.text.startswith("0x") and "@" not in op.text:
                L.append(f"\tpush32({nxt:#x}u); " + self.goto(int(op.text.split()[0], 16), addr))
            else:
                L.append(f"\tpush32({nxt:#x}u); cpu.pc = ea; goto dispatch;")
            return
        if base == "rts":
            self.tick()
            L.append("\tcpu.pc = pop32(); goto dispatch;")
            return
        if base == "rtr":
            self.tick()
            L.append("\tset_ccr(pop16()); cpu.pc = pop32(); goto dispatch;")
            return
        if base == "rte":
            self.tick()
            L.append("\ttmp = pop16(); cpu.pc = pop32(); set_sr(tmp); goto dispatch;")
            return
        if base == "trap":
            self.tick()
            n = Operand(o[0], 8, nxt).imm
            L.append(f"\tcpu.pc = take_exception(32 + {n}, {nxt:#x}u); goto dispatch;")
            return
        if base == "nop":
            return
        if base == "stop":
            self.tick()
            imm = Operand(o[0], 16, nxt).imm
            L.append(f"\tset_sr({imm:#x}u); cpu.stopped = 1; while (cpu.stopped) {{ TICK(64); if (irq_pending > (int)cpu.ipl) "
                     f"{{ cpu.pc = take_interrupt(irq_pending, {nxt:#x}u); goto dispatch; }} }}")
            return
        if base in ("illegal", ".short", ".long", "reset", "trapv", "bkpt"):
            L.append(f"\ttrace_die(\"illegal instruction\", {addr:#x}u);")
            return
        raise RecompError(f"instruction not translated: {mn}")


def generate(instrs, covered, entry_pc):
    t = Translator(instrs, covered)
    body = t.translate()
    out = ['#include "m68k.h"', "#include <stdlib.h>", "static int recomp_trace;", "",
           "/* Generated by agk recomp. Each label is an instruction's address. */",
           "void run_translated(void) {",
           "\tstatic void *table[0x100000];       /* chip RAM, by address / 2 */",
           "\tstatic void *slow[0x100000];        /* $C00000.. likewise */",
           "\tstatic int ready = 0;", "\tu32 ea, tmp; int ok;",
           "\tif (!ready) {", "\t\tready = 1; recomp_trace = getenv(\"RECOMP_TRACE\") != 0;"]
    for a in sorted(t.addrs):
        if a < 0x200000:
            out.append(f"\t\ttable[{a:#x}u >> 1] = &&{t.label(a)};")
        elif 0xC00000 <= a < 0xE00000:
            out.append(f"\t\tslow[({a:#x}u - 0xC00000u) >> 1] = &&{t.label(a)};")
    out += ["\t}", "\tgoto dispatch;", "dispatch:",
            "\tif (recomp_trace) fprintf(stderr, \"-> %06X  sr %04X a7 %06X\\n\", cpu.pc, get_sr(), A[7]);",
            "\tif (cpu.pc < 0x200000u) { if (table[cpu.pc >> 1]) goto *table[cpu.pc >> 1]; }",
            "\telse if (cpu.pc >= 0xC00000u && cpu.pc < 0xE00000u && slow[(cpu.pc - 0xC00000u) >> 1]) goto *slow[(cpu.pc - 0xC00000u) >> 1];",
            "\ttrace_die(\"jump to code that was never executed under the emulator (run agk inspect --coverage further)\", cpu.pc);",
            "\treturn;"]
    out += body
    out += ["}", ""]
    return "\n".join(out), t.unknown


def write_project(outdir, instrs, covered, cpu_regs, mem_files, regs, runtime_dir):
    import shutil
    os.makedirs(outdir, exist_ok=True)
    text, unknown = generate(instrs, covered, cpu_regs["PC"])
    with open(os.path.join(outdir, "game.c"), "w") as f:
        f.write(text)
    for name in os.listdir(runtime_dir):
        if name.endswith((".c", ".h")):
            shutil.copy(os.path.join(runtime_dir, name), outdir)
    # the start state: registers, the custom registers, the memory image
    with open(os.path.join(outdir, "start.h"), "w") as f:
        f.write("/* the state the game starts from (agk cpu / agk regs) */\n")
        for i in range(8):
            f.write(f"#define START_D{i} {cpu_regs[f'D{i}']:#x}u\n")
        for i in range(8):
            f.write(f"#define START_A{i} {cpu_regs[f'A{i}']:#x}u\n")
        f.write(f"#define START_PC {cpu_regs['PC']:#x}u\n#define START_SR {cpu_regs['SR']:#x}u\n")
        f.write(f"#define START_USP {cpu_regs['USP']:#x}u\n#define START_SSP {cpu_regs['SSP']:#x}u\n")
        for name in ("DF0_CYL", "DF0_SIDE", "DF0_MOTOR"):     # the drive's head (older dumps: at track 0)
            f.write(f"#define START_{name} {regs.get(name, 0)}\n")
        f.write("static const struct { unsigned off, val; } start_regs[] = {\n")
        offsets = {"BPLCON0": 0x100, "BPLCON1": 0x102, "BPLCON2": 0x104, "DIWSTRT": 0x8E, "DIWSTOP": 0x90,
                   "DDFSTRT": 0x92, "DDFSTOP": 0x94, "BPL1MOD": 0x108, "BPL2MOD": 0x10A,
                   "DMACON": 0x96, "INTENA": 0x9A, "INTREQ": 0x9C, "ADKCON": 0x9E}
        for name, val in regs.items():
            off = offsets.get(name)
            if name.startswith("COLOR"):
                off = 0x180 + 2 * int(name[5:])
            if off is not None:
                f.write(f"\t{{{off:#x}, {val & 0xFFFF:#x}}},\n")
                continue
            # 32-bit pointers: written as their two halves
            ptr = None
            if name in ("COP1LC", "COP2LC"):
                ptr = 0x80 if name == "COP1LC" else 0x84
            elif re.fullmatch(r"BPL[1-8]PT", name):
                ptr = 0xE0 + 4 * (int(name[3]) - 1)
            elif re.fullmatch(r"SPR[0-7]PT", name):
                ptr = 0x120 + 4 * int(name[3])
            elif re.fullmatch(r"AUD[0-3]LC", name):
                ptr = 0xA0 + 16 * int(name[3])
            if ptr is not None:
                f.write(f"\t{{{ptr:#x}, {(val >> 16) & 0xFFFF:#x}}}, {{{ptr + 2:#x}, {val & 0xFFFF:#x}}},\n")
        f.write("};\n")
        f.write("static const struct { unsigned cia, reg, val; } start_cia[] = {\n")
        cia_regs = {"PRA": 0, "PRB": 1, "DDRA": 2, "DDRB": 3, "TALATCH": 4, "TBLATCH": 6, "IMR": 13, "CRA": 14, "CRB": 15,
                    "TA": 16, "TB": 17}
        for name, val in regs.items():
            m = re.fullmatch(r"CIA([AB])_(\w+)", name)
            if m and m.group(2) in cia_regs:
                f.write(f"\t{{{0 if m.group(1) == 'A' else 1}, {cia_regs[m.group(2)]}, {val & 0xFFFF:#x}}},\n")
        f.write("};\n")
    with open(os.path.join(outdir, "memory.bin"), "wb") as f:
        for path, base in mem_files:
            data = open(path, "rb").read()
            f.write(base.to_bytes(4, "little") + len(data).to_bytes(4, "little") + data)
    return unknown


def read_cpu(path):
    regs = {}
    for line in open(path):
        parts = line.split()
        if len(parts) == 2:
            regs[parts[0]] = int(parts[1], 16)
    return regs
