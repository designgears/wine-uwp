#!/usr/bin/env python
"""For the DLLs the UWP stack loads natively, list imports (by name) into Wine builtins that Wine's spec files
declare as stubs or do not declare at all. Usage: import-audit.py <wine source dir> <dll paths...>"""
import sys, os, re, pefile
wine = sys.argv[1]
def spec_state(dll):
    base = dll.lower().replace('.dll', '')
    cands = [os.path.join(wine, 'dlls', base, base + '.spec')]
    for c in cands:
        if os.path.exists(c):
            names, stubs = set(), set()
            for l in open(c, errors='replace'):
                m = re.match(r'^\s*(?:@|\d+)\s+(stdcall|cdecl|varargs|stub|extern|thiscall)\s+(?:-\S+\s+)*([A-Za-z0-9_@?$]+)', l)
                if not m: continue
                (stubs if m.group(1) == 'stub' else names).add(m.group(2))
            return names, stubs
    return None
apisets = {}
for l in open(os.path.join(wine, 'dlls/apisetschema/apisetschema.spec'), errors='replace'):
    m = re.match(r'^apiset (\S+) = (\S+)', l)
    if m: apisets[m.group(1)] = m.group(2)
def resolve(dll):
    d = dll.lower().replace('.dll', '')
    if d.startswith('api-ms-') or d.startswith('ext-ms-'):
        key = re.sub(r'-\d+$', '', d)
        for k, v in apisets.items():
            if re.sub(r'-\d+$', '', k) == key: return v
        return None
    return dll
cache = {}
for path in sys.argv[2:]:
    p = pefile.PE(path, fast_load=True)
    p.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'], pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT']])
    missing = []
    for e in list(getattr(p, 'DIRECTORY_ENTRY_IMPORT', [])) + list(getattr(p, 'DIRECTORY_ENTRY_DELAY_IMPORT', [])):
        target = resolve(e.dll.decode())
        if not target or target.lower() in ('ucrtbase.dll',): continue
        if target not in cache: cache[target] = spec_state(target)
        st = cache[target]
        if st is None: continue
        names, stubs = st
        for i in e.imports:
            if not i.name: continue
            n = i.name.decode()
            if n in stubs: missing.append((target, n, 'stub'))
            elif n not in names: missing.append((target, n, 'absent'))
    print(f"== {os.path.basename(path)}: {len(missing)}")
    for t, n, k in sorted(set(missing)): print(f"   {k:6s} {t:20s} {n}")
