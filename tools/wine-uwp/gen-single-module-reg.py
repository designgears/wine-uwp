#!/usr/bin/env python
"""Pin every activatable class to the one copy of its DLL that lives in the prefix.

A DLL we install into the prefix system32 can end up loaded twice in one process: once by bare name
(XAML's resource-only loads, delay-loads, DllOverrides) and once through an absolute extraction path
coming from the generated ActivatableClassId map. The two copies have separate data segments and
separate lifetimes, so an object built by one can outlive the other's unload and leave a dangling
vtable. Findings-M4.md has the MrmCoreR case; Windows.UI.Xaml.Controls.dll hit the same thing.

Only classes whose prefix copy is byte-identical to the extracted copy are pinned. For every other
name, C:\\windows\\system32 holds Wine's own builtin, which is a different implementation, so
redirecting there would silently change which DLL serves the class.

  gen-single-module-reg.py <classes.reg> <extracted System32 dir> <prefix system32 dir> <out.reg>
"""
import sys, os, re, hashlib

src, extracted, sys32, out = sys.argv[1:5]

def digest(path):
    try:
        with open(path, "rb") as f:
            return hashlib.blake2b(f.read(), digest_size=16).digest()
    except OSError:
        return None

ext_by_lower = {f.lower(): f for f in os.listdir(extracted) if f.lower().endswith(".dll")}
same = set()
for lower, real in ext_by_lower.items():
    if os.path.exists(os.path.join(sys32, lower)):
        d = digest(os.path.join(sys32, lower))
        if d and d == digest(os.path.join(extracted, real)):
            same.add(lower)

text = open(src, encoding="utf-16").read()
lines = ["Windows Registry Editor Version 5.00", ""]
key, npin, pinned = None, 0, set()
for line in text.splitlines():
    if line.startswith("["):
        key = line
        continue
    m = re.match(r'"DllPath"="(.+)"$', line)
    if not m or not key:
        continue
    path = m.group(1).replace("\\\\", "\\")
    base = path.replace("/", "\\").rsplit("\\", 1)[-1].lower()
    if base not in same or path.lower().startswith("c:\\windows\\system32\\"):
        continue
    lines += [key, '"DllPath"="C:\\\\windows\\\\system32\\\\%s"' % base, ""]
    pinned.add(base)
    npin += 1

open(out, "w", encoding="utf-16").write("\r\n".join(lines))
print("pinned %d classes across %d modules -> %s" % (npin, len(pinned), out))
print("modules:", " ".join(sorted(pinned)))
