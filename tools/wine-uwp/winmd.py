#!/usr/bin/env python
"""Name a WinRT interface, and list its methods in vtable order, from a .winmd.

  winmd.py FILE.winmd --guid 845bf0b4-d8de-462f-8753-c80d43c6ff5a
  winmd.py FILE.winmd --type Windows.UI.Composition.ICompositionAnimation2
  winmd.py FILE.winmd --grep Animation

Private interfaces are not in the public metadata; everything else is, and knowing the method order
is what turns a dcomp slot number into a method name. Minimal ECMA-335 reader: no dependencies.
"""
import struct, sys, uuid

TDR = ("TypeDef", "TypeRef", "TypeSpec")
CODED = {
    "TypeDefOrRef": (2, (2, 1, 0x1b)),
    "HasConstant": (2, (4, 8, 0x17)),
    "HasCustomAttribute": (5, (6, 4, 1, 2, 8, 9, 0x0a, 0x00, 0x0e, 0x17, 0x14, 0x11, 0x1a, 0x1b,
                               0x20, 0x23, 0x26, 0x27, 0x28, 0x2a, 0x2c, 0x2b)),
    "HasFieldMarshal": (1, (4, 8)),
    "HasDeclSecurity": (2, (2, 6, 0x20)),
    "MemberRefParent": (3, (2, 1, 0x1a, 6, 0x1b)),
    "HasSemantics": (1, (0x14, 0x17)),
    "MethodDefOrRef": (1, (6, 0x0a)),
    "MemberForwarded": (1, (4, 6)),
    "Implementation": (2, (0x26, 0x23, 0x27)),
    "CustomAttributeType": (3, (0xff, 0xff, 6, 0x0a, 0xff)),
    "ResolutionScope": (2, (0, 0x1a, 0x23, 1)),
    "TypeOrMethodDef": (1, (2, 6)),
}
SCHEMA = {
    0x00: (("Generation", "u2"), ("Name", "str"), ("Mvid", "guid"), ("EncId", "guid"), ("EncBaseId", "guid")),
    0x01: (("ResolutionScope", "ResolutionScope"), ("Name", "str"), ("Namespace", "str")),
    0x02: (("Flags", "u4"), ("Name", "str"), ("Namespace", "str"), ("Extends", "TypeDefOrRef"),
           ("FieldList", "#4"), ("MethodList", "#6")),
    0x03: (("Field", "#4"),),
    0x04: (("Flags", "u2"), ("Name", "str"), ("Signature", "blob")),
    0x05: (("Method", "#6"),),
    0x06: (("RVA", "u4"), ("ImplFlags", "u2"), ("Flags", "u2"), ("Name", "str"), ("Signature", "blob"), ("ParamList", "#8")),
    0x07: (("Param", "#8"),),
    0x08: (("Flags", "u2"), ("Sequence", "u2"), ("Name", "str")),
    0x09: (("Class", "#2"), ("Interface", "TypeDefOrRef")),
    0x0a: (("Class", "MemberRefParent"), ("Name", "str"), ("Signature", "blob")),
    0x0b: (("Type", "u1"), ("Pad", "u1"), ("Parent", "HasConstant"), ("Value", "blob")),
    0x0c: (("Parent", "HasCustomAttribute"), ("Type", "CustomAttributeType"), ("Value", "blob")),
    0x0d: (("Parent", "HasFieldMarshal"), ("NativeType", "blob")),
    0x0e: (("Action", "u2"), ("Parent", "HasDeclSecurity"), ("PermissionSet", "blob")),
    0x0f: (("PackingSize", "u2"), ("ClassSize", "u4"), ("Parent", "#2")),
    0x10: (("Offset", "u4"), ("Field", "#4")),
    0x11: (("Signature", "blob"),),
    0x12: (("Parent", "#2"), ("EventList", "#20")),
    0x13: (("Event", "#20"),),
    0x14: (("EventFlags", "u2"), ("Name", "str"), ("EventType", "TypeDefOrRef")),
    0x15: (("Parent", "#2"), ("PropertyList", "#23")),
    0x16: (("Property", "#23"),),
    0x17: (("Flags", "u2"), ("Name", "str"), ("Type", "blob")),
    0x18: (("Semantics", "u2"), ("Method", "#6"), ("Association", "HasSemantics")),
    0x19: (("Class", "#2"), ("MethodBody", "MethodDefOrRef"), ("MethodDeclaration", "MethodDefOrRef")),
    0x1a: (("Name", "str"),),
    0x1b: (("Signature", "blob"),),
    0x1c: (("MappingFlags", "u2"), ("MemberForwarded", "MemberForwarded"), ("ImportName", "str"), ("ImportScope", "#26")),
    0x1d: (("RVA", "u4"), ("Field", "#4")),
    0x20: (("HashAlgId", "u4"), ("Major", "u2"), ("Minor", "u2"), ("Build", "u2"), ("Rev", "u2"),
           ("Flags", "u4"), ("PublicKey", "blob"), ("Name", "str"), ("Culture", "str")),
    0x21: (("Processor", "u4"),),
    0x22: (("Platform", "u4"), ("Major", "u4"), ("Minor", "u4")),
    0x23: (("Major", "u2"), ("Minor", "u2"), ("Build", "u2"), ("Rev", "u2"), ("Flags", "u4"),
           ("PublicKeyOrToken", "blob"), ("Name", "str"), ("Culture", "str"), ("HashValue", "blob")),
    0x24: (("Processor", "u4"), ("AssemblyRef", "#35")),
    0x25: (("Platform", "u4"), ("Major", "u4"), ("Minor", "u4"), ("AssemblyRef", "#35")),
    0x26: (("Flags", "u4"), ("Name", "str"), ("HashValue", "blob")),
    0x27: (("Flags", "u4"), ("TypeDefId", "u4"), ("TypeName", "str"), ("TypeNamespace", "str"), ("Implementation", "Implementation")),
    0x28: (("Offset", "u4"), ("Flags", "u4"), ("Name", "str"), ("Implementation", "Implementation")),
    0x29: (("NestedClass", "#2"), ("EnclosingClass", "#2")),
    0x2a: (("Number", "u2"), ("Flags", "u2"), ("Owner", "TypeOrMethodDef"), ("Name", "str")),
    0x2b: (("Method", "MethodDefOrRef"), ("Instantiation", "blob")),
    0x2c: (("Owner", "#42"), ("Constraint", "TypeDefOrRef")),
}

