#!/usr/bin/env bash
# Green means the hosted CoreWindow is presented and the XAML root visual (a WinRT
# sprite carrying the app's background brush) was resolved and painted by the
# fallback renderer. This is the current form of "the tree has drawable content";
# dcv_SetWidth/dcv_SetContent stay 0/NULL on this path until real D2D surfaces exist.
# The gate matches paint_public_visual's trace, which reports the size it painted and
# whether the brush colour came from XAML ("matched 1") or from the renderer fallback.
set -euo pipefail

root=$(cd "$(dirname "$0")" && pwd)
log=${1:-${UWP_VISUAL_CONTENT_LOG:-/tmp/wine-uwp-visual-content-check.log}}

if [ "$#" -eq 0 ]; then
  if ! UWP_PRESENTATION_LOG="$log" "$root/check-presentation.sh"; then
    exit 1
  fi
elif ! rg -q 'presented desktop hwnd' "$log"; then
  echo "RED: captured trace contains no presented target (log: $log)"
  exit 1
fi

if ! rg -q 'resolved foreign child .* -> interop' "$log"; then
  echo "RED: target presented, but no foreign WinRT visual was retained as the render root (log: $log)"
  exit 1
fi

if ! rg -q 'painted public sprite .* [1-9][0-9]*(\.[0-9]+)?x[1-9][0-9]*(\.[0-9]+)? matched' "$log"; then
  echo "RED: root visual retained, but the renderer painted no non-hairline sprite (log: $log)"
  exit 1
fi

echo "GREEN: XAML root visual resolved and painted (log: $log)"
