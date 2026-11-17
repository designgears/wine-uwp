#!/usr/bin/env bash
# Provide the Microsoft.Services.Store.Engagement framework (x64) in DATADIR/stage/frameworks. Old UWP packages
# (Minecraft for Windows) depend on it, and neither a Windows image nor the Store serves it any more; Microsoft's
# Store Services SDK installer, on the Visual Studio Marketplace, carries it.
# Usage: packaging/fetch-store-engagement.sh DATADIR
set -euo pipefail
[ $# = 1 ] || { echo "usage: $0 DATADIR" >&2; exit 2; }
fw=$1/stage/frameworks
if ls -d "$fw"/Microsoft.Services.Store.Engagement_*_x64__8wekyb3d8bbwe >/dev/null 2>&1; then
  echo "already present in $fw"; exit 0
fi
command -v 7z >/dev/null || { echo "7z is needed to unpack the SDK installer" >&2; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/sdk.msi" \
  "https://marketplace.visualstudio.com/_apis/public/gallery/publishers/AdMediator/vsextensions/MicrosoftStoreServicesSDK/latest/vspackage"
mkdir "$tmp/out"
7z x -y -o"$tmp/out" "$tmp/sdk.msi" >/dev/null
for f in "$tmp"/out/*; do
  m=$(unzip -p "$f" AppxManifest.xml 2>/dev/null) || continue
  grep -q 'Name="Microsoft.Services.Store.Engagement"' <<<"$m" || continue
  grep -q 'ProcessorArchitecture="x64"' <<<"$m" || continue
  v=$(tr '\n' ' ' <<<"$m" | grep -o '<Identity[^>]*' | grep -o ' Version="[^"]*"' | cut -d'"' -f2)
  d=$fw/Microsoft.Services.Store.Engagement_${v}_x64__8wekyb3d8bbwe
  mkdir -p "$d"; unzip -q -o "$f" -d "$d"
  echo "$d"; exit 0
done
echo "no x64 Store Engagement framework in the SDK installer" >&2; exit 1
