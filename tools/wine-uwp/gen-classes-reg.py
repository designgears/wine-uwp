#!/usr/bin/env python
"""Turn the Windows SOFTWARE hive's WindowsRuntime\\ActivatableClassId map into a .reg for the wine-uwp prefix.

Every in-process class whose DLL we extracted gets registered with an absolute Z: path to the extracted
System32 copy. Server-activated (out-of-proc) classes are skipped: there is no RuntimeBroker in Wine.
Usage: gen-classes-reg.py <SOFTWARE hive> <extracted System32 dir> <out.reg> [--json out.json]
"""
import sys, os, json
from regipy.registry import RegistryHive
hive, sysdir, outreg = sys.argv[1:4]
jsonout = sys.argv[5] if len(sys.argv) > 5 and sys.argv[4] == "--json" else None
h = RegistryHive(hive)
k = h.get_key(r"\Microsoft\WindowsRuntime\ActivatableClassId")
present = {f.lower(): f for f in os.listdir(sysdir)}
prefix_sys32 = os.environ.get("WINE_UWP_SYSTEM32", os.path.expanduser("~/.wine-uwp/drive_c/windows/system32"))
in_prefix = {f.lower() for f in os.listdir(prefix_sys32)} if os.path.isdir(prefix_sys32) else set()
winpath = "Z:" + os.path.abspath(sysdir).replace("/", "\\")
lines = ["Windows Registry Editor Version 5.00", ""]
allmap, nreg, nskip_server, nskip_missing = {}, 0, 0, 0
def clean(v):
    return v.hex() if isinstance(v, bytes) else v
for s in k.iter_subkeys():
    d = {v.name: clean(v.value) for v in s.get_values()}
    allmap[s.name] = d
    dll = d.get("DllPath")
    if not dll:
        nskip_server += 1; continue
    base = dll.replace("\\", "/").rsplit("/", 1)[-1].lower()
    if base not in present:
        nskip_missing += 1; continue
    lines.append(r"[HKEY_LOCAL_MACHINE\Software\Microsoft\WindowsRuntime\ActivatableClassId\%s]" % s.name)
    # A DLL that also lives in the prefix must be registered by its prefix path, or the process
    # loads it twice and the two copies get separate data and lifetimes (Findings-M4.md).
    target = r"C:\windows\system32\%s" % base if base in in_prefix else winpath + "\\" + present[base]
    lines.append('"DllPath"="%s"' % target.replace("\\", "\\\\"))
    lines.append('"ActivationType"=dword:%08x' % int(d.get("ActivationType", 0)))
    lines.append('"Threading"=dword:%08x' % int(d.get("Threading", 0)))
    lines.append('"TrustLevel"=dword:%08x' % int(d.get("TrustLevel", 0)))
    lines.append("")
    nreg += 1
open(outreg, "w", encoding="utf-16").write("\r\n".join(lines))
if jsonout: json.dump(allmap, open(jsonout, "w"), indent=0)
print(f"registered {nreg} classes, skipped {nskip_server} server-activated, {nskip_missing} with DLLs not extracted -> {outreg}")
