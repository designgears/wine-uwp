#!/usr/bin/env python3
"""shownframes.py LOG...: swap chain frames that reached the screen, per 250 ms over the 4 s after the viewport starts
presenting (the first frame after a gap of more than 5 s: a project opening), and the five longest gaps between them.
Needs WINEDEBUG=+timestamp,trace+dcompgate ("swap chain frame N shown" lines). Frames the app presents but that are
never shown do not count, which is the point: presents and paints both looked fine while the viewport stood still."""
import sys
for f in sys.argv[1:]:
    ts = [float(l.split(':', 1)[0]) for l in open(f, errors='replace') if 'swap chain frame' in l and 'shown' in l]
    if not ts:
        print(f, 'none'); continue
    start = ts[0]
    for a, b in zip(ts, ts[1:]):
        if b - a > 5: start = b
    w = [t for t in ts if start <= t < start + 4]
    b = [0] * 16
    for t in w: b[min(15, int((t - start) * 4))] += 1
    gaps = sorted(((y - x) * 1000 for x, y in zip(w, w[1:])), reverse=True)[:5]
    print("%s: %s | worst gaps %s" % (f, ' '.join('%2d' % x for x in b), ' '.join('%.0f' % g for g in gaps)))
