#!/usr/bin/env python3
"""Install a UWP app package (.msixbundle, .appxbundle, .msix or .appx) for wine-uwp.

    install-app.py PACKAGE DATADIR --windows TREE [--stage NAME]
    install-app.py PACKAGE --info [--icon OUT.png] [--windows TREE]
    install-app.py APPINSTALLER --latest

PACKAGE may also be an .appinstaller file or URL (what a vendor's download page hands out): the bundle it names is
read straight from the server with range requests, so nothing the size of the package is written to disk, and its
URI is kept as DATADIR/stage/NAME/.wine-uwp-appinstaller for update checks.

This is the registration step Windows' deployment service does, reduced to what wine-uwp needs:

  1. from a bundle, take the x64 application package (resource packages are not needed: the main package's
     resources.pri carries the neutral resources, and a package staged out of a Windows install ran the same way);
  2. unpack it into DATADIR/stage/NAME, undoing the zip's percent-encoding of file names;
  3. find every framework it declares (PackageDependency) in TREE/Program Files/WindowsApps, the harvested
     Windows image, and copy the newest one that satisfies MinVersion into DATADIR/stage/frameworks, where run.sh
     puts it on the package graph;
  4. copy the framework DLLs the app loads by bare name next to it, as stage-app.sh does;
  5. write DATADIR/stage/NAME/identity.env, which run.sh sources: package full and family name, AUMID, entry exe
     and window title; DATADIR/stage/NAME/.wine-uwp-icon.png, the app's logo, for a desktop entry; and
     .wine-uwp-protocols, the URI schemes it owns, one per line, for the desktop's scheme handlers.

--info reads the package without installing it and prints one JSON object: name, title, publisher, version,
architecture, frameworks (with "available" when --windows is given), and the logo if --icon names a file.

Progress goes to stdout as "STAGE <what>" and "PROGRESS <done> <total>" lines for the setup wizard. The last line
is "INSTALLED <stage name>". Only the standard library is used.
"""

import argparse
import json
import hashlib
import io
import os
import re
import shutil
import struct
import sys
import tempfile
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zipfile
import zlib

ARCH = "x64"


def say(*args):
    print(*args, flush=True)


def local(tag):
    """An element's tag without its XML namespace."""
    return tag.rsplit("}", 1)[-1]


def children(elem, name):
    return [e for e in elem.iter() if local(e.tag) == name]


def publisher_id(publisher):
    """The 13-character publisher id in a package family name: the first 8 bytes of the SHA-256 of the UTF-16LE
    publisher string, as 13 5-bit groups (the last one padded) in Crockford's base32 alphabet."""
    digest = hashlib.sha256(publisher.encode("utf-16-le")).digest()[:8]
    bits = int.from_bytes(digest, "big") << 1
    alphabet = "0123456789abcdefghjkmnpqrstvwxyz"
    return "".join(alphabet[(bits >> (60 - 5 * i)) & 31] for i in range(13))


def version_tuple(v):
    return tuple(int(x) for x in v.split("."))


def is_url(p):
    return str(p).startswith(("https://", "http://"))


class HttpFile(io.RawIOBase):
    """A file on a web server, read with range requests: a zip's directory and a few members cost a few requests,
    and a sequential read of the whole thing streams it. Nothing is written to disk."""

    BLOCK = 8 << 20

    def __init__(self, url):
        self.url, self.pos, self.cache = url, 0, (0, b"")
        with self._open("HEAD") as r:
            if r.headers.get("Accept-Ranges", "").lower() != "bytes":
                raise SystemExit(f"{url}: the server does not support partial downloads")
            self.size = int(r.headers["Content-Length"])

    def _open(self, method="GET", rng=None):
        req = urllib.request.Request(self.url, method=method, headers={"User-Agent": "wine-uwp"})
        if rng:
            req.add_header("Range", "bytes=%d-%d" % rng)
        return urllib.request.urlopen(req, timeout=60)

    def _fetch(self, start, end):
        for attempt in range(5):
            try:
                with self._open(rng=(start, end - 1)) as r:
                    data = r.read()
                if len(data) == end - start:
                    return data
            except OSError as e:
                if attempt == 4:
                    raise SystemExit(f"download failed: {e}")
        raise SystemExit("download failed: short reads from the server")

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, off, whence=0):
        self.pos = {0: off, 1: self.pos + off, 2: self.size + off}[whence]
        return self.pos

    def readinto(self, buf):
        n = max(0, min(len(buf), self.size - self.pos))
        if n == 0:
            return 0
        start, data = self.cache
        if not (start <= self.pos and self.pos + n <= start + len(data)):
            if n >= self.BLOCK:
                buf[:n] = self._fetch(self.pos, self.pos + n)
                self.pos += n
                return n
            start = self.pos
            data = self._fetch(start, min(self.size, start + self.BLOCK))
            self.cache = (start, data)
        off = self.pos - start
        buf[:n] = data[off:off + n]
        self.pos += n
        return n


def open_raw(source):
    """A seekable binary file for a local path or a URL."""
    return io.BufferedReader(HttpFile(source), 1 << 16) if is_url(source) else open(source, "rb")


def read_appinstaller(source):
    """An .appinstaller file (local or a URL): its own update URI, the main bundle's URI and its version."""
    if is_url(source):
        with urllib.request.urlopen(urllib.request.Request(source, headers={"User-Agent": "wine-uwp"}), timeout=60) as r:
            data = r.read()
    else:
        data = open(source, "rb").read()
    root = ET.fromstring(data)
    main = next(iter(children(root, "MainBundle") + children(root, "MainPackage")), None)
    if main is None or not main.get("Uri"):
        raise SystemExit(f"{source} names no package to download")
    # only the main bundle is used: frameworks come from the Windows files, never from the vendor
    return {"uri": root.get("Uri") or (source if is_url(source) else None),
            "package": main.get("Uri"), "version": main.get("Version")}


