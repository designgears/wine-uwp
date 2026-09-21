#!/usr/bin/env python3
"""Tell a pristine Microsoft PE from one we have byte-patched.

Two independent tests, neither needing a reference copy of the file:

  authenticode  recompute the SHA-256 the signature seals (the whole file minus the
                CheckSum field, the certificate-table directory entry, and the
                certificate blob) and look for it inside the blob. Only works on
                embedded-signed files; much of System32 is catalog-signed instead
                and reports 'unsigned'.
  checksum      the PE optional-header CheckSum. Microsoft ships these correct, and a
                byte patch invalidates one unless the patcher recomputes it - which
                ours never did. This is the test that works on catalog-signed files.

The two agree on every embedded-signed file in uwp-bin/win11, which is what makes the
checksum result trustworthy on the ones Authenticode cannot judge.

  ./pecheck.py <file>...            one line per file
  ./pecheck.py --sweep <dir>        report only the files that look modified
"""
import sys, os, struct, hashlib


def _dirs(d):
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    if d[pe:pe + 4] != b'PE\0\0':
        raise ValueError('not a PE')
    opt = pe + 24
    magic = struct.unpack_from('<H', d, opt)[0]
    ddir = opt + (112 if magic == 0x20b else 96)
    return opt + 64, ddir + 4 * 8, struct.unpack_from('<I', d, ddir - 4)[0]


def authenticode(d):
    cksum_off, cert_off, nrva = _dirs(d)
    if nrva < 5:
        return 'unsigned'
    cva, csz = struct.unpack_from('<II', d, cert_off)
    if csz == 0:
        return 'unsigned'
    h = hashlib.sha256()
    h.update(d[:cksum_off])
    h.update(d[cksum_off + 4:cert_off])
    h.update(d[cert_off + 8:cva])
    return 'pristine' if h.digest() in d[cva:cva + csz] else 'MODIFIED'


def checksum(d):
    cksum_off, _, _ = _dirs(d)
    stored = struct.unpack_from('<I', d, cksum_off)[0]
    if stored == 0:
        return 'absent'
    b = bytearray(d)
    struct.pack_into('<I', b, cksum_off, 0)
    s = 0
    for i in range(0, len(b) & ~1, 2):
        s += b[i] | (b[i + 1] << 8)
        s = (s & 0xffff) + (s >> 16)
    if len(b) & 1:
        s += b[-1]
        s = (s & 0xffff) + (s >> 16)
    s = (s & 0xffff) + (s >> 16)
    return 'pristine' if stored == ((s + len(b)) & 0xffffffff) else 'MODIFIED'


def verdict(path):
    d = open(path, 'rb').read()
    a, c = authenticode(d), checksum(d)
    # Authenticode is conclusive where it applies, and it EXCLUDES the CheckSum field
    # from its digest - so a genuine Microsoft file can carry a stale/incorrect CheckSum
    # and still be untouched. mrt100_app.dll in this very tree is one. Never let the
    # checksum overrule a valid signature; only consult it when there is no signature.
    if a != 'unsigned':
        return a, a, c
    return (c if c != 'absent' else 'unknown'), a, c


def main(argv):
    if argv and argv[0] == '--sweep':
        bad = 0
        for root, _, files in os.walk(argv[1]):
            for f in sorted(files):
                p = os.path.join(root, f)
                try:
                    state, a, c = verdict(p)
                except Exception:
                    continue
                if state == 'MODIFIED':
                    bad += 1
                    print(f'MODIFIED  authenticode={a:9s} checksum={c:9s}  {p}')
        print(f'--- {bad} modified')
        return 1 if bad else 0
    for p in argv:
        state, a, c = verdict(p)
        print(f'{state:9s} authenticode={a:9s} checksum={c:9s}  {p}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
