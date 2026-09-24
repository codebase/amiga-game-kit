"""Generate pixel art with the Retro Diffusion API straight into a project's
art/ folder, constrained to the game's palette.

    agk art-gen NAME "a red slime with big eyes" --kind bob --size 32x16
    agk art-gen NAME "..." --dry-run      # free price check, generates nothing

The API key comes from $RD_API_KEY or ~/.config/agk/credentials
(a line RD_API_KEY=rdpk-...). Never put it in the project.
"""
import base64
import json
import os
import time
import urllib.error
import urllib.request
import uuid

from .art import parse_palette, to_rgb8
from .image import Image

API = "https://api.retrodiffusion.ai/v2"
DEFAULT_STYLE = "rd_plus__low_res"     # 16-128 px, cheap; rd_pro__* is higher quality (12-256 px)
CREDENTIALS = os.path.expanduser("~/.config/agk/credentials")


class GenError(Exception):
    pass


def api_key():
    if os.environ.get("RD_API_KEY"):
        return os.environ["RD_API_KEY"]
    if os.path.exists(CREDENTIALS):
        with open(CREDENTIALS) as f:
            for line in f:
                k, _, v = line.strip().partition("=")
                if k == "RD_API_KEY" and v:
                    return v
    raise GenError("no Retro Diffusion API key: set RD_API_KEY or add 'RD_API_KEY=rdpk-...' "
                   f"to {CREDENTIALS} (keys: https://www.retrodiffusion.ai/app/devtools)")


def _request(method, path, body=None, extra_headers=None):
    req = urllib.request.Request(
        API + path, method=method,
        data=json.dumps(body).encode() if body is not None else None,
        headers={"X-RD-Token": api_key(), "Content-Type": "application/json",
                 "User-Agent": "amiga-game-kit", **(extra_headers or {})})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        detail = e.read().decode(errors="replace")[:500]
        if e.code == 402 or "balance" in detail.lower():
            raise GenError(f"Retro Diffusion: not enough balance ({detail})")
        raise GenError(f"Retro Diffusion HTTP {e.code}: {detail}")
    except urllib.error.URLError as e:
        raise GenError(f"can't reach Retro Diffusion: {e.reason}")


def palette_png(colors):
    """A 1-pixel-high PNG with one pixel per colour, for input_palette."""
    rgb = b"".join(bytes(to_rgb8(c)) for c in colors)
    return base64.b64encode(Image(rgb, len(colors), 1).to_png()).decode()


def generate(prompt, width, height, colors, style=DEFAULT_STYLE, seed=None, n=1, dry_run=False,
             tile_x=False, remove_bg=True):
    """Returns (list of PNG bytes, cost, remaining balance)."""
    body = {
        # The API's guidance: describe the subject, give a plain contrasting
        # background, and let remove_bg make it transparent.
        "prompt": f"{prompt}, on a plain white background" if remove_bg else prompt,
        "prompt_style": style, "width": width, "height": height, "num_images": n,
        "remove_bg": remove_bg,
    }
    if tile_x:
        body["tile_x"] = True
    if colors:
        body["input_palette"] = palette_png(colors)
    if seed is not None:
        body["seed"] = seed
    if dry_run:
        body["check_cost"] = True
        r = _request("POST", "/inferences", body)
        return [], r.get("balance_cost"), r.get("remaining_balance")
    accepted = _request("POST", "/inferences", body, {"Idempotency-Key": str(uuid.uuid4())})
    task_id = accepted["task_id"]
    for _ in range(90):
        task = _request("GET", f"/inferences/tasks/{task_id}")
        if task["status"] in ("pending", "running", "accepted"):
            time.sleep(2)
            continue
        if task["status"] == "failed":
            raise GenError(f"generation failed (refunded): {task.get('error')}")
        res = task["result"]
        return ([base64.b64decode(b) for b in res.get("base64_images", [])],
                res.get("balance_cost"), res.get("remaining_balance"))
    raise GenError(f"generation still running after 3 minutes (task {task_id})")


def add_to_art_toml(art_dir, name, source, kind, channel=None, frame_width=None, extra=None):
    path = os.path.join(art_dir, "art.toml")
    text = open(path).read() if os.path.exists(path) else "# Reference: agk help-art\n"
    if f"[{name}]" in text:
        return False
    lines = [f"\n[{name}]", f'source = "{source}"', f'kind = "{kind}"']
    if channel is not None:
        lines.append(f"channel = {channel}")
    if frame_width:
        lines.append(f"frame_width = {frame_width}")
    for k, v in (extra or {}).items():
        lines.append(f"{k} = {json.dumps(v)}")
    with open(path, "w") as f:
        f.write(text.rstrip("\n") + "\n" + "\n".join(lines) + "\n")
    return True


def sprite_colors_from(project_dir, spec):
    if spec:
        return [int(c, 0) for c in spec.split(",")]
    pal = os.path.join(project_dir, "art", "palette.txt")
    if not os.path.exists(pal):
        return []
    return [c for _, c in sorted(parse_palette(pal).items())]


ANIMATIONS = ("walking", "idle", "jump", "crouch", "attack", "destroy")


def animate(input_png_bytes, action, width, height, frames=8, dry_run=False, prompt=None):
    """Animate a start frame (Retro Diffusion advanced animation). Returns
    (spritesheet PNG bytes or None, cost, balance). The start frame must be
    32-256 px, a multiple of 8, without transparency."""
    import subprocess
    if action not in ANIMATIONS:
        raise GenError(f"action must be one of {', '.join(ANIMATIONS)}")
    # Flatten transparency onto white (the API wants RGB without alpha)
    from .image import load_png_rgba, Image as _Img
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".png") as t:
        t.write(input_png_bytes); t.flush()
        w, h, px = load_png_rgba(t.name)
    rgb = bytearray()
    for r, g, b, a in px:
        k = a / 255
        rgb += bytes((round(r * k + 255 * (1 - k)), round(g * k + 255 * (1 - k)), round(b * k + 255 * (1 - k))))
    flat = base64.b64encode(_Img(bytes(rgb), w, h).to_png()).decode()
    body = {"prompt": prompt or action, "prompt_style": f"rd_advanced_animation__{action}",
            "width": width, "height": height, "num_images": 1, "input_image": flat,
            "frames_duration": frames, "return_spritesheet": True, "remove_bg": True}
    if dry_run:
        body["check_cost"] = True
        r = _request("POST", "/inferences", body)
        return None, r.get("balance_cost"), r.get("remaining_balance")
    accepted = _request("POST", "/inferences", body, {"Idempotency-Key": str(uuid.uuid4())})
    return wait_task(accepted["task_id"])


def wait_task(task_id, minutes=20):
    """Poll a task (animations take several minutes). Never resubmit a paid
    request - if this times out, call wait_task again with the same id."""
    for _ in range(minutes * 6):
        task = _request("GET", f"/inferences/tasks/{task_id}")
        if task["status"] in ("pending", "running", "accepted"):
            time.sleep(10)
            continue
        if task["status"] == "failed":
            raise GenError(f"generation failed (refunded): {task.get('error')}")
        res = task["result"]
        imgs = res.get("base64_images", [])
        return (base64.b64decode(imgs[0]) if imgs else None, res.get("balance_cost"), res.get("remaining_balance"))
    raise GenError(f"still running after {minutes} minutes - resume with: agk art-animate --resume {task_id}")
