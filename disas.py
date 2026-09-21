#!/usr/bin/env python
"""Disassemble one function out of a PE, with call targets named from the cached PDB publics.

  disas.py DLL 0x37600 [bytes]     default length 0x400, stops at the first ret-padding run

Reading a private method's actual code is usually the only way to learn its parameter shape or
which failure path it takes; pdbsyms.py names addresses, xrefs.py finds callers, this shows why.
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
            return sorted((int(a, 16), n) for a, n in
                          (l.rstrip("\n").split(" ", 1) for l in open(path)))
    raise SystemExit("no cached publics for %s; run pdbsyms.py %s first" % (name, dll))

def main():
    dll, rva = sys.argv[1], int(sys.argv[2], 16)
    length = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x400
    pe = pefile.PE(dll)
    syms = publics(dll)
    addrs = [a for a, _ in syms]
    def name(a):
        i = bisect.bisect_right(addrs, a) - 1
        if i < 0: return None
        at, n = syms[i]
        return n if at == a else "%s+0x%x" % (n, a - at)
    raw = pe.get_data(rva, length)
    tmp = "/tmp/disas-%x.bin" % rva
    open(tmp, "wb").write(raw)
    out = subprocess.run(["objdump", "-D", "-b", "binary", "-m", "i386:x86-64", "-M", "intel",
                          "--adjust-vma=%#x" % rva, tmp], capture_output=True, text=True).stdout
    want = set()
    lines = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\t[0-9a-f ]+\t(\w+)\s+(0x[0-9a-f]+)", line)
        if m and m.group(2) in ("call", "jmp"):
            want.add(int(m.group(3), 16))
        lines.append(line)
    pretty = {}
    if want:
        ordered = sorted(want)
        mangled = [name(a) or "?" for a in ordered]
        dem = subprocess.run(["llvm-cxxfilt"], text=True, capture_output=True,
                             input="\n".join(m.split("+")[0] for m in mangled)).stdout.splitlines()
        for a, m, d in zip(ordered, mangled, dem):
            pretty[a] = d + ("+" + m.split("+")[1] if "+" in m else "")
    for line in lines:
        m = re.search(r"(call|jmp)\s+(0x[0-9a-f]+)", line)
        if m and int(m.group(2), 16) in pretty:
            line += "   ; " + pretty[int(m.group(2), 16)]
        print(line)

main()
