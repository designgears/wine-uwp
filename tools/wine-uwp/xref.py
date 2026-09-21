#!/usr/bin/env python3
"""Find who references a string in a PE, and which function that reference lives in.

For apps we have no symbols for, the app's own log vocabulary is the map (see AGENTS.md). This
turns a string into a code address you can put a ud2 probe on:

    xref.py Solitaire.exe --string 'Dx11RenderWindow::UpdateWindowRect'

It scans .text for rip-relative LEAs pointing at the string, then looks the referencing address up
in the .pdata exception table to get the *enclosing function's* start - which is where a probe has
to go, not the LEA itself.

Everything printed is an RVA unless labelled 'file'. Add ImageBase for a static address; for a
running process use the load base from /proc/<pid>/maps.
"""
import argparse, struct, sys

class PE:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        assert d[pe:pe+4] == b'PE\0\0', 'not a PE'
        nsec, = struct.unpack_from('<H', d, pe + 6)
        optsz, = struct.unpack_from('<H', d, pe + 20)
        opt = pe + 24
        magic, = struct.unpack_from('<H', d, opt)
        assert magic == 0x20b, 'only PE32+ (x86-64) is supported'
        self.image_base, = struct.unpack_from('<Q', d, opt + 24)
        self.size_of_image, = struct.unpack_from('<I', d, opt + 56)
        dd = opt + 112
        self.pdata_rva, self.pdata_size = struct.unpack_from('<II', d, dd + 3 * 8)
        self.sections = []
        for i in range(nsec):
            off = opt + optsz + i * 40
            name = d[off:off+8].rstrip(b'\0').decode('latin1')
            vsz, va, rsz, praw = struct.unpack_from('<IIII', d, off + 8)
            self.sections.append((name, va, vsz, praw, rsz))

    def rva2off(self, rva):
        for name, va, vsz, praw, rsz in self.sections:
            if va <= rva < va + max(vsz, rsz):
                return praw + (rva - va)
        return None

    def off2rva(self, off):
        for name, va, vsz, praw, rsz in self.sections:
            if praw and praw <= off < praw + rsz:
                return va + (off - praw)
        return None

    def section(self, name):
        for s in self.sections:
            if s[0] == name:
                return s
        return None

    def func_at(self, rva):
        """Enclosing RUNTIME_FUNCTION (begin, end) for an RVA, or None."""
        off = self.rva2off(self.pdata_rva)
        if off is None:
            return None
        lo, hi = 0, self.pdata_size // 12 - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            b, e, _ = struct.unpack_from('<III', self.data, off + mid * 12)
            if rva < b:
                hi = mid - 1
            elif rva >= e:
                lo = mid + 1
            else:
                return (b, e)
        return None

def find_leas(pe, target_rva):
    """rip-relative LEAs (REX + 8d, mod=00 rm=101) whose target is target_rva."""
    sec = pe.section('.text')
    if not sec:
        return []
    _, va, vsz, praw, rsz = sec
    blob = pe.data[praw:praw + rsz]
    hits = []
    for i in range(len(blob) - 7):
        if 0x48 <= blob[i] <= 0x4f and blob[i+1] == 0x8d and (blob[i+2] & 0xc7) == 0x05:
            disp, = struct.unpack_from('<i', blob, i + 3)
            insn_rva = va + i
            if insn_rva + 7 + disp == target_rva:
                hits.append(insn_rva)
    return hits

def find_calls(pe, target_rva):
    """Direct `call rel32` (e8) and `jmp rel32` (e9) sites targeting target_rva."""
    sec = pe.section('.text')
    if not sec:
        return []
    _, va, vsz, praw, rsz = sec
    blob = pe.data[praw:praw + rsz]
    hits = []
    for i in range(len(blob) - 5):
        if blob[i] in (0xe8, 0xe9):
            disp, = struct.unpack_from('<i', blob, i + 1)
            insn_rva = va + i
            if insn_rva + 5 + disp == target_rva:
                hits.append((insn_rva, 'call' if blob[i] == 0xe8 else 'jmp'))
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pe')
    ap.add_argument('--string', action='append', default=[],
                    help='exact string to locate and cross-reference (repeatable)')
    ap.add_argument('--rva', type=lambda x: int(x, 0), action='append', default=[],
                    help='cross-reference this RVA directly')
    ap.add_argument('--callers', type=lambda x: int(x, 0), action='append', default=[],
                    help='find call/jmp sites targeting this function RVA')
    args = ap.parse_args()

    pe = PE(args.pe)
    print(f'image base {pe.image_base:#x}  size {pe.size_of_image:#x}  '
          f'pdata {pe.pdata_rva:#x}+{pe.pdata_size:#x}')

    targets = list(args.rva)
    for s in args.string:
        needle = s.encode('latin1')
        start = 0
        found = []
        while True:
            # NUL-terminated and preceded by a terminator, so a suffix inside a longer string
            # does not match - the mistake that sent an earlier session chasing the wrong xref.
            i = pe.data.find(needle + b'\0', start)
            if i < 0:
                break
            start = i + 1
            if i > 0 and pe.data[i-1] not in (0, 0xff):
                continue
            rva = pe.off2rva(i)
            if rva is not None:
                found.append((i, rva))
        if not found:
            print(f'!! string not found: {s!r}')
        for off, rva in found:
            print(f'string {s!r}: file {off:#x} rva {rva:#x}')
            targets.append(rva)

    for c in args.callers:
        report_callers(pe, c)

    for t in targets:
        leas = find_leas(pe, t)
        if not leas:
            print(f'  rva {t:#x}: no rip-relative LEA references it')
        for lea in leas:
            fn = pe.func_at(lea)
            if fn:
                print(f'  rva {t:#x}: referenced by LEA at {lea:#x}  '
                      f'-> function {fn[0]:#x}..{fn[1]:#x} (probe at {fn[0]:#x}, '
                      f'file {pe.rva2off(fn[0]):#x})')
            else:
                print(f'  rva {t:#x}: referenced by LEA at {lea:#x} (no .pdata entry)')

def report_callers(pe, target):
    hits = find_calls(pe, target)
    if not hits:
        print(f'  fn {target:#x}: no direct call site (called through a pointer, or inlined)')
    for site, kind in hits:
        fn = pe.func_at(site)
        where = (f'function {fn[0]:#x}..{fn[1]:#x} (probe at {fn[0]:#x}, '
                 f'file {pe.rva2off(fn[0]):#x})') if fn else 'no .pdata entry'
        print(f'  fn {target:#x}: {kind} at {site:#x} in {where}')


if __name__ == '__main__':
    main()
