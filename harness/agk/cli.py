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
                if (f.endswith((".c", ".h", ".s", ".asm", ".i", ".cmake", ".toml", ".png"))
                    or f == "CMakeLists.txt" or (os.path.basename(base) == "art" and f.endswith(".txt"))):
                    newest = max(newest, os.path.getmtime(os.path.join(base, f)))
    return newest


def need_adf(proj, build=True):
    """Make sure build/<name>.adf exists and is newer than the sources,
    rebuilding if needed so tests never run against stale code."""
    # Regenerate art first (cheap; files are only rewritten when their content
    # changes), so edited art or a newer converter counts as a source change.
    if not _run_art(proj, quiet=True):
        raise SystemExit(1)
    newest = _newest_source(proj)
    for gen in ("art.c", "art.h"):
        p = os.path.join(proj["dir"], "build", "art", gen)
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


def cmd_art(args):
    proj = load_project(args.project)
    if not os.path.exists(os.path.join(proj["dir"], "art", "art.toml")):
        raise SystemExit("no art/art.toml in this project - see: agk help-art")
    return 0 if _run_art(proj) else 1


def cmd_art_gen(args):
    from . import rd
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
                                         remove_bg=not args.opaque)
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
    n, warnings = art.export_text(src, dst, args.colors, args.frame_width, args.frame_height)
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
        return subprocess.run(["open", "-a", "FS-UAE", cfg]).returncode
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
    if not _run_art(proj, quiet=True):
        return 1
    return subprocess.run(_build_cmd(proj, args.define or [])).returncode


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
    for f in res["failures"]:
        print(f"  ! {f}")
    print(f"  serial: {rel(os.path.join(res['outdir'], 'serial.txt'))}")


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
    failed = 0
    for t in tests:
        exe = os.path.join(bindir, os.path.splitext(t)[0])
        cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-g", "-fsanitize=address,undefined",
               "-I", os.path.join(proj["dir"], "src"), *srcs, os.path.join(udir, t), "-o", exe]
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
    p.add_argument("--colors", help="restrict to these 12-bit colours, e.g. 0xFFF,0xFA0,0x000 "
                                    "(default: art/palette.txt)")
    p.add_argument("--style", default="rd_plus__low_res",
                   help="Retro Diffusion style (rd_plus__low_res $0.025; rd_pro__default $0.18, best)")
    p.add_argument("--seed", type=int)
    p.add_argument("-n", type=int, default=1, help="number of variants (saved as NAME_altN.png)")
    p.add_argument("--dry-run", action="store_true", help="free price check; generates nothing")

    sub.add_parser("help-scenario", help="print the scenario language reference")
    sub.add_parser("help-art", help="print the art pipeline reference")
    sub.add_parser("help", help="show this help")

    args = ap.parse_args(argv)
    if args.cmd == "help":
        ap.print_help()
        return 0
    if args.cmd == "help-scenario":
        print(scenario.__doc__)
        return 0
    if args.cmd == "help-art":
        from . import art
        print(art.__doc__)
        return 0
    try:
        return {"doctor": cmd_doctor, "build": cmd_build, "run": cmd_run, "test": cmd_test,
                "unit": cmd_unit, "new": cmd_new, "art": cmd_art, "art-gen": cmd_art_gen,
                "art-export": cmd_art_export, "art-animate": cmd_art_animate,
                "play": cmd_play, "art-clean": cmd_art_clean}[args.cmd](args)
    except runner.RunError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
