#!/usr/bin/env python3
"""profdiff.py BEFORE AFTER OUT: a WINE_UWP_PROFILE dump of just the samples taken between two snapshots.

The profiler writes cumulative counts, so copy <prefix>.<pid> at the two moments you care about and subtract;
OUT keeps the module and thread-name lines of AFTER and can be read with profsym.py."""
# profdiff.py before after out: S lines of 'after' minus 'before' (cumulative dumps), M/T lines from 'after'
import sys
def load(p):
    s={}; other=[]
    for l in open(p):
        q=l.split()
        if q and q[0]=='S': s[' '.join([q[1],q[2]]+q[4:])]=(q, int(q[3]))
        else: other.append(l)
    return s, other
b,_=load(sys.argv[1]); a,other=load(sys.argv[2])
with open(sys.argv[3],'w') as f:
    f.writelines(other)
    for k,(q,c) in a.items():
        d=c-(b[k][1] if k in b else 0)
        if d>0: f.write(' '.join(q[:3]+[str(d)]+q[4:])+'\n')
