"""Where the project's data lives, for the Python tools.

The repo now contains its own `uwp-bin` and `prefix`, so a clone plus those two directories is the
whole environment. Both are overridable by environment variable so the tools still work against a
setup elsewhere; the fallback is the repo they are sitting in, not `~`, because the home directory
no longer has any of this.

`~/src/wine` is deliberately *not* here: it is shared with another session and stays put.
"""
import os

ROOT = os.path.dirname(os.path.abspath(__file__))

UWP_BIN = os.environ.get("UWP_BIN", os.path.join(ROOT, "uwp-bin"))
PREFIX = os.environ.get("WINEPREFIX", os.path.join(ROOT, "prefix"))
PDB = os.environ.get("UWP_PDB_DIR", os.path.join(UWP_BIN, "pdb"))
WINE_BUILD = os.environ.get("WINE_BUILD", os.path.expanduser("~/src/wine-build"))
SYSTEM32 = os.environ.get("WINE_UWP_SYSTEM32", os.path.join(PREFIX, "drive_c", "windows", "system32"))
