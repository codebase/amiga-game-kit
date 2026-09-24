"""Machine profiles and Kickstart ROM discovery.

ROMs are never shipped with AGK. We find the user's own files by SHA-1 so the
file names don't matter and bad dumps are caught before they waste a run.
"""
import hashlib
import os
from dataclasses import dataclass, field

from .paths import ROOT, THIRD_PARTY

# SHA-1 of known-good dumps (TOSEC [!] / Amiga Forever).
KNOWN_ROMS = {
    "891e9a547772fe0c6c19b610baf8bc4ea7fcb785": "kick13-34005-a500",
    "3b7f1493b27e212830f989f26ca76c02049f09ca": "kick31-40063-a500",
    "e21545723fe8374e91342617604f1b3d703094f1": "kick31-40068-a1200",
}

AROS_DIR = os.path.join(THIRD_PARTY, "vAmiga/Resources/Assets.xcassets/Binary")
AROS_ROM = os.path.join(AROS_DIR, "aros-20260820-rom.dataset/aros-20260820-rom.bin")
AROS_EXT = os.path.join(AROS_DIR, "aros-20260820-ext.dataset/aros-20260820-ext.bin")


@dataclass
class Profile:
    name: str
    description: str
    scheme: str
    rom: str                      # KNOWN_ROMS value, or "aros"
    config: list = field(default_factory=list)


PROFILES = {p.name: p for p in [
    Profile("a500", "A500, OCS, 512K chip + 512K slow, Kickstart 1.3 (baseline)",
            "A500_OCS_1MB", "kick13-34005-a500"),
    Profile("a500-ks31", "A500, ECS, 1MB, Kickstart 3.1",
            "A500_ECS_1MB", "kick31-40063-a500"),
    Profile("a500-aros", "A500, OCS, 1MB + 2MB fast, AROS ROM (free, for CI)",
            "A500_OCS_1MB", "aros", ["mem set FAST_RAM 2048"]),
    Profile("a1200", "A1200, AGA, 2MB, Kickstart 3.1 (experimental; AGA colours differ slightly, needs own goldens)",
            "A1200_2MB", "kick31-40068-a1200", ["cpu set OVERCLOCKING 0"]),
]}


def rom_dirs():
    dirs = [os.path.join(ROOT, "roms")]
    if os.environ.get("AGK_ROMS"):
        dirs.insert(0, os.environ["AGK_ROMS"])
    return dirs


def sha1_file(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def find_roms():
    """Map known ROM ids -> path for every recognised file in the ROM dirs."""
    found = {}
    for d in rom_dirs():
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            path = os.path.join(d, name)
            if os.path.isfile(path) and os.path.getsize(path) in (262144, 524288):
                rom_id = KNOWN_ROMS.get(sha1_file(path))
                if rom_id and rom_id not in found:
                    found[rom_id] = path
    return found


def resolve(profile_name):
    """Return (profile, rom_path, ext_path) or raise with an actionable message."""
    if profile_name not in PROFILES:
        raise SystemExit(f"unknown profile '{profile_name}' (have: {', '.join(PROFILES)})")
    p = PROFILES[profile_name]
    if p.rom == "aros":
        if not os.path.exists(AROS_ROM):
            raise SystemExit("AROS ROM not found - run tools/setup")
        return p, AROS_ROM, AROS_EXT
    roms = find_roms()
    if p.rom not in roms:
        raise SystemExit(
            f"profile '{p.name}' needs ROM {p.rom}, not found in {', '.join(rom_dirs())}.\n"
            f"Put your own licensed Kickstart there (any file name; matched by SHA-1),\n"
            f"or use profile 'a500-aros' which needs no Kickstart.")
    return p, roms[p.rom], None
