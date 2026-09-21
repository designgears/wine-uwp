#!/usr/bin/env python
"""Map addresses to modules using +loaddll lines in a Wine log. Usage: addr2mod.py LOG [ADDR...]
With no ADDR, maps every exception address, handler and 'Call from' address found in the log."""
import sys, re
log = open(sys.argv[1], errors="replace").read()
mods = [(int(a, 16), n) for n, a in re.findall(r'Loaded L"([^"]+)" at ([0-9A-Fa-f]+)', log)]
def where(t):
    c = [m for m in mods if m[0] <= t]
    if not c: return "?"
    b = max(c, key=lambda m: m[0]); return "%s+0x%x" % (b[1].rsplit("\\\\", 1)[-1], t - b[0])
addrs = [int(a, 16) for a in sys.argv[2:]] or \
        [int(a, 16) for a in re.findall(r'dispatch_exception code=\w+ \([^)]*\) flags=\d+ addr=([0-9A-Fa-f]+)|Call from ([0-9A-Fa-f]+) to|Unhandled exception code \w+ flags \d+ addr 0x([0-9a-f]+)', log) for a in a if a]
for t in addrs: print(hex(t), where(t))
