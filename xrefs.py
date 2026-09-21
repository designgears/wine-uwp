#!/usr/bin/env python
"""Who calls this RVA? Direct call/jmp sites in a PE, named through the cached PDB publics.

  xrefs.py DLL 0x37f80 [0x...]     list callers of each RVA, with the enclosing symbol

Complements pdbsyms.py (which names an address); this finds the callers of one. Only direct
rel32 call/jmp are found: virtual calls through a vtable will not show up here, so an empty
result means "nobody calls it directly", not "nobody calls it".
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uwppaths
import sys, os, re, bisect, subprocess, pefile

CACHE = uwppaths.PDB

def publics(dll):
    name = os.path.basename(dll)
    for cand in (name.replace(".dll", ".pdb") + ".syms", name + ".pdb.syms"):
        path = os.path.join(CACHE, cand)
        if os.path.exists(path):
            syms = sorted((int(a, 16), n) for a, n in
                          (l.rstrip("\n").split(" ", 1) for l in open(path)))
            return syms
    raise SystemExit("no cached publics for %s; run pdbsyms.py %s first" % (name, dll))

def main():
    dll = sys.argv[1]
    targets = {int(a, 16) for a in sys.argv[2:]}
    if not targets: raise SystemExit(__doc__)
    pe = pefile.PE(dll)
    syms = publics(dll)
    addrs = [a for a, _ in syms]
    def enclosing(rva):
        i = bisect.bisect_right(addrs, rva) - 1
        if i < 0: return "??"
        a, n = syms[i]
        return "%s+0x%x" % (n, rva - a) if a != rva else n
    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    base, data = text.VirtualAddress, text.get_data()
    hits = []
    for i in range(len(data) - 5):
        op = data[i]
        if op != 0xe8 and op != 0xe9: continue          # call rel32 / jmp rel32
        rel = int.from_bytes(data[i + 1:i + 5], "little", signed=True)
        site = base + i
        if site + 5 + rel in targets:
            hits.append((site, site + 5 + rel))
    names = subprocess.run(["llvm-cxxfilt"], text=True, capture_output=True,
                           input="\n".join(enclosing(s).split("+")[0] for s, _ in hits)).stdout.splitlines()
    for (site, target), pretty in zip(hits, names):
        off = enclosing(site)
        print("%#x -> %#x  %s%s" % (site, target, pretty,
                                    "+" + off.split("+")[1] if "+" in off else ""))
    if not hits: print("no direct callers")

main()
