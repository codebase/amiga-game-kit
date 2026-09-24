"""MCP server (stdio) exposing agk to AI agents.

    python3 -m agk.mcp          (or: tools/agk-mcp)

Tools return text summaries plus the screenshots as images, so a multimodal
agent can look at what the game actually drew. Stdlib only: newline-delimited
JSON-RPC 2.0 on stdin/stdout, as the MCP stdio transport specifies.
"""
import base64
import json
import os
import subprocess
import sys

from . import scenario
from .paths import ROOT

AGK = [sys.executable, "-m", "agk"]
ENV = dict(os.environ, PYTHONPATH=os.path.join(ROOT, "harness"))
MAX_IMAGES = 6

PROJECT = {"type": "string", "description": "Game project directory (contains agk.toml). Default: current directory."}
PROFILE = {"type": "string", "description": "Machine profile: a500 (KS1.3, default), a500-ks31, a500-aros, a1200."}

TOOLS = [
    {"name": "agk_build",
     "description": "Cross-compile the game into a bootable ADF floppy image. Shows compiler errors/warnings "
                    "for the project's own files.",
     "inputSchema": {"type": "object", "properties": {"project": PROJECT}}},
    {"name": "agk_run",
     "description": "Boot the game in a headless, deterministic Amiga emulator, play scenario steps and return "
                    "the screenshots (320x256, game coordinates) plus the serial log. Steps use the scenario "
                    "language, e.g. ['press right 20', 'wait 5', 'screenshot moved', 'expect-serial \"x=192\"']. "
                    "Time is in frames (50/s) from when the game prints 'AGK ready'. Call agk_scenario_help for "
                    "all commands. Build first with agk_build.",
     "inputSchema": {"type": "object", "properties": {
         "project": PROJECT, "profile": PROFILE,
         "steps": {"type": "array", "items": {"type": "string"}, "description": "Scenario lines."}},
         "required": ["steps"]}},
    {"name": "agk_test",
     "description": "Run the project's emulator tests (tests/*.agk) on its profiles and compare screenshots "
                    "with the golden images. Failures include a red-highlight diff image. Set update=true only "
                    "when a visual change is intended, to accept current screenshots as the new goldens.",
     "inputSchema": {"type": "object", "properties": {
         "project": PROJECT, "profile": PROFILE,
         "only": {"type": "string", "description": "Run just this test (file name without .agk)."},
         "update": {"type": "boolean", "description": "Accept current screenshots as goldens."}}}},
    {"name": "agk_unit",
     "description": "Compile the game's rules (unit_sources in agk.toml) for the host and run tests/unit/*.c. "
                    "Fast (milliseconds); use it after every change to game logic.",
     "inputSchema": {"type": "object", "properties": {"project": PROJECT}}},
    {"name": "agk_doctor",
     "description": "Check that Docker, the emulator and Kickstart ROMs are set up; list usable profiles.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "agk_scenario_help",
     "description": "Reference for the scenario language used by agk_run steps and tests/*.agk files.",
     "inputSchema": {"type": "object", "properties": {}}},
]


def _agk(*args, timeout=900):
    cp = subprocess.run([*AGK, *args], capture_output=True, text=True, env=ENV, timeout=timeout)
    return cp.returncode, (cp.stdout + ("\n" + cp.stderr if cp.stderr.strip() else "")).strip()


def _image(path):
    with open(path, "rb") as f:
        return {"type": "image", "data": base64.b64encode(f.read()).decode(), "mimeType": "image/png"}


def _text(t):
    return {"type": "text", "text": t}


def _run_summary(res):
    lines = [f"{'PASS' if res['ok'] else 'FAIL'} {res['scenario']} [{res['profile']}]"]
    lines += [f"! {f}" for f in res["failures"]]
    tail = res.get("serial_tail", "").strip()
    if tail:
        lines.append("serial (tail):\n" + tail)
    return "\n".join(lines)


def call(name, a):
    project = a.get("project") or "."
    if name == "agk_scenario_help":
        return [_text(scenario.__doc__)], False
    if name == "agk_doctor":
        rc, out = _agk("doctor")
        return [_text(out)], rc != 0
    if name == "agk_build":
        rc, out = _agk("build", project)
        return [_text(out or ("build ok" if rc == 0 else "build failed"))], rc != 0
    if name == "agk_unit":
        rc, out = _agk("unit", project)
        return [_text(out)], rc != 0
    if name == "agk_run":
        args = ["run", project, "--json"] + (["-p", a["profile"]] if a.get("profile") else [])
        for s in a.get("steps") or []:
            args += ["-s", s]
        rc, out = _agk(*args)
        try:
            res = json.loads(out[out.index("{"):])
        except ValueError:
            return [_text(out)], True
        content = [_text(_run_summary(res))]
        for n, shot in list(res["screenshots"].items())[:MAX_IMAGES]:
            content += [_text(f"screenshot '{n}' (320x256):"), _image(shot["screen_png"])]
        return content, not res["ok"]
    if name == "agk_test":
        args = ["test", project, "--json"]
        args += ["-p", a["profile"]] if a.get("profile") else []
        args += ["--only", a["only"]] if a.get("only") else []
        args += ["--update"] if a.get("update") else []
        rc, out = _agk(*args)
        try:
            results = json.loads(out[out.index("["):])
        except ValueError:
            return [_text(out)], True
        content, images = [], 0
        passed = sum(r["ok"] for r in results)
        content.append(_text(f"{passed}/{len(results)} passed\n" + "\n".join(
            _run_summary(r) for r in results if not r["ok"])))
        for r in results:
            for n, shot in r["screenshots"].items():
                g = shot.get("golden", {})
                if g.get("status") == "different" and g.get("diff") and images < MAX_IMAGES:
                    content += [_text(f"{r['scenario']} [{r['profile']}] '{n}': changed pixels in red"),
                                _image(g["diff"])]
                    images += 1
        return content, rc != 0
    raise KeyError(name)


def handle(msg):
    method, mid = msg.get("method"), msg.get("id")
    if mid is None:
        return None  # notification (e.g. notifications/initialized)
    if method == "initialize":
        result = {"protocolVersion": msg.get("params", {}).get("protocolVersion", "2025-06-18"),
                  "capabilities": {"tools": {}},
                  "serverInfo": {"name": "amiga-game-kit", "version": "0.1.0"},
                  "instructions": "Tools to build, run and test Commodore Amiga games made with the Amiga "
                                  "Game Kit. Typical loop: agk_unit -> agk_build -> agk_run/agk_test, and "
                                  "look at the returned screenshots."}
    elif method == "ping":
        result = {}
    elif method == "tools/list":
        result = {"tools": TOOLS}
    elif method == "tools/call":
        p = msg.get("params", {})
        try:
            content, is_error = call(p.get("name"), p.get("arguments") or {})
        except KeyError:
            return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": f"unknown tool {p.get('name')}"}}
        except subprocess.TimeoutExpired:
            content, is_error = [_text("timed out")], True
        result = {"content": content, "isError": is_error}
    else:
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": f"method not found: {method}"}}
    return {"jsonrpc": "2.0", "id": mid, "result": result}


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            resp = {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "parse error"}}
        else:
            resp = handle(msg)
        if resp is not None:
            sys.stdout.write(json.dumps(resp) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
