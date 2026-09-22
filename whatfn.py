#!/usr/bin/env python
"""Name the function containing an address, from its own strings.

  whatfn.py PE 0x684d0d [0x8908ca ...]

For an app with no symbols, the fastest identification of a stack frame is the log
vocabulary the enclosing function references: find the function bounds in .pdata, then
resolve every rip-relative LEA in its body that lands in a read-only section and print
whatever string is there. Written for reading Solitaire stack dumps.
"""
import os, sys, bisect
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pefile

def funcs(pe):
    d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    raw = pe.get_data(d.VirtualAddress, d.Size)
    out = []
    for i in range(0, len(raw) - 11, 12):
        begin = int.from_bytes(raw[i:i+4], "little")
        end = int.from_bytes(raw[i+4:i+8], "little")
        if begin: out.append((begin, end))
    out.sort()
    return out

def string_at(img, off, limit=200):
    for enc, step in (("ascii", 1), ("utf-16-le", 2)):
        b = bytearray()
        i = off
        while i < len(img) and len(b) < limit * step:
            c = img[i:i+step]
            if c == b"\0" * step: break
            b += c
            i += step
        if len(b) >= 4 * step:
            try:
                s = b.decode(enc)
            except UnicodeDecodeError:
                continue
            if all(0x20 <= ord(ch) < 0x7f or ch in "\r\n\t" for ch in s):
                return s
    return None

def main():
    pe = pefile.PE(sys.argv[1])
    img = pe.get_memory_mapped_image()
    base = pe.OPTIONAL_HEADER.ImageBase
    fns = funcs(pe)
    starts = [f[0] for f in fns]

    for arg in sys.argv[2:]:
        a = int(arg, 16)
        if a > base: a -= base
        i = bisect.bisect_right(starts, a) - 1
        if i < 0 or a >= fns[i][1]:
            print("%#x: not in any .pdata function" % a)
            continue
        begin, end = fns[i]
        print("%#x: function %#x..%#x (+%#x)" % (a, begin, end, a - begin))
        seen = set()
        j = begin
        while j < end - 6:
            # 48 8d xx <rel32>  -- lea r64, [rip+rel32]
            if img[j] in (0x48, 0x4c) and img[j+1] == 0x8d and (img[j+2] & 0xc7) == 0x05:
                rel = int.from_bytes(img[j+3:j+7], "little", signed=True)
                tgt = j + 7 + rel
                if 0 <= tgt < len(img) and tgt not in seen:
                    seen.add(tgt)
                    s = string_at(img, tgt)
                    if s: print("    %#x  %r" % (tgt, s[:160]))
                j += 7
            else:
                j += 1

main()
