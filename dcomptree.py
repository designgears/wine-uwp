#!/usr/bin/env python3
"""Where is this visual in dcomp's tree? Rebuilds parent links from a WINEDEBUG=+dcomptree log
(add +timestamp to use TIME).

  dcomptree.py LOG VISUAL [TIME]     print VISUAL's ancestor chain as it stood at TIME (log seconds,
                                     default: end of log), then each ancestor's last painted position

dcomptree logs every create, insert and remove with the pointer the caller passed, which may be an
interface of the object rather than the object (an interop visual's IVisual view, a public visual's
interop view). Pointers within 0x100 bytes above a known object are taken to be that object.

The visit lines carry position, clip and (for interop visuals) offset / transform fields, so a visual
that is in the tree but painted somewhere absurd shows up at once - that is how a menu placed at
y = 1.5e29 was found (Findings-M4, 2026-09-25).
"""
import bisect
import re
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    log, start = sys.argv[1], int(sys.argv[2], 16)
    until = float(sys.argv[3]) if len(sys.argv) > 3 else float('inf')
    events, ids, dcomps, visits = [], set(), set(), {}
    with open(log, 'rb') as f:
        for raw in f:
            if b'dcomptree' not in raw:
                continue
            line = raw.decode('latin1')
            try:
                t = float(line.split(':', 1)[0])
            except ValueError:
                t = 0.0     # no +timestamp: TIME cannot be used
            if t > until:
                break
            m = re.search(r'visit (dcomp|public) ([0-9A-F]+) (.*)', line)
            if m:
                visits[int(m.group(2), 16)] = (t, m.group(3).strip())
                continue
            events.append(line)
            m = re.search(r'create dcomp ([0-9A-F]+)', line)
            if m:
                ids.add(int(m.group(1), 16))
                dcomps.add(int(m.group(1), 16))
            m = re.search(r'public ([0-9A-F]+) Insert', line)
            if m:
                ids.add(int(m.group(1), 16))
    known = sorted(ids | set(visits))    # leaves are only ever seen as children and in visits

    def norm(p):
        i = bisect.bisect_right(known, p) - 1
        return known[i] if i >= 0 and p - known[i] < 0x100 else p

    parent = {}
    for line in events:
        m = re.search(r'add (?:dcomp|public) ([0-9A-F]+) to ([0-9A-F]+)', line)
        if m:
            parent[norm(int(m.group(1), 16))] = norm(int(m.group(2), 16))
            continue
        m = (re.search(r'dcomp ([0-9A-F]+) Insert\w+ (?:dcomp|public) ([0-9A-F]+)', line) or
             re.search(r'public ([0-9A-F]+) Insert\w+ child ([0-9A-F]+)', line))
        if m:
            parent[norm(int(m.group(2), 16))] = norm(int(m.group(1), 16))
            continue
        m = re.search(r'dcomp ([0-9A-F]+) remove ([0-9A-F]+)', line)
        if m:
            owner, child = norm(int(m.group(1), 16)), norm(int(m.group(2), 16))
            if parent.get(child) == owner:
                del parent[child]

    node, chain = norm(start), []
    while node not in chain:
        chain.append(node)
        if node not in parent:
            break
        node = parent[node]
    for n in reversed(chain):
        kind = 'dcomp' if n in dcomps else 'public'
        seen = visits.get(n)
        print(f'{n:016x} {kind:6} ' + (f'@{seen[0]:.3f} {seen[1]}' if seen else 'never visited'))


if __name__ == '__main__':
    main()
