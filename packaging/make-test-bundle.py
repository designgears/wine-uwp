#!/usr/bin/env python3
"""Rebuild an .msixbundle from a staged package, to test the installer without downloading the app.

    make-test-bundle.py STAGE_DIR OUT.msixbundle

Every file the package's own AppxBlockMap.xml lists goes into an x64 .msix (stored, names percent-encoded as in
a real package, plus AppxManifest.xml, AppxBlockMap.xml, AppxSignature.p7x, [Content_Types].xml and AppxMetadata/),
and that .msix into a bundle whose AppxBundleManifest.xml also names an arm64 package, so the installer has to
pick. install-app.py then installs it like the real download; appxverify.py checks the result against the block map.
"""

import os
import re
import sys
import tempfile
import urllib.parse
import zipfile


def main():
    stage, out = sys.argv[1], sys.argv[2]
    man = open(os.path.join(stage, "AppxManifest.xml"), "rb").read().decode("utf-8", "replace")
    ident = re.search(r"<Identity\b[^>]*>", man).group(0)
    name = re.search(r'Name="([^"]+)"', ident).group(1)
    version = re.search(r'Version="([^"]+)"', ident).group(1)
    blockmap = open(os.path.join(stage, "AppxBlockMap.xml"), "rb").read().decode("utf-8", "replace")
    files = [urllib.parse.unquote(f).replace("\\", "/") for f in re.findall(r'<File Name="([^"]*)"', blockmap)]
    extra = ["AppxManifest.xml", "AppxBlockMap.xml", "AppxSignature.p7x", "[Content_Types].xml"]
    meta = os.path.join(stage, "AppxMetadata")
    if os.path.isdir(meta):
        extra += ["AppxMetadata/" + f for f in os.listdir(meta)]
    inner = f"{name}_{version}_x64.msix"
    with tempfile.TemporaryDirectory(dir=os.path.dirname(os.path.abspath(out))) as tmp:
        msix = os.path.join(tmp, inner)
        with zipfile.ZipFile(msix, "w", zipfile.ZIP_STORED) as z:
            for f in dict.fromkeys(files + extra):   # the manifest is in the block map too
                if not os.path.exists(os.path.join(stage, f)):
                    continue
                arc = f if f == "[Content_Types].xml" else urllib.parse.quote(f, safe="/")
                z.write(os.path.join(stage, f), arc)
        bundle_manifest = f'''<?xml version="1.0" encoding="UTF-8"?>
<Bundle xmlns="http://schemas.microsoft.com/appx/2013/bundle" SchemaVersion="5.0">
<Identity Name="{name}" Publisher="CN=test" Version="{version}"/>
<Packages>
<Package Type="application" Version="{version}" Architecture="arm64" FileName="{name}_{version}_arm64.msix" Offset="0" Size="1"/>
<Package Type="application" Version="{version}" Architecture="x64" FileName="{inner}" Offset="0" Size="1"/>
</Packages></Bundle>'''
        with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
            z.writestr("AppxMetadata/AppxBundleManifest.xml", bundle_manifest)
            z.write(msix, inner)
    print(f"{out}: {name} {version}, {len(files)} files")


if __name__ == "__main__":
    main()