def resolve(source):
    """What to read for a package argument: an .appinstaller (a file, as a vendor's download page hands out, or its
    URL) is followed to the bundle it names. Returns (package path or URL, the parsed .appinstaller or None)."""
    if str(source).lower().split("?")[0].endswith(".appinstaller"):
        a = read_appinstaller(source)
        return a["package"], a
    return source, None


class Slice(io.RawIOBase):
    """A read-only window onto part of a file: a package stored uncompressed inside a bundle, read in place."""

    def __init__(self, path, start, size):
        self.f, self.start, self.size, self.pos = open_raw(path), start, size, 0

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, off, whence=0):
        self.pos = {0: off, 1: self.pos + off, 2: self.size + off}[whence]
        return self.pos

    def readinto(self, buf):
        n = max(0, min(len(buf), self.size - self.pos))
        self.f.seek(self.start + self.pos)
        got = self.f.readinto(memoryview(buf)[:n])
        self.pos += got
        return got

    def close(self):
        self.f.close()
        super().close()


# ---- encrypted packages (EAppx / EMsix): the Store's DRM container -------------------------------------------------
#
# Layout (little-endian): a header (magic EXPH for a package, EXBH for a bundle; offsets of a footer table, the
# signature and code integrity blobs, the block map's file id, the key ids, the package full name), the files back to
# back, and the footer table: 40 bytes per file with its key index (0xffff = not encrypted), compression (1 = raw
# DEFLATE), file id, offset, and uncompressed and stored lengths. The block map (AppxBlockMap.xml, never encrypted)
# names the files; its Id attributes are the file ids in hex. A bundle's members are EXPH packages stored whole.
#
# An encrypted file is XTS-AES-128 in 512-byte data units, padded on disk to a whole unit, and DEFLATE-compressed
# after encryption when compressed. The 32-byte content key is the data key then the tweak key; unit i of a file has
# the tweak (T + i) as a 128-bit little-endian number, T being the four 8-byte quarters of SHA-256("\" + name, then
# the lower-case family name "<name>_<publisher id>", both UTF-16LE) XORed together. Content keys come from the
# Store's licence for the package (shims/xboxauth/windevice.inc fetches them for the Xbox app).

EAPPX_MAGICS = (b"EXPH", b"EXBH")
EAPPX_HEADER = struct.Struct("<4sHQQQQQHIIQHIIQIH")
EAPPX_FOOTER = struct.Struct("<HHHHQQQQ")


