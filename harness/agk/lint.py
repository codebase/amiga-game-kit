"""agk lint: finds the game's own functions that call GCC's maths library.

On a 68000, C that looks cheap can compile to a library call:
- `a * b` on 32-bit ints: ___mulsi3 (MULS/MULU only take 16 bits)
- `/` and `%` on ints: ___divsi3, ___modsi3, ... (DIVS/DIVU give 16-bit results)
- 64-bit and floating-point maths: always a library call (there's no FPU)

Each one costs far more than the instruction you'd pick by hand, and in code
that runs every frame it adds up. The lint disassembles the project's object
files (build/CMakeFiles/NAME.dir, so ACE's and the kit's code, and the
art and sound agk generates, are left out)
and lists every function that reaches one.

Functions that run once are left out unless --all: those named *Create,
*Destroy, *Init, *Load, *Setup or listed in agk.toml [lint] allow = [...],
and any function that only those call (found from the disassembly's calls
and function pointers).
"""
import glob
import os
import re
import subprocess
import sys

# The helpers, and what each one means in C
HELPERS = {
    "___mulsi3": "32-bit multiply",
    "___divsi3": "signed 32-bit divide",
    "___udivsi3": "unsigned 32-bit divide",
    "___modsi3": "signed 32-bit %",
    "___umodsi3": "unsigned 32-bit %",
    "___muldi3": "64-bit multiply",
    "___divdi3": "64-bit divide",
    "___udivdi3": "64-bit divide",
    "___moddi3": "64-bit %",
    "___umoddi3": "64-bit %",
    "___ashldi3": "64-bit shift",
    "___ashrdi3": "64-bit shift",
    "___lshrdi3": "64-bit shift",
}
_FLOAT = re.compile(r"^___(add|sub|mul|div|neg|cmp|eq|ne|lt|le|gt|ge|float|fix|extend|trunc)\w*[sd]f\w*$")

HINTS = {
    "mul": "use 16-bit operands (a UWORD/WORD pair compiles to MULU/MULS), or shifts for powers of two",
    "div": "use a shift for powers of two, or DIVU/DIVS with a 16-bit divisor (inline asm)",
    "wide": "avoid 64-bit types in game code",
    "float": "use fixed point (there's no FPU; every float op is a library call)",
}

SETUP = re.compile(r"(Create|Destroy|Init|Load|Setup)$")


def describe(sym):
    if sym in HELPERS:
        what = HELPERS[sym]
        kind = "wide" if "64" in what else "mul" if "multiply" in what else "div"
        return what, kind
    if _FLOAT.match(sym):
        return "floating point", "float"
    return None


def clean(sym):
    """_logicUpdate.part.0 -> logicUpdate (C names; GCC's clones folded in)"""
    name = sym[1:] if sym.startswith("_") else sym
    return re.sub(r"\.(part|isra|constprop|cold|lto_priv)\.\d+$", "", name)


def parse(dis):
    """objdump -dr output -> ({(object file, function): {helper: references}},
                              {function: {functions that call or reference it}})"""
    found, refs, funcs = {}, [], set()
    obj = func = None
    for line in dis.splitlines():
        m = re.match(r"^(\S+):\s+file format", line)
        if m:
            obj = m.group(1)
            continue
        m = re.match(r"^[0-9a-f]+ <([^>+]+)>:$", line)
        if m:
            func = m.group(1)
            funcs.add(clean(func))
            continue
        if not func:
            continue
        m = re.search(r"RELOC\d+\s+(\S+)", line)
        if m and describe(m.group(1)):
            r = found.setdefault((obj, func), {})
            r[m.group(1)] = r.get(m.group(1), 0) + 1
        # calls, tail calls and function pointers: a relocation, or a PC-relative <_name>
        for target in ([m.group(1)] if m else []) + re.findall(r"<([^>+]+)>", line):
            refs.append((clean(func), clean(target)))
    callers = {}
    for caller, callee in refs:
        if callee in funcs and callee != caller:
            callers.setdefault(callee, set()).add(caller)
    return found, callers


def setup_only(callers, allow=()):
    """Setup functions by name (or agk.toml allow), and every function that
    only setup functions call. Functions nothing references (main, callbacks
    the game registers) count as per-frame."""
    names = set(callers) | {c for cs in callers.values() for c in cs}
    setup = {n for n in names if n in allow or SETUP.search(n)}
    grew = True
    while grew:
        grew = False
        for f, cs in callers.items():
            if f not in setup and cs and cs <= setup:
                setup.add(f)
                grew = True
    return setup


def objects(proj):
    base = os.path.join(proj["dir"], "build", "CMakeFiles", f"{proj['name']}.dir")
    return sorted(o for o in glob.glob(os.path.join(base, "**", "*.obj"), recursive=True)
                  # agk's generated art.c / sound.c: set up once, not per frame
                  if not re.search(r"[/\\](art|sound)[/\\](art|sound)\.c\.obj$", o))


def objdump(workdir, files, image, flags="-dr"):
    """objdump each file (one process per file: the toolchain's objdump can
    crash when given several) -> (text, [files it failed on])"""
    script = (f'for f in "$@"; do m68k-amigaos-objdump {flags} "$f" || echo "AGK-OBJDUMP-FAILED $f"; done')
    r = subprocess.run(["docker", "run", "--rm", "-v", f"{workdir}:/w", "-w", "/w", image,
                        "sh", "-c", script, "sh", *files], capture_output=True, text=True, errors="replace")
    if r.returncode != 0 and not r.stdout:
        raise RuntimeError(f"objdump failed: {r.stderr.strip()[:500]}")
    failed = re.findall(r"^AGK-OBJDUMP-FAILED (\S+)$", r.stdout, re.M)
    return r.stdout, failed


def disassemble(proj, objs, image):
    rel = [os.path.relpath(o, proj["dir"]) for o in objs]
    try:
        text, failed = objdump(proj["dir"], rel, image)
    except RuntimeError as e:
        raise SystemExit(f"lint: {e}")
    for f in failed:
        print(f"lint: objdump couldn't read {f}; its functions aren't checked", file=sys.stderr)
    return text


def source_of(obj):
    # build/CMakeFiles/NAME.dir/src/main.c.obj -> src/main.c
    m = re.search(r"\.dir/(.+)\.obj$", obj)
    return m.group(1) if m else obj


def findings(parsed, allow=(), everything=False):
    """-> [(source, function, [(helper, count, what, kind)])], and how many setup functions were skipped"""
    found, callers = parsed
    setup = setup_only(callers, set(allow))
    out, skipped = [], 0
    for (obj, func), refs in sorted(found.items()):
        name = clean(func)
        if not everything and (name in setup or SETUP.search(name) or name in allow):
            skipped += 1
            continue
        calls = [(h, n) + describe(h) for h, n in sorted(refs.items())]
        out.append((source_of(obj), name, calls))
    return out, skipped


def report(items, skipped):
    lines = []
    for src, func, calls in items:
        what = ", ".join(f"{w} ({h.lstrip('_')} x{n})" for h, n, w, _ in calls)
        lines.append(f"{src}: {func}() calls the maths library: {what}")
        for kind in sorted({k for *_, k in calls}):
            lines.append(f"    fix: {HINTS[kind]}")
    if skipped:
        lines.append(f"({skipped} setup function(s) left out: *Create/*Destroy/*Init/*Load/*Setup, "
                     f"agk.toml [lint] allow, and what only they call; --all shows them)")
    return lines
