"""agk release: a build turned into a download.

    build/release/NAME-VERSION.zip      NAME.adf (boots on any Amiga), NAME (the
                                        executable, for hard disks), README.txt,
                                        NAME.png (a screenshot, 640x512)
    build/release/NAME-VERSION.readme   the same facts in Aminet's .readme format

agk.toml:
    [release]
    title = "VOIDRUNNER"                  # (default: the name, in capitals)
    version = "1.0"                       # (default: 1.0)
    author = "Your Name"
    short = "Horizontal shoot-'em-up"     # one line, up to 40 characters (Aminet's Short:)
    type = "game/shoot"                   # Aminet's Type: (game/shoot, game/jump, game/race, game/misc...)
    description = \"\"\"...\"\"\"               # a paragraph or two
    controls = "Joystick in port 2, fire starts. Esc quits."
    screenshot = "demo/screenshot.agk"    # a scenario; its last screenshot is the picture
                                          # (default: the screen 3 s after boot)
"""
import datetime
import os
import zipfile

from .image import Image

REQUIREMENTS = "Any Amiga with 512 KB of chip RAM and Kickstart 1.3 or later (OCS, PAL)."


class ReleaseError(Exception):
    pass


def config(proj, cfg):
    """agk.toml's [release] with defaults -> dict"""
    r = dict(cfg or {})
    r.setdefault("title", proj["name"].upper())
    r.setdefault("version", "1.0")
    r.setdefault("author", "")
    r.setdefault("short", f"{r['title']}, an Amiga game")
    r.setdefault("type", "game/misc")
    r.setdefault("description", "")
    r.setdefault("controls", "Joystick in port 2.")
    if len(r["short"]) > 40:
        raise ReleaseError(f"[release] short is {len(r['short'])} characters; Aminet allows 40")
    return r


def readme_text(proj, r, date):
    lines = [f"{r['title']} {r['version']}", "=" * len(f"{r['title']} {r['version']}"), ""]
    if r["description"]:
        lines += [r["description"].strip(), ""]
    lines += [f"Controls: {r['controls']}", "",
              f"Requirements: {REQUIREMENTS}", "",
              f"{proj['name']}.adf is a bootable floppy image: write it to a disk, or mount it in an",
              f"emulator or on a Gotek. {proj['name']} is the game itself, for a hard disk: copy it",
              "anywhere and run it from the Shell or Workbench.", ""]
    if r["author"]:
        lines += [f"By {r['author']}.", ""]
    lines += [f"Built {date} with the Amiga Game Kit (https://github.com/codebase/amiga-game-kit)."]
    return "\n".join(lines) + "\n"


def aminet_readme(proj, r, zip_name):
    """Aminet's .readme format: the header fields it indexes, then the text."""
    head = [f"Short:        {r['short']}",
            f"Author:       {r['author'] or 'unknown'}",
            f"Uploader:     {r['author'] or 'unknown'}",
            f"Type:         {r['type']}",
            f"Version:      {r['version']}",
            "Architecture: m68k-amigaos >= 1.3",
            f"Distribution: Aminet", ""]
    body = [f"{r['title']} {r['version']}", ""]
    if r["description"]:
        body += [r["description"].strip(), ""]
    body += [f"Controls: {r['controls']}", "", f"Requirements: {REQUIREMENTS}", "",
             f"{zip_name} holds a bootable ADF and the executable for hard disks."]
    return "\n".join(head + body) + "\n"


def scaled(img, factor=2):
    """An Image with square pixels (a .screen.png) scaled up, nearest neighbour"""
    w, h = img.width, img.height
    rows = []
    for y in range(h):
        row = img.rgb[y * w * 3:(y + 1) * w * 3]
        out = b"".join(row[x * 3:x * 3 + 3] * factor for x in range(w))
        rows += [out] * factor
    return Image(b"".join(rows), w * factor, h * factor)


def build_zip(proj, r, adf, exe, screen_png, out_dir, date=None):
    """-> (zip path, aminet readme path)"""
    date = date or datetime.date.today().isoformat()
    os.makedirs(out_dir, exist_ok=True)
    base = f"{proj['name']}-{r['version']}"
    zip_path = os.path.join(out_dir, base + ".zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(adf, f"{proj['name']}.adf")
        z.write(exe, proj["name"])
        z.writestr("README.txt", readme_text(proj, r, date))
        if screen_png:
            png = os.path.join(out_dir, f"{proj['name']}.png")
            scaled(Image.load_png(screen_png)).save_png(png)
            z.write(png, f"{proj['name']}.png")
    readme_path = os.path.join(out_dir, base + ".readme")
    with open(readme_path, "w") as f:
        f.write(aminet_readme(proj, r, base + ".zip"))
    return zip_path, readme_path
