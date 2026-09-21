#!/usr/bin/env python
"""Name things inside Microsoft binaries using Microsoft's public symbol server.

  pdbsyms.py DLL                      download DLL's PDB into ~/uwp-bin/pdb (cached)
  pdbsyms.py DLL --ordinals 122 134   name ordinal-only exports
  pdbsyms.py DLL --rva 0x6260 ...     name arbitrary RVAs (nearest public symbol below)
Requires: pefile, curl, llvm-pdbutil.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uwppaths
import sys, os, re, subprocess, bisect, pefile
CACHE = uwppaths.PDB
def pdb_for(dll):
    p = pefile.PE(dll, fast_load=True)
    p.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DEBUG']])
    for e in p.DIRECTORY_ENTRY_DEBUG:
        if e.struct.Type == 2 and hasattr(e.entry, 'Signature_String'):
            name = e.entry.PdbFileName.decode().strip("\0").replace("\\", "/").rsplit("/", 1)[-1]
            sig = e.entry.Signature_String.upper()
            os.makedirs(CACHE, exist_ok=True)
            out = os.path.join(CACHE, name)
            if not (os.path.exists(out) and os.path.getsize(out) > 0):
                url = f"https://msdl.microsoft.com/download/symbols/{name}/{sig}/{name}"
                subprocess.run(["curl", "-sSL", "-A", "Microsoft-Symbol-Server/10.0.0.0", "-o", out, url], check=True)
                if os.path.getsize(out) == 0: os.remove(out); raise SystemExit(f"no pdb on server for {name} {sig}")
            return out
    raise SystemExit("no CodeView record")
def publics(pdb):
    cache = pdb + ".syms"
    if os.path.exists(cache):
        return [(int(a, 16), n) for a, n in (l.rstrip("\n").split(" ", 1) for l in open(cache))]
    secs = subprocess.run(["llvm-pdbutil", "dump", "--section-headers", pdb], capture_output=True, text=True).stdout
    va = {int(m.group(1)): int(m.group(2), 16) for m in re.finditer(r'SECTION HEADER #(\d+)\s*\n.*?\n\s*[0-9A-Fa-f]+ virtual size\s*\n\s*([0-9A-Fa-f]+) virtual address', secs)}
    txt = subprocess.run(["llvm-pdbutil", "dump", "--publics", pdb], capture_output=True, text=True).stdout
    syms = []
    for m in re.finditer(r'S_PUB32 \[[^\]]*\] `([^`]+)`\s*\n\s*flags = [^,]*, addr = (\d+):(\d+)', txt):
        seg, off = int(m.group(2)), int(m.group(3))
        if seg in va: syms.append((va[seg] + off, m.group(1)))
    syms.sort()
    with open(cache, "w") as f:
        for a, n in syms: f.write("%x %s\n" % (a, n))
    return syms
def main():
    dll = sys.argv[1]; pdb = pdb_for(dll); syms = publics(pdb)
    addrs = [a for a, _ in syms]
    def name_at(rva):
        i = bisect.bisect_right(addrs, rva) - 1
        if i < 0: return "??"
        a, n = syms[i]; return n if a == rva else "%s+0x%x" % (n, rva - a)
    if "--ordinals" in sys.argv:
        p = pefile.PE(dll); exp = {e.ordinal: e.address for e in p.DIRECTORY_ENTRY_EXPORT.symbols}
        for o in sys.argv[sys.argv.index("--ordinals") + 1:]:
            o = int(o); print(o, hex(exp.get(o, 0)), name_at(exp[o]) if o in exp else "no such ordinal")
    elif "--rva" in sys.argv:
        for r in sys.argv[sys.argv.index("--rva") + 1:]: print(r, name_at(int(r, 16)))
    else: print(pdb, len(syms), "public symbols")
main()
