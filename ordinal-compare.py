#!/usr/bin/env python
"""Compare, for every by-ordinal import from a Windows DLL into a DLL Wine builds, the name Windows exports at that
ordinal with the name our build exports there. A mismatch means the Windows caller reaches a different function
of ours (uxtheme 121 was GetThemeAnimationProperty, which wrote through a stack value).
Usage: ordinal-compare.py <Windows System32 dir> <Wine build dir>   (run with uwp-bin/venv/bin/python)"""
import os, sys, collections, pefile
sysdir, build = sys.argv[1], sys.argv[2]
def exports(path):
    p = pefile.PE(path, fast_load=True); p.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
    if not hasattr(p, 'DIRECTORY_ENTRY_EXPORT'): return {}
    return {e.ordinal: (e.name.decode() if e.name else None) for e in p.DIRECTORY_ENTRY_EXPORT.symbols}
wine = {}
for d in os.listdir(os.path.join(build, 'dlls')):
    f = os.path.join(build, 'dlls', d, 'x86_64-windows', d + '.dll')
    if os.path.exists(f): wine[d.lower() + '.dll'] = f
win = {f.lower(): os.path.join(sysdir, f) for f in os.listdir(sysdir)}
uses = collections.defaultdict(set)
for t in sorted(os.listdir(sysdir)):
    if not t.lower().endswith('.dll'): continue
    try:
        p = pefile.PE(os.path.join(sysdir, t), fast_load=True)
        p.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'], pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT']])
    except Exception: continue
    for e in list(getattr(p, 'DIRECTORY_ENTRY_IMPORT', [])) + list(getattr(p, 'DIRECTORY_ENTRY_DELAY_IMPORT', [])):
        dll = e.dll.decode().lower()
        if dll in wine:
            for i in e.imports:
                if not i.name: uses[(dll, i.ordinal)].add(t)
cache = {}
for (dll, o), users in sorted(uses.items()):
    if dll not in cache: cache[dll] = (exports(win[dll]) if dll in win else {}, exports(wine[dll]))
    w, n = cache[dll]
    wn, nn = w.get(o, '?'), n.get(o, 'MISSING')
    if wn and wn != '?' and wn != nn:
        print(f"{dll} @{o}: Windows {wn}, Wine {nn}  <- {', '.join(sorted(users))[:120]}")