class WinMD:
    def __init__(self, path):
        d = open(path, "rb").read()
        self.d = d
        e = struct.unpack_from("<I", d, 0x3c)[0]
        coff = e + 4
        nsec, = struct.unpack_from("<H", d, coff + 2)
        optsz, = struct.unpack_from("<H", d, coff + 16)
        opt = coff + 20
        magic, = struct.unpack_from("<H", d, opt)
        dd = opt + (112 if magic == 0x20b else 96)
        self.secs = []
        for i in range(nsec):
            o = opt + optsz + i * 40
            vsz, va, rawsz, raw = struct.unpack_from("<IIII", d, o + 8)
            self.secs.append((va, max(vsz, rawsz), raw))
        cli_rva, _ = struct.unpack_from("<II", d, dd + 14 * 8)
        meta_rva, _ = struct.unpack_from("<II", d, self.off(cli_rva) + 8)
        m = self.off(meta_rva)
        assert d[m:m + 4] == b"BSJB"
        vlen, = struct.unpack_from("<I", d, m + 12)
        p = m + 16 + vlen + 4
        nstreams, = struct.unpack_from("<H", d, p - 2)
        self.streams = {}
        for _ in range(nstreams):
            off, size = struct.unpack_from("<II", d, p)
            p += 8
            name = d[p:d.index(b"\0", p)].decode()
            p += (len(name) + 4) & ~3
            self.streams[name] = (m + off, size)
        self._tables()

    def off(self, rva):
        for va, sz, raw in self.secs:
            if va <= rva < va + sz:
                return raw + (rva - va)
        raise KeyError(hex(rva))

    def _tables(self):
        base, _ = self.streams.get("#~") or self.streams["#-"]
        heapsizes = self.d[base + 6]
        self.wstr = 4 if heapsizes & 1 else 2
        self.wguid = 4 if heapsizes & 2 else 2
        self.wblob = 4 if heapsizes & 4 else 2
        valid, = struct.unpack_from("<Q", self.d, base + 8)
        p = base + 24
        self.rows = {}
        for t in range(64):
            if valid >> t & 1:
                self.rows[t], = struct.unpack_from("<I", self.d, p)
                p += 4
        self.tbase = {}
        for t in sorted(self.rows):
            self.tbase[t] = p
            p += self.rows[t] * self.rowsize(t)

    def isize(self, kind):
        if kind in ("u1",): return 1
        if kind in ("u2",): return 2
        if kind in ("u4",): return 4
        if kind == "str": return self.wstr
        if kind == "guid": return self.wguid
        if kind == "blob": return self.wblob
        if kind.startswith("#"):
            return 4 if self.rows.get(int(kind[1:]), 0) >= 0x10000 else 2
        bits, tabs = CODED[kind]
        big = max((self.rows.get(t, 0) for t in tabs if t != 0xff), default=0)
        return 4 if big >= (1 << (16 - bits)) else 2

    def rowsize(self, t):
        return sum(self.isize(k) for _, k in SCHEMA[t])

    def row(self, t, i):
        """1-based row index -> dict of column values (heap indexes left raw)."""
        p = self.tbase[t] + (i - 1) * self.rowsize(t)
        out = {}
        for name, kind in SCHEMA[t]:
            n = self.isize(kind)
            v = int.from_bytes(self.d[p:p + n], "little")
            out[name] = v
            p += n
        return out

    def string(self, i):
        b, _ = self.streams["#Strings"]
        return self.d[b + i:self.d.index(b"\0", b + i)].decode("utf-8", "replace")

    def blob(self, i):
        b, _ = self.streams["#Blob"]
        p = b + i
        n = self.d[p]
        if n & 0x80 == 0:
            p += 1
        elif n & 0xc0 == 0x80:
            n = ((n & 0x3f) << 8) | self.d[p + 1]; p += 2
        else:
            n = ((n & 0x1f) << 24) | (self.d[p + 1] << 16) | (self.d[p + 2] << 8) | self.d[p + 3]; p += 4
        return self.d[p:p + n]

    def type_name(self, i):
        r = self.row(2, i)
        ns, nm = self.string(r["Namespace"]), self.string(r["Name"])
        return f"{ns}.{nm}" if ns else nm

    def methods(self, i):
        """Method names of TypeDef i, in metadata (= vtable) order."""
        first = self.row(2, i)["MethodList"]
        last = self.row(2, i + 1)["MethodList"] if i < self.rows[2] else self.rows[6] + 1
        return [self.string(self.row(6, m)["Name"]) for m in range(first, last)]

    def attr_name(self, coded):
        """CustomAttributeType coded index -> the attribute class's name."""
        bits, tabs = CODED["CustomAttributeType"]
        t, i = tabs[coded & ((1 << bits) - 1)], coded >> bits
        if t == 0x0a:            # MemberRef: the .ctor, whose Class names the attribute
            cls = self.row(0x0a, i)["Class"]
            cb, ctabs = CODED["MemberRefParent"]
            ct, ci = ctabs[cls & ((1 << cb) - 1)], cls >> cb
            if ct == 1: return self.string(self.row(1, ci)["Name"])
            if ct == 2: return self.string(self.row(2, ci)["Name"])
        elif t == 6:
            return self.string(self.row(6, i)["Name"])
        return ""

    def guids(self):
        """GUID -> TypeDef row, from each type's GuidAttribute."""
        out = {}
        bits, tabs = CODED["HasCustomAttribute"]
        for i in range(1, self.rows.get(0x0c, 0) + 1):
            r = self.row(0x0c, i)
            if tabs[r["Parent"] & ((1 << bits) - 1)] != 2:
                continue
            if self.attr_name(r["Type"]) != "GuidAttribute":
                continue
            v = self.blob(r["Value"])
            if len(v) < 18 or v[0:2] != b"\x01\x00":
                continue
            out[str(uuid.UUID(bytes_le=v[2:18]))] = r["Parent"] >> bits
        return out

def main():
    md = WinMD(sys.argv[1])
    a = sys.argv[2:]
    if "--guid" in a:
        g = a[a.index("--guid") + 1].lower().strip("{}")
        row = md.guids().get(g)
        if not row:
            print("not in this winmd"); return
        print(md.type_name(row))
        for n, name in enumerate(md.methods(row)):
            print("  slot %2d  %s" % (n + 6, name))
    elif "--type" in a:
        want = a[a.index("--type") + 1].lower()
        rg = {v: k for k, v in md.guids().items()}
        for i in range(1, md.rows[2] + 1):
            if md.type_name(i).lower() == want:
                print(md.type_name(i), rg.get(i, ""))
                for n, name in enumerate(md.methods(i)):
                    print("  slot %2d  %s" % (n + 6, name))
    elif "--grep" in a:
        pat = a[a.index("--grep") + 1].lower()
        rg = {v: k for k, v in md.guids().items()}
        for i in range(1, md.rows[2] + 1):
            t = md.type_name(i)
            if pat in t.lower():
                print(t, rg.get(i, ""))
    else:
        print(__doc__)

main()