class _Xts:
    """XTS-AES-128 decryption through the host's libcrypto (OpenSSL), one call per 512-byte unit."""

    def __init__(self, key):
        import ctypes
        import ctypes.util
        name = ctypes.util.find_library("crypto")
        if not name:
            raise SystemExit("decrypting this package needs the host's OpenSSL (libcrypto), which was not found")
        lc = ctypes.CDLL(name)
        lc.EVP_CIPHER_CTX_new.restype = ctypes.c_void_p
        lc.EVP_aes_128_xts.restype = ctypes.c_void_p
        lc.EVP_DecryptInit_ex.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p,
                                          ctypes.c_char_p]
        lc.EVP_DecryptUpdate.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int),
                                         ctypes.c_void_p, ctypes.c_int]
        self.ct, self.lc = ctypes, lc
        self.ctx = lc.EVP_CIPHER_CTX_new()
        if lc.EVP_DecryptInit_ex(self.ctx, lc.EVP_aes_128_xts(), None, key, None) != 1:
            raise SystemExit("libcrypto refused the content key")
        self.n = ctypes.c_int()

    def decrypt(self, data, tweak, unit):
        """data: whole 512-byte units, the first being unit number `unit` of a file with tweak base `tweak`."""
        ct, lc, ctx = self.ct, self.lc, self.ctx
        src = bytearray(data)
        out = bytearray(len(src))
        sbase = ct.addressof((ct.c_char * len(src)).from_buffer(src))
        obase = ct.addressof((ct.c_char * len(out)).from_buffer(out))
        init, update, n = lc.EVP_DecryptInit_ex, lc.EVP_DecryptUpdate, ct.byref(self.n)
        mask = (1 << 128) - 1
        for off in range(0, len(src), 512):
            iv = ((tweak + unit + off // 512) & mask).to_bytes(16, "little")
            if init(ctx, None, None, None, iv) != 1 or update(ctx, obase + off, n, sbase + off, min(512, len(src) - off)) != 1:
                raise SystemExit("decryption failed")
        return bytes(out)


def _eappx_tweak(name, family):
    h = hashlib.sha256(("\\" + name.lstrip("\\")).encode("utf-16le") + family.lower().encode("utf-16le")).digest()
    q = [int.from_bytes(h[i:i + 8], "little") for i in range(0, 32, 8)]
    return q[0] ^ q[1] ^ q[2] ^ q[3]


_KEYS = {}     # read once: --keys-once deletes its file, and both a bundle and its package ask


def content_keys():
    """The content keys handed to the installer: "keyid:key;..." (hex) in WINE_UWP_CONTENT_KEYS, or --keys FILE with
    "keyid key" lines. Key ids are the 16 bytes the licence names them by."""
    if _KEYS:
        return dict(_KEYS)
    keys = {}
    text = os.environ.pop("WINE_UWP_CONTENT_KEYS", "")
    for item in text.split(";"):
        if ":" in item:
            kid, key = item.split(":", 1)
            keys[bytes.fromhex(kid)] = bytes.fromhex(key)
    for opt in ("--keys", "--keys-once"):
        if opt not in sys.argv:
            continue
        src = sys.argv[sys.argv.index(opt) + 1]
        if src == "-":
            lines = [sys.stdin.readline()]
        else:
            with open(src) as f:
                lines = f.read().splitlines()
            if opt == "--keys-once":
                # the Xbox app's Gaming Services stand-in leaves them in $XDG_RUNTIME_DIR for this one read
                os.unlink(src)
        for line in lines:
            for item in line.replace(";", "\n").splitlines():
                parts = item.replace(":", " ").split()
                if len(parts) >= 2:
                    keys[bytes.fromhex(parts[0])] = bytes.fromhex(parts[1])
    _KEYS.update(keys)
    keys.update(_KEYS)
    return keys


class _EAppxMember:
    def __init__(self, name, size, footer, encrypted):
        self.name, self.file_size, self.footer, self.encrypted = name, size, footer, encrypted
        # what install-app.py's zip code expects: names with "/" and percent-encoding undone by extract()
        self.filename = name.replace("\\", "/").replace("%", "%25")

    def is_dir(self):
        return False


class _EAppxReader(io.RawIOBase):
    """One file of an EAppx package, read sequentially: stored bytes, inflated, decrypted, cut to its size."""

    CHUNK = 4 << 20

    def __init__(self, pkg, m):
        self.pkg, self.m = pkg, m
        fid, off, ulen, slen, kidx, comp = m.footer
        self.left = m.file_size
        stored = slen
        if kidx != 0xFFFF and comp != 1:
            stored = (slen + 511) // 512 * 512      # an encrypted file is padded to a whole unit
        self.src = Slice(pkg.path, pkg.base + off, stored) if pkg.path is not None else None
        self.inflate = zlib.decompressobj(-15) if comp == 1 else None
        self.xts = pkg.cipher(kidx) if kidx != 0xFFFF else None
        self.tweak = _eappx_tweak(m.name, pkg.family) if self.xts else 0
        self.unit = 0
        self.pending = b""      # inflated or stored bytes not yet decrypted (less than a unit)
        self.out = b""

    def readable(self):
        return True

    def _more(self):
        raw = self.src.read(self.CHUNK)
        if not raw:
            if self.inflate:
                raw = self.inflate.flush()
                self.inflate = None
            if not raw:
                if self.pending and self.xts:
                    data, self.pending = self.pending + b"\0" * (-len(self.pending) % 512), b""
                    self.out += self.xts.decrypt(data, self.tweak, self.unit)
                    return True
                return False
        elif self.inflate:
            raw = self.inflate.decompress(raw)
        if not self.xts:
            self.out += raw
            return True
        data = self.pending + raw
        whole = len(data) // 512 * 512
        self.pending = data[whole:]
        if whole:
            self.out += self.xts.decrypt(data[:whole], self.tweak, self.unit)
            self.unit += whole // 512
        return True

    def readinto(self, buf):
        while not self.out and self.left and self._more():
            pass
        n = min(len(buf), len(self.out), self.left)
        buf[:n] = self.out[:n]
        self.out = self.out[n:]
        self.left -= n
        return n

    def close(self):
        if self.src:
            self.src.close()
        super().close()


class EAppxPackage:
    """An EXPH package, at `base` inside `path` (a file or URL), with the zipfile methods install-app.py uses."""

    def __init__(self, path, base=0):
        self.path, self.base = path, base
        with open_raw(path) as f:
            f.seek(base)
            head = f.read(8192)
            h = EAPPX_HEADER.unpack_from(head, 0)
            magic, foff, flen, bm_id, kcount = h[0], h[3], h[4], h[14], h[16]
            if magic != b"EXPH":
                raise SystemExit(f"not an encrypted package (magic {magic!r})")
            self.key_ids = [head[88 + 32 * i:120 + 32 * i] for i in range(kcount)]
            p = 88 + 32 * kcount
            _, nbytes = struct.unpack_from("<HH", head, p)
            self.full_name = head[p + 4:p + 4 + nbytes].decode("utf-16le")
            parts = self.full_name.split("_")
            self.family = f"{parts[0]}_{parts[-1]}"
            f.seek(base + foff)
            table = f.read(flen)
        self.footers = {}
        for i in range(flen // EAPPX_FOOTER.size):
            _, _, kidx, comp, fid, off, ulen, slen = EAPPX_FOOTER.unpack_from(table, i * EAPPX_FOOTER.size)
            self.footers[fid] = (fid, off, ulen, slen, kidx, comp)
        self.keys, self.ciphers = content_keys(), {}
        bm = self._read_raw(self.footers[bm_id])
        self.blockmap = bm
        self.members = []
        for e in ET.fromstring(bm):
            if local(e.tag) != "File":
                continue
            fid = int(e.get("Id"), 16)
            self.members.append(_EAppxMember(e.get("Name"), int(e.get("Size")), self.footers[fid],
                                             e.get("Encrypted", "false") == "true"))

    def _read_raw(self, footer):
        fid, off, ulen, slen, kidx, comp = footer
        with Slice(self.path, self.base + off, slen) as s:
            data = s.read()
        return zlib.decompress(data, -15) if comp == 1 else data

    def cipher(self, kidx):
        if kidx not in self.ciphers:
            kid = self.key_ids[kidx][:16]
            if kid not in self.keys:
                raise SystemExit(f"{self.full_name} is encrypted and its content key ({kid.hex()}) was not given; "
                                 "it comes with the Store licence")
            self.ciphers[kidx] = _Xts(self.keys[kid])
        return self.ciphers[kidx]

    def namelist(self):
        return [m.filename for m in self.members]

    def infolist(self):
        return list(self.members)

    def getinfo(self, name):
        return next(m for m in self.members if m.filename == name)

    def open(self, m):
        if isinstance(m, str):
            m = self.getinfo(m)
        return io.BufferedReader(_EAppxReader(self, m), 1 << 20)

    def read(self, name):
        with self.open(name) as f:
            return f.read()

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


# ---- GDK packages (.msixvc): an XVD container ------------------------------------------------------------------
#
# 4096-byte pages. At 0x200 "msft-xvd", then (little-endian) flags at 0x208, the drive size at 0x218, the VDUID at
# 0x220, the lengths of the embedded XVD, user data, XVC info and dynamic header at 0x288..0x297 and the mutable data
# page count at 0x470. The file is laid out as header (3 pages), embedded XVD, mutable data, hash tree, user data, XVC
# info, dynamic header, drive, each rounded up to whole pages. The hash tree has levels L3 L2 L1 L0 in that order;
# L0 has an entry per page from the user data on (170 entries of 24 bytes a page, the last 4 bytes of an entry the
# page's XTS data unit), each level above one entry per page of the level below.
#
# The XVC info holds the content id, a table of 192 key ids (GUIDs) and, from 0xDA8, 0x80-byte region headers: id,
# key index (0xffff = plain), first segment, byte offset and length. The user data has a table of named files; its
# SegmentMetadata.bin (" PFX") lists the package's files in order, each filling whole pages one after the other
# from the start of the region whose first segment it is. An encrypted page is XTS-AES-128 with the content key's
# second half as the data key and its first as the tweak key, tweak = data unit (u32), region id (u32), first 8
# bytes of the VDUID.

XVD_PAGE = 4096


class _XtsPages:
    """XVD page decryption through the host's libcrypto."""

    def __init__(self, key):
        self.x = _Xts(key[16:] + key[:16])   # OpenSSL takes the data key first

    def decrypt(self, data, ivs):
        x = self.x
        ct, lc, ctx = x.ct, x.lc, x.ctx
        src = bytearray(data)
        out = bytearray(len(src))
        sbase = ct.addressof((ct.c_char * len(src)).from_buffer(src))
        obase = ct.addressof((ct.c_char * len(out)).from_buffer(out))
        n = ct.byref(x.n)
        for i, iv in enumerate(ivs):
            off = i * XVD_PAGE
            if lc.EVP_DecryptInit_ex(ctx, None, None, None, iv) != 1 or \
                    lc.EVP_DecryptUpdate(ctx, obase + off, n, sbase + off, XVD_PAGE) != 1:
                raise SystemExit("decryption failed")
        return bytes(out)


class _XvdMember:
    def __init__(self, name, size, region, page):
        self.name, self.file_size, self.region, self.page = name, size, region, page
        self.filename = name.replace("\\", "/").replace("%", "%25")

    def is_dir(self):
        return False


class _XvdReader(io.RawIOBase):
    """One file of a package: its pages, decrypted when its region is, cut to its size."""

    PAGES = 1024

    def __init__(self, pkg, m):
        self.pkg, self.m, self.page, self.left, self.out = pkg, m, m.page, m.file_size, b""
        self.pages_left = max(1, -(-m.file_size // XVD_PAGE))

    def readable(self):
        return True

    def readinto(self, buf):
        if not self.out and self.left:
            n = min(self.PAGES, self.pages_left)
            self.out = self.pkg.read_pages(self.m.region, self.page, n)
            self.page += n
            self.pages_left -= n
        k = min(len(buf), len(self.out), self.left)
        buf[:k] = self.out[:k]
        self.out = self.out[k:]
        self.left -= k
        return k


class MsixvcPackage:
    """A GDK package (.msixvc, a file or URL) with the zipfile methods install-app.py uses."""

    def __init__(self, path):
        self.path = path
        self.f = open_raw(path)
        h = self._read(0, XVD_PAGE)
        if h[0x200:0x208] != b"msft-xvd":
            raise SystemExit("not a GDK package")
        drive = struct.unpack_from("<Q", h, 0x218)[0]
        self.vduid = h[0x220:0x228]
        emb, user, xvc, dyn = struct.unpack_from("<IIII", h, 0x288)
        pages = lambda n: -(-n // XVD_PAGE)
        n = pages(user) + pages(xvc) + pages(dyn) + pages(drive)
        levels = [max(1, -(-n // 170))]
        for _ in range(3):
            below = levels[-1]
            levels.append(-(-(0 if below == 1 else below) // 170))
        self.tree = 3 + pages(emb) + h[0x470]
        self.l0 = self.tree + sum(levels[1:])          # L3, L2 and L1 come first
        self.user = self.tree + sum(levels)
        xvc_at = self.user + pages(user)
        info = self._read(xvc_at * XVD_PAGE, pages(xvc) * XVD_PAGE)
        self.key_ids = [info[16 + 16 * i:32 + 16 * i] for i in range(192)]
        version, count = struct.unpack_from("<II", info, 0xD10)
        if version < 1:
            raise SystemExit("this GDK package has no region table (XVC info version 0)")
        self.regions = {}
        order = []
        for i in range(count):
            r = info[0xDA8 + 0x80 * i:0xDA8 + 0x80 * (i + 1)]
            rid, kidx, _, flags, first = struct.unpack_from("<IHHII", r, 0)
            off, length = struct.unpack_from("<QQ", r, 0x50)
            self.regions[rid] = (kidx, off // XVD_PAGE, length // XVD_PAGE)
            order.append((first, rid))
        ud = self._read(self.user * XVD_PAGE, user)
        table = struct.unpack_from("<I", ud, 0)[0]
        t = ud[table:]
        self.full_name = t[4:524].decode("utf-16le").split("\0")[0]
        files = {}
        for i in range(struct.unpack_from("<I", t, 524)[0]):
            e = t[528 + 528 * i:528 * (i + 2)]
            size, off = struct.unpack_from("<II", e, 520)
            files[e[:520].decode("utf-16le").split("\0")[0].lower()] = ud[16 + off:16 + off + size]
        seg = files.get("segmentmetadata.bin")
        if not seg or seg[:4] != b" PFX":
            raise SystemExit("this GDK package has no segment table; only packages with one are supported")
        hlen, segs = struct.unpack_from("<II", seg, 12)
        names = hlen + 16 * segs
        entries = []
        for i in range(segs):
            flags, plen, poff, size = struct.unpack_from("<HHIQ", seg, hlen + 16 * i)
            entries.append((seg[names + poff:names + poff + 2 * plen].decode("utf-16le"), size))
        # each content region takes its segments in order, page after page from its start
        self.members = []
        for rid, (kidx, first_page, npages) in self.regions.items():
            if rid >= 0x40000000 and rid != 0x40000003:
                continue
            first = next(f for f, r in order if r == rid)
            page, i = first_page, first
            while i < len(entries) and page < first_page + npages:
                name, size = entries[i]
                if name.startswith(("\\", "/")) or ".." in name.replace("\\", "/").split("/"):
                    raise SystemExit(f"refusing unsafe path in package: {name}")
                self.members.append(_XvdMember(name, size, rid, page))
                page += max(1, -(-size // XVD_PAGE))
                i += 1
        self.keys, self.ciphers = content_keys(), {}
        # the L0 hash level, read once: fetching it beside each run of data pages made a download from the CDN
        # alternate between two parts of the file, and HttpFile keeps one block, so it fetched each block over again
        self.l0_data = self._read(self.l0 * XVD_PAGE, levels[0] * XVD_PAGE)

    def _read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def read_pages(self, rid, page, n):
        data = self._read(page * XVD_PAGE, n * XVD_PAGE)
        kidx = self.regions[rid][0]
        if kidx >= 192:
            return data
        if kidx not in self.ciphers:
            kid = self.key_ids[kidx]
            if kid not in self.keys:
                raise SystemExit(f"{self.full_name} is encrypted and its content key ({kid.hex()}) was not given; "
                                 "it comes with the Store licence")
            self.ciphers[kidx] = _XtsPages(self.keys[kid])
        # the data units, from the L0 hash entries of these pages (170 to a tree page)
        b = page - self.user
        tree = self.l0_data
        rid_bytes = struct.pack("<I", rid)
        ivs = []
        for k in range(b, b + n):
            e = (k // 170) * XVD_PAGE + (k % 170) * 24
            ivs.append(tree[e + 20:e + 24] + rid_bytes + self.vduid)
        return self.ciphers[kidx].decrypt(data, ivs)

    def namelist(self):
        return [m.filename for m in self.members]

    def infolist(self):
        return list(self.members)

    def getinfo(self, name):
        return next(m for m in self.members if m.filename == name)

    def open(self, m):
        if isinstance(m, str):
            m = self.getinfo(m)
        return io.BufferedReader(_XvdReader(self, m), 1 << 20)

    def read(self, name):
        with self.open(name) as f:
            return f.read()

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


def open_msixvc(path):
    with open_raw(path) as f:
        f.seek(0x200)
        if f.read(8) != b"msft-xvd":
            return None
    return MsixvcPackage(path)


def open_eappx(path):
    """An encrypted package or bundle: the bundle's x64 application package. None if `path` is not one."""
    with open_raw(path) as f:
        head = f.read(8192)
    if head[:4] not in EAPPX_MAGICS:
        return None
    if head[:4] == b"EXPH":
        return EAppxPackage(path)
    h = EAPPX_HEADER.unpack_from(head, 0)
    foff, flen, bm_id = h[3], h[4], h[14]
    with open_raw(path) as f:
        f.seek(foff)
        table = f.read(flen)
        footers = [EAPPX_FOOTER.unpack(table[i:i + 40]) for i in range(0, flen, 40)]
        files = {}
        for _, _, kidx, comp, fid, off, ulen, slen in footers:
            files[fid] = (off, slen, comp)
        off, slen, comp = files[bm_id]
        f.seek(off)
        bm = f.read(slen)
        bm = zlib.decompress(bm, -15) if comp == 1 else bm
        manifest_id = next(int(e.get("Id"), 16) for e in ET.fromstring(bm)
                           if local(e.tag) == "File" and e.get("Name", "").lower() == "appxmetadata\\appxbundlemanifest.xml")
        off, slen, comp = files[manifest_id]
        f.seek(off)
        data = f.read(slen)
    root = ET.fromstring(zlib.decompress(data, -15) if comp == 1 else data)
    for pkg in children(root, "Package"):
        if pkg.get("Type", "application").lower() == "application" and pkg.get("Architecture", "").lower() == ARCH:
            return EAppxPackage(path, int(pkg.get("Offset")))
    archs = sorted({p.get("Architecture", "?") for p in children(root, "Package")})
    raise SystemExit(f"this bundle has no {ARCH} application package (it has: {', '.join(archs)})")


def pick_from_bundle(path, bundle, tmp):
    """The x64 application package inside a bundle: read in place when it is stored uncompressed (bundles store
    their packages that way), extracted to tmp otherwise. A file-like object or path, or None if not a bundle."""
    names = bundle.namelist()
    manifest = next((n for n in names if n.lower() == "appxmetadata/appxbundlemanifest.xml"), None)
    if not manifest:
        return None
    root = ET.fromstring(bundle.read(manifest))
    candidates = []
    for pkg in children(root, "Package"):
        # a bundle may also carry the Store's stub packages (AppxMetadata\Stub\...), small installers of the real one
        if pkg.get("FileName", "").replace("\\", "/").lower().startswith("appxmetadata/"):
            continue
        if pkg.get("Type", "application").lower() == "application" and pkg.get("Architecture", "").lower() == ARCH:
            candidates.append(pkg.get("FileName").replace("\\", "/"))
    if not candidates:
        archs = sorted({p.get("Architecture", "?") for p in children(root, "Package")})
        raise SystemExit(f"this bundle has no {ARCH} application package (it has: {', '.join(archs)})")
    inner = candidates[0]
    member = bundle.getinfo(next(n for n in names if urllib.parse.unquote(n) == inner or n == inner))
    if member.compress_type == zipfile.ZIP_STORED:
        with open_raw(path) as f:
            f.seek(member.header_offset)
            local = f.read(30)
            name_len, extra_len = struct.unpack_from("<HH", local, 26)
        return Slice(path, member.header_offset + 30 + name_len + extra_len, member.file_size)
    out = os.path.join(tmp, os.path.basename(inner))
    with bundle.open(member) as src, open(out, "wb") as dst:
        shutil.copyfileobj(src, dst, 1 << 20)
    return out


def read_manifest(pkg):
    data = pkg.read(next(n for n in pkg.namelist() if n.lower() == "appxmanifest.xml"))
    root = ET.fromstring(data)
    ident = children(root, "Identity")[0]
    app = children(root, "Application")[0]
    names = [e.text for e in children(root, "DisplayName") if e.text]
    pubnames = [e.text for e in children(root, "PublisherDisplayName") if e.text]
    visual = next(iter(children(app, "VisualElements")), None)
    logo = (visual.get("Square44x44Logo") or visual.get("Square150x150Logo")) if visual is not None else None
    deps = [(d.get("Name"), d.get("MinVersion", "0.0.0.0"), d.get("Publisher", ""))
            for d in children(root, "PackageDependency")]
    return {
        "name": ident.get("Name"),
        "publisher": ident.get("Publisher"),
        "version": ident.get("Version"),
        "arch": ident.get("ProcessorArchitecture", "neutral"),
        "resource_id": ident.get("ResourceId", ""),
        "app_id": app.get("Id"),
        "exe": app.get("Executable", "").replace("\\", "/"),
        "title": next((n for n in names if not n.startswith("ms-resource:")), None),
        "publisher_name": next((n for n in pubnames if not n.startswith("ms-resource:")), None),
        "logo": logo.replace("\\", "/") if logo else None,
        "deps": deps,
        # URI schemes the app owns (a browser sign-in comes back through one)
        "protocols": sorted({e.get("Name").lower() for e in children(root, "Protocol") if e.get("Name")}),
    }


def pick_logo(names, logo):
    """The best file for a manifest logo path: the plain unplated 256 px variant if there is one, then the largest
    target size or scale. MRT stores qualified names (Logo.targetsize-256.png) next to the unqualified path."""
    if not logo:
        return None
    base, ext = os.path.splitext(logo.lower())
    best = None
    for n in names:
        rel = urllib.parse.unquote(n)
        low = rel.lower()
        if not (low.startswith(base) and low.endswith(ext)):
            continue
        quals = low[len(base):-len(ext)].lstrip(".")
        size = max([int(x) for x in re.findall(r"(?:targetsize|scale)-(\d+)", quals)] or [0])
        score = (size >= 256 and "unplated" in quals and "lightunplated" not in quals, size, quals == "")
        if best is None or score > best[0]:
            best = (score, n)
    return best[1] if best else None


def extract(pkg, dest):
    members = [m for m in pkg.infolist() if not m.is_dir()]
    total = sum(m.file_size for m in members) or 1
    done = 0
    for m in members:
        # package zips percent-encode names ("%20" for a space); [Content_Types].xml is stored literally
        rel = urllib.parse.unquote(m.filename)
        if rel.startswith("/") or ".." in rel.split("/"):
            raise SystemExit(f"refusing unsafe path in package: {m.filename}")
        out = os.path.join(dest, rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with pkg.open(m) as src, open(out, "wb") as dst:
            # progress in a large file too, so a download that is mostly one file does not look stalled
            while True:
                chunk = src.read(8 << 20)
                if not chunk:
                    break
                dst.write(chunk)
                done += len(chunk)
                say(f"PROGRESS {done * 1000 // total} 1000")
        say(f"PROGRESS {done * 1000 // total} 1000")


def find_framework(dirs, name, min_version, publisher, older=False):
    """The newest x64 framework package for name with version >= min_version in any of dirs (the harvested
    WindowsApps, and stage/frameworks): (directory name, the dir it is in), or None. With older, the newest of any
    version: a package built against a newer framework than the Windows ISO has (VCLibs 14.0.33728 for Shapr3D
    26.180, the ISO's is 14.0.30704) runs on the older one, since these keep their ABI within a major version."""
    pid = publisher_id(publisher) if publisher else None
    best = None
    pat = re.compile(re.escape(name) + r"_([0-9.]+)_" + ARCH + r"__([0-9a-z]{13})$", re.I)
    for parent in dirs:
        for d in os.listdir(parent) if parent and os.path.isdir(parent) else []:
            m = pat.match(d)
            if not m or (pid and m.group(2).lower() != pid):
                continue
            v = version_tuple(m.group(1))
            if (older or v >= version_tuple(min_version)) and (best is None or v > best[0]):
                best = (v, d, parent)
    return best[1:] if best else None


def open_package(path, tmp):
    encrypted = open_eappx(path) or open_msixvc(path)
    if encrypted:
        return encrypted
    try:
        outer = zipfile.ZipFile(open_raw(path))
    except (zipfile.BadZipFile, FileNotFoundError, IsADirectoryError):
        raise SystemExit(f"{path} is not an app package")
    with outer:
        inner = pick_from_bundle(path, outer, tmp)
    return zipfile.ZipFile(inner or open_raw(path))


def info_only(args):
    package, ai = resolve(args.package)
    with tempfile.TemporaryDirectory(prefix="wine-uwp-info-") as tmp:
        with open_package(package, tmp) as pkg:
            info = read_manifest(pkg)
            member = pick_logo(pkg.namelist(), info["logo"])
            if args.icon and member:
                with pkg.open(member) as src, open(args.icon, "wb") as dst:
                    shutil.copyfileobj(src, dst)
    apps_dir = os.path.join(args.windows, "Program Files", "WindowsApps") if args.windows else None
    fw_dir = os.path.join(args.datadir, "stage", "frameworks") if args.datadir else None
    out = {
        "name": info["name"], "title": info["title"] or info["name"], "publisher": info["publisher_name"],
        "version": info["version"], "architecture": info["arch"],
        "icon": args.icon if args.icon and member else None,
        "protocols": info["protocols"],
        "appinstaller": ai and ai["uri"],
        "download": package if is_url(package) else None,
        # available: in the Windows files; older: only an older version is, which is what gets used
        "frameworks": [{"name": n, "min_version": v,
                        "available": bool(find_framework([apps_dir, fw_dir], n, v, p)) if apps_dir else None,
                        "older": bool(find_framework([apps_dir, fw_dir], n, v, p, older=True)) if apps_dir else None}
                       for n, v, p in info["deps"]],
    }
    print(json.dumps(out, indent=1))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("package")
    ap.add_argument("datadir", nargs="?")
    ap.add_argument("--windows", help="the harvested Windows tree (WIN11)")
    ap.add_argument("--info", action="store_true", help="describe the package as JSON, install nothing")
    ap.add_argument("--icon", help="with --info: write the app's logo here")
    ap.add_argument("--latest", action="store_true",
                    help="PACKAGE is an .appinstaller (file or URL): print the version it offers, install nothing")
    ap.add_argument("--stage", help="stage directory name (default: the package name's last part, lowercased)")
    ap.add_argument("--keys-once", help="like --keys, and the file is deleted once read")
    ap.add_argument("--keys", help="an encrypted package's content keys: a file of \"keyid key\" hex lines, or - for "
                    "one \"keyid:key;...\" line on stdin")
    args = ap.parse_args()
    if args.latest:
        print(read_appinstaller(args.package)["version"] or "")
        return 0
    if args.info:
        return info_only(args)
    if not args.datadir or not args.windows:
        ap.error("installing needs DATADIR and --windows")

    apps_dir = os.path.join(args.windows, "Program Files", "WindowsApps")
    sys32 = os.path.join(args.windows, "Windows", "System32")
    with tempfile.TemporaryDirectory(prefix="wine-uwp-install-", dir=args.datadir) as tmp:
        say("STAGE Reading the package")
        package, ai = resolve(args.package)
        appinstaller = ai and ai["uri"]
        if is_url(package):
            say(f"downloading {package}")
        pkg = open_package(package, tmp)
        info = read_manifest(pkg)
        if info["arch"].lower() not in (ARCH, "neutral"):
            raise SystemExit(f"{info['name']} is built for {info['arch']}, not {ARCH}")
        pid = publisher_id(info["publisher"])
        full = f"{info['name']}_{info['version']}_{info['arch']}_{info['resource_id']}_{pid}"
        family = f"{info['name']}_{pid}"
        stage = args.stage or info["name"].rsplit(".", 1)[-1].lower()
        say(f"package {full}")

        # each framework from the Windows files; an older version than the package asks for if that is all there is
        fw_root = os.path.join(args.datadir, "stage", "frameworks")
        missing = []
        frameworks = []
        for name, minv, publisher in info["deps"]:
            found = find_framework([apps_dir, fw_root], name, minv, publisher)
            if not found:
                found = find_framework([apps_dir, fw_root], name, minv, publisher, older=True)
                if found:
                    say(f"framework {name}: wants {minv}, using {found[0]}")
            if found:
                frameworks.append(found)
            else:
                missing.append(f"{name} >= {minv}")
        if missing:
            raise SystemExit("these frameworks are not in the Windows files: " + ", ".join(missing))

        icon_member = pick_logo(pkg.namelist(), info["logo"])
        # built beside the installed copy and swapped in at the end, so a failed update leaves the old app working
        final = os.path.join(args.datadir, "stage", stage)
        dest = final + ".new"
        say(f"STAGE Unpacking {info['title'] or info['name']} {info['version']}")
        if os.path.exists(dest):
            shutil.rmtree(dest)
        os.makedirs(dest)
        extract(pkg, dest)
        if icon_member:
            shutil.copy2(os.path.join(dest, urllib.parse.unquote(icon_member)), os.path.join(dest, ".wine-uwp-icon.png"))
        pkg.close()

    say("STAGE Installing frameworks")
    os.makedirs(fw_root, exist_ok=True)
    for d, parent in frameworks:
        target = os.path.join(fw_root, d)
        if not os.path.isdir(target):
            shutil.copytree(os.path.join(parent, d), target)
        say(f"framework {d}")
    frameworks = [d for d, _ in frameworks]
    # the DLLs an app loads by bare name, next to it (stage-app.sh does the same)
    for d in frameworks:
        src = os.path.join(fw_root, d)
        if d.startswith(("Microsoft.NET.Native.Runtime.", "Microsoft.VCLibs.140.00_")):
            for f in os.listdir(src):
                if f.lower().endswith(".dll"):
                    shutil.copy2(os.path.join(src, f), dest)
        elif d.startswith("Microsoft.NET.Native.Framework."):
            shutil.copy2(os.path.join(src, "SharedLibrary.dll"), dest)
    mrt = next((f for f in os.listdir(sys32) if f.lower() == "mrt100.dll"), None) if os.path.isdir(sys32) else None
    if mrt:
        shutil.copy2(os.path.join(sys32, mrt), dest)

    # launch.env and wineuwp-launch.exe: what lets a plain `wine wineuwp-launch.exe` start the app (Bottles, desktop
    # entries); run.sh computes the same from the stage directory
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    defaults = os.path.join(root, "launch-defaults.env")
    def winpath(p):
        # inside a prefix's drive_c, the C: path: Windows.Storage cannot resolve a Z: path that runs through the
        # prefix directory itself (a Bottles bottle keeps the app there)
        p = os.path.abspath(p)
        if "/drive_c/" in p:
            return "C:\\" + p.split("/drive_c/", 1)[1].replace("/", "\\")
        return "Z:" + p.replace("/", "\\")
    with open(os.path.join(dest, "launch.env"), "w") as f:
        if os.path.exists(defaults):
            f.write(open(defaults).read())
        deps = "".join(f"{d}|{d.split('_')[0]}_{d.rsplit('__', 1)[1]}|{winpath(os.path.join(fw_root, d))};"
                       for d in frameworks)
        f.write(f"export WINE_UWP_PACKAGE_DEPENDENCIES='{deps}'\n")
        f.write("export WINEPATH='" + ":".join(os.path.abspath(os.path.join(fw_root, d)) for d in frameworks) + "'\n")
    shim_dir = os.environ.get("SHIM_DIR") or os.path.join(os.environ.get("UWP_BIN", os.path.join(root, "uwp-bin")),
                                                          "build-probes")
    launcher = os.path.join(shim_dir, "wineuwp-launch.exe")
    if os.path.exists(launcher):
        shutil.copy2(launcher, dest)
    else:
        say(f"warning: no {launcher}; the app starts through run.sh only")

    title = (info["title"] or info["name"].rsplit(".", 1)[-1]).replace("'", "")
    with open(os.path.join(dest, "identity.env"), "w") as f:
        f.write(f"export WINE_UWP_PACKAGE_FULL_NAME={full}\n")
        f.write(f"export WINE_UWP_PACKAGE_FAMILY_NAME={family}\n")
        f.write("# no WINE_UWP_PACKAGE_PATH: run.sh computes it from the stage directory\n")
        f.write(f"export WINE_UWP_AUMID={family}!{info['app_id']}\n")
        f.write(f"export WINE_UWP_WINDOW_TITLE='{title}'\n")
        f.write(f"export UWP_EXE='{info['exe']}'\n")
    with open(os.path.join(dest, ".wine-uwp-protocols"), "w") as f:
        f.writelines(p + "\n" for p in info["protocols"])
    # where updates come from: the vendor's .appinstaller URI, checked by the setup UI (--latest); an update from a
    # plain package keeps the one the installed copy had
    old_uri = os.path.join(final, ".wine-uwp-appinstaller")
    if not appinstaller and os.path.exists(old_uri):
        appinstaller = open(old_uri).read().strip()
    if appinstaller:
        with open(os.path.join(dest, ".wine-uwp-appinstaller"), "w") as f:
            f.write(appinstaller + "\n")
    if os.path.exists(final):
        os.rename(final, final + ".old")
    os.rename(dest, final)
    shutil.rmtree(final + ".old", ignore_errors=True)
    say(f"STAGE Installed {title} {info['version']}")
    say(f"INSTALLED {stage}")


def cleanup_partial(datadir):
    """A failed or cancelled install leaves no half-unpacked copy behind (the app is gigabytes)."""
    stage = os.path.join(datadir, "stage")
    for d in os.listdir(stage) if os.path.isdir(stage) else ():
        if d.endswith((".new", ".old")):
            shutil.rmtree(os.path.join(stage, d), ignore_errors=True)


if __name__ == "__main__":
    import signal
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    try:
        rc = main()
    except BaseException as e:
        failed = not (isinstance(e, SystemExit) and e.code in (0, None))
        a = [x for x in sys.argv[1:] if not x.startswith("-")]
        if failed and "--info" not in sys.argv and len(a) >= 2:
            cleanup_partial(a[1])
        raise
    sys.exit(rc)
