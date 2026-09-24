#!/usr/bin/env python
"""Emit classic in-process COM class registrations (HKCR\\CLSID) from the Windows SOFTWARE hive.

gen-classes-reg.py covers WinRT activatable classes and gen-interfaces-reg.py covers proxy/stub
CLSIDs; this covers everything else the UWP stack CoCreates by CLSID. Only classes whose
InprocServer32 DLL was extracted are written, and a DLL that also lives in the prefix is registered
by its prefix path so the process never maps two copies (Findings-M4.md). DLLs Wine implements
itself keep their builtin registration.
Usage: gen-clsid-reg.py <SOFTWARE hive> <extracted System32 dir> <out.reg>
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uwppaths
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fasthive import Hive

hive, sysdir, outreg = sys.argv[1:4]
h = Hive(hive)
present = {f.lower(): f for f in os.listdir(sysdir)}
prefix_sys32 = uwppaths.SYSTEM32
in_prefix = {f.lower() for f in os.listdir(prefix_sys32)} if os.path.isdir(prefix_sys32) else set()
winpath = "Z:" + os.path.abspath(sysdir).replace("/", "\\")
keep_builtin = {"ole32.dll", "combase.dll", "oleaut32.dll", "rpcrt4.dll", "actxprxy.dll", "shell32.dll",
                "propsys.dll", "shcore.dll", "windows.storage.dll", "urlmon.dll", "user32.dll",
                "d2d1.dll", "d3d11.dll", "dxgi.dll", "dwrite.dll", "mf.dll", "mfplat.dll", "msxml6.dll",
                "iertutil.dll", "windowscodecs.dll", "uiautomationcore.dll"}

def esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')

lines = ["Windows Registry Editor Version 5.00", ""]
nreg = nskip = 0
for c in h.subkeys(h.key(r"\Classes\CLSID")):
    vals = None
    for sub in h.subkeys(c):
        if h.name(sub).lower() == "inprocserver32":
            vals = h.values(sub); break
    dll = vals.get("(default)", "") if vals else ""
    if not isinstance(dll, str) or not dll:
        continue
    base = dll.replace("\\", "/").rsplit("/", 1)[-1].lower()
    if base in keep_builtin or base not in present:
        nskip += 1; continue
    name = h.values(c).get("(default)", "")
    lines.append(r"[HKEY_LOCAL_MACHINE\Software\Classes\CLSID\%s]" % h.name(c))
    if isinstance(name, str) and name:
        lines.append('@="%s"' % esc(name))
    lines.append(r"[HKEY_LOCAL_MACHINE\Software\Classes\CLSID\%s\InprocServer32]" % h.name(c))
    target = r"C:\windows\system32\%s" % base if base in in_prefix else winpath + "\\" + present[base]
    lines.append('@="%s"' % esc(target))
    lines.append('"ThreadingModel"="%s"' % vals.get("ThreadingModel", "Both"))
    lines.append("")
    nreg += 1

open(outreg, "w", encoding="utf-16").write("\r\n".join(lines))
print(f"registered {nreg} in-process CLSIDs, skipped {nskip} (builtin or DLL not extracted) -> {outreg}")
