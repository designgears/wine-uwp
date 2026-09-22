#!/usr/bin/env python
"""Verify a staged appx file against the package's own AppxBlockMap.xml.

  appxverify.py <stage-dir> [file ...]        default: every file in the block map

The block map carries a SHA256 per 64 KiB of each file's uncompressed content, which is the only
way to tell a byte-patched app binary from a pristine one once the backup is gone - `pecheck.py`
cannot help, because an appx payload has no Authenticode signature and no PE checksum of its own.
"""
import os, sys, hashlib, base64
import xml.etree.ElementTree as ET

BLOCK = 64 * 1024

def main():
    stage = sys.argv[1]
    want = set(sys.argv[2:])
    root = ET.parse(os.path.join(stage, "AppxBlockMap.xml")).getroot()
    ns = {"b": root.tag.split("}")[0].strip("{")}
    bad = ok = missing = 0
    for f in root.findall("b:File", ns):
        name = f.get("Name").replace("\\", "/")
        if want and os.path.basename(name) not in want and name not in want: continue
        path = os.path.join(stage, name)
        if not os.path.exists(path):
            print("missing  %s" % name); missing += 1; continue
        hashes = [b.get("Hash") for b in f.findall("b:Block", ns)]
        with open(path, "rb") as fh:
            i, good = 0, True
            while True:
                chunk = fh.read(BLOCK)
                if not chunk: break
                h = base64.b64encode(hashlib.sha256(chunk).digest()).decode()
                if i >= len(hashes) or h != hashes[i]:
                    print("MODIFIED %s  block %d (offset %#x)" % (name, i, i * BLOCK))
                    good = False
                i += 1
            if i != len(hashes):
                print("MODIFIED %s  block count %d, expected %d" % (name, i, len(hashes)))
                good = False
        if good: ok += 1
        else: bad += 1
    print("%d pristine, %d modified, %d missing" % (ok, bad, missing))
    sys.exit(1 if bad else 0)

main()
