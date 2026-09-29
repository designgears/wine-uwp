#!/usr/bin/env python3
"""Install a UWP app package (.msixbundle, .appxbundle, .msix or .appx) for wine-uwp.

    install-app.py PACKAGE DATADIR --windows TREE [--stage NAME]

This is the registration step Windows' deployment service does, reduced to what wine-uwp needs:

  1. from a bundle, take the x64 application package (resource packages are not needed: the main package's
     resources.pri carries the neutral resources, and a package staged out of a Windows install ran the same way);
  2. unpack it into DATADIR/stage/NAME, undoing the zip's percent-encoding of file names;
  3. find every framework it declares (PackageDependency) in TREE/Program Files/WindowsApps, the harvested
     Windows image, and copy the newest one that satisfies MinVersion into DATADIR/stage/frameworks, where run.sh
     puts it on the package graph;
  4. copy the framework DLLs the app loads by bare name next to it, as stage-app.sh does;
  5. write DATADIR/stage/NAME/identity.env, which run.sh sources: package full and family name, AUMID, entry exe
     and window title.

Progress goes to stdout as "STAGE <what>" and "PROGRESS <done> <total>" lines for the setup wizard. The last line
is "INSTALLED <stage name>". Only the standard library is used.
"""

import argparse
import hashlib
import os
import re
import shutil
import sys
import tempfile
import urllib.parse
import xml.etree.ElementTree as ET
import zipfile

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


def pick_from_bundle(bundle, tmp):
    """The x64 application package inside a bundle, extracted to tmp. Its path, or None if not a bundle."""
    names = bundle.namelist()
    manifest = next((n for n in names if n.lower() == "appxmetadata/appxbundlemanifest.xml"), None)
    if not manifest:
        return None
    root = ET.fromstring(bundle.read(manifest))
    candidates = []
    for pkg in children(root, "Package"):
        if pkg.get("Type", "application").lower() == "application" and pkg.get("Architecture", "").lower() == ARCH:
            candidates.append(pkg.get("FileName"))
    if not candidates:
        archs = sorted({p.get("Architecture", "?") for p in children(root, "Package")})
        raise SystemExit(f"this bundle has no {ARCH} application package (it has: {', '.join(archs)})")
    inner = candidates[0]
    member = next(n for n in names if urllib.parse.unquote(n) == inner or n == inner)
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
        "deps": deps,
    }


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
            shutil.copyfileobj(src, dst, 1 << 20)
        done += m.file_size
        say(f"PROGRESS {done * 100 // total} 100")


def find_framework(apps_dir, name, min_version, publisher):
    """The newest x64 framework package directory for name with version >= min_version, or None."""
    pid = publisher_id(publisher) if publisher else None
    best = None
    pat = re.compile(re.escape(name) + r"_([0-9.]+)_" + ARCH + r"__([0-9a-z]{13})$", re.I)
    for d in os.listdir(apps_dir) if os.path.isdir(apps_dir) else []:
        m = pat.match(d)
        if not m or (pid and m.group(2).lower() != pid):
            continue
        v = version_tuple(m.group(1))
        if v >= version_tuple(min_version) and (best is None or v > best[0]):
            best = (v, d)
    return best[1] if best else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("package")
    ap.add_argument("datadir")
    ap.add_argument("--windows", required=True, help="the harvested Windows tree (WIN11)")
    ap.add_argument("--stage", help="stage directory name (default: the package name's last part, lowercased)")
    args = ap.parse_args()

    apps_dir = os.path.join(args.windows, "Program Files", "WindowsApps")
    sys32 = os.path.join(args.windows, "Windows", "System32")
    with tempfile.TemporaryDirectory(prefix="wine-uwp-install-", dir=args.datadir) as tmp:
        say("STAGE Reading the package")
        try:
            outer = zipfile.ZipFile(args.package)
        except zipfile.BadZipFile:
            raise SystemExit(f"{args.package} is not an app package (not a zip archive)")
        with outer:
            inner = pick_from_bundle(outer, tmp)
        pkg = zipfile.ZipFile(inner or args.package)
        info = read_manifest(pkg)
        if info["arch"].lower() not in (ARCH, "neutral"):
            raise SystemExit(f"{info['name']} is built for {info['arch']}, not {ARCH}")
        pid = publisher_id(info["publisher"])
        full = f"{info['name']}_{info['version']}_{info['arch']}_{info['resource_id']}_{pid}"
        family = f"{info['name']}_{pid}"
        stage = args.stage or info["name"].rsplit(".", 1)[-1].lower()
        say(f"package {full}")

        missing = []
        frameworks = []
        for name, minv, publisher in info["deps"]:
            d = find_framework(apps_dir, name, minv, publisher)
            if d:
                frameworks.append(d)
            else:
                missing.append(f"{name} >= {minv}")
        if missing:
            raise SystemExit("these frameworks are not in the Windows files: " + ", ".join(missing))

        dest = os.path.join(args.datadir, "stage", stage)
        say(f"STAGE Unpacking {info['title'] or info['name']} {info['version']}")
        if os.path.exists(dest):
            shutil.rmtree(dest)
        os.makedirs(dest)
        extract(pkg, dest)
        pkg.close()

    say("STAGE Installing frameworks")
    fw_root = os.path.join(args.datadir, "stage", "frameworks")
    os.makedirs(fw_root, exist_ok=True)
    for d in frameworks:
        target = os.path.join(fw_root, d)
        if not os.path.isdir(target):
            shutil.copytree(os.path.join(apps_dir, d), target)
        say(f"framework {d}")
    # the DLLs an app loads by bare name, next to it (stage-app.sh does the same)
    for d in frameworks:
        src = os.path.join(apps_dir, d)
        if d.startswith(("Microsoft.NET.Native.Runtime.", "Microsoft.VCLibs.140.00_")):
            for f in os.listdir(src):
                if f.lower().endswith(".dll"):
                    shutil.copy2(os.path.join(src, f), dest)
        elif d.startswith("Microsoft.NET.Native.Framework."):
            shutil.copy2(os.path.join(src, "SharedLibrary.dll"), dest)
    mrt = next((f for f in os.listdir(sys32) if f.lower() == "mrt100.dll"), None) if os.path.isdir(sys32) else None
    if mrt:
        shutil.copy2(os.path.join(sys32, mrt), dest)

    title = (info["title"] or info["name"].rsplit(".", 1)[-1]).replace("'", "")
    with open(os.path.join(dest, "identity.env"), "w") as f:
        f.write(f"export WINE_UWP_PACKAGE_FULL_NAME={full}\n")
        f.write(f"export WINE_UWP_PACKAGE_FAMILY_NAME={family}\n")
        f.write("# no WINE_UWP_PACKAGE_PATH: run.sh computes it from the stage directory\n")
        f.write(f"export WINE_UWP_AUMID={family}!{info['app_id']}\n")
        f.write(f"export WINE_UWP_WINDOW_TITLE='{title}'\n")
        f.write(f"export UWP_EXE='{info['exe']}'\n")
    say(f"STAGE Installed {title} {info['version']}")
    say(f"INSTALLED {stage}")


if __name__ == "__main__":
    sys.exit(main())
