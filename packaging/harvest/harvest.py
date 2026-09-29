#!/usr/bin/env python3
"""Harvest the Windows files wine-uwp needs from a Windows 11 ISO, without root.

    harvest.py ISO OUTDIR [--manifest manifest.txt] [--keep-wim]

OUTDIR ends up shaped like uwp-bin/win11: OUTDIR/Windows/System32/..., Windows/Fonts/..., and so on. The files
listed in the manifest (paths relative to the Windows image root) are extracted from sources/install.wim (or
install.esd) with 7-Zip, matched case-insensitively. Progress goes to stdout as "STAGE <name>" and "PROGRESS <done>
<total>" lines, which is what the setup wizard reads; everything else is plain text.

Nothing here is Microsoft's: the ISO comes from Microsoft's own server on the user's machine, and only the listed
files are taken out of it.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def say(*args):
    print(*args, flush=True)


def seven_zip():
    for name in ("7z", "7zz", "7za"):
        path = shutil.which(name)
        if path:
            return path
    raise SystemExit("7-Zip (7z) is required")


def list_archive(sz, archive):
    """Paths in an archive, from 7z's technical listing."""
    out = subprocess.run([sz, "l", "-slt", "-ba", archive], check=True, capture_output=True, text=True,
                         errors="replace").stdout
    paths = []
    for line in out.splitlines():
        if line.startswith("Path = "):
            paths.append(line[7:].replace("\\", "/"))
    return paths


def extract_wim(sz, iso, tmp):
    """Pull sources/install.wim (or .esd) out of the ISO."""
    names = list_archive(sz, iso)
    wim = next((n for n in names if n.lower() in ("sources/install.wim", "sources/install.esd")), None)
    if not wim:
        raise SystemExit("no sources/install.wim or install.esd in this ISO - is it a Windows 11 image?")
    say("STAGE Unpacking the Windows image")
    subprocess.run([sz, "e", "-y", f"-o{tmp}", iso, wim], check=True, stdout=subprocess.DEVNULL)
    return os.path.join(tmp, os.path.basename(wim))


def pick_image(names):
    """A multi-image WIM prefixes paths with the image index; take the first image that has Windows/."""
    prefixes = sorted({n.split("/", 1)[0] for n in names if "/" in n and n.split("/", 1)[0].isdigit()})
    for p in prefixes:
        if f"{p}/Windows/System32/ntdll.dll".lower() in (n.lower() for n in names if n.startswith(p + "/")):
            return p + "/"
    return ""   # single-image WIM: paths start at the image root


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso")
    ap.add_argument("outdir")
    ap.add_argument("--manifest", default=os.path.join(HERE, "manifest.txt"))
    ap.add_argument("--keep-wim", action="store_true")
    ap.add_argument("--wim", help="an already extracted install.wim/esd (skips unpacking the ISO)")
    args = ap.parse_args()

    sz = seven_zip()
    wanted = [l.strip() for l in open(args.manifest) if l.strip() and not l.startswith("#")]
    tmp = tempfile.mkdtemp(prefix="wine-uwp-harvest-", dir=os.path.dirname(os.path.abspath(args.outdir)))
    try:
        wim = args.wim or extract_wim(sz, args.iso, tmp)
        say("STAGE Reading the image's file list")
        names = list_archive(sz, wim)
        prefix = pick_image(names)
        by_lower = {}
        for n in names:
            if n.startswith(prefix):
                by_lower.setdefault(n[len(prefix):].lower(), n)
        found, missing = [], []
        for w in wanted:
            src = by_lower.get(w.lower())
            (found if src else missing).append((w, src))
        say(f"{len(found)} of {len(wanted)} files are in the image")

        say("STAGE Extracting files")
        listfile = os.path.join(tmp, "list.txt")
        with open(listfile, "w") as f:
            for _, src in found:
                f.write(src + "\n")
        staging = os.path.join(tmp, "x")
        proc = subprocess.Popen([sz, "x", "-y", "-bsp1", f"-o{staging}", wim, f"@{listfile}"],
                                stdout=subprocess.PIPE, text=True, errors="replace")
        for line in proc.stdout:
            line = line.strip()
            if "%" in line:
                pct = line.split("%", 1)[0].strip().split()[-1]
                if pct.isdigit():
                    say(f"PROGRESS {pct} 100")
        if proc.wait():
            raise SystemExit("7-Zip failed to extract the files")

        # move into the manifest's own casing, which install-overrides.sh and the generators expect
        for w, src in found:
            dst = os.path.join(args.outdir, w)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.move(os.path.join(staging, src), dst)
        with open(os.path.join(args.outdir, "harvest-missing.txt"), "w") as f:
            f.writelines(w + "\n" for w, _ in missing)
        say(f"STAGE Done: {len(found)} files, {len(missing)} not in this image (harvest-missing.txt)")
        if args.keep_wim and not args.wim:
            shutil.move(wim, os.path.join(args.outdir, os.path.basename(wim)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
