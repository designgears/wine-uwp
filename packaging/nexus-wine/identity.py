#!/usr/bin/env python3
"""Package identity for an executable installed by Xodus Nexus, as shell exports.

    identity.py EXE

Walks up from EXE to the directory holding the package's AppxManifest.xml and prints what install-app.py writes into
identity.env (full and family name, AUMID, title) plus WINE_UWP_PACKAGE_PATH, so a game Nexus installed gets the
same identity as one our installer staged. Exits 1 when EXE belongs to no package."""
import importlib.util
import os
import shlex
import sys
import xml.etree.ElementTree as ET

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("install_app", os.path.join(here, "..", "install-app.py"))
install_app = importlib.util.module_from_spec(spec)
spec.loader.exec_module(install_app)


def unix_path(p):
    """A Windows path Nexus may pass (Z:\\...) as a host path."""
    if len(p) > 2 and p[1] == ":" and p[0].upper() == "Z":
        return p[2:].replace("\\", "/")
    return p


def package_root(exe):
    d = os.path.dirname(os.path.abspath(unix_path(exe)))
    while d and d != "/":
        for name in os.listdir(d):
            if name.lower() == "appxmanifest.xml":
                return d, os.path.join(d, name)
        d = os.path.dirname(d)
    return None, None


def main():
    root, manifest = package_root(sys.argv[1])
    if not manifest:
        return 1
    tree = ET.parse(manifest).getroot()
    ident = install_app.children(tree, "Identity")[0]
    apps = install_app.children(tree, "Application")
    name, publisher = ident.get("Name"), ident.get("Publisher", "")
    version, arch = ident.get("Version"), ident.get("ProcessorArchitecture", "neutral")
    pid = install_app.publisher_id(publisher)
    full = f"{name}_{version}_{arch}_{ident.get('ResourceId', '')}_{pid}"
    family = f"{name}_{pid}"
    app_id = apps[0].get("Id") if apps else "App"
    titles = [e.text for e in install_app.children(tree, "DisplayName") if e.text and not e.text.startswith("ms-resource:")]
    exports = {
        "WINE_UWP_PACKAGE_FULL_NAME": full,
        "WINE_UWP_PACKAGE_FAMILY_NAME": family,
        "WINE_UWP_PACKAGE_PATH": "Z:" + root.replace("/", "\\"),
        "WINE_UWP_AUMID": f"{family}!{app_id}",
        "WINE_UWP_WINDOW_TITLE": titles[0] if titles else name,
    }
    for k, v in exports.items():
        print(f"export {k}={shlex.quote(v)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
