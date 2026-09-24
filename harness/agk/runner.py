"""Drive vAmiga headless: boot (or restore a cached boot snapshot), play a
scenario, collect artifacts."""
import hashlib
import os
import re
import subprocess
import time

from . import profiles
from .image import Image
from .paths import CACHE, VAMIGA

ECHO_PREFIX = "vAmiga% "
SERIAL_ECHO = re.compile(r"^(?:vAmiga% )?T: ")
DEFAULT_BOOT_TIMEOUT = 3000   # frames (60s PAL) - floppy loads on Kick 1.3 are slow


class RunError(Exception):
    pass


def _sha(path, n=None):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()[:n] if n else h.hexdigest()


def _setup_lines(profile, rom, ext):
    setup = f"regression setup {profile.scheme} {rom}" + (f" {ext}" if ext else "")
    # Frame skipping (default 16 in warp) would make screenshots stale.
    return [setup, *profile.config, "denise set FRAME_SKIPPING 0"]


def boot_key(adf, profile_name, rom, boot_text):
    parts = [_sha(adf), profile_name, _sha(rom), boot_text, _sha(VAMIGA)]
    return hashlib.sha256("\0".join(parts).encode()).hexdigest()[:20]


def _run_vamiga(lines, workdir, timeout=600):
    script = os.path.join(workdir, "run.retrosh")
    with open(script, "w") as f:
        f.write("\n".join(lines + ["shutdown"]) + "\n")
    t0 = time.time()
    try:
        proc = subprocess.run([VAMIGA, "-v", script], capture_output=True, text=True,
                              errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        raise RunError(f"emulator did not finish within {timeout}s wall-clock")
    out = [l[len(ECHO_PREFIX):] if l.startswith(ECHO_PREFIX) else l
           for l in proc.stdout.splitlines() if not SERIAL_ECHO.match(l)]
    with open(os.path.join(workdir, "emulator.log"), "w") as f:
        f.write("\n".join(out) + "\n")
    return proc.returncode, out, time.time() - t0


def _emulator_error(out):
    """Pick the failing line + message out of RetroShell's verbose output."""
    for i, line in enumerate(out):
        if line.startswith("waitserial: timeout"):
            if "<<<" in out[i:]:
                a = out.index("<<<", i)
                b = out.index(">>>", a) if ">>>" in out[a:] else len(out)
                tail = "\n".join(out[a + 1:b]).replace("\r", "").strip()
                return f"{line}\nserial so far:\n{tail or '(nothing)'}"
            return line
        if re.match(r"^Line \d+: ", line) and i + 1 < len(out) and out[i + 1] != "std::exception":
            return f"{line} -> {out[i + 1]}"
    return "emulator exited with an error (see emulator.log)"


def _extract_regs(out, comps):
    """Collect the output block printed after each 'r <comp>' echo line."""
    blocks, current = {}, None
    for line in out:
        cmd = line.strip()
        m = re.match(r"^r (\w+)$", cmd)
        if m or cmd in ("commander", "debugger", "shutdown"):
            current = m.group(1) if m and m.group(1) in comps and m.group(1) not in blocks else None
            if current:
                blocks[current] = []
        elif current:
            blocks[current].append(line)
    return {c: "\n".join(blocks.get(c, [])).strip("\n") for c in comps}


def ensure_boot(adf, profile_name, boot_text, boot_timeout=DEFAULT_BOOT_TIMEOUT, workdir=None):
    """Return path to a snapshot taken on the first frame boundary after boot_text
    appeared on serial. Cached per (adf, profile, rom, boot text, emulator)."""
    profile, rom, ext = profiles.resolve(profile_name)
    os.makedirs(CACHE, exist_ok=True)
    snap = os.path.join(CACHE, boot_key(adf, profile_name, rom, boot_text) + ".vasnap")
    if os.path.exists(snap):
        return snap, None
    workdir = workdir or os.path.join(CACHE, "boot")
    os.makedirs(workdir, exist_ok=True)
    lines = _setup_lines(profile, rom, ext) + [
        f"regression run {os.path.abspath(adf)}",
        f'waitserial "{boot_text}" {boot_timeout}',
        "wait 1 frames",
        f"agk serial {snap}.serial.txt",
        f"agk snapsave {snap}.tmp.vasnap",
    ]
    code, out, secs = _run_vamiga(lines, workdir)
    if code != 0 or not os.path.exists(f"{snap}.tmp.vasnap"):
        raise RunError(f"boot failed ({profile_name}): {_emulator_error(out)}")
    os.replace(f"{snap}.tmp.vasnap", snap)
    return snap, secs


def run(adf, profile_name, scenario, outdir, boot_text, fresh=False):
    """Play a parsed scenario. Returns a result dict (never raises for test failures)."""
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        if f.endswith((".raw", ".png", ".bin", ".txt")):
            os.remove(os.path.join(outdir, f))
    profile, rom, ext = profiles.resolve(profile_name)
    result = {"scenario": scenario.name, "profile": profile_name, "outdir": outdir,
              "ok": True, "failures": [], "screenshots": {}, "boot_seconds": None}

    lines = _setup_lines(profile, rom, ext)
    if fresh:
        # Boot inside this run (no snapshot) - used to verify snapshot parity.
        lines += [f"regression run {os.path.abspath(adf)}",
                  f'waitserial "{boot_text}" {DEFAULT_BOOT_TIMEOUT}', "wait 1 frames"]
    else:
        try:
            snap, boot_secs = ensure_boot(adf, profile_name, boot_text)
        except RunError as e:
            result.update(ok=False, failures=[str(e)])
            return result
        result["boot_seconds"] = boot_secs
        lines += [f"agk snapload {snap}", "denise set FRAME_SKIPPING 0"]

    # Scenario time 0 is one full frame after the boot snapshot point: video
    # buffers aren't part of snapshots, so this makes the first screenshot
    # identical whether we booted fresh or restored.
    lines.append("wait 1 frames")
    lines += [l.replace("{out}", outdir) for l in scenario.lines]
    lines.append(f"agk serial {outdir}/serial.txt")
    code, out, secs = _run_vamiga(lines, outdir)
    result["seconds"] = round(secs, 2)

    serial = ""
    if not fresh and os.path.exists(f"{snap}.serial.txt"):
        # The serial log isn't part of the snapshot: prepend the boot output
        serial = open(f"{snap}.serial.txt", errors="replace").read()
    if os.path.exists(os.path.join(outdir, "serial.txt")):
        serial += open(os.path.join(outdir, "serial.txt"), errors="replace").read()
    serial = serial.replace("\r\n", "\n").replace("\r", "")
    with open(os.path.join(outdir, "serial.txt"), "w") as f:
        f.write(serial)
    result["serial_tail"] = serial[-800:]

    if code != 0:
        result["ok"] = False
        result["failures"].append(_emulator_error(out))

    for name in scenario.screenshots:
        raw = os.path.join(outdir, f"{name}.raw")
        if os.path.exists(raw):
            img = Image.from_raw_file(raw)
            img.save_png(os.path.join(outdir, f"{name}.png"))
            img.screen().save_png(os.path.join(outdir, f"{name}.screen.png"))
            os.remove(raw)
            result["screenshots"][name] = {"png": os.path.join(outdir, f"{name}.png"),
                                           "screen_png": os.path.join(outdir, f"{name}.screen.png"),
                                           "sha256": hashlib.sha256(img.rgb).hexdigest()}
        elif code == 0:
            result["ok"] = False
            result["failures"].append(f"screenshot '{name}' was not produced")

    for name, comps in scenario.regs:
        text = _extract_regs(out, comps)
        with open(os.path.join(outdir, f"{name}.txt"), "w") as f:
            for c in comps:
                f.write(f"== {c} ==\n{text[c]}\n\n")

    for pattern, should_match, lineno in scenario.expects:
        found = re.search(pattern, serial, re.M) is not None
        if found != should_match:
            result["ok"] = False
            what = "expected serial to match" if should_match else "expected serial NOT to match"
            last = [l for l in serial.splitlines() if l.strip()][-3:]
            result["failures"].append(f"line {lineno}: {what} /{pattern}/"
                                      + (f"; last serial lines: {' | '.join(last)}" if last else ""))
    return result
