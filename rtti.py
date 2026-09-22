#!/usr/bin/env python
"""Find a C++ class's vtable and slots from its MSVC RTTI, by name substring.

  rtti.py PE 'FUE_OfflineContentUpsell'

Walks the type descriptors, finds the complete object locator that names each one and the vtable
pointer that precedes it, and prints the slots. For a stripped app this is how a virtual method
gets an address to probe.
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pefile

def main():
    pe = pefile.PE(sys.argv[1]); needle = sys.argv[2].encode()
    img = pe.get_memory_mapped_image(); data = bytes(img)
    base = pe.OPTIONAL_HEADER.ImageBase
    q = lambda r: struct.unpack_from('<Q', img, r)[0]
    u = lambda r: struct.unpack_from('<I', img, r)[0]

    tds, pos = [], 0
    while True:
        pos = data.find(b'.?A', pos + 1)
        if pos < 0: break
        end = data.find(b'\0', pos)
        if end < 0 or end - pos > 800: continue
        name = data[pos:end]
        if needle in name: tds.append((pos - 16, name.decode(errors='replace')))

    rdata = [s for s in pe.sections if s.Name.rstrip(b'\0') in (b'.rdata', b'.data')]
    for td, name in tds:
        print("%#x  %s" % (td, name))
        for s in rdata:
            lo, hi = s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize
            for r in range(lo, hi - 24, 4):
                if u(r + 12) == td and u(r) in (0, 1):
                    for p in range(lo, hi - 8, 8):
                        if q(p) - base == r:
                            vt = p + 8
                            slots = [q(vt + 8 * k) - base for k in range(12)]
                            print("    vtable %#x: %s" % (vt, " ".join("%d:%#x" % (i, v) for i, v in enumerate(slots) if 0 < v < 0xdc9000)))
                            break
                    break

main()
