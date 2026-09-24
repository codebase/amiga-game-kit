#!/usr/bin/env python3
"""Spike harness: run an ADF headless in vAmiga, inject input, capture a
screenshot + serial output, print a JSON summary.

  harness/agk_run.py --adf game.adf [--rom roms/kick13-34005-a500.rom]
                     [--boot 40] [--step "2:joystick2 pull right"] ...
                     [--tail 1] [--name run1] [--out out/]

Each --step is "<wait>:<RetroShell command>", where <wait> is seconds ("2")
or exact video frames ("37f", needs the vAmiga frames patch). After all steps
the harness waits --tail seconds and takes a screenshot (which also ends the
run: vAmiga's `screenshot save` exits the emulator). All waits are measured in
emulated time, not wall-clock time.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "harness"))
from raw2png import raw_to_png  # noqa: E402

VAMIGA = os.environ.get("AGK_VAMIGA", os.path.join(ROOT, "third_party/build-vamiga/VAHeadless"))
SERIAL_CHAR = re.compile(r"T: (\[[0-9a-f]{2}\]|.)$")


def parse_serial(stdout: str) -> str:
    """vAmiga echoes each serial byte as 'T: <c>' (or 'T: [hh]' for control codes)."""
    chars = []
    for line in stdout.splitlines():
        m = SERIAL_CHAR.search(line)
        if not m:
            continue
        tok = m.group(1)
        chars.append(chr(int(tok[1:3], 16)) if len(tok) == 4 else tok)
    return "".join(chars).replace("\r\n", "\n").replace("\r", "")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--adf", required=True)
    ap.add_argument("--rom", default=os.path.join(ROOT, "roms/kick13-34005-a500.rom"))
    ap.add_argument("--ext", default="", help="extension ROM (e.g. AROS ext)")
    ap.add_argument("--scheme", default="A500_OCS_1MB")
    ap.add_argument("--pre", action="append", default=[], help="config command before power-on")
    ap.add_argument("--boot", type=int, default=40, help="seconds before first step")
    ap.add_argument("--step", action="append", default=[], help="'<wait>:<command>'")
    ap.add_argument("--tail", type=int, default=1, help="seconds after last step")
    ap.add_argument("--name", default="agk_run")
    ap.add_argument("--out", default=os.path.join(ROOT, "out"))
    args = ap.parse_args()

    setup = f"regression setup {args.scheme} {os.path.abspath(args.rom)}"
    if args.ext:
        setup += f" {os.path.abspath(args.ext)}"
    lines = [setup, *args.pre,
             "serial set DEVICE RETROSHELL",
             f"regression run {os.path.abspath(args.adf)}",
             f"wait {args.boot} seconds"]
    for step in args.step:
        delay, cmd = step.split(":", 1)
        unit = "frames" if delay.endswith("f") else "seconds"
        delay = int(delay.rstrip("fs"))
        if delay > 0:
            lines.append(f"wait {delay} {unit}")
        lines.append(cmd)
    if args.tail > 0:
        lines.append(f"wait {args.tail} seconds")
    lines.append(f"screenshot save {args.name}")

    os.makedirs(args.out, exist_ok=True)
    raw_path = f"/tmp/{args.name}.raw"
    if os.path.exists(raw_path):
        os.remove(raw_path)
    with tempfile.NamedTemporaryFile("w", suffix=".retrosh", delete=False) as f:
        f.write("\n".join(lines) + "\n")
        script = f.name

    proc = subprocess.run([VAMIGA, "-v", script], capture_output=True, text=True, timeout=600)
    serial = parse_serial(proc.stdout)
    result = {"name": args.name, "exit": proc.returncode, "script": lines, "serial": serial}

    if os.path.exists(raw_path):
        raw = open(raw_path, "rb").read()
        png_path = os.path.join(args.out, f"{args.name}.png")
        with open(png_path, "wb") as f:
            f.write(raw_to_png(raw))
        result["screenshot"] = png_path
        result["sha256"] = hashlib.sha256(raw).hexdigest()
    else:
        result["error"] = "no screenshot produced"
        result["stdout_tail"] = proc.stdout[-2000:]

    with open(os.path.join(args.out, f"{args.name}.serial.txt"), "w") as f:
        f.write(serial)
    print(json.dumps(result, indent=2))
    return 0 if "sha256" in result else 1


if __name__ == "__main__":
    sys.exit(main())
