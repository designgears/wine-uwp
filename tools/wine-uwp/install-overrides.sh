#!/usr/bin/env bash
# Copy the Windows DLLs named in the native override list into the prefix's system32 so Wine can find
# them by bare name (delay-loads and plain LoadLibrary calls do not go through the registry paths).
# wineboot re-installs Wine's own PE builtins (lowercase names) whenever the Wine version string changes,
# leaving case-duplicates next to the genuine copies; the loader then picks the exact-case builtin.
# We therefore remove every case variant and install the genuine file under the lowercase name.
set -euo pipefail
export WINEPREFIX=$HOME/.wine-uwp
SYS32=$WINEPREFIX/drive_c/windows/system32
WINE_BIN=${WINE_BIN_DIR:-$HOME/src/wine-build/}
SRC=$HOME/uwp-bin/win11/Windows/System32
# Nothing to do when neither the source tree, the Wine build, this script nor the prefix's system32
# listing changed since the last run: the file-by-file comparison below costs about three seconds
# of every launch. WINE_UWP_FORCE_OVERRIDES=1 skips the check.
STAMP=$WINEPREFIX/.install-overrides.stamp
stamp=$( { find "$SRC" "$(dirname "$SRC")/Globalization" -printf '%p %s %T@\n' 2>/dev/null; stat -c '%Y' "$SYS32" "$WINE_BIN/dlls/ntdll/x86_64-windows/ntdll.dll" "$0" "$(dirname "$0")/globalization.reg" 2>/dev/null; echo "${1:-}"; } | md5sum | cut -d' ' -f1 )
if [ -z "${WINE_UWP_FORCE_OVERRIDES:-}" ] && [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$stamp" ]; then
  echo "overrides unchanged"
  exit 0
fi

LIST=${1:-windows.ui.xaml windows.ui twinapi.appcore coremessaging windows.applicationmodel wintypes threadpoolwinrt msvcp_win mrmcorer bcp47mrm bcp47langs profapi rometadata windows.globalization icu icuuc icuin winlangdb globinputhost msftedit msxml6 msxml6r wincorlib userdatatypehelperutil fontgroupsoverride diagnosticdatasettings coreprivacysettingsstore rmclient}
for n in $LIST; do
  f=$(ls "$SRC" | grep -i "^$n\.dll$" | head -1)
  [ -n "$f" ] || { echo "missing $n"; continue; }
  lower=$(echo "$f" | tr 'A-Z' 'a-z')
  for dup in $(ls "$SYS32" | grep -i "^$n\.dll$"); do
    if [ "$dup" != "$lower" ] || ! cmp -s "$SRC/$f" "$SYS32/$dup"; then rm -f "$SYS32/$dup"; fi
  done
  [ -f "$SYS32/$lower" ] || cp -f "$SRC/$f" "$SYS32/$lower"
done
echo "installed: $LIST"

# Resource-only modules: Wine ships no builtin with these names, so they just have to exist in
# system32 for LoadLibraryExW(LOAD_LIBRARY_AS_IMAGE_RESOURCE) to find them by bare name. XAML reads
# its built-in type info from Windows.UI.Xaml.*.dll and its default styles from the Resources.* set.
RESOURCE_LIST=${RESOURCE_LIST:-MrmDeploy.dll Windows.UI.Xaml.Controls.dll Windows.UI.Xaml.Phone.dll Windows.UI.Xaml.InkControls.dll Windows.UI.Xaml.Maps.dll Windows.UI.XamlHost.dll Windows.UI.Xaml.Resources.Common.dll Windows.UI.Xaml.Resources.19h1.dll Windows.UI.Xaml.Resources.21h1.dll Windows.UI.Xaml.Resources.rs1.dll Windows.UI.Xaml.Resources.rs2.dll Windows.UI.Xaml.Resources.rs3.dll Windows.UI.Xaml.Resources.rs4.dll Windows.UI.Xaml.Resources.rs5.dll Windows.UI.Xaml.Resources.th.dll Windows.UI.Xaml.Resources.win81.dll Windows.UI.Xaml.Resources.win8rtm.dll}
for f in $RESOURCE_LIST; do
  [ -f "$SRC/$f" ] || { echo "missing $f"; continue; }
  lower=$(echo "$f" | tr 'A-Z' 'a-z')
  [ -f "$SYS32/$lower" ] && cmp -s "$SRC/$f" "$SYS32/$lower" || cp -f "$SRC/$f" "$SYS32/$lower"
done
echo "installed: $(echo $RESOURCE_LIST | wc -w) resource modules"

# Windows DLLs that Wine ships no builtin for at all: they only have to exist in system32 so that
# delay-loads by bare name resolve. WindowManagementAPI backs the CoreWindow window layout.
NATIVE_ONLY=${NATIVE_ONLY:-WindowManagementAPI.dll CoreUIComponents.dll}
for f in $NATIVE_ONLY; do
  [ -f "$SRC/$f" ] || { echo "missing $f"; continue; }
  lower=$(echo "$f" | tr 'A-Z' 'a-z')
  [ -f "$SYS32/$lower" ] && cmp -s "$SRC/$f" "$SYS32/$lower" || cp -f "$SRC/$f" "$SYS32/$lower"
done
echo "installed: $(echo $NATIVE_ONLY | wc -w) native-only modules"

# Satellite en-US MUI files: Windows DLLs keep their string tables in System32\en-US\<dll>.mui and
# kernelbase's FindResourceExW falls back to them. Without windows.ui.xaml.dll.mui XAML fails loading
# its key names with ERROR_RESOURCE_TYPE_NOT_FOUND (Findings-M4.md).
MUI_DIR=$SYS32/en-US; mkdir -p "$MUI_DIR"
n_mui=0
for dll in "$SYS32"/*.dll; do
  b=$(basename "$dll")
  m=$(ls "$SRC/en-US" 2>/dev/null | grep -i "^$b\.mui$" | head -1) || true
  [ -n "$m" ] || continue
  # Only for genuine Windows copies: a Wine builtin has its own resources.
  src_dll=$(ls "$SRC" | grep -i "^$b$" | head -1) || true
  [ -n "$src_dll" ] && cmp -s "$SRC/$src_dll" "$dll" || continue
  lower=$(echo "$b" | tr 'A-Z' 'a-z').mui
  [ -f "$MUI_DIR/$lower" ] && cmp -s "$SRC/en-US/$m" "$MUI_DIR/$lower" || cp -f "$SRC/en-US/$m" "$MUI_DIR/$lower"
  n_mui=$((n_mui + 1))
done
echo "installed: $n_mui MUI files"
# WinRT metadata: Wine ships small placeholder .winmd files; the real ones (same names) must win so that
# wintypes/rometadata type lookups (ApiInformation, XAML type resolution) see the full Windows metadata.
WM=$SYS32/winmetadata; mkdir -p "$WM"
for f in "$SRC"/WinMetadata/*.winmd; do
  b=$(basename "$f"); lower=$(echo "$b" | tr 'A-Z' 'a-z')
  for dup in $(ls "$WM" | grep -i "^$lower$"); do
    if [ "$dup" != "$lower" ] || ! cmp -s "$f" "$WM/$dup"; then rm -f "$WM/$dup"; fi
  done
  [ -f "$WM/$lower" ] || cp -f "$f" "$WM/$lower"
done
echo "installed: $(ls "$WM" | wc -l) winmd files"
# Prefix-wide native overrides: services hosted by svchost do not inherit WINEDLLOVERRIDES from run.sh,
# so the same list has to live in the registry for CoreMessagingRegistrar to load the genuine DLLs.
for n in $LIST; do
  "${WINE_BIN}wine" reg add 'HKCU\Software\Wine\DllOverrides' /v "$n" /t REG_SZ /d native /f >/dev/null 2>&1
done

# Per-user language profile: bcp47mrm throws a C++ exception when it is missing (Findings-M4.md).
"${WINE_BIN}wine" reg import "$(dirname "$0")/globalization.reg" >/dev/null 2>&1
"${WINE_BIN}wine" reg import "$(dirname "$0")/wineuwp.reg" >/dev/null 2>&1

# MRT shares process resource state between its COM and WinRT entry points. Loading a second
# physical copy through an absolute extraction path breaks that state (see Findings-M4.md).
"${WINE_BIN}wine" reg import "$(dirname "$0")/mrt-single-module.reg" >/dev/null 2>&1

# The same split hits every DLL installed here: the generated class map points at the absolute
# extraction path while bare-name loads resolve to the prefix copy, so the process ends up with two
# copies that have separate data and separate lifetimes. Pin the activatable classes of those DLLs
# to the prefix copy. Only byte-identical copies are pinned, so Wine builtins keep serving their
# own classes.
PY=${PY:-$HOME/uwp-bin/venv/bin/python}
# Classic in-process COM classes the UWP stack CoCreates by CLSID (WindowServerFactory and friends).
# Needed once combase really marshals: a custom marshaler names its unmarshal class by CLSID.
CLSID_REG=$HOME/uwp-bin/win11/wine-uwp-clsid.reg
if [ -x "$PY" ]; then
  "$PY" "$(dirname "$0")/gen-clsid-reg.py" "$SRC/config/SOFTWARE" "$SRC" "$CLSID_REG" | tail -1
  if ! cmp -s "$CLSID_REG" "$CLSID_REG.imported"; then
    "${WINE_BIN}wine" reg import "$CLSID_REG" >/dev/null 2>&1 && cp -f "$CLSID_REG" "$CLSID_REG.imported"
  fi
fi
CLASSES_REG=${CLASSES_REG:-$HOME/uwp-bin/win11/wine-uwp-classes.reg}
SINGLE_REG=$HOME/uwp-bin/win11/wine-uwp-single-module.reg
if [ -f "$CLASSES_REG" ] && [ -x "$PY" ]; then
  "$PY" "$(dirname "$0")/gen-single-module-reg.py" "$CLASSES_REG" "$SRC" "$SYS32" "$SINGLE_REG" | tail -1
  if ! cmp -s "$SINGLE_REG" "$SINGLE_REG.imported"; then
    "${WINE_BIN}wine" reg import "$SINGLE_REG" >/dev/null 2>&1 && cp -f "$SINGLE_REG" "$SINGLE_REG.imported"
  fi
fi

# Windows.Internal.StateRepository.* stand-in. Those classes are service-activated in the Windows
# hive (ActivationType=1, Server=StateRepository) with no DllPath, so gen-classes-reg.py skips them
# and RoGetActivationFactory returns REGDB_E_CLASSNOTREG. See shims/staterepository.
SR_SHIM=$HOME/uwp-bin/build-probes/wineuwp_staterepository.dll
if [ -f "$SR_SHIM" ]; then
  SR_DST=$WINEPREFIX/drive_c/windows/system32/wineuwp_staterepository.dll
  if ! cmp -s "$SR_SHIM" "$SR_DST"; then cp -f "$SR_SHIM" "$SR_DST"; fi
  for cls in User Application; do
    K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.Internal.StateRepository.$cls"
    "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_staterepository.dll' /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 1 /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  done
  echo "installed: Windows.Internal.StateRepository stand-in"
fi

# ICU data: the genuine icu.dll (used by the native windows.globalization.dll) loads
# Windows\Globalization\ICU\icudtl.dat and the time-zone .res files next to it, and fails with
# E_FAIL without them.
GLOB_SRC=$HOME/uwp-bin/win11/Windows/Globalization
if [ -d "$GLOB_SRC/ICU" ]; then
  for sub in ICU "Time Zone"; do
    lsub=$(echo "$sub" | tr "A-Z" "a-z"); mkdir -p "$WINEPREFIX/drive_c/windows/globalization/$lsub"
    for f in "$GLOB_SRC/$sub"/*; do
      b=$(basename "$f"); d="$WINEPREFIX/drive_c/windows/globalization/$lsub/$b"
      [ -f "$d" ] && cmp -s "$f" "$d" || cp -f "$f" "$d"
    done
  done
  echo "installed: Globalization data"
fi

echo "$stamp" > "$STAMP"
