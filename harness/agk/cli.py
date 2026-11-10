"""agk - Amiga Game Kit command line.

    agk doctor                      check toolchain, emulator, ROMs
    agk build [PROJECT]             compile + make bootable ADF
    agk run [PROJECT] [-s STEP]...  boot, play steps, save screenshots + serial
    agk run tests/NAME.agk          run one scenario file (same as -f)
    agk test [PROJECT] [--update]   run tests/*.agk, compare screenshots to goldens
    agk play [PROJECT]              play it in FS-UAE (arrow keys + Space)
    agk unit [PROJECT]              compile game logic for the host and run tests/unit/*.c
    agk new DIR                     start a new game from the template
    agk art [PROJECT]               convert art/ (text or PNG) to sprites/BOBs + previews
    agk art-gen NAME "PROMPT"       generate pixel art with Retro Diffusion (palette-constrained)

PROJECT is a directory with an agk.toml (default: current directory).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tomllib
import types

from . import profiles, runner, scenario
from .image import SCREEN_X0, SCREEN_Y0, Image, diff
from .paths import ROOT, VAMIGA

DEFAULT_RUN_STEPS = ["screenshot screen"]


def load_project(path):
    path = os.path.abspath(path or ".")
    cfg_path = os.path.join(path, "agk.toml")
    if not os.path.exists(cfg_path):
        hint = ""
        name = os.path.basename(path)
        if os.path.exists(os.path.join(os.getcwd(), "tests", f"{name}.agk")):
            hint = f" - to run the test '{name}': agk test --only {name}  (or: agk run tests/{name}.agk)"
        raise SystemExit(f"no agk.toml in {path}{hint}")
    with open(cfg_path, "rb") as f:
        cfg = tomllib.load(f)
    name = cfg.get("name") or os.path.basename(path)
    return {
        "dir": path,
        "name": name,
        "profile": cfg.get("profile", "a500"),
        "profiles": cfg.get("test_profiles", [cfg.get("profile", "a500")]),
        "boot": cfg.get("boot", "AGK ready"),
        "adf": os.path.join(path, "build", f"{name}.adf"),
        "unit_sources": cfg.get("unit_sources", []),
        "lint_allow": cfg.get("lint", {}).get("allow", []),
        "release": cfg.get("release", {}),
        "sync": cfg.get("sync", "frames"),
        "cmake": {k: ("ON" if v is True else "OFF" if v is False else str(v))
                  for k, v in cfg.get("cmake", {}).items()},
    }


def _newest_source(proj):
    newest = 0.0
    # The kit's runtime library is compiled into every game too.
    for root in (proj["dir"], os.path.join(ROOT, "runtime"),
                 os.path.join(ROOT, "third_party", "ACE", "src"), os.path.join(ROOT, "third_party", "ACE", "include")):
        for base, dirs, files in os.walk(root):
            dirs[:] = [d for d in dirs if d not in ("build", "tests", ".git")]  # prune in place
            for f in files:
                if (f.endswith((".c", ".h", ".s", ".asm", ".i", ".cmake", ".toml", ".png", ".tmj", ".tsj"))
                    or os.path.basename(base) == "disk"
                    or f == "CMakeLists.txt" or (os.path.basename(base) == "art" and f.endswith(".txt"))
                    or (os.path.basename(base) == "sound" and f.endswith(".mml"))):
                    newest = max(newest, os.path.getmtime(os.path.join(base, f)))
    return newest


def need_adf(proj, build=True):
    """Make sure build/<name>.adf exists and is newer than the sources,
    rebuilding if needed so tests never run against stale code."""
    # Regenerate art first (cheap; files are only rewritten when their content
    # changes), so edited art or a newer converter counts as a source change.
    if (not _run_art(proj, quiet=True) or not _run_sound(proj, quiet=True, previews=False)
            or not _run_maps(proj, quiet=True) or not _run_disk(proj, quiet=True)):
        raise SystemExit(1)
    newest = _newest_source(proj)
    for gen in ("art/art.c", "art/art.h", "sound/sound.c", "sound/sound.h", "maps/maps.c", "maps/maps.h"):
        p = os.path.join(proj["dir"], "build", gen)
        if os.path.exists(p):
            newest = max(newest, os.path.getmtime(p))
    stale = not os.path.exists(proj["adf"]) or os.path.getmtime(proj["adf"]) < newest
    if not stale:
        return
    if not build:
        raise SystemExit(f"{rel(proj['adf'])} is missing or older than the sources - run: agk build")
    print("sources changed since the last build - building first", file=sys.stderr)
    rc = subprocess.run(_build_cmd(proj), stdout=sys.stderr).returncode
    if rc != 0:
        raise SystemExit(rc)


def rel(p):
    r = os.path.relpath(p)
    return os.path.abspath(p) if r.startswith(os.pardir + os.sep + os.pardir) else r


# ---------------------------------------------------------------- commands

def cmd_doctor(args):
    ok = True

    def line(good, what, hint=""):
        nonlocal ok
        ok &= good
        print(f"  [{'ok' if good else '!!'}] {what}" + (f"  -> {hint}" if hint and not good else ""))

    print("toolchain")
    docker = shutil.which("docker") is not None
    line(docker, "docker installed", "install Docker Desktop")
    if docker:
        up = subprocess.run(["docker", "info"], capture_output=True).returncode == 0
        line(up, "docker daemon running", "start Docker Desktop")
    print("emulator")
    have = os.path.exists(VAMIGA)
    line(have, f"VAHeadless at {rel(VAMIGA)}", "run tools/setup")
    if have:
        line("waitserial" in open(VAMIGA, "rb").read().decode("latin-1"),
             "AGK patch applied", "re-run tools/setup")
    print("profiles")
    roms = profiles.find_roms()
    for p in profiles.PROFILES.values():
        if p.rom == "aros":
            good = os.path.exists(profiles.AROS_ROM)
            line(good, f"{p.name:10} {p.description}", "run tools/setup")
        else:
            good = p.rom in roms
            where = f" ({rel(roms[p.rom])})" if good else ""
            print(f"  [{'ok' if good else '--'}] {p.name:10} {p.description}{where}"
                  + ("" if good else f"  -> needs {p.rom} in roms/"))
    return 0 if ok else 1


def _build_cmd(proj, extra=()):
    """tools/build with -D options from agk.toml [cmake] plus command-line ones.
    (-D is the reliable way: a plain set() in CMakeLists loses to ACE's cache
    defaults on the first configure.)"""
    opts = dict(proj["cmake"])
    for d in extra:
        k, _, v = d.partition("=")
        opts[k] = v
    return [os.path.join(ROOT, "tools", "build"), proj["dir"], *(f"-D{k}={v}" for k, v in opts.items())]


def _run_art(proj, quiet=False):
    """Convert art/ (if any). Returns False on errors (already printed)."""
    from . import art
    try:
        assets = art.build(proj["dir"])
    except art.ArtError as e:
        print(f"art error: {e}", file=sys.stderr)
        return False
    if assets is None:
        return True
    for a in assets:
        if not quiet or a.warnings:
            extra = (f"channel {a.channel}, colours {art._hex(a.sprite_colors)}" if a.kind == "sprite"
                     else f"{a.depth} planes, {len(a.colors_used())} colours")
            print(f"  art {a.name}: {a.kind} {a.w}x{a.h} x{len(a.frames)} frame(s), {extra}", file=sys.stderr)
        for w in a.warnings:
            print(f"    warning: {w}", file=sys.stderr)
    if not quiet:
        print(f"  previews: {rel(os.path.join(proj['dir'], 'build', 'art', 'preview'))}/", file=sys.stderr)
    return True


def _run_maps(proj, quiet=False):
    """Convert maps/ (if any). Returns False on errors (already printed)."""
    from . import tilemap
    try:
        maps = tilemap.build(proj["dir"])
    except tilemap.MapError as e:
        print(f"map error: {e}", file=sys.stderr)
        return False
    if maps is None:
        return True
    for m in maps:
        if not quiet or m["warnings"]:
            layers = ", ".join(f"{n} ({ts})" if ts else n for n, _, ts in m["layers"])
            print(f"  map {m['name']}: {m['w']}x{m['h']} tiles of {m['tile']} px; layers {layers}; "
                  f"{len(m['objects'])} objects", file=sys.stderr)
        for w in m["warnings"]:
            print(f"    warning: {w}", file=sys.stderr)
    if not quiet:
        print(f"  previews: {rel(os.path.join(proj['dir'], 'build', 'maps', 'preview'))}/", file=sys.stderr)
    return True


def _run_disk(proj, quiet=False):
    """Pack disk/ (if any) into build/disk/. Returns False on errors (already printed)."""
    from . import pack
    try:
        done = pack.build(proj["dir"])
    except (ValueError, OSError) as e:
        print(f"disk error: {e}", file=sys.stderr)
        return False
    if done is None:
        return True
    for name, raw, packed in done:
        if not quiet:
            print(f"  disk {name}: {raw} -> {packed} bytes", file=sys.stderr)
    return True


def cmd_map(args):
    proj = load_project(args.project)
    if not os.path.isdir(os.path.join(proj["dir"], "maps")):
        print("no maps/ directory: put Tiled maps there as JSON (.tmj); agk help-map", file=sys.stderr)
        return 1
    return 0 if _run_maps(proj) else 1


def _run_sound(proj, quiet=False, previews=True):
    """Convert sound/ (if any). Returns False on errors (already printed)."""
    from . import sound
    try:
        res = sound.build(proj["dir"], previews=previews)
    except sound.SoundError as e:
        print(f"sound error: {e}", file=sys.stderr)
        return False
    if res is None or quiet:
        return True
    for s in res["sfx"]:
        print(f"  sfx {s['name']}: {s['seconds']:.2f} s, {len(s['data'])} bytes, volume {s['volume']}, "
              f"priority {s['priority']}", file=sys.stderr)
    for m in res["music"]:
        i = m["info"]
        insts = ", ".join(f"{n} ({w}{f' cycle {c}' if c else ''})" for n, w, c, _b in i["instruments"])
        print(f"  music {m['name']}: {i['seconds']:.1f} s loop, {i['rows']} rows at {i['tempo']} BPM, "
              f"{i['patterns']} pattern(s), {len(m['mod'])} bytes; instruments: {insts}", file=sys.stderr)
    print(f"  chip RAM for samples: {res['chip_bytes']} bytes", file=sys.stderr)
    print(f"  previews (listen, or look at the spectrograms): "
          f"{rel(os.path.join(res['out_dir'], 'preview'))}/", file=sys.stderr)
    return True


def cmd_sound(args):
    proj = load_project(args.project)
    if not os.path.exists(os.path.join(proj["dir"], "sound", "sound.toml")):
        raise SystemExit("no sound/sound.toml in this project - see: agk help-sound")
    return 0 if _run_sound(proj) else 1


def cmd_art(args):
    proj = load_project(args.project)
    if not os.path.exists(os.path.join(proj["dir"], "art", "art.toml")):
        raise SystemExit("no art/art.toml in this project - see: agk help-art")
    return 0 if _run_art(proj) else 1


def cmd_art_gen(args):
    from . import rd, art
    proj = load_project(args.project)
    try:
        w, h = (int(v) for v in args.size.lower().split("x"))
    except ValueError:
        raise SystemExit("--size must look like 32x16")
    if args.kind == "sprite" and w > 16:
        raise SystemExit("a hardware sprite is at most 16 px wide - use --kind bob, or --size 16xH")
    art_dir = os.path.join(proj["dir"], "art")
    os.makedirs(art_dir, exist_ok=True)
    try:
        colors = [] if args.free_colors else rd.sprite_colors_from(proj["dir"], args.colors)
        # Retro Diffusion's smallest sizes are 16 px; generate at least that big
        gw, gh = max(w, 16), max(h, 16)
        images, cost, left = rd.generate(args.prompt, gw, gh, colors, args.style, args.seed,
                                         args.n, dry_run=args.dry_run, tile_x=args.tile_x,
                                         remove_bg=not args.opaque, aga=art.project_aga(proj["dir"]))
    except rd.GenError as e:
        raise SystemExit(f"art-gen: {e}")
    if args.dry_run:
        print(f"would cost ${cost} (balance ${left}); nothing generated")
        return 0
    if not images:
        raise SystemExit("art-gen: the API returned no images")
    saved = []
    for i, png in enumerate(images):
        name = args.name if i == 0 else f"{args.name}_alt{i}"
        path = os.path.join(art_dir, f"{name}.png")
        with open(path, "wb") as f:
            f.write(png)
        saved.append(path)
    extra = {}
    if args.kind == "bitmap":
        extra["depth"] = args.depth
        if args.free_colors:
            extra["palette"] = "auto"
        if args.tile_x:
            extra["wrap_x"] = 320
    rd.add_to_art_toml(art_dir, args.name, f"{args.name}.png", args.kind,
                       channel=args.channel if args.kind == "sprite" else None, extra=extra)
    print(f"generated {', '.join(rel(p) for p in saved)} (${cost}, balance ${left})")
    if (gw, gh) != (w, h):
        print(f"note: generated at {gw}x{gh} (the API minimum); crop or set frame sizes in art.toml")
    if args.free_colors and args.kind != "bitmap":
        fit = "--palette" if args.kind == "bob" else "--colors 15 (--pin 0xRGB@REG for shared registers)"
        print(f"next: fit it to the game's colours as editable text art: agk art-export {args.name} {fit} "
              f"[--dither] [--trim], then point art.toml at {args.name}.txt")
    return 0 if _run_art(proj) else 1


def cmd_art_export(args):
    from . import art
    proj = load_project(args.project)
    src = os.path.join(proj["dir"], "art", f"{args.name}.png")
    dst = os.path.join(proj["dir"], "art", f"{args.name}.txt")
    if not os.path.exists(src):
        raise SystemExit(f"{rel(src)} not found")
    if os.path.exists(dst) and not args.force:
        raise SystemExit(f"{rel(dst)} exists - use --force to overwrite")
    aga = art.project_aga(proj["dir"])
    targets = None
    if args.palette is not None:
        pal_path = os.path.join(proj["dir"], "art", "palette.txt")
        if not os.path.exists(pal_path):
            raise SystemExit(f"--palette: {rel(pal_path)} not found")
        pal = art.parse_palette(pal_path, aga)
        lo, _, hi = (args.palette or "1-7").partition("-")
        try:
            idx = range(int(lo), int(hi or lo) + 1)
        except ValueError:
            raise SystemExit("--palette takes a range of palette.txt entries, e.g. 1-7")
        targets = [pal[i] for i in idx if i in pal]
        if not targets:
            raise SystemExit(f"--palette {args.palette}: no such entries in palette.txt")
    pins = {}
    for p in args.pin or []:
        c, _, r = p.partition("@")
        try:
            pins[int(r)] = int(c, 0)
        except ValueError:
            raise SystemExit(f"--pin {p}: use COLOUR@REGISTER, e.g. 0xFFF@31")
    try:
        n, warnings = art.export_text(src, dst, args.colors, args.frame_width, args.frame_height, aga=aga,
                                      targets=targets, pins=pins, dither=args.dither,
                                      trim=args.trim)
    except art.ArtError as e:
        raise SystemExit(f"art-export: {e}")
    print(f"wrote {rel(dst)} ({n} colours)")
    for w in warnings:
        print(f"  warning: {w}")
    print(f'next: point art.toml at it (source = "{args.name}.txt") and edit the pixels')
    return 0


def cmd_art_animate(args):
    from . import rd
    proj = load_project(args.project)
    art_dir = os.path.join(proj["dir"], "art")
    out = os.path.join(art_dir, f"{args.name}_{args.action}.png")
    try:
        if args.resume:
            png, cost, left = rd.wait_task(args.resume)
        else:
            src = os.path.join(art_dir, f"{args.name}.png")
            if not os.path.exists(src):
                raise SystemExit(f"{rel(src)} not found - the start frame")
            w, h, _ = __import__("agk.image", fromlist=["x"]).load_png_rgba(src)
            print(f"animating {rel(src)} ({args.action}, {args.frames} frames) - this takes several minutes")
            png, cost, left = rd.animate(open(src, "rb").read(), args.action, w, h, args.frames,
                                         dry_run=args.dry_run)
    except rd.GenError as e:
        raise SystemExit(f"art-animate: {e}")
    if args.dry_run:
        print(f"would cost ${cost} (balance ${left}); nothing generated")
        return 0
    with open(out, "wb") as f:
        f.write(png)
    print(f"saved {rel(out)} (${cost}, balance ${left}) - a sprite sheet; use it with frame_width, "
          f"or agk art-export to edit the frames by hand")
    return 0


def cmd_play(args):
    """Play the game in FS-UAE (a normal, windowed Amiga emulator)."""
    proj = load_project(args.project)
    need_adf(proj)
    profile_name = args.profile or proj["profile"]
    profile, rom, ext = profiles.resolve(profile_name)
    model = {"A500_OCS_1MB": "A500", "A500_ECS_1MB": "A500+", "A1200_2MB": "A1200"}.get(profile.scheme, "A500")
    lines = ["[fs-uae]",
             f"# Written by agk play for {proj['name']} ({profile_name}).",
             f"amiga_model = {model}",
             f"kickstart_file = {rom}",
             f"floppy_drive_0 = {proj['adf']}",
             "floppy_drive_speed = 0",           # turbo loading
             "joystick_port_0 = mouse",
             "joystick_port_1 = keyboard",      # arrow keys = joystick in port 2
             "keyboard_key_space = action_joy_1_fire_button",
             "window_width = 960", "window_height = 768"]
    if ext:
        lines.append(f"kickstart_ext_file = {ext}")
    if "FAST_RAM" in " ".join(profile.config):
        lines.append("fast_memory = 2048")
    if profile.scheme == "A500_OCS_1MB":
        lines += ["chip_memory = 512", "slow_memory = 512"]
    cfg = os.path.join(proj["dir"], "build", f"{proj['name']}.fs-uae")
    with open(cfg, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {rel(cfg)}")
    print("controls: arrow keys = joystick (port 2), Space = fire; F12 = FS-UAE menu")
    if sys.platform == "darwin":
        if not os.path.exists("/Applications/FS-UAE.app"):
            raise SystemExit("FS-UAE not found - install it with: brew install --cask fs-uae-emulator")
        return subprocess.run(["open", "-n", "-a", "FS-UAE", "--args", cfg]).returncode
    exe = shutil.which("fs-uae")
    if not exe:
        raise SystemExit("FS-UAE not found - install it (https://fs-uae.net), then run: fs-uae " + cfg)
    subprocess.Popen([exe, cfg])
    return 0


def cmd_art_clean(args):
    from . import art
    proj = load_project(args.project)
    art_dir = os.path.join(proj["dir"], "art")
    src = os.path.join(art_dir, args.src)
    dst = os.path.join(art_dir, args.out or args.src)
    crop = tuple(int(v) for v in args.crop.split(":")) if args.crop else None
    fade = None
    if args.fade_bottom:
        rows, colour = args.fade_bottom.split(":")
        fade = (int(rows), int(colour, 0))
    notes = art.clean_png(src, dst, args.fill_holes, args.despeckle, crop, fade)
    for n in notes:
        print(f"  {n}")
    print(f"wrote {rel(dst)} - run agk art and look at the preview")
    return 0


def cmd_build(args):
    proj = load_project(args.project)
    if (not _run_art(proj, quiet=True) or not _run_sound(proj, quiet=True) or not _run_maps(proj, quiet=True)
            or not _run_disk(proj)):
        return 1
    rc = subprocess.run(_build_cmd(proj, args.define or [])).returncode
    if rc == 0:
        try:
            items, _ = _lint(proj)
        except (SystemExit, OSError) as e:        # the build is fine; say why the check didn't run
            print(f"lint: skipped ({e})")
            items = []
        if items:
            print(f"lint: {len(items)} function(s) call the 68000 maths library "
                  f"({', '.join(f for _, f, _ in items[:4])}{', ...' if len(items) > 4 else ''}): agk lint")
    return rc


def _lint(proj, everything=False):
    from . import lint
    objs = lint.objects(proj)
    if not objs:
        return [], 0
    image = os.environ.get("AGK_TOOLCHAIN_IMAGE", "amigadev/crosstools:m68k-amigaos")
    return lint.findings(lint.parse(lint.disassemble(proj, objs, image)), proj["lint_allow"], everything)


def cmd_lint(args):
    from . import lint
    proj = load_project(args.project)
    need_adf(proj, True)
    items, skipped = _lint(proj, args.all)
    for line in lint.report(items, skipped):
        print(line)
    if not items:
        print("lint: no maths library calls" + (" outside setup functions" if skipped else ""))
    return 1 if items else 0


def _report(res, as_json):
    if as_json:
        print(json.dumps(res, indent=2))
        return
    status = "PASS" if res["ok"] else "FAIL"
    boot = f", boot {res['boot_seconds']:.1f}s (cached next time)" if res.get("boot_seconds") else ""
    print(f"{status} {res['scenario']} [{res['profile']}] {res.get('seconds', 0):.1f}s{boot}")
    for name, shot in res["screenshots"].items():
        print(f"  screenshot {name}: {rel(shot['screen_png'])} (320x256, game coordinates)"
              f" | full frame: {rel(shot['png'])}")
        if "golden" in shot:
            g = shot["golden"]
            if g["status"] == "match":
                print("    golden: match")
            elif g["status"] == "new":
                print(f"    golden: none yet (run with --update to accept)")
            elif g["status"] in ("updated", "created"):
                print(f"    golden: {g['status']} {rel(g['path'])}")
            else:
                x0, y0, x1, y1 = g["bbox"]
                gx0, gy0 = max(0, (x0 - SCREEN_X0) // 2), max(0, y0 - SCREEN_Y0)
                gx1, gy1 = min(319, (x1 - SCREEN_X0) // 2), min(255, y1 - SCREEN_Y0)
                where = (f"around game x={gx0}..{gx1} y={gy0}..{gy1}" if gx0 <= gx1 and gy0 <= gy1
                         else "outside the 320x256 playfield (in the border)")
                print(f"    golden: DIFFERENT - {g['pixels']} px changed {where}, see {rel(g['diff'])}")
    if res.get("mem"):
        m = res["mem"]
        print(f"  memory: chip free {m['chip']} KB at the lowest (largest piece {m['chipmax']} KB), "
              f"fast {m['fast']} KB")
    if res.get("audio_png"):
        marks = ", ".join(f"{k}@{v}" for k, v in res.get("marks", {}).items())
        print(f"  audio: {rel(res['audio'])} | spectrogram: {rel(res['audio_png'])} (marks, frame: {marks})")
    for f in res["failures"]:
        print(f"  ! {f}")
    print(f"  serial: {rel(os.path.join(res['outdir'], 'serial.txt'))}")


def cmd_inspect(args):
    from . import inspect as insp
    adf = os.path.abspath(args.adf)
    if not os.path.isfile(adf):
        raise SystemExit(f"{args.adf} not found")
    stem = os.path.splitext(os.path.basename(adf))[0]
    outdir = os.path.abspath(args.out or os.path.join("agk-inspect", stem))
    os.makedirs(outdir, exist_ok=True)
    image = os.environ.get("AGK_TOOLCHAIN_IMAGE", "amigadev/crosstools:m68k-amigaos")
    dump = os.path.join(outdir, "chip.bin")
    steps = list(args.step or [])
    if args.file:
        steps = open(args.file).read().splitlines() + steps
    if not steps and not (args.disasm and os.path.exists(dump)):
        steps = ["screenshot screen"]
    res = None
    if steps:
        try:
            sc = scenario.parse("\n".join(steps), "inspect")
        except scenario.ScenarioError as e:
            raise SystemExit(f"scenario error: {e}")
        if sc.start_at or sc.checkpoints:
            raise SystemExit("inspect: use 'snapshot NAME' and --from, not checkpoints (those need a project)")
        boot = int(round(args.boot * 50))
        if args.coverage:
            # every executed instruction address, and the CPU at the end: what agk recomp needs
            sc.lines = ["agk coverage start"] + sc.lines + [f"agk coverage save {outdir}/coverage.bin",
                                                             f"agk cpu {outdir}/cpu.txt",
                                                             f"mem save bin {outdir}/slow.bin {0xC00000} {0x80000}"]
            sc.origins = [0] + sc.origins + [0, 0, 0]
        with runner.outdir_lock(outdir):
            res = runner.run(adf, args.profile, sc, outdir, boot, sync="frames", snapshot=args.start,
                             sample=args.sample or bool(args.disasm) or args.coverage)
        _report(res, False)
        for f in sorted(os.listdir(outdir)):
            if f.endswith(".vasnap"):
                print(f"  snapshot: {rel(os.path.join(outdir, f))} (start there with --from)")
            elif f.endswith(".bin"):
                print(f"  memory: {rel(os.path.join(outdir, f))}")
        if args.sample and os.path.exists(os.path.join(outdir, "profile.bin")):
            _, lines = prof_load(os.path.join(outdir, "profile.bin"))
            print()
            for line in insp.report(lines):
                print(line)
    if args.disasm:
        if not os.path.exists(dump):
            raise SystemExit(f"--disasm needs a memory dump: run with --sample or a 'dump-chip' step first")
        addr, _, length = args.disasm.partition(":")
        try:
            addr, length = int(addr, 0), int(length or "0x100", 0)
        except ValueError:
            raise SystemExit("--disasm takes ADDR[:LENGTH], e.g. 0x21D00:0x200")
        print(f"\n{rel(dump)} at ${addr:06X}, {length} bytes:")
        try:
            for line in insp.disassemble(dump, addr, length, image, outdir):
                print("  " + line)
        except RuntimeError as e:
            raise SystemExit(f"inspect: {e}")
    return 0 if res is None or res["ok"] else 1


# The machines a game can be recompiled for: the runtime directories copied into the
# build (the shared 68000 + host first, then the machine's chips and main) and the two
# address regions that can hold code (the dispatch tables; bus.h's REGION0/1).
MACHINES = {
    "amiga": {"dirs": ["runtime", "amiga"], "regions": ((0, 0x200000), (0xC00000, 0x200000))},
    "md": {"dirs": ["runtime", "md"], "regions": ((0, 0x400000), (0xFF0000, 0x10000))},
}


def _md_rom(path):
    """a Mega Drive ROM's bytes (.md/.bin/.gen; .smd interleaved is undone), checked by its header"""
    data = open(path, "rb").read()
    if path.lower().endswith(".smd") and len(data) % 16384 == 512:
        out = bytearray(len(data) - 512)
        for b in range((len(data) - 512) // 16384):
            blk = data[512 + b * 16384:512 + (b + 1) * 16384]
            for i in range(8192):
                out[b * 16384 + 2 * i + 1] = blk[i]
                out[b * 16384 + 2 * i] = blk[8192 + i]
        data = bytes(out)
    if data[0x100:0x104] not in (b"SEGA", b" SEG"):
        raise SystemExit(f"recomp: {rel(path)} doesn't look like a Mega Drive ROM (no SEGA header at $100)")
    return data


def cmd_recomp(args):
    from . import recomp
    machine = MACHINES[args.machine]
    image = os.environ.get("AGK_TOOLCHAIN_IMAGE", "amigadev/crosstools:m68k-amigaos")
    if args.machine == "md":
        # a cartridge: the ROM is the whole start state (its reset vector), the coverage
        # comes from the native build's own interpreter (game --coverage FILE), so the
        # first build runs everything interpreted and each later one translates what was played
        if not args.rom:
            raise SystemExit("recomp: --machine md needs --rom GAME.md")
        out = os.path.abspath(args.out or os.path.splitext(os.path.basename(args.rom))[0] + "-native")
        os.makedirs(out, exist_ok=True)
        src = out
        rom = _md_rom(args.rom)
        rom_path = os.path.join(out, "rom.bin")
        with open(rom_path, "wb") as f:
            f.write(rom)
        print(f"{rel(args.rom)}: {rom[0x120:0x150].decode('ascii', 'replace').strip() or rom[0x150:0x180].decode('ascii', 'replace').strip()}, "
              f"{len(rom) // 1024}K, region {rom[0x1F0:0x1F3].decode('ascii', 'replace').strip()}")
        images = [(0, rom_path, len(rom))]
        cov_files = [p for p in [os.path.join(out, "coverage.bin")] if os.path.exists(p)]
        cov_files += sorted(os.path.join(out, f) for f in os.listdir(out) if f.startswith("coverage_") and f.endswith(".bin"))
        need = {}
    else:
        if not args.dir:
            raise SystemExit("recomp: give the agk inspect output directory (chip.bin, coverage.bin, cpu.txt, chip.regs.txt)")
        src = os.path.abspath(args.dir)
        need = {n: os.path.join(src, n) for n in ("chip.bin", "coverage.bin", "cpu.txt", "chip.regs.txt")}
        for n, p in need.items():
            if not os.path.exists(p):
                raise SystemExit(f"recomp: {rel(p)} missing: run agk inspect GAME.adf ... --coverage first")
        out = os.path.abspath(args.out or os.path.join(src, "recomp"))
        cov_files = sorted(os.path.join(src, f) for f in os.listdir(src) if f.startswith("coverage") and f.endswith(".bin"))
        images = [(0, need["chip.bin"], os.path.getsize(need["chip.bin"]))]
        slow = os.path.join(src, "slow.bin")
        if os.path.exists(slow):
            images.append((0xC00000, slow, os.path.getsize(slow)))
    ranges, covered = recomp.coverage_ranges(cov_files) if cov_files else ([], set())
    print(f"coverage from {len(cov_files)} file(s): {', '.join(os.path.basename(f) for f in cov_files) or 'none'}")
    if not ranges and args.machine != "md":
        raise SystemExit("recomp: the coverage is empty")
    if ranges:
        print(f"{len(covered)} instructions executed in {len(ranges)} ranges, "
              f"${ranges[0][0]:06X}..${ranges[-1][1]:06X}")
    else:
        print("no coverage yet: this build interprets everything; play it with --coverage coverage.bin and agk recomp again")
    try:
        instrs = recomp.disassemble(images, ranges, image, src, covered) if ranges else []
    except recomp.RecompError as e:
        raise SystemExit(f"recomp: {e}")
    guards = set()                        # instructions earlier native runs saw the game rewrite (patched.txt)
    patched = os.path.join(out, "patched.txt")
    if os.path.exists(patched):
        guards = {int(line, 16) for line in open(patched) if line.strip()}
        print(f"{len(guards)} instruction(s) the game rewrites (patched.txt): each checks its opcode as it runs")
    anchors = [int(a, 16) for a in (args.anchor or [])]
    needed = os.path.join(out, "needed.txt")
    if os.path.exists(needed):            # where earlier native runs jumped to and stopped
        anchors += [int(line, 16) for line in open(needed) if line.strip()]
        print(f"{len(set(anchors))} address(es) earlier native runs needed (needed.txt): translating from them too")
    if not args.no_follow and (instrs or anchors):
        try:
            instrs, added = recomp.follow_branches(images, instrs, image, src, anchors=anchors)
        except recomp.RecompError as e:
            raise SystemExit(f"recomp: {e}")
        print(f"{added} more instructions reached by following branches the emulator didn't take")
    if args.machine == "md":
        rom = open(images[0][1], "rb").read()
        cpu_regs = {"PC": int.from_bytes(rom[4:8], "big"), "SSP": int.from_bytes(rom[0:4], "big"), "SR": 0x2700,
                    "A7": int.from_bytes(rom[0:4], "big")}
        flat = {"HZ": 60 if b"U" in rom[0x1F0:0x1F3] or b"J" in rom[0x1F0:0x1F3] else 50}
    else:
        from . import rip
        snap = args.snapshot or os.path.join(src, "end.vasnap")
        if os.path.exists(snap) and not args.no_resnap:
            # the memory, the CPU and the registers straight from the snapshot: the dumps a
            # scenario wrote at its end can be a frame away from the snapshot it saved, and
            # the native build must start exactly where --diff's emulator run starts
            print(f"start state taken from {rel(snap)}")
            _recomp_state(args, src, snap)
        cpu_regs = recomp.read_cpu(need["cpu.txt"])
        regs = rip.read_regs(need["chip.regs.txt"])
        flat = {k.upper(): v for k, v in regs.items() if k != "colors"}
        for i, c in enumerate(regs["colors"]):
            flat[f"COLOR{i:02d}"] = c
    mem_files = [(path, base) for base, path, _ in images]
    runtime_dir = [os.path.join(ROOT, "recomp", d) for d in machine["dirs"]]
    # --hook ADDR=file.c:function: the function runs before the instruction at ADDR
    hooks, hook_files = {}, []
    for spec in args.hook or []:
        m = re.fullmatch(r"(?:0x)?([0-9a-fA-F]+)=([^:]+\.c):(\w+)", spec)
        if not m:
            raise SystemExit(f"recomp: --hook wants ADDR=file.c:function, not {spec!r}")
        if not os.path.exists(m.group(2)):
            raise SystemExit(f"recomp: hook file {m.group(2)} not found")
        hooks[int(m.group(1), 16)] = m.group(3)
        hook_files.append(os.path.abspath(m.group(2)))
    try:
        unknown = recomp.write_project(out, instrs, covered, cpu_regs, mem_files, flat, runtime_dir, hooks, guards,
                                       regions=machine["regions"], machine=args.machine)
        for hf in set(hook_files):
            shutil.copy(hf, out)
    except recomp.RecompError as e:
        raise SystemExit(f"recomp: {e}")
    print(f"wrote {rel(out)}/game.c: {len(instrs)} instructions; {len(unknown)} branch targets never executed "
          f"(the interpreter takes them if reached)")
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        raise SystemExit("recomp: no host C compiler")
    srcs = [os.path.join(out, f) for f in os.listdir(out) if f.endswith(".c")]
    cmd = [cc, "-std=gnu11", "-O1", "-Wall", "-Wno-unused-label", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
           "-Wno-unused-function", "-I", out, *srcs, "-o", os.path.join(out, "game")]
    sdl = _sdl_flags()
    if sdl:
        cmd = cmd[:1] + ["-DUSE_SDL"] + sdl + cmd[1:]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[-3000:])
        raise SystemExit("recomp: the generated C didn't compile (a translator bug: the message above says where)")
    if r.stderr.strip():
        print(r.stderr.strip()[-1500:])
    game = rel(os.path.join(out, "game"))
    if sdl:
        print(f"built {game}: play it with {game} --play --adf GAME.adf (arrows/WASD + space are the joystick, "
              f"the keyboard is the Amiga's, click grabs the mouse / F11 releases it, F12 full speed, Esc quits), "
              f"or --frames N --png out.png headless")
    else:
        print(f"built {game} headless (no SDL2 found: brew install sdl2 / apt install libsdl2-dev for a window): "
              f"run it with --frames N --png out.png [--adf GAME.adf]")
    if args.diff:
        return _recomp_diff(args, src, out, cmd)
    return 0


