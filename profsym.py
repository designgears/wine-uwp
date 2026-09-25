#!/usr/bin/env python3
"""Symbolize a WINE_UWP_PROFILE dump (<prefix>.<pid>: 'M base size path' and 'S tid rip count callers...').

  profsym.py DUMP                  per thread: sample count, top modules
  profsym.py DUMP TID [N]          one thread: top N leaf functions, each with its commonest caller chains
  BUSY=1 profsym.py ...            leave out samples parked in a blocking system call (waits, sleeps)

Names come from $UWP_BIN/pdb/<module>.pdb.syms for Windows DLLs, llvm-symbolizer on the build tree for Wine
builtins, and otherwise the enclosing .pdata function (module+function start), which at least groups the
samples of one function together. Run with $UWP_BIN/venv/bin/python (pefile).
"""
import bisect
import collections
import os
import subprocess
import sys

import pefile

UWP_BIN = os.environ.get('UWP_BIN', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'uwp-bin'))
WINE_BUILD = os.environ.get('WINE_BUILD', os.path.expanduser('~/src/wine-build'))


def unix_path(p):
    if len(p) > 2 and p[1] == ':' and p[0] in 'zZ':
        return p[2:].replace('\\', '/')
    return None


class Module:
    def __init__(self, base, size, path):
        self.base, self.size, self.path = base, size, path
        self.name = path.split('\\')[-1]
        self._syms = self._pdata = self._builtin = None
        self._cache = {}

    def syms(self):
        if self._syms is None:
            self._syms = []
            pdb = os.path.join(UWP_BIN, 'pdb')
            want = os.path.splitext(self.name)[0].lower() + '.pdb.syms'
            for f in os.listdir(pdb) if os.path.isdir(pdb) else []:
                if f.lower() == want:
                    for line in open(os.path.join(pdb, f), errors='replace'):
                        p = line.split(' ', 1)
                        try:
                            self._syms.append((int(p[0], 16), p[1].strip()))
                        except (ValueError, IndexError):
                            pass
                    self._syms.sort()
                    break
        return self._syms

    def is_wine_builtin(self):
        """the loaded file carries Wine's builtin marker (a Windows PDB for the same name would be wrong)"""
        path = self.path
        if len(path) > 2 and path[1] == ':' and path[0] in 'cC':
            prefix = os.environ.get('WINEPREFIX', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'prefix'))
            path = os.path.join(prefix, 'drive_c', path[3:].replace('\\', '/'))
        else:
            path = unix_path(path) or ''
        try:
            with open(path, 'rb') as f:
                return b'Wine builtin DLL' in f.read(0x80)
        except OSError:
            return False

    def builtin(self):
        if self._builtin is None:
            stem = os.path.splitext(self.name)[0].lower()
            cand = [os.path.join(WINE_BUILD, 'dlls', stem, 'x86_64-windows', self.name.lower()),
                    os.path.join(WINE_BUILD, 'programs', stem, 'x86_64-windows', self.name.lower())]
            self._builtin = next((c for c in cand if os.path.exists(c)), '')
            self._linked = 0
            if self._builtin:
                pe = pefile.PE(self._builtin, fast_load=True)
                self._linked = pe.OPTIONAL_HEADER.ImageBase
        return self._builtin

    def pdata(self):
        if self._pdata is None:
            self._pdata = []
            path = unix_path(self.path)
            if path and os.path.exists(path):
                try:
                    pe = pefile.PE(path, fast_load=True)
                    pe.parse_data_directories([pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
                    self._pdata = sorted(e.struct.BeginAddress for e in getattr(pe, 'DIRECTORY_ENTRY_EXCEPTION', []))
                except Exception:
                    pass
        return self._pdata

    def name_of(self, off):
        if off in self._cache:
            return self._cache[off]
        s = [] if self.is_wine_builtin() else self.syms()
        if s:
            i = bisect.bisect_right([x[0] for x in s], off) - 1
            r = s[i][1][:90] if i >= 0 else '+%x' % off
        elif self.builtin():
            # from the build tree: the symbol lookup fails with an absolute --obj path
            out = subprocess.run(['llvm-symbolizer', '--obj=' + os.path.relpath(self.builtin(), WINE_BUILD),
                                  hex(self._linked + off)],    # no --functions=short: it loses the name
                                 capture_output=True, text=True, cwd=WINE_BUILD).stdout.split('\n')
            r = out[0] if out and out[0] and out[0] != '??' else '+%x' % off
        elif self.pdata():
            p = self.pdata()
            i = bisect.bisect_right(p, off) - 1
            r = 'fn_%x' % p[i] if i >= 0 else '+%x' % off
        else:
            r = '+%x' % off
        self._cache[off] = r
        return r


def main():
    dump = sys.argv[1]
    mods, samples, names = [], [], {}
    for line in open(dump, errors='replace'):
        p = line.split()
        if not p:
            continue
        if p[0] == 'M':
            mods.append(Module(int(p[1], 16), int(p[2], 16), line.split(None, 3)[3].strip()))
        elif p[0] == 'T' and len(p) > 2:
            names[p[1]] = line.split(None, 2)[2].strip()
        elif p[0] == 'S':
            samples.append((p[1], int(p[2], 16), int(p[3]), [int(x, 16) for x in p[4:] if int(x, 16)]))
    mods.sort(key=lambda m: m.base)
    bases = [m.base for m in mods]

    def where(a):
        i = bisect.bisect_right(bases, a) - 1
        if i >= 0 and a < mods[i].base + mods[i].size:
            m = mods[i]
            return m, a - m.base
        return None, a

    def label(a):
        m, off = where(a)
        return '%s!%s' % (m.name, m.name_of(off)) if m else hex(a)

    # a sample whose leaf is a blocking system call is a thread waiting, not working
    WAITS = ('NtWaitFor', 'NtDelayExecution', 'NtRemoveIoCompletion', 'NtUserMsgWaitForMultipleObjectsEx',
             'NtUserGetMessage', 'NtSignalAndWaitForSingleObject', 'NtWaitForAlertByThreadId', 'NtReadFile',
             'NtUserWaitMessage', 'NtWaitForKeyedEvent', 'NtWaitForWorkViaWorkerFactory', 'NtContinue')
    busy_only = os.environ.get('BUSY')
    if busy_only:
        samples = [x for x in samples if not any(label(x[1]).split('!', 1)[-1].startswith(w) for w in WAITS)]
    per_thread = collections.Counter()
    for t, r, c, cs in samples:
        per_thread[t] += c
    if len(sys.argv) < 3:
        print('total samples', sum(per_thread.values()))
        for t, c in per_thread.most_common(20):
            m = collections.Counter()
            for tt, r, cc, cs in samples:
                if tt == t:
                    m[(where(r)[0].name if where(r)[0] else '?')] += cc
            print('%-5s %-16s %6d  %s' % (t, names.get(t, '')[:16], c, ', '.join('%s %d' % x for x in m.most_common(5))))
        return
    tid, n = sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 25
    leaves, chains = collections.Counter(), collections.defaultdict(collections.Counter)
    for t, r, c, cs in samples:
        if t != tid:
            continue
        leaf = label(r)
        leaves[leaf] += c
        chains[leaf][' < '.join(label(x) for x in cs[:int(os.environ.get('DEPTH', 5))])] += c
    total = sum(leaves.values())
    print('thread %s (%s): %d samples' % (tid, names.get(tid, 'unnamed'), total))
    for leaf, c in leaves.most_common(n):
        print('%6d %5.1f%%  %s' % (c, 100.0 * c / total, leaf))
        for ch, cc in chains[leaf].most_common(2):
            print('               %5d  < %s' % (cc, ch))


if __name__ == '__main__':
    main()
