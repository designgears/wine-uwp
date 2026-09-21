#!/usr/bin/env bash
# Tight diagnostic loop for the current XAML composition blocker.
# Green means XAML built/committed a visual tree and dcomp attached a target to an HWND.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
log=${UWP_PRESENTATION_LOG:-/tmp/wine-uwp-presentation-check.log}

for attempt in 1 2 3; do
  WINEDEBUG=+loaddll,+dcomp \
  WINE_UWP_ACTIVATE_ON_VIEW=1 \
  WINE_UWP_BREAK_MOD=windows.ui.xaml.dll \
  WINE_UWP_BREAK_OFF=1be350 \
  UWP_TIMEOUT=12 \
  UWP_LOG="$log" \
    "$root/tools/wine-uwp/run.sh" >/dev/null 2>&1 || true

  if rg -q "compositor_interop_CreateGraphicsDevice" "$log" &&
     rg -q "compositor_CreateContainerVisual" "$log" &&
     rg -q "compositor_CreateSpriteVisual" "$log" &&
     rg -q "CommitMainDevice" "$log"; then
    if rg -q "CreateDesktopWindowTarget|CreateTargetForCurrentView|presented desktop hwnd" "$log"; then
      echo "GREEN: visual tree committed to a presentation target (log: $log)"
      exit 0
    fi
  fi
done

for signal in \
  compositor_interop_CreateGraphicsDevice \
  compositor_CreateContainerVisual \
  compositor_CreateSpriteVisual \
  CommitMainDevice; do
  if ! rg -q "$signal" "$log"; then
    echo "RED: render walk did not reach $signal (log: $log)"
    exit 1
  fi
done

echo "RED: visual tree committed, but no presentation target is attached (log: $log)"
exit 1
