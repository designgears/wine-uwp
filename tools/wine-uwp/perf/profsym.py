#!/usr/bin/env python3
"""profsym.py <profile file> [top N]: symbolize the output of wineuwp's WINE_UWP_PROFILE sampler.

Samples in waits (NtWaitFor*, message waits, vkWaitForFences ...) are counted apart; for each busy thread the
top leaf functions are listed with their most common callers (a stack-scan guess). Windows DLLs are named
with the .pdb.syms files in $UWP_PDB_DIR, Wine builtins with nm on the build tree."""
import sys, os, re, bisect, subprocess, collections, glob

path = sys.argv[1]; topn = int(sys.argv[2]) if len(sys.argv) > 2 else 25
PDB = os.environ.get('UWP_PDB_DIR', os.path.expanduser('~/uwp-bin/pdb')); BUILD = os.environ.get('WINE_BUILD', os.path.expanduser('~/src/wine-build'))
PREFIX = os.environ.get('UWP_PREFIX', os.path.expanduser('~/.wine-uwp'))
modules = []; samples = []
for line in open(path, errors='replace'):
    p = line.rstrip('\n').split(' ', 3)
    if p[0] == 'M': modules.append((int(p[1], 16), int(p[2], 16), p[3]))
    elif p[0] == 'S':
        q = line.split()
        samples.append((int(q[1], 16), int(q[2], 16), int(q[3]), int(q[4], 16) if len(q) > 4 else 0))
modules.sort()
starts = [m[0] for m in modules]
symcache = {}

def load_syms(modpath):
    base = os.path.basename(modpath.replace('\\', '/'))
    name = os.path.splitext(base)[0]
    if name.lower() in symcache: return symcache[name.lower()]
    syms = None
    # a Wine builtin placeholder in the prefix, or a module loaded from the build tree, is Wine's own code
    unix = modpath.replace('\\', '/')
    if re.match(r'^[A-Za-z]:/', unix):
        drive = unix[0].lower()
        unix = (os.path.join(PREFIX, 'drive_c') if drive == 'c' else '') + unix[2:] if drive == 'c' else unix[2:]
    builtin = 'wine-build' in modpath
    try:
        with open(unix, 'rb') as fh: builtin = builtin or b'Wine builtin DLL' in fh.read(4096)
    except OSError: pass
    for cand in ([] if builtin else glob.glob(os.path.join(PDB, '*.pdb.syms'))):
        if os.path.basename(cand).lower() == name.lower() + '.pdb.syms':
            syms = []
            for l in open(cand, errors='replace'):
                a, _, n = l.rstrip('\n').partition(' ')
                try: syms.append((int(a, 16), n))
                except ValueError: pass
            break
    if syms is None:
        cands = glob.glob(os.path.join(BUILD, 'dlls', name.lower(), 'x86_64-windows', base.lower())) or \
                glob.glob(os.path.join(BUILD, 'dlls', name.lower(), 'x86_64-windows', '*.dll'))
        if cands:
            f = cands[0]
            ib = 0
            for l in subprocess.run(['objdump', '-p', f], capture_output=True, text=True).stdout.splitlines():
                if l.startswith('ImageBase'): ib = int(l.split()[1], 16)
            syms = []
            for l in subprocess.run(['nm', '-n', '--defined-only', f], capture_output=True, text=True).stdout.splitlines():
                q = l.split()
                if len(q) >= 3 and q[1] in 'tTwW':
                    syms.append((int(q[0], 16) - ib, q[2]))
    syms = sorted(syms or [])
    symcache[name.lower()] = syms
    return syms

def symbolize(rip):
    i = bisect.bisect_right(starts, rip) - 1
    if i < 0 or rip >= modules[i][0] + modules[i][1]:
        return '[unix or jit]'
    base, size, mp = modules[i]
    rva = rip - base
    mname = os.path.basename(mp.replace('\\', '/'))
    syms = load_syms(mp)
    if syms:
        j = bisect.bisect_right([s[0] for s in syms], rva) - 1
        if j >= 0: return '%s!%s' % (mname, syms[j][1][:90])
    return '%s+%x' % (mname, rva)

per_thread = collections.Counter()
for tid, rip, n, caller in samples: per_thread[tid] += n
waits = re.compile(r'NtWaitFor|NtUserMsgWaitForMultipleObjectsEx|NtDelayExecution|NtRemoveIoCompletion|NtUserGetMessage|__wine_syscall|NtWaitForAlertByThreadId|NtUserPeekMessage|ZwWait|NtUserWaitMessage|wait_select|NtSignalAndWait|RtlWaitOnAddress|NtWaitForKeyedEvent|NtReplyWaitReceivePort', re.I)
busy = collections.Counter(); wait_n = collections.Counter(); bysym = collections.defaultdict(collections.Counter)
callers = collections.defaultdict(collections.Counter)
for tid, rip, n, caller in samples:
    s = symbolize(rip)
    if waits.search(s) or 'vkWaitForFences' in s: wait_n[tid] += n
    else:
        busy[tid] += n; bysym[tid][s] += n
        callers[(tid, s)][symbolize(caller) if caller else '?'] += n
total_busy = sum(busy.values())
print("busy samples %d of %d" % (total_busy, sum(per_thread.values())))
for tid, n in busy.most_common(6):
    print("\nthread %x: %d busy, %d waiting" % (tid, n, wait_n[tid]))
    for s, c in bysym[tid].most_common(topn):
        print("  %5d %5.1f%%  %s" % (c, 100.0 * c / n, s))
        for cs, cc in callers[(tid, s)].most_common(2):
            if c >= 5: print("              <- %4d %s" % (cc, cs))
