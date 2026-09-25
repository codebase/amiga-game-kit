"""Record a scenario as a video with sound (agk record).

Runs the scenario in the emulator like `agk run`, but screenshots every frame
of scenario time (each `wait` / `press` frame; nothing during `wait-serial`)
and records Paula's output, then encodes them with ffmpeg:
  - MP4 (H.264 + AAC), the 320x256 playfield scaled 4x with square pixels
  - optionally a silent GIF (for README files, where GitHub shows GIFs inline)

A frame is 612 KB of raw pixels while recording: a 30 s video needs ~1 GB of
temporary space in the output directory (deleted afterwards).
"""
import math
import os
import re
import shutil
import subprocess
from dataclasses import replace

from .image import SCREEN_X0, SCREEN_Y0, SCREEN_W, SCREEN_H, WIDTH, HEIGHT

FPS = 50
FRAME = "video_{:05d}.raw"


class RecordError(Exception):
    pass


def video_scenario(sc):
    """A copy of a parsed scenario that screenshots every frame of scenario time."""
    lines, origins, n = [], [], 0
    for line, origin in zip(sc.lines, sc.origins):
        m = re.match(r"^wait (\d+) frames$", line)
        if not m:
            lines.append(line)
            origins.append(origin)
            continue
        for _ in range(int(m.group(1))):
            if n == 0:
                lines.append("agk audio mark _video")   # the audio track starts here
                origins.append(origin)
            lines += ["wait 1 frames", "agk screenshot {out}/" + FRAME.format(n)]
            origins += [origin, origin]
            n += 1
    if n == 0:
        raise RecordError("the scenario has no frames to record: use wait / press")
    return replace(sc, lines=lines, origins=origins), n


def _frames(outdir, count):
    """The playfield of each raw frame, 640x256 (vAmiga's hires pixels)."""
    w = SCREEN_W * 2
    for i in range(count):
        path = os.path.join(outdir, FRAME.format(i))
        if not os.path.exists(path):
            raise RecordError(f"frame {i} was not captured (the emulator stopped early? see emulator.log)")
        with open(path, "rb") as f:
            raw = f.read()
        if len(raw) != WIDTH * HEIGHT * 3:
            raise RecordError(f"frame {i}: unexpected size {len(raw)}")
        rows = []
        for y in range(SCREEN_Y0, SCREEN_Y0 + SCREEN_H):
            o = (y * WIDTH + SCREEN_X0) * 3
            rows.append(raw[o:o + w * 3])
        yield b"".join(rows)
        os.remove(path)


def encode(outdir, count, mp4, gif=None, gif_seconds=None, scale=4):
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RecordError("ffmpeg not found - install it (e.g. brew install ffmpeg)")
    wav = os.path.join(outdir, "audio.wav")
    offset = 0.0
    marks = wav + ".marks"
    if os.path.exists(marks):
        for line in open(marks):
            name, _, idx = line.strip().rpartition(" ")
            if name == "_video":
                # a screenshot shows the last finished frame: start the sound one frame later
                offset = int(idx) / 44100 + 1 / FPS
    cmd = [ffmpeg, "-y", "-loglevel", "error",
           "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{SCREEN_W * 2}x{SCREEN_H}", "-r", str(FPS), "-i", "-"]
    if os.path.exists(wav):
        # vAmiga mixes Paula well below full scale: bring the loudest moment to -1 dBFS
        from . import sound
        pcm, _rate = sound.read_wav(wav)
        peak = max((abs(v) for v in pcm), default=0.0)
        gain = -1 - 20 * math.log10(peak) if peak > 1e-4 else 0.0
        cmd += ["-ss", f"{offset:.4f}", "-i", wav, "-map", "0:v", "-map", "1:a",
                "-af", f"volume={gain:.1f}dB", "-c:a", "aac", "-b:a", "192k"]
    cmd += ["-vf", f"scale={SCREEN_W * scale}:{SCREEN_H * scale}:flags=neighbor",
            "-c:v", "libx264", "-preset", "slow", "-crf", "16", "-pix_fmt", "yuv420p",
            "-movflags", "+faststart", "-shortest", mp4]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    try:
        for frame in _frames(outdir, count):
            p.stdin.write(frame)
    finally:
        p.stdin.close()
        rc = p.wait()
    if rc:
        raise RecordError(f"ffmpeg failed ({rc})")
    if gif:
        limit = ["-t", str(gif_seconds)] if gif_seconds else []
        # 64 colours and only changed rectangles per frame: ~0.5 MB per second
        vf = ("fps=25,scale=640:512:flags=neighbor,split[a][b];"
              "[a]palettegen=max_colors=64:stats_mode=diff[p];"
              "[b][p]paletteuse=dither=none:diff_mode=rectangle")
        r = subprocess.run([ffmpeg, "-y", "-loglevel", "error", *limit, "-i", mp4, "-vf", vf, gif])
        if r.returncode:
            raise RecordError("ffmpeg failed making the GIF")
