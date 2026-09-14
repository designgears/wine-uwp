#!/usr/bin/env python
"""Emit COM interface and proxy/stub registrations from the Windows SOFTWARE hive for the wine-uwp prefix.

Every HKCR\\Interface\\{iid} with a ProxyStubClsid32 is copied; every proxy CLSID whose InprocServer32 DLL was
extracted gets an InprocServer32 pointing at the extracted copy (absolute Z: path), so Wine's combase can
marshal WinRT/COM interfaces through Microsoft's own NDR proxy DLLs.
Usage: gen-interfaces-reg.py <SOFTWARE hive> <extracted System32 dir> <out.reg>
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fasthive import Hive
hive, sysdir, outreg = sys.argv[1:4]
h = Hive(hive)
present = {f.lower(): f for f in os.listdir(sysdir)}
winpath = "Z:" + os.path.abspath(sysdir).replace("/", "\\")
keep_builtin = {"ole32.dll", "combase.dll", "oleaut32.dll", "rpcrt4.dll", "actxprxy.dll", "shell32.dll", "propsys.dll", "shcore.dll", "windows.storage.dll"}
def esc(s): return s.replace("\\", "\\\\").replace('"', '\\"')
lines = ["Windows Registry Editor Version 5.00", ""]
proxies = {}
n_if = 0
for k in h.subkeys(h.key(r"\Classes\Interface")):
    ps = None
    for sub in h.subkeys(k):
        if h.name(sub).lower() == "proxystubclsid32":
            ps = h.values(sub).get("(default)"); break
    if not isinstance(ps, str) or not ps: continue
    iid = h.name(k)
    name = h.values(k).get("(default)", "")
    lines.append(r"[HKEY_LOCAL_MACHINE\Software\Classes\Interface\%s]" % iid)
    if isinstance(name, str) and name: lines.append('@="%s"' % esc(name))
    lines.append(r"[HKEY_LOCAL_MACHINE\Software\Classes\Interface\%s\ProxyStubClsid32]" % iid)
    lines.append('@="%s"' % ps)
    lines.append("")
    proxies[ps.lower()] = ps
    n_if += 1
n_px = n_skip = 0
for c in h.subkeys(h.key(r"\Classes\CLSID")):
    cname = h.name(c)
    if cname.lower() not in proxies: continue
    vals = None
    for sub in h.subkeys(c):
        if h.name(sub).lower() == "inprocserver32": vals = h.values(sub); break
    dll = vals.get("(default)", "") if vals else ""
    if not isinstance(dll, str) or not dll: n_skip += 1; continue
    base = dll.replace("\\", "/").rsplit("/", 1)[-1].lower()
    if base in keep_builtin: continue
    if base not in present: n_skip += 1; continue
    lines.append(r"[HKEY_LOCAL_MACHINE\Software\Classes\CLSID\%s\InprocServer32]" % cname)
    target = r"C:\windows\system32\mrmcorer.dll" if base == "mrmcorer.dll" else winpath + "\\" + present[base]
    lines.append('@="%s"' % esc(target))
    lines.append('"ThreadingModel"="%s"' % vals.get("ThreadingModel", "Both"))
    lines.append("")
    n_px += 1
open(outreg, "w", encoding="utf-16").write("\r\n".join(lines))
print(f"{n_if} interfaces, {n_px} proxy DLL registrations, {n_skip} proxies skipped (DLL not extracted) -> {outreg}")
