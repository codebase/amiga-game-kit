"""Drive vAmiga headless: boot (or restore a cached boot snapshot), play a
scenario, collect artifacts."""
import fcntl
import hashlib
import math
import os
import re
import subprocess
import time

from . import profiles
from .scenario import REALIGN, serial_text
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
    return [setup, *profile.config, *_display_lines()]


def _display_lines():
    # Frame skipping (default 16 in warp) would make screenshots stale, and the
    # default monitor emulation (CRT gamma, brightness...) would distort colours:
    # with PALETTE RGB a colour register value 0xRGB appears as (R*17, G*17, B*17).
    # Neither setting is part of a snapshot, so this runs after restores too.
    return ["denise set FRAME_SKIPPING 0", "monitor set PALETTE RGB"]


def boot_key(adf, profile_name, rom, boot_text):
    parts = [_sha(adf), profile_name, _sha(rom), boot_text, _sha(VAMIGA)]
    return hashlib.sha256("\0".join(parts).encode()).hexdigest()[:20]


def _run_vamiga(lines, workdir, timeout=300):
    script = os.path.join(workdir, "run.retrosh")
    with open(script, "w") as f:
        f.write("\n".join(lines + ["shutdown"]) + "\n")
    t0 = time.time()
    try:
        proc = subprocess.run([VAMIGA, "-v", script], capture_output=True, text=True,
                              errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        raise RunError(
            f"emulator stopped making progress ({timeout}s wall-clock). Usually the game crashed the CPU "
            f"(stack overflow, infinite recursion, jump into garbage), which halts emulation so no wait "
            f"ever finishes. Run the same thing with --fresh and print progress with agkPrint() to find "
            f"where it stops. Script: {script}")
    out = [l[len(ECHO_PREFIX):] if l.startswith(ECHO_PREFIX) else l
           for l in proc.stdout.splitlines() if not SERIAL_ECHO.match(l)]
    with open(os.path.join(workdir, "emulator.log"), "w") as f:
        f.write("\n".join(out) + "\n")
    return proc.returncode, out, time.time() - t0


def _emulator_error(out, line_to_source=None, script=None):
    """Pick the failing line + message out of RetroShell's verbose output.
    line_to_source maps a script line number to a scenario line number."""
    executed = 0  # how many script lines RetroShell has echoed so far
    for i, line in enumerate(out):
        if script and executed < len(script) and line.strip() == script[executed]:
            executed += 1
        if line.startswith("waitserial: timeout"):
            src = line_to_source(executed) if line_to_source and executed else None
            if src:
                line = f"line {src}: {line}"
            if "<<<" in out[i:]:
                a = out.index("<<<", i)
                b = out.index(">>>", a) if ">>>" in out[a:] else len(out)
                tail = "\n".join(out[a + 1:b]).replace("\r", "").strip()
                return f"{line}\nserial so far:\n{tail or '(nothing)'}"
            return line
        m = re.match(r"^Line (\d+): (.*)$", line)
        if m and i + 1 < len(out) and out[i + 1] != "std::exception":
            src = line_to_source(int(m.group(1))) if line_to_source else None
            where = f"line {src}" if src else "emulator setup"
            return f"{where}: {out[i + 1]} (emulator command: {m.group(2)})"
    return "emulator exited with an error"


def _nearest_color(screen, x, y, want, radius=24):
    """Where is the expected colour closest to (x,y)? Helps place checks."""
    best = None
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            px, py = x + dx, y + dy
            if 0 <= px < screen.width and 0 <= py < screen.height:
                r, g, b = screen.pixel(px, py)
                if (r >> 4, g >> 4, b >> 4) == want:
                    d = abs(dx) + abs(dy)
                    if best is None or d < best[0]:
                        best = (d, px, py)
    if best:
        return f"; nearest 0x{want[0]:X}{want[1]:X}{want[2]:X} is at ({best[1]},{best[2]})"
    return f"; no 0x{want[0]:X}{want[1]:X}{want[2]:X} within {radius}px"


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
    key = boot_key(adf, profile_name, rom, boot_text)
    snap = os.path.join(CACHE, key + ".vasnap")
    # One boot per key at a time; parallel runs of the same build wait for it
    # and then share the snapshot instead of clobbering each other's files.
    with open(os.path.join(CACHE, key + ".lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return _ensure_boot_locked(adf, profile_name, profile, rom, ext, boot_text, boot_timeout,
                                   snap, workdir or os.path.join(CACHE, "boot-" + key))


def _ensure_boot_locked(adf, profile_name, profile, rom, ext, boot_text, boot_timeout, snap, workdir):
    if os.path.exists(snap):
        os.utime(snap)
        return snap, None
    os.makedirs(workdir, exist_ok=True)
    lines = _setup_lines(profile, rom, ext) + [
        f"regression run {os.path.abspath(adf)}",
        f"waitserial {serial_text(boot_text)} {boot_timeout}",
        "wait 1 frames",
        f"agk serial {snap}.serial.txt",
        f"agk snapsave {snap}.tmp.vasnap",
    ]
    code, out, secs = _run_vamiga(lines, workdir, timeout=120)
    if code != 0 or not os.path.exists(f"{snap}.tmp.vasnap"):
        raise RunError(f"boot failed ({profile_name}): {_emulator_error(out)}\n"
                       f"(emulator log: {os.path.join(workdir, 'emulator.log')})")
    os.replace(f"{snap}.tmp.vasnap", snap)
    _prune_cache()
    return snap, secs


def _prune_cache(keep=24, min_age=3600):
    """Boot snapshots are ~17MB each; keep the most recently used ones, and
    never delete one used in the last hour (another run may be using it)."""
    snaps = sorted((os.path.join(CACHE, f) for f in os.listdir(CACHE) if f.endswith(".vasnap")
                    and not f.endswith(".tmp.vasnap")), key=os.path.getmtime, reverse=True)
    now = time.time()
    for old in snaps[keep:]:
        if now - os.path.getmtime(old) < min_age:
            continue
        key = os.path.basename(old)[:-len(".vasnap")]
        for path in (old, f"{old}.serial.txt", os.path.join(CACHE, key + ".lock")):
            if os.path.exists(path):
                os.remove(path)


def _sync_lines(lines, sync):
    """Adapt scenario lines to the sync mode: 'frames' (video frames) or
    'ticks' (game frames marked by agkPerfBegin/agkTick)."""
    out = []
    for l in lines:
        if l == REALIGN:
            if sync == "frames":
                out.append("wait 1 frames")
            continue
        m = re.match(r"^wait (\d+) frames$", l)
        out.append(f"wait {m.group(1)} ticks" if m and sync == "ticks" else l)
    return out


class outdir_lock:
    """Hold while running into an output directory and reading its results:
    two runs of the same test at once take turns instead of deleting each
    other's files."""

    def __init__(self, outdir):
        self.path = os.path.join(outdir, ".lock")
        os.makedirs(outdir, exist_ok=True)

    def __enter__(self):
        self.f = open(self.path, "w")
        fcntl.flock(self.f, fcntl.LOCK_EX)
        return self

    def __exit__(self, *exc):
        self.f.close()


def run(adf, profile_name, scenario, outdir, boot_text, fresh=False, sync="frames"):
    """Play a parsed scenario. Returns a result dict (never raises for test
    failures). Call inside `with outdir_lock(outdir):` if other processes may
    use the same outdir."""
    for f in os.listdir(outdir):
        if f.endswith((".raw", ".png", ".bin", ".txt", ".wav", ".marks")):
            os.remove(os.path.join(outdir, f))
    profile, rom, ext = profiles.resolve(profile_name)
    result = {"scenario": scenario.name, "profile": profile_name, "outdir": outdir,
              "ok": True, "failures": [], "screenshots": {}, "boot_seconds": None}

    lines = _setup_lines(profile, rom, ext)
    if fresh:
        # Boot inside this run (no snapshot) - used to verify snapshot parity.
        lines += [f"regression run {os.path.abspath(adf)}",
                  f"waitserial {serial_text(boot_text)} {DEFAULT_BOOT_TIMEOUT}", "wait 1 frames"]
    else:
        try:
            snap, boot_secs = ensure_boot(adf, profile_name, boot_text)
        except RunError as e:
            result.update(ok=False, failures=[str(e)])
            return result
        result["boot_seconds"] = boot_secs
        lines += [f"agk snapload {snap}", *_display_lines()]

    # Scenario time 0 is one full frame after the boot snapshot point: video
    # buffers aren't part of snapshots, so this makes the first screenshot
    # identical whether we booted fresh or restored.
    if sync == "ticks":
        # Scenario time 0 = the start of a game frame, after a full frame has
        # been drawn since the snapshot point.
        lines += ["wait 1 frames", "agk sync true", "wait 1 ticks"]
    else:
        lines.append("wait 1 frames")
    lines.append("agk audio start")   # audio.wav starts at scenario time 0
    first = len(lines) + 1   # script line number (1-based) of the scenario's first line
    lines += _sync_lines([l.replace("{out}", outdir) for l in scenario.lines], sync)
    lines.append(f"agk audio save {outdir}/audio.wav")

    def line_to_source(n):
        i = n - first
        return scenario.origins[i] if 0 <= i < len(scenario.origins) and scenario.origins[i] else None
    lines.append(f"agk serial {outdir}/serial.txt")
    try:
        code, out, secs = _run_vamiga(lines, outdir)
    except RunError as e:
        result.update(ok=False, failures=[str(e)])
        return result
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
        result["failures"].append(_emulator_error(out, line_to_source, lines)
                                  + f" (emulator log: {os.path.join(outdir, 'emulator.log')})")

    for name in scenario.screenshots:
        raw = os.path.join(outdir, f"{name}.raw")
        if os.path.exists(raw):
            img = Image.from_raw_file(raw)
            if not profile.aga:
                img = img.canonical12()
            img.save_png(os.path.join(outdir, f"{name}.png"))
            img.screen().save_png(os.path.join(outdir, f"{name}.screen.png"))
            os.remove(raw)
            result["screenshots"][name] = {"png": os.path.join(outdir, f"{name}.png"),
                                           "screen_png": os.path.join(outdir, f"{name}.screen.png"),
                                           "sha256": hashlib.sha256(img.rgb).hexdigest()}
        elif code == 0:
            result["ok"] = False
            result["failures"].append(f"screenshot '{name}' was not produced")

    if scenario.perf:
        reports = [dict(kv.split("=") for kv in re.findall(r"\w+=\d+", l))
                   for l in serial.splitlines() if l.startswith("AGK perf ")]
        for kind, limit, lineno in scenario.perf:
            if not reports:
                result["ok"] = False
                result["failures"].append(
                    f"line {lineno}: no 'AGK perf' lines on serial - call agkPerfBegin()/agkPerfEnd() "
                    f"(agk/perf.h) every frame; reports come every 50 frames, so run at least that long")
                break
            if kind == "dropped":
                dropped = sum(int(r.get("dropped", 0)) for r in reports)
                if dropped:
                    result["ok"] = False
                    result["failures"].append(f"line {lineno}: {dropped} dropped frame(s) "
                                              f"(worst load {max(int(r.get('maxload', 0)) for r in reports)}%)")
            else:
                worst = max(int(r.get("maxload", 0)) for r in reports)
                if worst > limit:
                    result["ok"] = False
                    result["failures"].append(f"line {lineno}: a frame used {worst}% of the frame time, limit {limit}%")
        result["perf"] = reports

    for shot, x, y, want, lineno, near in scenario.colors:
        info = result["screenshots"].get(shot)
        if not info:
            continue  # missing screenshot is already reported
        screen = Image.load_png(info["screen_png"])
        r, g, b = screen.pixel(x, y)
        got = (r >> 4, g >> 4, b >> 4)
        if got != want and near and _nearest_color(screen, x, y, want, near).startswith("; nearest"):
            got = want  # found within the allowed radius
        if got != want:
            result["ok"] = False
            hint = _nearest_color(screen, x, y, want)
            result["failures"].append(
                f"line {lineno}: pixel ({x},{y}) of '{shot}' is 0x{got[0]:X}{got[1]:X}{got[2]:X}, "
                f"expected 0x{want[0]:X}{want[1]:X}{want[2]:X}" + hint)

    _check_audio(scenario, outdir, result)

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


def _check_audio(scenario, outdir, result):
    wav = os.path.join(outdir, "audio.wav")
    if not os.path.exists(wav):
        if scenario.audio:
            result["ok"] = False
            result["failures"].append("no audio.wav was recorded (the emulator stopped early?)")
        return
    result["audio"] = wav
    if not (scenario.audio or scenario.marks):
        return
    from . import sound
    pcm, rate = sound.read_wav(wav)
    marks = {"start": 0, "end": len(pcm)}
    if os.path.exists(wav + ".marks"):
        for line in open(wav + ".marks"):
            name, _, idx = line.strip().rpartition(" ")
            if name:
                marks[name] = int(idx)
    frame = rate // 50
    result["marks"] = {k: v // frame for k, v in marks.items()}   # in frames
    png = os.path.join(outdir, "audio.png")
    sound.spectrogram_png(png, pcm, rate, [(k, v) for k, v in marks.items() if k not in ("start", "end")])
    result["audio_png"] = png
    levels = sound.rms_windows(pcm, rate)
    for kind, a, b, lineno in scenario.audio:
        if a not in marks or b not in marks:
            result["ok"] = False
            result["failures"].append(f"line {lineno}: mark '{a if a not in marks else b}' was never reached")
            continue
        fa, fb = marks[a] // frame, marks[b] // frame
        win = levels[fa:max(fa + 1, fb)]
        loud = max(win, default=0.0)
        at = fa + win.index(loud) if win else fa
        db = 20 * math.log10(loud + 1e-9)
        span = f"between {a} (frame {fa}) and {b} (frame {fb})"
        if kind == "sound" and loud < sound.AUDIBLE_RMS:
            result["ok"] = False
            result["failures"].append(f"line {lineno}: no sound {span}: loudest {db:.0f} dBFS "
                                      f"(audible: > -40) - see {os.path.relpath(png)}")
        elif kind == "silence" and loud >= sound.AUDIBLE_RMS:
            result["ok"] = False
            result["failures"].append(f"line {lineno}: sound {span}, loudest at frame {at} ({db:.0f} dBFS) "
                                      f"- see {os.path.relpath(png)}")
