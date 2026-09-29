#!/usr/bin/env python3
"""Resolve the few Windows-internal symbols wine-uwp needs, for whatever Windows build was harvested.

    resolve_symbols.py SYSTEM32_DIR OUT_TABLE [--cache DIR]

wineuwp has to find a handful of non-exported things inside Microsoft's DLLs (CoreApplicationView's private
vtables, gPsmRegistered, a few DXamlCore functions). Their addresses change with every Windows build, so instead of
baking them in, this reads each DLL's CodeView record, fetches the matching PDB from Microsoft's public symbol server
(msdl.microsoft.com, the same place any debugger gets it), and looks the names up in the PDB's public symbols.

The table has one line per symbol:  <dll> <timedatestamp> <sizeofimage> <rva> <name>
wineuwp only uses a line whose stamp and size match the DLL it has loaded.

No third-party modules: the PE header and the PDB (MSF 7.00) are parsed here directly.
"""

import argparse
import os
import struct
import sys
import urllib.request

# DLL -> public symbol names (MSVC-decorated) wineuwp looks up
WANTED = {
    "twinapi.appcore.dll": [
        "?gPsmRegistered@CoreApplication@Core@ApplicationModel@Windows@@0KC",
        "??_7CoreApplicationView@Core@ApplicationModel@Windows@@6BICoreApplicationView@123@@",
        "PREFIX:??_7CoreApplicationView@Core@ApplicationModel@Windows@@6B?$ImplementsHelper@U?$RuntimeClassFlags@$02@WRL@Microsoft@@$00U?$CloakedIid@UICoreApplicationViewInternal@",
        "PREFIX:??_7CoreApplicationView@Core@ApplicationModel@Windows@@6B?$ImplementsHelper@U?$RuntimeClassFlags@$02@WRL@Microsoft@@$00UICoreApplicationViewInternal3@",
        "PREFIX:??_7CoreApplicationView@Core@ApplicationModel@Windows@@6B?$ImplementsHelper@U?$RuntimeClassFlags@$02@WRL@Microsoft@@$00U?$CloakedIid@UICoreApplicationView_Activation@",
    ],
    "windows.ui.xaml.dll": [
        "?GetCurrent@DXamlCore@DirectUI@@SAPEAV12@XZ",
        "?ConfigureCoreWindow@DXamlCore@DirectUI@@QEAAJPEAUICoreWindow@Core@UI@Windows@@_N@Z",
        "?GetCurrentNoRef@FrameworkApplication@DirectUI@@SAPEAV12@XZ",
        "?InvokeOnWindowCreated@FrameworkApplication@DirectUI@@QEAAJXZ",
    ],
}
# short names wineuwp uses for the PREFIX: entries (the full decorated names are long and build-stable only in prefix)
SHORT = {
    "UICoreApplicationViewInternal@": "vtbl.ICoreApplicationViewInternal",
    "UICoreApplicationViewInternal3@": "vtbl.ICoreApplicationViewInternal3",
    "UICoreApplicationView_Activation@": "vtbl.ICoreApplicationView_Activation",
}
SYMSRV = "https://msdl.microsoft.com/download/symbols"


