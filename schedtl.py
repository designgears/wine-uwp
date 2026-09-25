#!/usr/bin/env python3
# schedtl.py <pid> <seconds> <out>: every 5 ms, each thread's cumulative run time (ns) from schedstat
import os, sys, time
pid, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
names = {}
rows = []
t0 = time.monotonic()
while time.monotonic() - t0 < secs:
    now = time.monotonic() - t0
    row = {}
    for t in os.listdir(f'/proc/{pid}/task'):
        try:
            row[t] = int(open(f'/proc/{pid}/task/{t}/schedstat').read().split()[0])
            if t not in names: names[t] = open(f'/proc/{pid}/task/{t}/comm').read().strip()
        except OSError: pass
    rows.append((now, row))
    time.sleep(0.005)
with open(out, 'w') as f:
    for now, row in rows:
        f.write('%.4f %s\n' % (now, ' '.join('%s:%d' % kv for kv in row.items())))
    f.write('NAMES ' + ' '.join('%s:%s' % (k, v.replace(' ', '_')) for k, v in names.items()) + '\n')
