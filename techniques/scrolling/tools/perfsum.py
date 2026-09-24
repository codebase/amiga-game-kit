# Summarise AGK perf windows by what the camera did during each window.
#   agk run -f tests/perf.agk && python3 tools/perfsum.py build/agk-run/a500/perf/serial.txt
import re, sys
for f in sys.argv[1:]:
    lines = open(f).read().splitlines()
    buf = next((l for l in lines if 'AGK buffer' in l), '')
    last = None; cats = {}
    for l in lines:
        m = re.search(r'AGK frame=\d+ x=(\d+) cam=(\d+)', l)
        if m: prev, last = last, (int(m[1]), int(m[2])); cur = (prev, last); continue
        m = re.search(r'AGK perf frames=\d+ dropped=(\d+) load=(\d+) maxload=(\d+)', l)
        if m and last and 'cur' in dir() and cur[0]:
            (x0, c0), (x1, c1) = cur
            dc = abs(c1 - c0)
            kind = 'idle' if x0 == x1 else ('walk, no scroll' if dc == 0 else ('scroll %dpx' % round(dc / 50) if dc >= 45 else 'mixed'))
            cats.setdefault(kind, []).append((int(m[2]), int(m[3]), int(m[1])))
    print(f, buf)
    for k, v in sorted(cats.items()):
        print('  %-11s windows=%d load avg=%.1f  maxload max=%d  dropped=%d' % (
            k, len(v), sum(a for a, _, _ in v) / len(v), max(b for _, b, _ in v), sum(d for _, _, d in v)))
