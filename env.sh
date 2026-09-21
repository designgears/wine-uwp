# Shared paths for every script in this repo. Source it; do not run it.
#
# Everything the project owns now lives inside the repo, so a clone plus these two directories is
# the whole environment. Both are overridable, because the machine that has them is not necessarily
# the machine reading this:
#
#   UWP_BIN     staged apps, the Windows image we borrow DLLs from, PDBs, built probes/shims
#   WINEPREFIX  the live Wine prefix
#
# The Wine *checkout* stays outside the repo because it is a large build tree that predates it;
# our Wine source is vendored in wine/ and synced with sync-wine.sh. WINE_SRC / WINE_BIN_DIR point
# at the checkout and can be overridden.
WINE_UWP_ROOT=${WINE_UWP_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)}
export WINE_UWP_ROOT
export UWP_BIN=${UWP_BIN:-$WINE_UWP_ROOT/uwp-bin}
export WINEPREFIX=${WINEPREFIX:-$WINE_UWP_ROOT/prefix}
export WINE_SRC=${WINE_SRC:-$HOME/src/wine}
export WINE_BIN_DIR=${WINE_BIN_DIR:-$HOME/src/wine-build/}
