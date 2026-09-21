#!/usr/bin/env python
"""Find imports-by-ordinal from Windows DLLs into DLLs that Wine implements itself, and name the ordinals
with Microsoft's public symbols. Output is the list of ordinals Wine's .spec files must pin.
Usage: ordinal-audit.py <System32 dir> [only these importer DLLs...]"""
import sys, os, collections, subprocess, pefile
sysdir = sys.argv[1]; only = {a.lower() for a in sys.argv[2:]}
builtins = {f.lower() for f in os.listdir('/usr/lib/wine/x86_64-windows') if f.endswith('.dll')}
res = collections.defaultdict(lambda: collections.defaultdict(set))
for t in sorted(os.listdir(sysdir)):
    if not t.lower().endswith('.dll') or (only and t.lower() not in only): continue
    try:
        p = pefile.PE(os.path.join(sysdir, t), fast_load=True)
        p.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'], pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT']])
    except Exception: continue
    for e in list(getattr(p, 'DIRECTORY_ENTRY_IMPORT', [])) + list(getattr(p, 'DIRECTORY_ENTRY_DELAY_IMPORT', [])):
        dll = e.dll.decode().lower()
        if dll not in builtins: continue
        for i in e.imports:
            if not i.name: res[dll][i.ordinal].add(t)
here = os.path.dirname(os.path.abspath(__file__))
for dll, ords in sorted(res.items()):
    target = os.path.join(sysdir, dll)
    if not os.path.exists(target):
        target = next((os.path.join(sysdir, f) for f in os.listdir(sysdir) if f.lower() == dll), None)
    names = {}
    if target:
        try:
            out = subprocess.run([sys.executable, os.path.join(here, "pdbsyms.py"), target, "--ordinals", *map(str, sorted(ords))], capture_output=True, text=True).stdout
            for l in out.splitlines():
                parts = l.split(None, 2)
                if len(parts) == 3: names[int(parts[0])] = parts[2]
        except Exception as ex: names = {}
    print(f"== {dll}")
    for o, importers in sorted(ords.items()):
        print(f"  {o:5d}  {names.get(o, '?'):50s}  <- {', '.join(sorted(importers)[:5])}{' ...' if len(importers) > 5 else ''}")
