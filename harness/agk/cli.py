"""agk - Amiga Game Kit command line.

    agk doctor                      check toolchain, emulator, ROMs
    agk build [PROJECT]             compile + make bootable ADF
    agk run [PROJECT] [-s STEP]...  boot, play steps, save screenshots + serial
    agk test [PROJECT] [--update]   run tests/*.agk, compare screenshots to goldens
    agk unit [PROJECT]              compile game logic for the host and run tests/unit/*.c
    agk new DIR                     start a new game from the template

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
        raise SystemExit(f"no agk.toml in {path}")
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
    }


def need_adf(proj):
    if not os.path.exists(proj["adf"]):
        raise SystemExit(f"{proj['adf']} not found - run: agk build {os.path.relpath(proj['dir'])}")


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


def cmd_build(args):
    proj = load_project(args.project)
    return subprocess.run([os.path.join(ROOT, "tools", "build"), proj["dir"]]).returncode


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
            elif g["status"] == "updated":
                print(f"    golden: updated {rel(g['path'])}")
            else:
                x0, y0, x1, y1 = g["bbox"]
                gx0, gy0 = max(0, (x0 - SCREEN_X0) // 2), max(0, y0 - SCREEN_Y0)
                gx1, gy1 = (x1 - SCREEN_X0) // 2, y1 - SCREEN_Y0
                print(f"    golden: DIFFERENT - {g['pixels']} px changed around game x={gx0}..{gx1} "
                      f"y={gy0}..{gy1}, see {rel(g['diff'])}")
    for f in res["failures"]:
        print(f"  ! {f}")
    print(f"  serial: {rel(os.path.join(res['outdir'], 'serial.txt'))}")


def cmd_run(args):
    proj = load_project(args.project)
    need_adf(proj)
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
    outdir = os.path.abspath(args.out or os.path.join(proj["dir"], "build", "agk", profile, name))
    res = runner.run(proj["adf"], profile, sc, outdir, proj["boot"], fresh=args.fresh)
    _report(res, args.json)
    return 0 if res["ok"] else 1


def _check_goldens(res, golden_dir, profile, update):
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
            os.makedirs(golden_dir, exist_ok=True)
            if not os.path.exists(shared):
                shutil.copyfile(shot["png"], shared)
                info.update(status="updated", path=shared)
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
    need_adf(proj)
    tdir = os.path.join(proj["dir"], "tests")
    files = sorted(f for f in os.listdir(tdir) if f.endswith(".agk")) if os.path.isdir(tdir) else []
    if args.only:
        files = [f for f in files if os.path.splitext(f)[0] in args.only]
    if not files:
        raise SystemExit(f"no tests found in {rel(tdir)}")
    results = []
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
            res = runner.run(proj["adf"], profile, sc, outdir, proj["boot"])
            if not res["failures"] or res["screenshots"]:
                _check_goldens(res, os.path.join(tdir, "golden", name), profile, args.update)
            results.append(res)
            if not args.json:
                _report(res, False)
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

    p = sub.add_parser("run", help="boot the game, play steps, capture results",
                       description="Steps use the scenario language, e.g. -s 'press right 20' -s 'screenshot moved'. "
                                   "See 'agk help-scenario'.")
    p.add_argument("project", nargs="?")
    p.add_argument("-s", "--step", action="append", help="scenario line (repeatable)")
    p.add_argument("-f", "--file", help="scenario file instead of -s steps")
    p.add_argument("-p", "--profile")
    p.add_argument("-o", "--out")
    p.add_argument("--fresh", action="store_true", help="boot from scratch instead of the cached snapshot")
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("test", help="run tests/*.agk and compare screenshots with goldens")
    p.add_argument("project", nargs="?")
    p.add_argument("-p", "--profile", action="append", help="profile(s) to test (default: agk.toml test_profiles)")
    p.add_argument("--only", action="append", help="only run this test (repeatable)")
    p.add_argument("--update", action="store_true", help="accept current screenshots as goldens")
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("unit", help="compile game logic for the host and run tests/unit/*.c")
    p.add_argument("project", nargs="?")
    p.add_argument("-v", "--verbose", action="store_true")

    p = sub.add_parser("new", help="create a new game project from a template")
    p.add_argument("dir")
    p.add_argument("--name", help="executable name (default: directory name)")
    p.add_argument("--template", default="game")

    sub.add_parser("help-scenario", help="print the scenario language reference")

    args = ap.parse_args(argv)
    if args.cmd == "help-scenario":
        print(scenario.__doc__)
        return 0
    try:
        return {"doctor": cmd_doctor, "build": cmd_build, "run": cmd_run, "test": cmd_test,
                "unit": cmd_unit, "new": cmd_new}[args.cmd](args)
    except runner.RunError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
