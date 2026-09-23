#!/usr/bin/env bash
# Deliver a URI to the running packaged app as a Protocol activation, the way Windows' shell hands
# a scheme it owns (shapr3d:...) to the instance that is already running. wineuwp watches this file
# (WINE_UWP_ACTIVATE_INBOX, default C:\users\Public\wineuwp-activate.uri) and deletes it once read.
#
# Usage: ./activate-uri.sh 'shapr3d://...'
# A browser's sign-in redirect can be pasted here, or a host handler for the scheme can call it.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]:-$0}")/./env.sh"
[ $# -eq 1 ] || { echo "usage: $0 <uri>" >&2; exit 2; }
inbox="$WINEPREFIX/drive_c/users/Public/wineuwp-activate.uri"
printf '%s' "$1" > "$inbox.tmp" && mv -f "$inbox.tmp" "$inbox"
echo "queued for the running app: $1"