def _recomp_state(args, src, snap):
    """chip.bin, slow.bin, cpu.txt and chip.regs.txt from the snapshot the coverage run saved"""
    from . import rip
    profile, rom, ext = profiles.resolve(args.profile)
    lines = runner._setup_lines(profile, rom, ext) + [f"agk snapload {os.path.abspath(snap)}", *runner._display_lines(),
                                                      f"mem save bin {src}/chip.bin 0 {0x80000}",
                                                      f"mem save bin {src}/slow.bin {0xC00000} {0x80000}",
                                                      f"agk cpu {src}/cpu.txt", f"agk regs {src}/chip.regs.txt", "wait 1 frames"]
    code, outp, secs = runner._run_vamiga(lines, src, timeout=120)
    if "df0_cyl" not in rip.read_regs(os.path.join(src, "chip.regs.txt")):
        raise SystemExit(f"recomp: the emulator didn't write the state (is vAmiga rebuilt with the kit's patch?):\n{outp[-400:]}")


def _sdl_flags():
    """the compiler flags for SDL2, or [] when it isn't installed"""
    cfg = shutil.which("sdl2-config") or next((p for p in ("/opt/homebrew/bin/sdl2-config", "/usr/local/bin/sdl2-config")
                                                if os.path.exists(p)), None)
    if not cfg:
        return []
    # static when possible: the binary then runs on a Mac without Homebrew
    libdir = os.path.join(os.path.dirname(os.path.dirname(cfg)), "lib")
    static = os.path.exists(os.path.join(libdir, "libSDL2.a"))
    r = subprocess.run([cfg, "--cflags", "--static-libs" if static else "--libs"], capture_output=True, text=True)
    return r.stdout.split() if r.returncode == 0 else []


