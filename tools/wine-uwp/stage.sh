#!/usr/bin/env bash
# Stage the calculator package with its framework DLLs and our shims into ~/uwp-bin/stage/calculator.
# Usage: tools/wine-uwp/stage.sh [path-to-unpacked-msix]   (default: ~/uwp-bin/calculator/pkg)
set -euo pipefail
SRC=${1:-$HOME/uwp-bin/calculator/pkg}
S=$HOME/uwp-bin/stage/calculator
F=$HOME/uwp-bin/frameworks
B=$HOME/uwp-bin/build-probes
rm -rf "$S"; mkdir -p "$S"
cp -r "$SRC"/. "$S"/
WA="$HOME/uwp-bin/win11/Program Files/WindowsApps"
if [ -d "$WA" ]; then
  # Exact framework package versions from the Windows install (matches the manifest's PackageDependency)
  cp "$WA"/Microsoft.NET.Native.Runtime.2.2_*_x64__*/*.dll "$S"/
  cp "$WA"/Microsoft.NET.Native.Framework.2.2_*_x64__*/SharedLibrary.dll "$S"/
  # Microsoft.UI.Xaml is a framework package, not app content: it has to load from the framework
  # package directory or WinUI decides it is app-local and looks for its themes under the app root.
  cp "$WA"/Microsoft.VCLibs.140.00_*_x64__*/*.dll "$S"/
  cp "$HOME"/uwp-bin/win11/Windows/System32/mrt100.dll "$S"/
  # Framework packages as separate package directories (package graph for MRM and manifest scanning)
  FW=$HOME/uwp-bin/stage/frameworks; rm -rf "$FW"; mkdir -p "$FW"
  for d in "$WA"/Microsoft.UI.Xaml.2.8_8.2511*_x64__* "$WA"/Microsoft.NET.Native.Framework.2.2_*_x64__* "$WA"/Microsoft.NET.Native.Runtime.2.2_*_x64__* "$WA"/Microsoft.VCLibs.140.00_*_x64__*; do
    cp -r "$d" "$FW"/
  done
else
  # Redistributable fallbacks obtained without Windows (see docs/LinuxPort/Findings-M0.md)
  cp "$F"/Microsoft.NET.Native.Runtime.2.2/*.dll "$S"/
  cp "$F"/Microsoft.NET.Native.Framework.2.2/SharedLibrary.dll "$S"/
  cp "$F"/Microsoft.UI.Xaml.2.8/Microsoft.UI.Xaml.dll "$S"/
  for p in msvcp140 vcruntime140 vcruntime140_1 concrt140 vccorlib140; do cp "$F"/vclibs-desktop/$p.dll "$S"/${p}_app.dll; done
  cp "$B"/mrt100.dll "$S"/
fi
# Do NOT stage Microsoft.UI.Xaml.Controls/Themes/generic.xaml here. XAML probes that path when
# it cannot resolve the theme resources out of the framework package, and it does find the file -
# then XamlScanner's XmlLite pass fails on it and the whole parse comes back E_XAMLPARSEFAILED
# (0x802b000a). Without the file, XamlControlsResources resolves out of the framework package's
# PRI and the app's XAML parses clean. See Findings-M4.md.

# The repo's GraphingImpl is a mock (draws nothing, parses nothing). The Store package ships the
# real engine as GraphingImpl.dll + omsautimmss.dll; when a copy of the package is at
# ~/uwp-bin/store-calc (see docs/LinuxPort/Overrides.md) use it so Graphing actually plots.
SC=$HOME/uwp-bin/store-calc
if [ -f "$SC/GraphingImpl.dll" ] && [ -f "$SC/omsautimmss.dll" ]; then
  cp "$SC"/GraphingImpl.dll "$SC"/omsautimmss.dll "$S"/
  echo "staged the Store graphing engine"
fi

echo "staged -> $S"
