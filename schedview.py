#!/usr/bin/env python3
# schedview.py <file> [start end]: per 10 ms slot, % busy of the busiest threads, as a character strip
import sys
lines = open(sys.argv[1]).read().split('\n')
names = dict(kv.split(':', 1) for kv in lines[-2].split()[1:]) if lines[-2].startswith('NAMES') else {}
rows = []
for l in lines:
    if not l or l.startswith('NAMES'): continue
    p = l.split(); rows.append((float(p[0]), {k: int(v) for k, v in (x.split(':') for x in p[1:])}))
t0, t1 = (float(sys.argv[2]), float(sys.argv[3])) if len(sys.argv) > 3 else (rows[0][0], rows[-1][0])
tot = {}
for (ta, a), (tb, b) in zip(rows, rows[1:]):
    if ta < t0 or tb > t1: continue
    for k in b:
        if k in a: tot[k] = tot.get(k, 0) + b[k] - a[k]
top = sorted(tot, key=lambda k: -tot[k])[:int(sys.argv[4]) if len(sys.argv) > 4 else 7]
for k in top: print('%-8s %-18s %6.0f ms' % (k, names.get(k, '?')[:18], tot[k] / 1e6))
# strip: one char per 10 ms: ' ' idle, '.' <30%, 'o' <70%, '#' >=70%
slot = 0.010
for k in top:
    s = ''; t = t0
    while t < t1:
        a = [r for r in rows if r[0] <= t]; b = [r for r in rows if r[0] >= t + slot]
        if not a or not b: break
        ra, rb = a[-1], b[0]
        busy = (rb[1].get(k, 0) - ra[1].get(k, 0)) / 1e9 / (rb[0] - ra[0]) if k in ra[1] and k in rb[1] else 0
        s += ' ' if busy < 0.05 else '.' if busy < 0.3 else 'o' if busy < 0.7 else '#'
        t += slot
    print('%-18s|%s|' % (names.get(k, '?')[:18], s))