def pe_info(path):
    """TimeDateStamp, SizeOfImage, section RVAs and the CodeView (RSDS) record of a PE file."""
    with open(path, "rb") as f:
        data = f.read()
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    nsec, = struct.unpack_from("<H", data, pe + 6)
    stamp, = struct.unpack_from("<I", data, pe + 8)
    optsize, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    size_of_image, = struct.unpack_from("<I", data, opt + 56)
    ndirs, = struct.unpack_from("<I", data, opt + 108)
    dbg_rva, dbg_size = struct.unpack_from("<II", data, opt + 112 + 6 * 8) if ndirs > 6 else (0, 0)
    secs = []
    for i in range(nsec):
        s = opt + optsize + i * 40
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, s + 8)
        secs.append((va, vsize, rawptr, rawsize))

    def rva_to_off(rva):
        for va, vsize, rawptr, rawsize in secs:
            if va <= rva < va + max(vsize, rawsize):
                return rawptr + rva - va
        return None

    rsds = None
    off = rva_to_off(dbg_rva) if dbg_rva else None
    for i in range(dbg_size // 28 if off is not None else 0):
        typ, size, rva, ptr = struct.unpack_from("<12xIIII", data, off + i * 28)
        if typ == 2 and data[ptr:ptr + 4] == b"RSDS":
            guid = data[ptr + 4:ptr + 20]
            age, = struct.unpack_from("<I", data, ptr + 20)
            name = data[ptr + 24:data.index(b"\0", ptr + 24)].decode().replace("\\", "/").rsplit("/", 1)[-1]
            d1, d2, d3 = struct.unpack_from("<IHH", guid)
            sig = f"{d1:08X}{d2:04X}{d3:04X}{guid[8:].hex().upper()}{age:X}"
            rsds = (name, sig)
    return stamp, size_of_image, [s[0] for s in secs], rsds


def fetch_pdb(name, sig, cache):
    os.makedirs(cache, exist_ok=True)
    out = os.path.join(cache, f"{sig}-{name}")
    if not os.path.exists(out):
        req = urllib.request.Request(f"{SYMSRV}/{name}/{sig}/{name}",
                                     headers={"User-Agent": "Microsoft-Symbol-Server/10.0.0.0"})
        with urllib.request.urlopen(req, timeout=120) as resp, open(out + ".part", "wb") as f:
            while chunk := resp.read(1 << 20):
                f.write(chunk)
        os.rename(out + ".part", out)
    return out


def msf_streams(path):
    """Read an MSF 7.00 file and return a function giving the bytes of stream n."""
    f = open(path, "rb")
    sb = f.read(56)
    if not sb.startswith(b"Microsoft C/C++ MSF 7.00\r\n\x1aDS"):
        raise ValueError(f"{path}: not an MSF 7.00 PDB")
    block, _, _, dir_bytes, _, map_addr = struct.unpack_from("<6I", sb, 32)

    def read_blocks(blocks, length):
        buf = bytearray()
        for b in blocks:
            f.seek(b * block)
            buf += f.read(block)
        return bytes(buf[:length])

    ndir = (dir_bytes + block - 1) // block
    f.seek(map_addr * block)
    dir_blocks = struct.unpack(f"<{ndir}I", f.read(4 * ndir))
    d = read_blocks(dir_blocks, dir_bytes)
    nstreams, = struct.unpack_from("<I", d, 0)
    sizes = struct.unpack_from(f"<{nstreams}I", d, 4)
    pos = 4 + 4 * nstreams
    blocks = []
    for s in sizes:
        n = 0 if s == 0xffffffff else (s + block - 1) // block
        blocks.append(struct.unpack_from(f"<{n}I", d, pos))
        pos += 4 * n
    return lambda i: read_blocks(blocks[i], 0 if sizes[i] == 0xffffffff else sizes[i])


def pdb_publics(path, sections):
    """Public symbols (S_PUB32) of a PDB as {name: rva}."""
    stream = msf_streams(path)
    dbi = stream(3)
    sym_stream, = struct.unpack_from("<H", dbi, 20)   # DBI header: SymRecordStream
    recs = stream(sym_stream)
    out, pos = {}, 0
    while pos + 4 <= len(recs):
        length, kind = struct.unpack_from("<HH", recs, pos)
        if kind == 0x110E:   # S_PUB32: flags, offset, segment, name
            _, offset, seg = struct.unpack_from("<IIH", recs, pos + 4)
            end = recs.index(b"\0", pos + 14)
            if 0 < seg <= len(sections):
                out[recs[pos + 14:end].decode("latin-1")] = sections[seg - 1] + offset
        pos += 2 + length
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("system32")
    ap.add_argument("out")
    # XDG_CACHE_HOME: inside a Flatpak that is the app's own cache, the only one it may write
    cache = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
    ap.add_argument("--cache", default=os.path.join(cache, "wine-uwp", "pdb"))
    args = ap.parse_args()
    listing = {n.lower(): n for n in os.listdir(args.system32)}
    lines, failed = [], 0
    for dll, names in WANTED.items():
        if dll not in listing:
            print(f"missing {dll}", file=sys.stderr)
            failed += 1
            continue
        stamp, size, sections, rsds = pe_info(os.path.join(args.system32, listing[dll]))
        if not rsds:
            print(f"{dll}: no CodeView record", file=sys.stderr)
            failed += 1
            continue
        print(f"STAGE Fetching symbols for {dll}", flush=True)
        syms = pdb_publics(fetch_pdb(*rsds, args.cache), sections)
        for want in names:
            if want.startswith("PREFIX:"):
                prefix = want[7:]
                hits = [n for n in syms if n.startswith(prefix)]
                key = next(v for k, v in SHORT.items() if prefix.endswith(k))
            else:
                hits = [want] if want in syms else []
                key = want
            if len(hits) != 1:
                print(f"{dll}: {len(hits)} matches for {want[:80]}", file=sys.stderr)
                failed += 1
                continue
            lines.append(f"{dll} {stamp:08x} {size:x} {syms[hits[0]]:x} {key}\n")
    with open(args.out, "w") as f:
        f.writelines(lines)
    print(f"{len(lines)} symbols written to {args.out}" + (f", {failed} FAILED" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
