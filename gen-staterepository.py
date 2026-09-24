#!/usr/bin/env python
"""Emit the slot table the Windows.Internal.StateRepository stand-in uses.

The shim cannot guess which argument of a slot is the out-parameter - that is exactly the mistake
that corrupts typed out-parameters (see Findings-M4.md, the composition default slot). It does not
have to: Windows.StateRepository.dll's WRL server vtables give the method order, its public PDB
gives the mangled names, and llvm-undname turns those into prototypes. In every interface in this
family the out-parameter is the *last* parameter, so knowing the arity is enough to write to the
right place - and the last parameter's type says what to write.

  gen-staterepository.py <Windows.StateRepository.dll> <pdb.syms> <out.h> [Class ...]
"""
import bisect, re, struct, subprocess, sys

dll, symfile, outfile = sys.argv[1:4]
classes = sys.argv[4:] or ["User", "Application", "MrtApplication"]

f = open(dll, "rb").read()
e = struct.unpack_from("<I", f, 0x3c)[0]; coff = e + 4
nsec, = struct.unpack_from("<H", f, coff + 2)
optsz, = struct.unpack_from("<H", f, coff + 16)
opt = coff + 20
base, = struct.unpack_from("<Q", f, opt + 24)
secs = []
for i in range(nsec):
    o = opt + optsz + i * 40
    vsz, va, rawsz, raw = struct.unpack_from("<IIII", f, o + 8)
    secs.append((va, max(vsz, rawsz), raw))
def r2o(rva):
    for va, sz, raw in secs:
        if va <= rva < va + sz: return raw + (rva - va)
    return None

syms = []
for line in open(symfile):
    a, _, n = line.partition(" ")
    try: syms.append((int(a, 16), n.strip()))
    except ValueError: pass
syms.sort()
keys = [a for a, _ in syms]
exact = {}
for a, n in syms: exact.setdefault(n, a)
def sym_at(rva):
    i = bisect.bisect_right(keys, rva) - 1
    return syms[i][1] if i >= 0 else ""

_dm_cache = {}
def demangle(names):
    """One prototype per input name, order preserved.

    llvm-undname prints three lines per symbol (the echo, the result, a blank) and only two for
    anything it cannot demangle, so batching and pairing the lines up silently shifts prototypes
    onto the wrong slots - which is how an arity ends up wrong and the shim writes through the
    wrong argument. One name per call is slower and right."""
    out = []
    for name in names:
        if name not in _dm_cache:
            r = subprocess.run(["llvm-undname", name], capture_output=True, text=True).stdout
            lines = [l for l in r.splitlines() if l.strip() and l != name]
            _dm_cache[name] = lines[0] if lines else ""
        out.append(_dm_cache[name])
    return out

def split_params(proto):
    """The parameter list of a demangled prototype, split at depth 0."""
    i = proto.rindex("(")
    inner = proto[i + 1:proto.rindex(")")]
    if not inner.strip() or inner.strip() == "void": return []
    out, depth, cur = [], 0, ""
    for ch in inner:
        if ch in "<(": depth += 1
        elif ch in ">)": depth -= 1
        if ch == "," and not depth:
            out.append(cur.strip()); cur = ""
        else: cur += ch
    out.append(cur.strip())
    return out

def element_of(param):
    """For an interface or collection out-parameter, the StateRepository class it yields."""
    m = re.search(r"IVectorView<[^>]*StateRepository::(\w+)\s*\*", param)
    if m: return m.group(1)
    m = re.search(r"StateRepository::I(\w+)\s*\*\*", param)
    if m: return m.group(1)
    return ""

def kind_of(param):
    """What to write through the final out-parameter."""
    p = param.replace(" ", "")
    if p.endswith("**"):
        if "HSTRING__" in p: return "SR_OUT_HSTRING"
        if "IVectorView" in p: return "SR_OUT_COLLECTION"
        return "SR_OUT_IFACE"
    if p.endswith("*"):
        if re.search(r"(__int64|unsigned__int64)\*$", p): return "SR_OUT_ZERO8"
        if p.endswith("char*") or p.endswith("bool*"): return "SR_OUT_ZERO1"
        return "SR_OUT_ZERO4"
    return "SR_OUT_NONE"

# Vtables sit consecutively in .rdata, so the next ??_7 symbol bounds this one. Walking until a
# slot "looks like" the end does not work: identical-code folding makes a slot resolve to a
# same-shaped method of an unrelated class, and that truncated three of the four tables.
vtable_addrs = sorted(a for a, n in syms if n.startswith("??_7"))

def vtable_slots(rva, limit=96):
    o = r2o(rva)
    i = bisect.bisect_right(vtable_addrs, rva)
    end = vtable_addrs[i] if i < len(vtable_addrs) else rva + limit * 8
    names = []
    for i in range((min(end - rva, limit * 8)) // 8):
        v, = struct.unpack_from("<Q", f, o + i * 8)
        if not v: break
        t = v - base
        if t < 0 or t > 0x800000: break
        names.append(sym_at(t))
    return names

rows = []
for cls in classes:
    primary = f"??_7{cls}Server@StateRepository@Internal@Windows@@6B@"
    if primary not in exact:
        # Classes with several row interfaces (PackageUser) have one vtable per interface,
        # named by the interface; the one named I<cls> is the base row interface.
        primary = f"??_7{cls}Server@StateRepository@Internal@Windows@@6BI{cls}@123@@"
    targets = [(primary, 0)]
    want = f"I{cls}Statics@StateRepository@Internal@Windows"
    # Several selector vtables mention I<cls>Statics in their template arguments; the one that *is*
    # the statics table is the one that does not start with the FtmBase selector (that is IMarshal).
    for name, addr in exact.items():
        if not name.startswith(f"??_7{cls}FactoryServer@StateRepository@Internal@Windows@@6B"): continue
        if "6B?$Selector@VFtmBase" in name: continue
        if want in name:
            targets.append((name, 1)); break
    for tag, statics in targets:
        if tag not in exact:
            print("missing vtable:", tag, file=sys.stderr); continue
        names = vtable_slots(exact[tag])
        protos = demangle(names[6:])
        for i, (mangled, proto) in enumerate(zip(names[6:], protos), start=6):
            if not proto or "(" not in proto:
                rows.append((cls, statics, i, 0, "SR_OUT_NONE", "?", "")); continue
            params = split_params(proto)
            kind = kind_of(params[-1]) if params else "SR_OUT_NONE"
            elem = element_of(params[-1]) if params else ""
            method = mangled[1:].split("@")[0] if mangled.startswith("?") else "?"
            rows.append((cls, statics, i, len(params), kind, method, elem))

with open(outfile, "w") as h:
    h.write("/* Generated by ./gen-staterepository.py - do not edit.\n"
            " * Slot -> arity and out-parameter kind, read from Windows.StateRepository.dll's server\n"
            " * vtables and its public PDB. The out-parameter is always the last one. */\n")
    h.write("struct sr_slot { const WCHAR *cls; unsigned char statics, slot, argc, kind;\n"
            "                 const WCHAR *element; const char *name; };\n")
    h.write("static const struct sr_slot sr_slots[] =\n{\n")
    for cls, statics, slot, argc, kind, method, elem in rows:
        e = ('L"Windows.Internal.StateRepository.%s"' % elem) if elem else "nullptr"
        h.write('    { L"Windows.Internal.StateRepository.%s", %d, %d, %d, %s, %s, "%s" },\n'
                % (cls, statics, slot, argc, kind, e, method))
    h.write("};\n")
print("%d slots across %d classes -> %s" % (len(rows), len(classes), outfile))
