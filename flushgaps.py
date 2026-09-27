#!/usr/bin/env python3
"""flushgaps.py LOG T0 T1: per second from T0, window-surface flushes to the display (WINEDEBUG=+timestamp,trace+win
"Flushing hwnd" lines, busiest window) against dcomp paints (trace+dcompgate), and the longest gap between flushes."""
import sys,re,collections
t0,t1=float(sys.argv[2]),float(sys.argv[3])
fl=collections.defaultdict(list); paints=collections.Counter()
for raw in open(sys.argv[1],'rb'):
    l=raw.decode('latin1')
    if 'Flushing hwnd' in l:
        t=float(l.split(':',1)[0])
        if t0<=t<=t1:
            m=re.search(r'Flushing hwnd (\S+)',l); fl[m.group(1)].append(t)
    elif 'paint, scene generation' in l:
        t=float(l.split(':',1)[0])
        if t0<=t<=t1: paints[int(t-t0)]+=1
if not fl: print('no flushes'); sys.exit()
h=max(fl,key=lambda k:len(fl[k])); ts=fl[h]
per=collections.Counter(int(t-t0) for t in ts)
for s in range(int(t1-t0)+1):
    g=[(b-a)*1000 for a,b in zip(ts,ts[1:]) if s<=a-t0<s+1]
    print('s%d: flushes %d paints %d  max gap %.0f ms, gaps>30ms %d' % (s, per[s], paints[s], max(g) if g else 0, sum(x>30 for x in g)))
