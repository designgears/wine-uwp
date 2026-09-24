#!/usr/bin/env python
"""Minimal, fast reader for Windows registry hive files (regf). Enough for enumerating keys and values.
Used by gen-*-reg.py because full-featured hive libraries take a quarter hour on HKCR\\Interface."""
import struct
class Hive:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        assert self.d[:4] == b"regf", "not a hive"
        self.root = self.nk(struct.unpack_from("<I", self.d, 0x24)[0])
    def cell(self, off):
        return 0x1000 + off
    def nk(self, off):
        p = self.cell(off) + 4
        assert self.d[p:p+2] == b"nk", (hex(off), self.d[p:p+2])
        return p
    def name(self, p):
        flags = struct.unpack_from("<H", self.d, p + 2)[0]
        nlen = struct.unpack_from("<H", self.d, p + 0x48)[0]
        raw = self.d[p + 0x4c:p + 0x4c + nlen]
        return raw.decode("latin-1") if flags & 0x20 else raw.decode("utf-16le", "replace")
    def subkeys(self, p):
        count = struct.unpack_from("<I", self.d, p + 0x14)[0]
        lst = struct.unpack_from("<I", self.d, p + 0x1c)[0]
        if not count or lst == 0xffffffff: return
        yield from self._list(lst)
    def _list(self, off):
        q = self.cell(off) + 4
        sig = self.d[q:q+2]
        n = struct.unpack_from("<H", self.d, q + 2)[0]
        if sig in (b"lf", b"lh"):
            for i in range(n): yield self.nk(struct.unpack_from("<I", self.d, q + 4 + 8 * i)[0])
        elif sig == b"li":
            for i in range(n): yield self.nk(struct.unpack_from("<I", self.d, q + 4 + 4 * i)[0])
        elif sig == b"ri":
            for i in range(n): yield from self._list(struct.unpack_from("<I", self.d, q + 4 + 4 * i)[0])
        else: raise ValueError("bad subkey list %r" % sig)
    def values(self, p):
        count = struct.unpack_from("<I", self.d, p + 0x24)[0]
        lst = struct.unpack_from("<I", self.d, p + 0x28)[0]
        if not count or lst == 0xffffffff: return {}
        q = self.cell(lst) + 4
        out = {}
        for i in range(count):
            v = self.cell(struct.unpack_from("<I", self.d, q + 4 * i)[0]) + 4
            if self.d[v:v+2] != b"vk": continue
            nlen, dlen, doff, typ, flags = struct.unpack_from("<HIIIH", self.d, v + 2)
            name = self.d[v + 0x14:v + 0x14 + nlen].decode("latin-1" if flags & 1 else "utf-16le", "replace") or "(default)"
            if dlen & 0x80000000:
                data = struct.pack("<I", doff)[:dlen & 0x7fffffff]
            else:
                s = self.cell(doff) + 4
                data = self.d[s:s + dlen]
            if typ in (1, 2):   # REG_SZ, REG_EXPAND_SZ
                out[name] = data.decode("utf-16le", "replace").split("\0")[0]
            elif typ == 4 and len(data) >= 4:
                out[name] = struct.unpack_from("<I", data)[0]
            elif typ == 7:
                out[name] = [x for x in data.decode("utf-16le", "replace").split("\0") if x]
            else:
                out[name] = data
        return out
    def key(self, path):
        p = self.root
        for part in [x for x in path.strip("\\").split("\\") if x]:
            lp = part.lower()
            for s in self.subkeys(p):
                if self.name(s).lower() == lp: p = s; break
            else: raise KeyError(path)
        return p
if __name__ == "__main__":
    import sys, time
    t = time.time(); h = Hive(sys.argv[1]); k = h.key(sys.argv[2] if len(sys.argv) > 2 else "\\")
    n = sum(1 for _ in h.subkeys(k)); print(h.name(k), n, "subkeys", h.values(k), "%.1fs" % (time.time() - t))
