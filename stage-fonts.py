#!/usr/bin/env python3
"""Materialise Segoe UI Variable's optical-size families as static fonts.

Windows 11's default UI font is "Segoe UI Variable Text", and DirectWrite presents the single
SegUIVar.ttf as three families - Small, Text and Display - by reading the variable font's named
instances. Wine's font enumeration exposes only the one family the name table declares, so every
request for a Win11-era UI font misses and falls back (see Findings-M4.md, probe-glyph). Instancing
the named instances into static fonts with the family names Windows reports gives Wine something to
find, without touching Wine's variable-font support.

Not wired into install-overrides.sh: staging these needs registry entries as well (Wine builds its
font list from the registry, not from a directory scan), and the combination has not been shown to
be harmless - see Findings-M4.md. Run it by hand if you want to experiment.

Usage: ./stage-fonts.py [path-to-SegUIVar.ttf] [output-dir]
Defaults: /mnt/win/Windows/Fonts/SegUIVar.ttf -> $UWP_BIN/win11/Windows/Fonts

The registry side, for reference - bare filenames resolve against the Fonts directory:
  [HKLM\Software\Microsoft\Windows NT\CurrentVersion\Fonts]
  "SegoeUIVariableTextRegular (TrueType)"="SegoeUIVariableTextRegular.ttf"
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uwppaths
import os, sys
from fontTools.ttLib import TTFont

SRC = sys.argv[1] if len(sys.argv) > 1 else "/mnt/win/Windows/Fonts/SegUIVar.ttf"
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(uwppaths.UWP_BIN, "win11/Windows/Fonts")

# Windows groups the named instances by optical size; the weight names inside a group become the
# style. "Regular" with no suffix is the Text group's regular - the default UI font.
GROUPS = {"Small": "Segoe UI Variable Small",
          "Text": "Segoe UI Variable Text",
          "Display": "Segoe UI Variable Display"}
# Only Regular and Bold can live in one legacy family; the rest need their own, with the
# typographic names (16/17) carrying the real grouping. Wine reads those when they are present.
RIBBI = {"Regular", "Bold"}
WEIGHT_VALUE = {"Light": 300, "Semilight": 350, "Regular": 400, "Semibold": 600, "Bold": 700}


def set_name(name_table, string, nameID):
    for rec in name_table.names:
        if rec.nameID == nameID:
            rec.string = string
    if not any(r.nameID == nameID for r in name_table.names):
        name_table.setName(string, nameID, 3, 1, 0x409)


def main():
    os.makedirs(OUT, exist_ok=True)
    base = TTFont(SRC)
    fvar = base["fvar"]
    written = 0

    for inst in fvar.instances:
        style = base["name"].getDebugName(inst.subfamilyNameID)
        parts = style.split()
        group = parts[-1] if parts[-1] in GROUPS else "Text"
        weight = " ".join(parts[:-1]) if parts[-1] in GROUPS else style
        weight = weight or "Regular"
        family = GROUPS[group]

        font = TTFont(SRC)
        # make this named instance the font's default position
        for axis in font["fvar"].axes:
            if axis.axisTag in inst.coordinates:
                axis.defaultValue = inst.coordinates[axis.axisTag]
        name = font["name"]
        # typographic family/subfamily: the grouping Windows reports
        set_name(name, family, 16)
        set_name(name, weight, 17)
        # legacy family/subfamily: same family for Regular and Bold, a per-weight family otherwise
        if weight in RIBBI:
            set_name(name, family, 1)
            set_name(name, weight, 2)
        else:
            set_name(name, "%s %s" % (family, weight), 1)
            set_name(name, "Regular", 2)
        full = "%s %s" % (family, weight)
        set_name(name, full, 4)
        set_name(name, full.replace(" ", ""), 6)
        font["OS/2"].usWeightClass = WEIGHT_VALUE.get(weight, 400)

        path = os.path.join(OUT, full.replace(" ", "") + ".ttf")
        font.save(path)
        written += 1
        print("  %-34s -> %s" % (full, os.path.basename(path)))

    print("wrote %d fonts to %s" % (written, OUT))


if __name__ == "__main__":
    main()
