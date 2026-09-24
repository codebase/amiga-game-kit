"""Scenario files: a line-based script of what to do to a running game.

Time is measured in video frames (PAL: 50/s) from the moment the game is
"ready" (the boot text appeared on serial). Every input change lands exactly
on a frame boundary, so the same scenario always produces the same result.

    # tests/move.agk
    press right 20          # hold joystick right for 20 frames, then release
    wait 5                  # let 5 more frames run
    screenshot moved        # compared against tests/golden/<profile>/move/moved.png
    expect-serial "x=192"   # regex, checked against the whole serial log

Commands:
    wait N                          run N frames
    wait-serial "TEXT" [TIMEOUT]    run until TEXT appears on serial (timeout in frames, default 500)
    press INPUT [N]                 hold INPUT for N frames (default 1), then release it
    hold INPUT / release [INPUT]    hold until released; bare 'release' releases everything
    key CODE                        tap a raw Amiga keycode (e.g. 0x45 = Esc); vAmiga holds it
                                    for 0.5s (25 frames) in the background - use 'wait' to let it act
    screenshot NAME                 capture the current frame
    dump-mem NAME ADDR LEN          save LEN bytes of memory at ADDR to NAME.bin
    regs NAME [cpu|agnus|copper|denise|paula|blitter|ciaa|ciab]...   save register view to NAME.txt
    expect-serial "REGEX"           serial log must match (checked after the run)
    expect-no-serial "REGEX"        serial log must not match

INPUT is up, down, left, right, fire, fire2, or a combination such as right+fire.
Joystick commands go to port 2 (the normal game port).
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


@dataclass
class Scenario:
    name: str
    lines: list = field(default_factory=list)        # RetroShell lines (with {out} placeholder)
    screenshots: list = field(default_factory=list)
    dumps: list = field(default_factory=list)         # (name, kind)
    regs: list = field(default_factory=list)          # (name, [components])
    expects: list = field(default_factory=list)       # (regex, should_match, lineno)
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
            text_arg = args[0].replace('"', "")
            sc.lines.append(f'waitserial "{text_arg}" {timeout}')
            sc.lines.append("wait 1 frames")   # re-align to a frame boundary

        elif cmd in ("press", "hold"):
            if not args or len(args) > (2 if cmd == "press" else 1):
                raise ScenarioError(f"line {lineno}: usage: {cmd} INPUT" + (" [FRAMES]" if cmd == "press" else ""))
            ins = _inputs(args[0], lineno)
            for i in ins:
                sc.lines.append(f"joystick2 {DIRECTIONS[i][0]}")
            if cmd == "hold":
                held.update(ins)
                continue
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

        else:
            raise ScenarioError(f"line {lineno}: unknown command '{cmd}'")

    for i in sorted(held):   # leave the joystick centred at the end
        sc.lines.append(f"joystick2 {DIRECTIONS[i][1]}")
    return sc
