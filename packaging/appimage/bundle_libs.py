#!/usr/bin/env python3
"""Copy the shared libraries a set of ELF files needs, from a Flatpak runtime, into an AppDir.

    bundle_libs.py RUNTIME_LIBDIR DEST_LIBDIR [--also SONAME]... ELF...

Follows DT_NEEDED recursively inside RUNTIME_LIBDIR (the GNOME runtime's lib/x86_64-linux-gnu). --also names
libraries that are dlopen()ed rather than linked (Wine opens most of its host libraries that way). Libraries the host
has to provide are left out (EXCLUDE): glibc, the GL/Vulkan loaders and drivers, X11 and Wayland, fontconfig and
freetype, audio, D-Bus, udev, and the C++ runtime (host GPU drivers need the host's). Everything in the runtime
needs at most GLIBC_2.38, which is the AppImage's floor.
"""

import os
import re
import shutil
import subprocess
import sys

EXCLUDE = re.compile(
    r"^(ld-linux.*|libc|libm|libdl|libpthread|librt|libresolv|libutil|libmvec|libanl|libnsl|"
    r"libGL.*|libEGL.*|libGLX.*|libGLdispatch|libOpenGL|libGLES.*|libdrm.*|libgbm|libvulkan|"
    r"libX.*|libxcb.*|libxkbcommon.*|libxkbregistry|libxshmfence|libwayland.*|"
    r"libfontconfig|libfreetype|libasound|libpulse.*|libdbus-1|libudev|libsystemd|"
    r"libstdc\+\+|libgcc_s|ntdll|win32u)\.so")


def needed(path):
    out = subprocess.run(["objdump", "-p", path], capture_output=True, text=True).stdout
    return re.findall(r"^\s*NEEDED\s+(\S+)", out, re.M)


def main():
    args = sys.argv[1:]
    runtime, dest = args[0], args[1]
    also, elves = [], []
    it = iter(args[2:])
    for a in it:
        if a == "--also":
            also.append(next(it))
        else:
            elves.append(a)
    os.makedirs(dest, exist_ok=True)
    todo = list(also)
    for e in elves:
        todo += needed(e)
    seen, copied, missing = set(), [], []
    while todo:
        name = todo.pop()
        if name in seen or EXCLUDE.match(name):
            continue
        seen.add(name)
        src = os.path.join(runtime, name)
        if not os.path.exists(src):
            missing.append(name)
            continue
        real = os.path.realpath(src)
        shutil.copy2(real, os.path.join(dest, name))
        copied.append(name)
        todo += needed(real)
    print(f"bundled {len(copied)} libraries" + (f"; not in the runtime (host must have them): {' '.join(sorted(missing))}"
                                               if missing else ""))


if __name__ == "__main__":
    main()
