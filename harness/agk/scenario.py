r"""Scenario files: a line-based script of what to do to a running game.

Time is measured in frames (PAL: 50/s) from the moment the game is "ready"
(the boot text appeared on serial). With sync = "ticks" in agk.toml (the
template's default) a frame is one iteration of the game's main loop, marked
by agkPerfBegin(): input changes land right before the game reads its input,
so the same scenario gives the same result on every machine profile no matter
how long the game's frame work takes. With sync = "frames" (games that don't
call agkPerfBegin) frames are video frames and changes land at line 0.

    # tests/move.agk
    press right 20          # hold joystick right for 20 frames, then release
    wait 5                  # let 5 more frames run
    screenshot moved        # compared against tests/golden/move/moved.png (agk test)
    expect-serial "x=192"   # regex, checked against the whole serial log
    expect-color moved 160 100 0xFA0   # pixel (160,100) of 'moved' is Amiga colour $FA0

Commands:
    wait N                          run N frames
    wait-serial "TEXT" [TIMEOUT]    run until the literal TEXT appears on serial (timeout in frames,
                                    default 500). With tick sync it resumes at the start of the next
                                    game frame; with frame sync, the game may run one more frame first.
    press INPUT [N]                 hold INPUT for N frames (default 1), then release it
    hold INPUT / release [INPUT]    hold until released; bare 'release' releases everything
    key CODE                        tap a raw Amiga keycode (e.g. 0x45 = Esc); vAmiga holds it
                                    for 0.5s (25 frames) in the background - use 'wait' to let it act
    screenshot NAME                 capture the current frame
    dump-mem NAME ADDR LEN          save LEN bytes of memory at ADDR to NAME.bin
    regs NAME [cpu|agnus|copper|denise|paula|blitter|ciaa|ciab]...   save register view to NAME.txt
    expect-serial "REGEX"           serial log must match (checked after the run)
    expect-no-serial "REGEX"        serial log must not match
    expect-color SHOT X Y 0xRGB     pixel X,Y (game coordinates, 320x256) of screenshot SHOT
                                    must be the 12-bit Amiga colour 0xRGB (e.g. 0xFA0)
    expect-no-dropped-frames        the game never missed a frame (needs agk/perf.h in the game)
    expect-max-load PERCENT         no frame used more than PERCENT of the frame time (agk/perf.h)

Goldens: `agk test` compares each screenshot with tests/golden/<test>/<shot>.png
(shared by all profiles; a profile that differs on purpose gets
tests/golden/<test>/<profile>/<shot>.png). Record them with `agk test --update`.

INPUT is up, down, left, right, fire, fire2, or a combination such as right+fire.
Joystick commands go to port 2 (the normal game port).

Regexes are Python `re.search` with MULTILINE: ^ and $ match at line starts and
ends, and . does not cross lines (use [\s\S] for that). Backslashes inside
"..." reach the regex unchanged ("\d+" works).

Timing: the game reads the joystick once per frame, so holding for N frames
moves it N times. The game's own frame counter doesn't equal scenario time (it
started counting before AGK ready). A screenshot shows the last frame the
display finished; with double buffering that is one step behind the serial
log. To check an exact state, sync on the game's own output (wait-serial
"x=100"), then screenshot.
"""
import re
import shlex
from dataclasses import dataclass, field

DIRECTIONS = {"left": ("pull left", "release x"), "right": ("pull right", "release x"),
              "up": ("pull up", "release y"), "down": ("pull down", "release y"),
              "fire": ("press 1", "unpress 1"), "fire2": ("press 2", "unpress 2")}
REG_COMPONENTS = {"cpu", "agnus", "copper", "denise", "paula", "blitter", "ciaa", "ciab"}
NAME_RE = re.compile(r"^[A-Za-z0-9_.-]+$")


class ScenarioError(Exception):
    pass


# Placeholder after wait-serial; the runner turns it into "wait 1 frames" in
# frame sync and drops it in tick sync (which resumes on a tick already).
REALIGN = "#realign"