def _input_lines(path, frames):
    """a native --input file (lines of "FRAME right left up down fire") as RetroShell
    joystick lines for the emulator, running `frames` frames in all"""
    from .scenario import DIRECTIONS
    out, cur = [], 0
    if path:
        for line in open(path):
            parts = line.split()
            if not parts:
                continue
            f = int(parts[0])
            if f > cur:
                out.append(f"wait {f - cur} frames")
                cur = f
            out += [f"joystick2 {DIRECTIONS[d][1]}" for d in ("left", "up", "fire")]      # release everything
            out += [f"joystick2 {DIRECTIONS[w][0]}" for w in parts[1:] if w in DIRECTIONS]
            if "mouse" in parts:
                k = parts.index("mouse")
                out.append(f"agk mouse {int(parts[k + 1]) + 10000} {int(parts[k + 2]) + 10000}")
            out.append(f"agk mousebutton {1 if 'click' in parts else 0}")
    if frames > cur:
        out.append(f"wait {frames - cur} frames")
    return out


def _compare_traces(emu, nat, native_stderr="", pc_only=False):
    """the two main-program instruction streams -> 0 if they agree, 1 (and a report) where they part;
    pc_only compares the path alone (where the two take different branches), not the values"""
    r = types.SimpleNamespace(stderr=native_stderr)
    names = ["PC"] + [f"D{i}" for i in range(8)] + [f"A{i}" for i in range(8)] + ["SR"]
    # The timing differs, so a loop that polls the vertical blank (or a frame counter
    # the handler bumps) runs a different number of times on each side, and the
    # flags differ at its test. Compare modulo that: ignore the SR, and when the
    # streams part, look ahead a little on either side for where they meet again.
    core = (lambda line: line[:6]) if pc_only else (lambda line: line[:-5])   # the pc, or the line without its SR
    i = j = 0
    resyncs = 0
    while i < len(emu) and j < len(nat):
        if core(emu[i]) == core(nat[j]):
            i += 1
            j += 1
            continue
        found = None
        for skip in range(1, 20000):      # (a polling loop can run for most of a frame)
            if i + skip < len(emu) and core(emu[i + skip]) == core(nat[j]):
                found = (skip, 0)
                break
            if j + skip < len(nat) and core(nat[j + skip]) == core(emu[i]):
                found = (0, skip)
                break
        if found and (pc_only or emu[i].split()[1:-1] == nat[j].split()[1:-1]):   # only the pc/loop count differs
            i += found[0]
            j += found[1]
            resyncs += 1
            continue
        ef, nf = emu[i].split(), nat[j].split()
        diffs = [f"{names[k]}: emulator {ef[k]} native {nf[k]}" for k in range(len(names) - 1) if ef[k] != nf[k]]
        print(f"diverge at instruction {i} (emulator pc ${ef[0]}, after {resyncs} loop resyncs): " + "; ".join(diffs))
        for k in range(max(0, i - 8), i):
            print(f"  {k}: {emu[k]}")
        print(f"  {i}: {emu[i]}   <- emulator")
        print(f"  {j}: {nat[j]}   <- native")
        return 1
    if j >= len(nat) and i < len(emu) - 20000:
        print(f"the native run stopped after {len(nat)} of {len(emu)} instructions"
              + (": " + r.stderr.strip()[-300:] if r.stderr.strip() else ""))
        return 1
    print(f"identical for {i} instructions (up to the flags in polling loops; {resyncs} resyncs)")
    return 0


