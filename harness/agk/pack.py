"""Packed data files on the disk: disk/ -> build/disk/NAME.pk -> the ADF.

Every file in a project's disk/ directory is packed (LZ, below) into
build/disk/NAME.pk by agk build, and tools/build writes build/disk/* to the
root of the floppy image. The game loads one with agk/pack.h:

    ULONG ulSize = agkLoadFile("level1.pk", pDest, ulDestMax);   // 0 on failure (an 'AGK ERR' says why)

It reads the packed file through AmigaDOS (so it works from a floppy, a hard
disk or WHDLoad, on Kickstart 1.3 up), unpacks into pDest, and returns the
unpacked size. Loading from a floppy takes real time (a 40 KB picture packs
to ~10 KB and loads in about half a second): call agkPerfSkip() after it so
the frame meter doesn't count the wait as dropped frames.

The format (agkUnpack in runtime/src/unpack.c reads it, ~40 lines of C):
    "AGKZ", the unpacked size (32 bits, big-endian), then tokens: a flag byte
    for the next 8 items, bit 7 first; a 1 bit = one literal byte follows, a
    0 bit = a match of two bytes: 12 bits of distance back (1-4096) and 4 bits
    of length minus 3 (3-18).
Plain LZSS: 2-4x on planar pictures and tile maps, worse on sampled sound
(which is better stored raw, as sound/ does).
"""
import os

MAGIC = b"AGKZ"
MAX_DIST = 4096
MIN_LEN, MAX_LEN = 3, 18


def pack(data):
    out = bytearray(MAGIC + len(data).to_bytes(4, "big"))
    n = len(data)
    heads = {}                   # 3 bytes -> [positions], newest last
    i = 0
    flags, items = 0, []
    nflags = 0

    def flush():
        nonlocal flags, items, nflags
        if nflags:
            out.append(flags << (8 - nflags))
            for it in items:
                out.extend(it)
        flags, items, nflags = 0, [], 0

    while i < n:
        best_len, best_dist = 0, 0
        if i + MIN_LEN <= n:
            key = data[i:i + 3]
            for pos in reversed(heads.get(key, ())):
                dist = i - pos
                if dist > MAX_DIST:
                    break
                length = 3
                limit = min(MAX_LEN, n - i)
                while length < limit and data[pos + length] == data[i + length]:
                    length += 1
                if length > best_len:
                    best_len, best_dist = length, dist
                    if length == limit:
                        break
        if best_len >= MIN_LEN:
            flags = (flags << 1) | 0
            items.append(bytes(((best_dist - 1) >> 4, (((best_dist - 1) & 15) << 4) | (best_len - MIN_LEN))))
            step = best_len
        else:
            flags = (flags << 1) | 1
            items.append(data[i:i + 1])
            step = 1
        nflags += 1
        if nflags == 8:
            flush()
        for k in range(i, i + step):
            if k + MIN_LEN <= n:
                lst = heads.setdefault(data[k:k + 3], [])
                lst.append(k)
                if len(lst) > 32:          # bounded search: a few candidates are enough
                    del lst[0]
        i += step
    flush()
    return bytes(out)


def unpack(packed):
    """The reference unpacker (the C one in runtime/src/unpack.c must agree)."""
    if packed[:4] != MAGIC:
        raise ValueError("not an AGKZ file")
    size = int.from_bytes(packed[4:8], "big")
    out = bytearray()
    i = 8
    while len(out) < size:
        flags = packed[i]
        i += 1
        for bit in range(7, -1, -1):
            if len(out) >= size:
                break
            if flags >> bit & 1:
                out.append(packed[i])
                i += 1
            else:
                a, b = packed[i], packed[i + 1]
                i += 2
                dist = ((a << 4) | (b >> 4)) + 1
                length = (b & 15) + MIN_LEN
                for _ in range(length):
                    out.append(out[-dist])
    return bytes(out)


def build(project_dir):
    """disk/* -> build/disk/NAME.pk. Returns [(name, raw size, packed size)] or None (no disk/)."""
    src_dir = os.path.join(project_dir, "disk")
    if not os.path.isdir(src_dir):
        return None
    out_dir = os.path.join(project_dir, "build", "disk")
    os.makedirs(out_dir, exist_ok=True)
    done = []
    for name in sorted(os.listdir(src_dir)):
        path = os.path.join(src_dir, name)
        if not os.path.isfile(path) or name.startswith("."):
            continue
        if len(name) > 27:   # AmigaDOS names: 30 characters, with ".pk"
            raise ValueError(f"disk/{name}: the name is too long for AmigaDOS (27 characters plus .pk)")
        data = open(path, "rb").read()
        packed = pack(data)
        out = os.path.join(out_dir, name + ".pk")
        if not os.path.exists(out) or open(out, "rb").read() != packed:
            with open(out, "wb") as f:
                f.write(packed)
        done.append((name + ".pk", len(data), len(packed)))
    return done