def serial_text(text):
    """Encode text for vAmiga's waitserial (hex, so '=' etc. survive parsing)."""
    return "hex:" + text.encode("latin-1").hex()


@dataclass
class Scenario:
    name: str
    lines: list = field(default_factory=list)        # RetroShell lines (with {out} placeholder)
    screenshots: list = field(default_factory=list)
    dumps: list = field(default_factory=list)         # (name, kind)
    regs: list = field(default_factory=list)          # (name, [components])
    expects: list = field(default_factory=list)       # (regex, should_match, lineno)
    colors: list = field(default_factory=list)        # (shot, x, y, (r4, g4, b4), lineno)
    perf: list = field(default_factory=list)          # ("dropped", 0, lineno) / ("maxload", pct, lineno)
    origins: list = field(default_factory=list)       # scenario line number of each entry in lines
    frames: int = 0                                   # frames of scenario time


def _inputs(spec, lineno):
    parts = spec.split("+")
    for p in parts:
        if p not in DIRECTIONS:
            raise ScenarioError(f"line {lineno}: unknown input '{p}' (use {', '.join(DIRECTIONS)})")
    return parts


def _int(tok, lineno, what):
    try:
        v = int(tok, 0)
    except ValueError:
        raise ScenarioError(f"line {lineno}: {what} must be a number, got '{tok}'")
    if v < 0:
        raise ScenarioError(f"line {lineno}: {what} must be >= 0")
    return v


def _name(tok, lineno, seen):
    if not NAME_RE.match(tok):
        raise ScenarioError(f"line {lineno}: name '{tok}' may only use letters, digits, _ . -")
    if tok in seen:
        raise ScenarioError(f"line {lineno}: name '{tok}' used twice")
    seen.add(tok)
    return tok


