#!/usr/bin/env bash
# Finish an app's web sign-in: open the URL the app handed to Windows.System.Launcher in a
# browser you can see, let you sign in (CAPTCHA and all), and deliver the app's custom-scheme
# callback back to the *running* instance (activate-uri.sh), which holds the PKCE verifier.
#
#   1. start the app with WINE_UWP_LAUNCHER=log   (the Launcher shim then writes the URL here
#      instead of opening your real browser)
#   2. click the app's Log In
#   3. ./browser-callback.sh shapr3d               (runs on YOUR display; needs node + Chrome)
#
# The URL is read from C:\users\Public\wineuwp-launched.uri, or given as a second argument.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]:-$0}")/./env.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)
scheme=${1:?usage: $0 <scheme> [url]}
launched="$WINEPREFIX/drive_c/users/Public/wineuwp-launched.uri"
if [ $# -ge 2 ]; then url=$2
else
  echo "waiting for the app to hand a URL to the Launcher ($launched)..."
  until [ -s "$launched" ]; do sleep 1; done
  url=$(cat "$launched"); rm -f "$launched"
fi
[ -d "$here/tools/browser-callback/node_modules/playwright-core" ] ||
  (cd "$here/tools/browser-callback" && npm install --silent)
echo "opening the sign-in page; finish signing in there"
callback=$(node "$here/tools/browser-callback/callback.js" "$url" "$scheme")
"$here/activate-uri.sh" "$callback"