def _recomp_diff(args, src, out, cmd):
    """Trace N instructions in the emulator (from the snapshot the data came from)
    and in the native build, and show where they part."""
    snap = args.snapshot or os.path.join(src, "end.vasnap")
    if not os.path.exists(snap):
        raise SystemExit(f"recomp --diff needs the snapshot the data was taken at ('snapshot end' in the inspect "
                         f"steps, or --snapshot FILE); {rel(snap)} not found")
    n = args.diff
    emu_trace = os.path.join(out, "trace_emu.txt")
    profile, rom, ext = profiles.resolve(args.profile)
    lines = runner._setup_lines(profile, rom, ext) + [f"agk snapload {os.path.abspath(snap)}", *runner._display_lines(),
                                                      f"agk trace {emu_trace} {n}"]
    lines += _input_lines(args.input, max(2, n // 2000 + 2))
    code, outp, secs = runner._run_vamiga(lines, out, timeout=600)
    if not os.path.exists(emu_trace):
        raise SystemExit(f"recomp --diff: the emulator wrote no trace:\n{outp[-500:]}")
    tcmd = list(cmd)
    tcmd[tcmd.index("-o") + 1] = os.path.join(out, "game_trace")
    tcmd.insert(1, "-DRECOMP_ITRACE")
    r = subprocess.run(tcmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("recomp --diff: the traced build failed:\n" + r.stderr[-1500:])
    nat_trace = os.path.join(out, "trace_native.txt")
    run_cmd = [os.path.join(out, "game_trace"), "--memory", os.path.join(out, "memory.bin"), "--itrace", nat_trace, str(n)]
    if args.input:
        run_cmd += ["--input", os.path.abspath(args.input)]
    if args.adf:
        run_cmd += ["--adf", os.path.abspath(args.adf)]
    r = subprocess.run(run_cmd, capture_output=True, text=True, timeout=600)
    # Interrupts land at different instructions (the native timing is approximate):
    # compare the main program only, the instructions at interrupt level 0
    def main_stream(path):
        if not os.path.exists(path):
            return [], 0
        kept, handlers = [], 0
        for line in open(path):
            line = line.rstrip()
            if (int(line[-4:], 16) >> 8) & 7:
                handlers += 1
            else:
                kept.append(line)
        return kept, handlers
    emu, emu_h = main_stream(emu_trace)
    nat, nat_h = main_stream(nat_trace)
    print(f"traces: emulator {len(emu)} main + {emu_h} handler instructions, native {len(nat)} + {nat_h}")
    return _compare_traces(emu, nat, r.stderr, pc_only=args.diff_pc)


def cmd_rip(args):
    from . import rip
    from .image import Image
    data = open(args.dump, "rb").read()
    if args.copper or args.screen:
        lists = rip.find_copper_lists(data)
        if args.copper:
            for line in rip.copper_report(lists):
                print(line)
        if args.screen:
            shown = rip.display_lists(lists)
            if not shown:
                raise SystemExit("rip: no copper list that sets up a display in the dump")
            c = shown[0]
            regs_path = args.regs or os.path.splitext(args.dump)[0] + ".regs.txt"
            regs = rip.read_regs(regs_path) if os.path.exists(regs_path) else None
            if regs is None:
                print("note: no .regs.txt next to the dump (dump-chip writes one): colours the list "
                      "doesn't set come out black", file=sys.stderr)
            img = rip.screen_from_copper(data, c, regs=regs)
            out = args.out or "screen.png"
            img.save_png(out)
            print(f"wrote {out} from the copper list at ${c['addr']:06X} ({len(c['bplpt'])} planes)")
        return 0
    if not args.addr or not args.size:
        raise SystemExit("rip: give ADDR and WIDTHxHEIGHTxPLANES, or --copper / --screen")
    try:
        addr = int(args.addr, 0)
        w, h, planes = (int(v) for v in args.size.lower().split("x"))
    except ValueError:
        raise SystemExit("rip: ADDR is a number (hex ok) and the size is WIDTHxHEIGHTxPLANES, e.g. 320x256x4")
    if w % 8 or not 1 <= planes <= 8:
        raise SystemExit("rip: the width must be a multiple of 8 and planes 1-8")
    pal = rip.parse_palette(args.palette, data) if args.palette else None
    if args.count:
        img = rip.tiles_sheet(data, addr, w, h, planes, args.count, pal, interleaved=args.interleaved)
    else:
        img = rip.planar_to_image(data, addr, w, h, planes, pal, interleaved=args.interleaved, modulo=args.modulo,
                                 planes_apart=args.planes_apart)
    out = args.out or f"rip_{addr:06X}.png"
    img.save_png(out)
    print(f"wrote {out} ({img.width}x{img.height})")
    return 0


def prof_load(path):
    from . import profile as prof
    return prof.load(path)


def cmd_run(args):
    # Friendly: `agk run tests/foo.agk` means "run this scenario file".
    if args.project and args.project.endswith(".agk") and os.path.isfile(args.project):
        args.file = args.file or args.project
        args.project = os.path.dirname(os.path.dirname(os.path.abspath(args.project)))
    proj = load_project(args.project)
    need_adf(proj, not args.no_build)
    if args.file:
        text = open(args.file).read()
        name = os.path.splitext(os.path.basename(args.file))[0]
    else:
        text = "\n".join(args.step or DEFAULT_RUN_STEPS)
        name = "run"
    try:
        sc = scenario.parse(text, name)
    except scenario.ScenarioError as e:
        raise SystemExit(f"scenario error: {e}")
    profile = args.profile or proj["profile"]
    # Separate from agk test's build/agk/<profile>/<test> so ad hoc runs don't clobber test output.
    outdir = os.path.abspath(args.out or os.path.join(proj["dir"], "build", "agk-run", profile, name))
    with runner.outdir_lock(outdir):
        res = runner.run(proj["adf"], profile, sc, outdir, proj["boot"], fresh=args.fresh, sync=proj["sync"])
        _report(res, args.json)
    return 0 if res["ok"] else 1


def cmd_record(args):
    from . import record
    path = args.scenario
    proj = load_project(args.project or os.path.dirname(os.path.dirname(os.path.abspath(path))))
    need_adf(proj, not args.no_build)
    name = os.path.splitext(os.path.basename(path))[0]
    try:
        sc, frames = record.video_scenario(scenario.parse(open(path).read(), name))
    except (scenario.ScenarioError, record.RecordError) as e:
        raise SystemExit(f"scenario error: {e}")
    profile = args.profile or proj["profile"]
    outdir = os.path.abspath(os.path.join(proj["dir"], "build", "agk-record", profile, name))
    mp4 = os.path.abspath(args.out or os.path.join(proj["dir"], "build", f"{name}.mp4"))
    print(f"recording {frames} pictures...", file=sys.stderr)
    with runner.outdir_lock(outdir):
        res = runner.run(proj["adf"], profile, sc, outdir, proj["boot"], sync=proj["sync"])
        if not res["ok"]:
            _report(res, False)
            return 1
        try:
            opts = {}
            if args.readme:       # small enough to commit: 640x512 MP4, a 6 s 480x384 GIF
                opts = dict(size="640x512", crf=24, gif_size="480x384", gif_seconds=6)
            if args.size:
                opts["size"] = args.size
            for k in ("gif_seconds", "gif_start", "gif_size", "gif_colors"):
                if getattr(args, k) is not None:
                    opts[k] = getattr(args, k)
            fps = record.encode(outdir, frames, mp4, gif=args.gif, **opts)
        except record.RecordError as e:
            raise SystemExit(f"record: {e}")
    print(f"wrote {rel(mp4)} ({os.path.getsize(mp4) // 1024} KB, {frames / fps:.1f} s at {fps:g} fps)")
    if args.gif:
        print(f"wrote {rel(args.gif)} ({os.path.getsize(args.gif) // 1024} KB)")
    return 0


def cmd_profile(args):
    from . import profile as prof
    path = args.scenario
    proj = load_project(args.project or os.path.dirname(os.path.dirname(os.path.abspath(path))))
    need_adf(proj, not args.no_build)
    name = os.path.splitext(os.path.basename(path))[0]
    try:
        sc = scenario.parse(open(path).read(), name)
    except scenario.ScenarioError as e:
        raise SystemExit(f"scenario error: {e}")
    # Sample the whole scenario
    sc.lines = ["agk profile start"] + sc.lines + ["agk profile save {out}/profile.bin"]
    sc.origins = [0] + sc.origins + [0]
    machine = args.profile or proj["profile"]
    outdir = os.path.abspath(os.path.join(proj["dir"], "build", "agk-profile", machine, name))
    image = os.environ.get("AGK_TOOLCHAIN_IMAGE", "amigadev/crosstools:m68k-amigaos")
    with runner.outdir_lock(outdir):
        binf = os.path.join(outdir, "profile.bin")
        if (args.disasm and os.path.exists(binf) and os.path.getmtime(binf) > os.path.getmtime(proj["adf"])
                and os.path.getmtime(binf) > os.path.getmtime(path)):
            print(f"(reusing {rel(binf)}: the build and the scenario haven't changed)", file=sys.stderr)
            res = {"ok": True, "failures": []}
        else:
            res = runner.run(proj["adf"], machine, sc, outdir, proj["boot"], sync=proj["sync"])
        if not os.path.exists(binf):
            _report(res, False)
            return 1
        try:
            anchor, lines = prof.load(binf)
            syms = prof.symbols_for(proj, image)
            if args.disasm:
                start = syms.start(args.disasm)
                if start is None:
                    raise prof.ProfileError(f"no function '{args.disasm}' (names as in the report)")
                _, obj = syms.object_of(start)
                build = os.path.join(proj["dir"], "build")
                obj = prof.object_file(build, obj)
                if not obj:
                    raise prof.ProfileError(f"no object file for '{args.disasm}' in the build tree")
                from .lint import objdump
                dis, _ = objdump(build, [obj], image, "-d")
                for line in prof.annotate(lines, anchor, syms, args.disasm, dis):
                    print(line)
                return 0
            a = prof.analyse(anchor, lines, syms)
        except prof.ProfileError as e:
            raise SystemExit(f"profile: {e}")
        text = prof.report(a, top=args.top)
        prof.chart(a, os.path.join(outdir, "load.png"))
        with open(os.path.join(outdir, "report.txt"), "w") as f:
            f.write("\n".join(text) + "\n")
    for line in text:
        print(line)
    print(f"\nchart: {rel(os.path.join(outdir, 'load.png'))} (load per game frame; line = 100%; "
          f"orange = blitter-bound, blue = CPU-bound, red = over budget)")
    if not res["ok"]:
        print("(the scenario itself failed: " + "; ".join(res["failures"]) + ")")
    return 0


def cmd_release(args):
    from . import release
    proj = load_project(args.project)
    need_adf(proj, not args.no_build)
    try:
        r = release.config(proj, proj["release"])
    except release.ReleaseError as e:
        raise SystemExit(f"release: {e}")
    if args.version:
        r["version"] = args.version
    exe = os.path.join(proj["dir"], "build", proj["name"])
    if not os.path.exists(exe):
        raise SystemExit(f"release: {rel(exe)} not found - agk build")
    # The screenshot: the [release] scenario's last one, or the screen 3 s after boot
    shot_path = os.path.join(proj["dir"], r["screenshot"]) if r.get("screenshot") else None
    if shot_path:
        if not os.path.exists(shot_path):
            raise SystemExit(f"release: screenshot scenario {rel(shot_path)} not found")
        text, name = open(shot_path).read(), os.path.splitext(os.path.basename(shot_path))[0]
    else:
        text, name = "wait 150\nscreenshot title\n", "screenshot"
    try:
        sc = scenario.parse(text, name)
    except scenario.ScenarioError as e:
        raise SystemExit(f"release: scenario error in {rel(shot_path)}: {e}")
    if not sc.screenshots:
        raise SystemExit(f"release: {rel(shot_path)} takes no screenshot")
    outdir = os.path.abspath(os.path.join(proj["dir"], "build", "release", "shot"))
    with runner.outdir_lock(outdir):
        res = runner.run(proj["adf"], proj["profile"], sc, outdir, proj["boot"], sync=proj["sync"])
    if not res["ok"]:
        _report(res, False)
        return 1
    screen = res["screenshots"][sc.screenshots[-1]]["screen_png"]
    out_dir = os.path.abspath(os.path.join(proj["dir"], "build", "release"))
    zip_path, readme = release.build_zip(proj, r, proj["adf"], exe, screen, out_dir)
    print(f"wrote {rel(zip_path)} ({os.path.getsize(zip_path) // 1024} KB: {proj['name']}.adf, {proj['name']}, "
          f"README.txt, {proj['name']}.png)")
    print(f"wrote {rel(readme)} (Aminet's .readme format; upload it with the zip)")
    missing = [k for k in ("author", "description") if not r.get(k)]
    if missing:
        print(f"note: agk.toml [release] has no {' or '.join(missing)}; the README says less than it could")
    return 0


def _check_goldens(res, golden_dir, profile, update, refreshed):
    """Goldens live in tests/golden/<test>/<shot>.png, shared by all profiles.
    A profile that legitimately renders differently gets an override in
    tests/golden/<test>/<profile>/<shot>.png."""
    for name, shot in res["screenshots"].items():
        shared = os.path.join(golden_dir, f"{name}.png")
        override = os.path.join(golden_dir, profile, f"{name}.png")
        gpath = override if os.path.exists(override) else shared
        got = Image.load_png(shot["png"])
        info = {"path": gpath}
        if update:
            # The first profile of this run defines the shared golden; later
            # profiles only get an override where they really differ from it.
            os.makedirs(golden_dir, exist_ok=True)
            if shared not in refreshed:
                refreshed.add(shared)
                if os.path.exists(shared) and Image.load_png(shared).rgb == got.rgb:
                    info.update(status="match", path=shared)
                else:
                    status = "updated" if os.path.exists(shared) else "created"
                    shutil.copyfile(shot["png"], shared)
                    info.update(status=status, path=shared)
                if os.path.exists(override):
                    os.remove(override)
            elif Image.load_png(shared).rgb == got.rgb:
                if os.path.exists(override):
                    os.remove(override)
                info.update(status="match", path=shared)
            else:
                os.makedirs(os.path.dirname(override), exist_ok=True)
                shutil.copyfile(shot["png"], override)
                info.update(status="updated", path=override)
        elif not os.path.exists(gpath):
            info["status"] = "new"
            res["ok"] = False
            res["failures"].append(f"screenshot '{name}' has no golden image (run agk test --update)")
        else:
            want = Image.load_png(gpath)
            if got.rgb == want.rgb:
                info["status"] = "match"
            else:
                count, bbox, dimg = diff(want, got)
                dpath = os.path.join(res["outdir"], f"{name}.diff.png")
                if dimg:
                    dimg.save_png(dpath)
                info.update(status="different", pixels=count, bbox=bbox, diff=dpath)
                res["ok"] = False
                res["failures"].append(f"screenshot '{name}' differs from {rel(gpath)}")
        shot["golden"] = info


def cmd_test(args):
    proj = load_project(args.project)
    need_adf(proj, not args.no_build)
    tdir = os.path.join(proj["dir"], "tests")
    files = sorted(f for f in os.listdir(tdir) if f.endswith(".agk")) if os.path.isdir(tdir) else []
    if args.only:
        files = [f for f in files if os.path.splitext(f)[0] in args.only]
    if not files:
        raise SystemExit(f"no tests found in {rel(tdir)}")
    results, refreshed = [], set()
    for profile in args.profile or proj["profiles"]:
        for f in files:
            name = os.path.splitext(f)[0]
            try:
                sc = scenario.parse(open(os.path.join(tdir, f)).read(), name)
            except scenario.ScenarioError as e:
                results.append({"scenario": name, "profile": profile, "ok": False,
                                "failures": [f"{f}: {e}"], "screenshots": {}, "outdir": tdir})
                continue
            outdir = os.path.join(proj["dir"], "build", "agk", profile, name)
            with runner.outdir_lock(outdir):
                res = runner.run(proj["adf"], profile, sc, outdir, proj["boot"], sync=proj["sync"])
                if not res["failures"] or res["screenshots"]:
                    _check_goldens(res, os.path.join(tdir, "golden", name), profile, args.update, refreshed)
                if not args.json:
                    _report(res, False)
            results.append(res)
    passed = sum(r["ok"] for r in results)
    if args.json:
        print(json.dumps(results, indent=2))
    else:
        print(f"\n{passed}/{len(results)} passed")
    return 0 if passed == len(results) else 1


def cmd_new(args):
    dest = os.path.abspath(args.dir)
    name = args.name or os.path.basename(dest)
    if not re.match(r"^[a-z][a-z0-9_]*$", name):
        raise SystemExit(f"name '{name}' must be lowercase letters, digits, _ (it becomes the executable name)")
    if os.path.exists(dest) and os.listdir(dest):
        raise SystemExit(f"{dest} exists and is not empty")
    template = os.path.join(ROOT, "templates", args.template)
    if not os.path.isdir(template):
        raise SystemExit(f"no template '{args.template}' (have: {', '.join(sorted(os.listdir(os.path.join(ROOT, 'templates'))))})")
    for base, dirs, files in os.walk(template):
        dirs[:] = [d for d in dirs if d != "build"]
        rel_dir = os.path.relpath(base, template)
        os.makedirs(os.path.join(dest, rel_dir), exist_ok=True)
        for f in files:
            src = os.path.join(base, f)
            dst = os.path.join(dest, rel_dir, f)
            try:
                text = open(src).read()
            except UnicodeDecodeError:
                shutil.copyfile(src, dst)
                continue
            with open(dst, "w") as out:
                out.write(text.replace("{{name}}", name).replace("{{kit}}", ROOT))
    print(f"created {rel(dest)} from template '{args.template}'")
    print(f"next: agk build {rel(dest)} && agk test {rel(dest)} --update")
    return 0


def cmd_unit(args):
    proj = load_project(args.project)
    udir = os.path.join(proj["dir"], "tests", "unit")
    tests = sorted(f for f in os.listdir(udir) if f.endswith(".c")) if os.path.isdir(udir) else []
    if not tests:
        raise SystemExit(f"no unit tests in {rel(udir)}")
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        raise SystemExit("no host C compiler found (set CC)")
    srcs = [os.path.join(proj["dir"], s) for s in proj["unit_sources"]]
    bindir = os.path.join(proj["dir"], "build", "unit")
    os.makedirs(bindir, exist_ok=True)
    # The game's maps (plain C tables) are part of its rules
    if not _run_maps(proj, quiet=True):
        return 1
    maps_c = [p for p in [os.path.join(proj["dir"], "build", "maps", "maps.c")] if os.path.exists(p)]
    failed = 0
    for t in tests:
        exe = os.path.join(bindir, os.path.splitext(t)[0])
        cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-g", "-fsanitize=address,undefined",
               "-I", os.path.join(proj["dir"], "src"), "-I", os.path.join(proj["dir"], "build", "maps"),
               *srcs, *maps_c, os.path.join(udir, t), "-o", exe]
        cp = subprocess.run(cmd, capture_output=True, text=True)
        if cp.returncode != 0:
            print(f"FAIL {t}: does not compile on the host (unit_sources must not use Amiga headers)")
            print(cp.stderr.strip())
            failed += 1
            continue
        rp = subprocess.run([exe], capture_output=True, text=True, timeout=60)
        print(f"{'PASS' if rp.returncode == 0 else 'FAIL'} {t}")
        out = (rp.stdout + rp.stderr).strip()
        if out and (rp.returncode != 0 or args.verbose):
            print("  " + out.replace("\n", "\n  "))
        failed += rp.returncode != 0
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="agk", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("doctor", help="check toolchain, emulator and ROMs")

    p = sub.add_parser("build", help="compile the project into build/<name>.adf")
    p.add_argument("project", nargs="?")
    p.add_argument("-D", dest="define", action="append", metavar="NAME=VALUE",
                   help="CMake option, e.g. -D ACE_DEBUG=ON. It stays in build/'s cache until changed or "
                        "build/ is deleted; put permanent ones in agk.toml [cmake]")

    p = sub.add_parser("inspect", help="boot any ADF (not ours): script it, screenshots, snapshots, dumps, profile",
                       description="Runs a scenario against any Amiga disk image, with no project and no kit "
                                   "runtime in the game. A foreign game prints no 'AGK ready', so --boot waits "
                                   "that many seconds instead (cached). Steps are the scenario language plus "
                                   "'snapshot NAME' and 'dump-chip NAME'. --sample records the program counter "
                                   "every raster line and reports the hot code by address and the shape of a "
                                   "frame; --disasm ADDR[:LEN] disassembles the chip-RAM dump there. See "
                                   "docs/reading-games.md.")
    p.add_argument("adf")
    p.add_argument("-s", "--step", action="append", help="scenario line (repeatable)")
    p.add_argument("-f", "--file", help="scenario file")
    p.add_argument("-p", "--profile", default="a500")
    p.add_argument("--boot", type=float, default=3.0, help="seconds to wait after power-on (default 3)")
    p.add_argument("--from", dest="start", metavar="SNAPSHOT", help="start from this .vasnap instead of booting")
    p.add_argument("--sample", action="store_true", help="sample the program counter; dump chip RAM at the end")
    p.add_argument("--coverage", action="store_true",
                   help="record every instruction address executed, and the CPU and memory at the end (for agk recomp)")
    p.add_argument("--disasm", metavar="ADDR[:LEN]", help="disassemble the chip-RAM dump at ADDR (hex ok)")
    p.add_argument("-o", "--out", help="output directory (default agk-inspect/<disk name>)")

    p = sub.add_parser("recomp", help="recompile a game (from agk inspect --coverage) to a native program",
                       description="Translates every instruction the game executed under the emulator into C "
                                   "(labels per address, the kit's 68000 helpers), bundles the chip runtime and "
                                   "the memory image, and compiles a native binary. See agk recomp's docs.")
    p.add_argument("dir", nargs="?", help="the agk inspect output directory (chip.bin, coverage.bin, cpu.txt, chip.regs.txt); not for --machine md")
    p.add_argument("-o", "--out", help="output directory (default DIR/recomp, or GAME-native for a ROM)")
    p.add_argument("--machine", choices=sorted(MACHINES), default="amiga", help="the machine the game is for (default amiga)")
    p.add_argument("--rom", help="--machine md: the cartridge ROM (.md/.bin/.gen/.smd); the build starts at its reset vector")
    p.add_argument("--diff", type=int, metavar="N", help="trace N instructions in the emulator and natively, show the first difference")
    p.add_argument("--snapshot", help="the .vasnap the data was taken at (default DIR/end.vasnap)")
    p.add_argument("--adf", help="the disk, for the native run")
    p.add_argument("--input", help="for --diff: a file of 'FRAME right left up down fire' lines, played in both")
    p.add_argument("--anchor", action="append", metavar="HEX", help="also translate the code at this address (repeatable)")
    p.add_argument("--hook", action="append", metavar="ADDR=file.c:function",
                   help="call this C function before the instruction at ADDR; it returns 1 to replace it (continuing at cpu.pc) "
                        "or 0 to let it run. The file is compiled in. Repeatable: how a port improves the game.")
    p.add_argument("--diff-pc", action="store_true", help="with --diff: compare the path only (where the branches differ), not the register values")
    p.add_argument("--no-resnap", action="store_true", help="use DIR's dumps as they are instead of re-taking them from end.vasnap")
    p.add_argument("--no-follow", action="store_true",
                   help="translate only what the coverage saw; don't follow branches into code the emulator never ran")
    p.add_argument("-p", "--profile", default="a500")

    p = sub.add_parser("rip", help="graphics out of a chip-RAM dump: the screen, a bitmap, a tile sheet",
                       description="Reads a chip-RAM dump (agk inspect's 'dump-chip NAME'). --copper lists the "
                                   "copper lists it finds (bitplane pointers, modulos, colours); --screen writes "
                                   "the picture the display was showing; ADDR and WIDTHxHEIGHTxPLANES write a "
                                   "bitmap, with --count N a sheet of N tiles stored one after another.")
    p.add_argument("dump")
    p.add_argument("addr", nargs="?")
    p.add_argument("size", nargs="?", help="WIDTHxHEIGHTxPLANES")
    p.add_argument("--copper", action="store_true")
    p.add_argument("--screen", action="store_true")
    p.add_argument("--count", type=int, help="tiles, one after another, as a sheet")
    p.add_argument("--interleaved", action="store_true")
    p.add_argument("--modulo", type=int, default=0, help="bytes between a row and the next, per plane")
    p.add_argument("--planes-apart", type=int, help="bytes from one plane to the next (non-interleaved)")
    p.add_argument("--palette", help="0xRGB,... or @ADDR (colour words in the dump); default a grey ramp")
    p.add_argument("--regs", help="the NAME.regs.txt from dump-chip (default: next to the dump)")
    p.add_argument("-o", "--out")

    p = sub.add_parser("lint", help="find game functions that call the slow 68000 maths library",
                       description="Lists the project's functions whose C compiles to libgcc calls: 32-bit "
                                   "multiply, divide and %%, 64-bit and float maths. Setup functions "
                                   "(*Create, *Destroy, *Init, *Load, *Setup, agk.toml [lint] allow) are "
                                   "left out. Exits 1 if any are found.")
    p.add_argument("project", nargs="?")
    p.add_argument("--all", action="store_true", help="include setup functions too")

    p = sub.add_parser("run", help="boot the game, play steps, capture results",
                       description="Steps use the scenario language, e.g. -s 'press right 20' -s 'screenshot moved'. "
                                   "See 'agk help-scenario'.")
    p.add_argument("project", nargs="?")
    p.add_argument("-s", "--step", action="append", help="scenario line (repeatable)")
    p.add_argument("-f", "--file", help="scenario file instead of -s steps")
    p.add_argument("-p", "--profile")
    p.add_argument("-o", "--out")
    p.add_argument("--fresh", action="store_true", help="boot from scratch instead of the cached snapshot")
    p.add_argument("--no-build", action="store_true", help="don't rebuild when sources are newer than the ADF")
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("record", help="record a scenario as an MP4 with sound (and a GIF)",
                       description="Plays SCENARIO.agk in the emulator, captures every frame and Paula's "
                                   "output, and encodes them with ffmpeg.")
    p.add_argument("scenario", help="scenario file, e.g. demo/showcase.agk")
    p.add_argument("project", nargs="?")
    p.add_argument("-o", "--out", help="MP4 path (default build/SCENARIO.mp4)")
    p.add_argument("--gif", help="also write a silent GIF (640x512, 25 fps)")
    p.add_argument("--gif-seconds", type=float, help="N seconds in the GIF (from --gif-start)")
    p.add_argument("--gif-start", type=float, metavar="S", help="the GIF starts S seconds in")
    p.add_argument("--gif-size", metavar="WxH", help="GIF size (default 640x512)")
    p.add_argument("--gif-colors", type=int, metavar="N", help="GIF colours (default 64; fewer = smaller)")
    p.add_argument("--size", metavar="WxH", help="MP4 size (default 1280x1024)")
    p.add_argument("--readme", action="store_true",
                   help="sizes for a README: 640x512 MP4 (crf 24), 6 s 480x384 GIF (add --gif-start)")
    p.add_argument("-p", "--profile")
    p.add_argument("--no-build", action="store_true")

    p = sub.add_parser("profile", help="where the frame time goes: functions, blitter, DMA (runs a scenario)",
                       description="Plays SCENARIO.agk with the emulator sampling the CPU and the bus on every "
                                   "raster line, then reports the load per game frame, the bus use, the "
                                   "functions that take the time and the heaviest frames, and draws "
                                   "load.png. Needs agk/perf.h in the game.")
    p.add_argument("scenario", help="scenario file, e.g. tests/perf.agk")
    p.add_argument("project", nargs="?")
    p.add_argument("-p", "--profile", help="machine profile")
    p.add_argument("--top", type=int, default=12, help="functions to list (default 12)")
    p.add_argument("--disasm", metavar="FUNC", help="samples per instruction of one function")
    p.add_argument("--no-build", action="store_true")

    p = sub.add_parser("release", help="zip the ADF, the executable, a README and a screenshot for Aminet / itch.io",
                       description="Builds, takes a screenshot (the [release] screenshot scenario's last one, "
                                   "or the screen 3 s after boot), and writes build/release/NAME-VERSION.zip "
                                   "plus an Aminet .readme. agk.toml [release] gives the title, version, "
                                   "author, description and controls (agk help-release).")
    p.add_argument("project", nargs="?")
    p.add_argument("--version", help="override [release] version")
    p.add_argument("--no-build", action="store_true")

    p = sub.add_parser("play", help="play the game in FS-UAE (arrow keys + Space)")
    p.add_argument("project", nargs="?")
    p.add_argument("-p", "--profile")

    p = sub.add_parser("test", help="run tests/*.agk and compare screenshots with goldens")
    p.add_argument("project", nargs="?")
    p.add_argument("-p", "--profile", action="append", help="profile(s) to test (default: agk.toml test_profiles)")
    p.add_argument("--only", action="append", help="only run this test (repeatable)")
    p.add_argument("--update", action="store_true", help="accept current screenshots as goldens")
    p.add_argument("--no-build", action="store_true", help="don't rebuild when sources are newer than the ADF")
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("unit", help="compile game logic for the host and run tests/unit/*.c")
    p.add_argument("project", nargs="?")
    p.add_argument("-v", "--verbose", action="store_true")

    p = sub.add_parser("new", help="create a new game project from a template")
    p.add_argument("dir")
    p.add_argument("--name", help="executable name (default: directory name)")
    p.add_argument("--template", default="game")

    p = sub.add_parser("art", help="convert art/ to Amiga sprites/BOBs and write previews")
    p.add_argument("project", nargs="?")

    p = sub.add_parser("art-animate", help="animate art/NAME.png (walking, idle, jump...) with Retro Diffusion")
    p.add_argument("name", nargs="?")
    p.add_argument("--project", default=None)
    p.add_argument("--action", default="walking", choices=["walking", "idle", "jump", "crouch", "attack", "destroy"])
    p.add_argument("--frames", type=int, default=8, choices=[4, 6, 8, 10, 12, 16])
    p.add_argument("--resume", metavar="TASK_ID", help="collect a generation that timed out (no new charge)")
    p.add_argument("--dry-run", action="store_true", help="free price check")

    p = sub.add_parser("art-clean", help="tidy AI art: fill holes, remove specks, crop, fade into mist")
    p.add_argument("src", help="PNG in art/")
    p.add_argument("--project", default=None)
    p.add_argument("-o", "--out", help="output PNG in art/ (default: overwrite src)")
    p.add_argument("--fill-holes", action="store_true", help="fill see-through holes not connected to the sky")
    p.add_argument("--despeckle", type=int, default=0, metavar="N", help="remove floating bits smaller than N px")
    p.add_argument("--crop", metavar="Y0:Y1", help="keep rows Y0..Y1-1")
    p.add_argument("--fade-bottom", metavar="ROWS:0xRGB", help="dither the last ROWS rows into a mist colour")

    p = sub.add_parser("art-export", help="turn art/NAME.png into editable text art art/NAME.txt")
    p.add_argument("name")
    p.add_argument("--project", default=None)
    p.add_argument("--colors", type=int, default=15, help="max colours (15 = attached sprite, 3 = sprite)")
    p.add_argument("--frame-width", type=int)
    p.add_argument("--frame-height", type=int, help="for grid sheets (e.g. 4x2 frames): the frame height")
    p.add_argument("--palette", nargs="?", const="1-7", metavar="RANGE",
                   help="fit to palette.txt's colours instead (entries RANGE, default 1-7: a bob "
                        "on a 3-plane playfield)")
    p.add_argument("--pin", action="append", metavar="0xRGB@REG",
                   help="keep this colour on this register, e.g. 0xFFF@31 (colours shared with "
                        "other sprites); repeatable")
    p.add_argument("--dither", action="store_true", help="dither shades that fall between two colours")
    p.add_argument("--trim", action="store_true",
                   help="drop blank rows, as many at the top as at the bottom (smaller blits)")
    p.add_argument("--force", action="store_true")

    p = sub.add_parser("art-gen", help="generate pixel art with Retro Diffusion into art/ (needs RD_API_KEY)",
                       description="Generates a PNG constrained to the game palette, saves it as art/NAME.png, "
                                   "adds it to art.toml and converts it. Use --dry-run for a free price check.")
    p.add_argument("name")
    p.add_argument("prompt", help="describe the subject; the style handles the pixel-art look")
    p.add_argument("--project", default=None)
    p.add_argument("--kind", choices=["bob", "sprite", "bitmap"], default="bob",
                   help="bitmap = background/parallax band/tileset (no mask)")
    p.add_argument("--depth", type=int, default=3, help="bitmap: bitplanes (colours = 2^depth)")
    p.add_argument("--tile-x", action="store_true", help="seamless horizontal tiling (looping backgrounds)")
    p.add_argument("--free-colors", action="store_true",
                   help="let the model choose colours; the asset gets palette = \"auto\" (its own palette)")
    p.add_argument("--opaque", action="store_true", help="keep the background (no transparency)")
    p.add_argument("--size", default="32x32", help="WxH in pixels (sprites: width <= 16)")
    p.add_argument("--channel", type=int, default=2, help="sprites: hardware channel")
    p.add_argument("--colors", help="restrict to 0xRGB colors (AGA: 0xRRGGBB), e.g. 0xFFF,0xFA0,0x000 "
                                    "(default: art/palette.txt)")
    p.add_argument("--style", default="rd_plus__low_res",
                   help="Retro Diffusion style (rd_plus__low_res $0.025; rd_pro__default $0.18, best)")
    p.add_argument("--seed", type=int)
    p.add_argument("-n", type=int, default=1, help="number of variants (saved as NAME_altN.png)")
    p.add_argument("--dry-run", action="store_true", help="free price check; generates nothing")

    sub.add_parser("help-scenario", help="print the scenario language reference")
    sub.add_parser("help-art", help="print the art pipeline reference")
    sub.add_parser("help-sound", help="print the sound and music (MML) reference")
    sub.add_parser("help-release", help="print the agk release reference")
    sub.add_parser("help-map", help="print the Tiled map import reference")
    sub.add_parser("help-disk", help="print the packed disk files reference")
    p = sub.add_parser("map", help="convert maps/*.tmj (Tiled) to C tables and draw previews")
    p.add_argument("project", nargs="?")
    p = sub.add_parser("sound", help="convert sound/ (effects + MML music) and write previews")
    p.add_argument("project", nargs="?")
    sub.add_parser("help", help="show this help")

    args = ap.parse_args(argv)
    if args.cmd == "help":
        ap.print_help()
        return 0
    if args.cmd == "help-scenario":
        print(scenario.__doc__)
        return 0
    if args.cmd == "help-sound":
        from . import sound
        print(sound.__doc__)
        return 0
    if args.cmd == "help-disk":
        from . import pack
        print(pack.__doc__)
        return 0
    if args.cmd == "help-map":
        from . import tilemap
        print(tilemap.__doc__)
        return 0
    if args.cmd == "help-release":
        from . import release
        print(release.__doc__)
        return 0
    if args.cmd == "help-art":
        from . import art
        print(art.__doc__)
        return 0
    try:
        return {"doctor": cmd_doctor, "build": cmd_build, "run": cmd_run, "test": cmd_test,
                "unit": cmd_unit, "new": cmd_new, "art": cmd_art, "art-gen": cmd_art_gen,
                "art-export": cmd_art_export, "art-animate": cmd_art_animate,
                "play": cmd_play, "art-clean": cmd_art_clean, "sound": cmd_sound,
                "record": cmd_record, "lint": cmd_lint,
                "profile": cmd_profile, "release": cmd_release, "map": cmd_map, "inspect": cmd_inspect, "rip": cmd_rip, "recomp": cmd_recomp}[args.cmd](args)
    except runner.RunError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
