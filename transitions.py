#!/usr/bin/env python3
# transitions.py <log> <times>: per UI transition (a "name uptime" line per action in <times>, e.g. open/close of a menu),
# the first painted frame, the frames that changed pixels, the largest gap between them and any batch-gate stalls.
# Needs WINEDEBUG=+timestamp,trace+dcompgate; take the uptime with `cut -d" " -f1 /proc/uptime` when acting.
import sys, re
log, times = sys.argv[1], sys.argv[2]
acts = [(l.split()[0], float(l.split()[1])) for l in open(times)]
fr = []; defer = []
for l in open(log, errors='replace'):
    m = re.match(r'(\d+\.\d+):[0-9a-f]+:trace:dcompgate:\S+ (.*)', l)
    if not m: continue
    t = float(m.group(1)); msg = m.group(2)
    if msg.startswith('frame done'):
        k = int(re.search(r'(\d+) kpx', msg).group(1))
        if k: fr.append(t)
    elif msg.startswith('painting after'): defer.append((t, int(re.search(r'after (\d+)', msg).group(1))))
for a, t in acts:
    f = [x for x in fr if t <= x < t + 0.6]
    if not f: print("%-5s no frames" % a); continue
    gaps = [(f[i+1]-f[i])*1000 for i in range(len(f)-1)]
    d = [ms for (x, ms) in defer if t <= x < t + 0.6]
    print("%-5s first %4.0f ms, %3d frames, last change %4.0f ms, max gap %3.0f, gaps>12ms: %s, gate %s" % (a, (f[0]-t)*1000, len(f), (f[-1]-t)*1000, max(gaps) if gaps else 0, [round(g) for g in gaps if g > 12], d))