def parse(text, name="scenario"):
    sc = Scenario(name)
    held, names = set(), set()

    def run_frames(n):
        if n > 0:
            sc.lines.append(f"wait {n} frames")
            sc.frames += n

    for lineno, raw in enumerate(text.splitlines(), 1):
        start = len(sc.lines)  # lines generated from here on come from this lineno
        try:
            toks = shlex.split(raw, comments=True)
        except ValueError as e:
            raise ScenarioError(f"line {lineno}: {e}")
        if not toks:
            continue
        cmd, args = toks[0], toks[1:]

        if cmd == "wait":
            if len(args) != 1:
                raise ScenarioError(f"line {lineno}: usage: wait FRAMES")
            run_frames(_int(args[0], lineno, "frames"))

        elif cmd == "wait-serial":
            if not 1 <= len(args) <= 2:
                raise ScenarioError(f'line {lineno}: usage: wait-serial "TEXT" [TIMEOUT]')
            timeout = _int(args[1], lineno, "timeout") if len(args) == 2 else 500
            sc.lines.append(f"waitserial {serial_text(args[0])} {timeout}")
            sc.lines.append(REALIGN)   # frame sync: re-align to a frame boundary

        elif cmd in ("press", "hold"):
            if not args or len(args) > (2 if cmd == "press" else 1):
                raise ScenarioError(f"line {lineno}: usage: {cmd} INPUT" + (" [FRAMES]" if cmd == "press" else ""))
            ins = _inputs(args[0], lineno)
            for i in ins:
                sc.lines.append(f"joystick2 {DIRECTIONS[i][0]}")
            if cmd == "hold":
                held.update(ins)
            else:
                run_frames(_int(args[1], lineno, "frames") if len(args) == 2 else 1)
                for i in ins:
                    sc.lines.append(f"joystick2 {DIRECTIONS[i][1]}")

        elif cmd == "release":
            targets = _inputs(args[0], lineno) if args else sorted(held)
            for i in targets:
                sc.lines.append(f"joystick2 {DIRECTIONS[i][1]}")
                held.discard(i)

        elif cmd == "key":
            if len(args) != 1:
                raise ScenarioError(f"line {lineno}: usage: key CODE")
            code = _int(args[0], lineno, "keycode")
            if code > 0x7F:
                raise ScenarioError(f"line {lineno}: keycode must be 0x00-0x7F")
            sc.lines.append(f"keyboard press {code}")

        elif cmd == "screenshot":
            if len(args) != 1:
                raise ScenarioError(f"line {lineno}: usage: screenshot NAME")
            n = _name(args[0], lineno, names)
            sc.screenshots.append(n)
            sc.lines.append(f"agk screenshot {{out}}/{n}.raw")

        elif cmd == "dump-mem":
            if len(args) != 3:
                raise ScenarioError(f"line {lineno}: usage: dump-mem NAME ADDR LEN")
            n = _name(args[0], lineno, names)
            addr, length = _int(args[1], lineno, "address"), _int(args[2], lineno, "length")
            if addr > 0xFFFFFF or length == 0 or addr + length > 0x1000000:
                raise ScenarioError(f"line {lineno}: dump-mem must stay inside the 24-bit address space "
                                    f"(0x000000-0xFFFFFF), length > 0")
            sc.dumps.append((n, "mem"))
            sc.lines.append(f"mem save bin {{out}}/{n}.bin {addr} {length}")

        elif cmd == "regs":
            if not args:
                raise ScenarioError(f"line {lineno}: usage: regs NAME [COMPONENT...]")
            n = _name(args[0], lineno, names)
            comps = args[1:] or ["cpu", "agnus", "copper", "denise"]
            for c in comps:
                if c not in REG_COMPONENTS:
                    raise ScenarioError(f"line {lineno}: unknown component '{c}' (use {', '.join(sorted(REG_COMPONENTS))})")
            sc.regs.append((n, comps))
            sc.lines.append("debugger")
            sc.lines += [f"r {c}" for c in comps]
            sc.lines.append("commander")

        elif cmd in ("expect-serial", "expect-no-serial"):
            if len(args) != 1:
                raise ScenarioError(f'line {lineno}: usage: {cmd} "REGEX"')
            try:
                re.compile(args[0])
            except re.error as e:
                raise ScenarioError(f"line {lineno}: bad regex: {e}")
            sc.expects.append((args[0], cmd == "expect-serial", lineno))

        elif cmd == "expect-no-dropped-frames":
            if args:
                raise ScenarioError(f"line {lineno}: usage: expect-no-dropped-frames")
            sc.perf.append(("dropped", 0, lineno))

        elif cmd == "expect-max-load":
            if len(args) != 1:
                raise ScenarioError(f"line {lineno}: usage: expect-max-load PERCENT")
            pct = _int(args[0], lineno, "percent")
            if not 1 <= pct <= 100:
                raise ScenarioError(f"line {lineno}: percent must be 1-100")
            sc.perf.append(("maxload", pct, lineno))

        elif cmd == "expect-color":
            if len(args) != 4:
                raise ScenarioError(f"line {lineno}: usage: expect-color SHOT X Y 0xRGB")
            x, y = _int(args[1], lineno, "x"), _int(args[2], lineno, "y")
            if x >= 320 or y >= 256:
                raise ScenarioError(f"line {lineno}: x,y must be inside the 320x256 playfield")
            rgb = _int(args[3], lineno, "colour")
            if rgb > 0xFFF:
                raise ScenarioError(f"line {lineno}: colour must be 12-bit, 0x000-0xFFF")
            sc.colors.append((args[0], x, y, ((rgb >> 8) & 15, (rgb >> 4) & 15, rgb & 15), lineno))

        else:
            raise ScenarioError(f"line {lineno}: unknown command '{cmd}'")
        sc.origins += [lineno] * (len(sc.lines) - start)

    for i in sorted(held):   # leave the joystick centred at the end
        sc.lines.append(f"joystick2 {DIRECTIONS[i][1]}")
    sc.origins += [0] * (len(sc.lines) - len(sc.origins))
    for shot, *_rest, lineno in sc.colors:
        if shot not in sc.screenshots:
            raise ScenarioError(f"line {lineno}: expect-color refers to unknown screenshot '{shot}'")
    return sc
