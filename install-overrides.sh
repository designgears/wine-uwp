#!/usr/bin/env bash
# Copy the Windows DLLs named in the native override list into the prefix's system32 so Wine can find
# them by bare name (delay-loads and plain LoadLibrary calls do not go through the registry paths).
# wineboot re-installs Wine's own PE builtins (lowercase names) whenever the Wine version string changes,
# leaving case-duplicates next to the genuine copies; the loader then picks the exact-case builtin.
# We therefore remove every case variant and install the genuine file under the lowercase name.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]:-$0}")/./env.sh"
export WINEPREFIX=${WINEPREFIX}
SYS32=$WINEPREFIX/drive_c/windows/system32
WINE_BIN=$WINE_BIN_DIR   # from env.sh, sourced above; always ends in a slash
# The Windows file tree: uwp-bin/win11 (copied from an installation) or one harvested from an ISO
# (packaging/harvest/harvest.py). WIN11 picks it; everything below reads from it.
WIN11=${WIN11:-${UWP_BIN}/win11}
SRC=${WIN11}/Windows/System32
# Our shim DLLs: the probe build directory in a checkout, share/wine-uwp/shims in an installed layout.
SHIM_DIR=${SHIM_DIR:-${UWP_BIN}/build-probes}
# A Wine PE module from WINE_BUILD: a build tree has dlls/<name>/x86_64-windows/<name>.dll, an
# installed Wine (bin/ next to lib/wine/) has lib/wine/x86_64-windows/<name>.dll.
wine_pe() {
  if [ -f "$WINE_BIN/dlls/$1/x86_64-windows/$1.dll" ]; then echo "$WINE_BIN/dlls/$1/x86_64-windows/$1.dll"
  else echo "$WINE_BIN../lib/wine/x86_64-windows/$1.dll"; fi
}
# Nothing to do when neither the source tree, the Wine build, this script nor the prefix's system32
# listing changed since the last run: the file-by-file comparison below costs about three seconds
# of every launch. WINE_UWP_FORCE_OVERRIDES=1 skips the check.
STAMP=$WINEPREFIX/.install-overrides.stamp
# The shim DLLs have to be in the stamp: without them, rebuilding a shim leaves the stamp
# unchanged, the installer skips itself, and the prefix keeps running the previous build - which
# looks exactly like the code change having no effect.
stamp=$( { find "$SRC" "$(dirname "$SRC")/Globalization" "$(dirname "$SRC")/Fonts" -printf '%p %s %T@\n' 2>/dev/null; stat -c '%Y' "$SYS32" "$(wine_pe ntdll)" "$(wine_pe wineuwp)" "$0" "$(dirname "$0")/globalization.reg" 2>/dev/null; stat -c '%n %s %Y' ${SHIM_DIR}/wineuwp_*.dll 2>/dev/null; echo "${1:-}"; } | md5sum | cut -d' ' -f1 )
if [ -z "${WINE_UWP_FORCE_OVERRIDES:-}" ] && [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$stamp" ]; then
  echo "overrides unchanged"
  exit 0
fi

LIST=${1:-windows.ui.xaml windows.ui twinapi.appcore coremessaging windows.applicationmodel wintypes threadpoolwinrt msvcp_win mrmcorer bcp47mrm bcp47langs profapi rometadata windows.globalization icu icuuc icuin winlangdb globinputhost msftedit msxml6 msxml6r wincorlib userdatatypehelperutil fontgroupsoverride diagnosticdatasettings coreprivacysettingsstore rmclient cryptowinrt}
for n in $LIST; do
  f=$(ls "$SRC" | grep -i "^$n\.dll$" | head -1)
  [ -n "$f" ] || { echo "missing $n"; continue; }
  lower=$(echo "$f" | tr 'A-Z' 'a-z')
  for dup in $(ls "$SYS32" | grep -i "^$n\.dll$"); do
    if [ "$dup" != "$lower" ] || ! cmp -s "$SRC/$f" "$SYS32/$dup"; then rm -f "$SYS32/$dup"; fi
  done
  [ -f "$SYS32/$lower" ] || cp -f --reflink=auto "$SRC/$f" "$SYS32/$lower"
done
echo "installed: $LIST"

# Resource-only modules: Wine ships no builtin with these names, so they just have to exist in
# system32 for LoadLibraryExW(LOAD_LIBRARY_AS_IMAGE_RESOURCE) to find them by bare name. XAML reads
# its built-in type info from Windows.UI.Xaml.*.dll and its default styles from the Resources.* set.
RESOURCE_LIST=${RESOURCE_LIST:-MrmDeploy.dll Windows.UI.Xaml.Controls.dll Windows.UI.Xaml.Phone.dll Windows.UI.Xaml.InkControls.dll Windows.UI.Xaml.Maps.dll Windows.UI.XamlHost.dll Windows.UI.Xaml.Resources.Common.dll Windows.UI.Xaml.Resources.19h1.dll Windows.UI.Xaml.Resources.21h1.dll Windows.UI.Xaml.Resources.rs1.dll Windows.UI.Xaml.Resources.rs2.dll Windows.UI.Xaml.Resources.rs3.dll Windows.UI.Xaml.Resources.rs4.dll Windows.UI.Xaml.Resources.rs5.dll Windows.UI.Xaml.Resources.th.dll Windows.UI.Xaml.Resources.win81.dll Windows.UI.Xaml.Resources.win8rtm.dll}
for f in $RESOURCE_LIST; do
  [ -f "$SRC/$f" ] || { echo "missing $f"; continue; }
  lower=$(echo "$f" | tr 'A-Z' 'a-z')
  [ -f "$SYS32/$lower" ] && cmp -s "$SRC/$f" "$SYS32/$lower" || cp -f --reflink=auto "$SRC/$f" "$SYS32/$lower"
done
echo "installed: $(echo $RESOURCE_LIST | wc -w) resource modules"

# Windows DLLs that Wine ships no builtin for at all: they only have to exist in system32 so that
# delay-loads by bare name resolve. WindowManagementAPI backs the CoreWindow window layout.
NATIVE_ONLY=${NATIVE_ONLY:-WindowManagementAPI.dll CoreUIComponents.dll}
for f in $NATIVE_ONLY; do
  [ -f "$SRC/$f" ] || { echo "missing $f"; continue; }
  lower=$(echo "$f" | tr 'A-Z' 'a-z')
  [ -f "$SYS32/$lower" ] && cmp -s "$SRC/$f" "$SYS32/$lower" || cp -f --reflink=auto "$SRC/$f" "$SYS32/$lower"
done
echo "installed: $(echo $NATIVE_ONLY | wc -w) native-only modules"

# Windows' own fonts, copied from a Windows installation into $UWP_BIN/win11/Windows/Fonts (see
# AGENTS.md). win32u loads every file in C:\windows\Fonts at startup and registers it under its
# bare file name, which is also where dwrite's system collection finds it, so installing the files
# is all it takes. Fonts found there are preferred over the same faces fontconfig finds on the host.
FONT_SRC=$(dirname "$SRC")/Fonts
FONT_DST=$WINEPREFIX/drive_c/windows/Fonts
if [ -d "$FONT_SRC" ]; then
  mkdir -p "$FONT_DST"
  n=0
  for f in "$FONT_SRC"/*; do
    b=$(basename "$f")
    cmp -s "$f" "$FONT_DST/$b" || { cp -f --reflink=auto "$f" "$FONT_DST/$b"; n=$((n + 1)); }
  done
  echo "installed: $(ls "$FONT_SRC" | wc -l) Windows fonts ($n copied)"
fi

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
  [ -f "$MUI_DIR/$lower" ] && cmp -s "$SRC/en-US/$m" "$MUI_DIR/$lower" || cp -f --reflink=auto "$SRC/en-US/$m" "$MUI_DIR/$lower"
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
  [ -f "$WM/$lower" ] || cp -f --reflink=auto "$f" "$WM/$lower"
done
echo "installed: $(ls "$WM" | wc -l) winmd files"
# Prefix-wide native overrides: services hosted by svchost do not inherit WINEDLLOVERRIDES from run.sh,
# so the same list has to live in the registry for CoreMessagingRegistrar to load the genuine DLLs.
for n in $LIST; do
  "${WINE_BIN}wine" reg add 'HKCU\Software\Wine\DllOverrides' /v "$n" /t REG_SZ /d native /f >/dev/null 2>&1
done

# Per-user language profile: bcp47mrm throws a C++ exception when it is missing (Findings-M4.md).
"${WINE_BIN}wine" reg import "$(dirname "$0")/globalization.reg" >/dev/null 2>&1

# MRT shares process resource state between its COM and WinRT entry points. Loading a second
# physical copy through an absolute extraction path breaks that state (see Findings-M4.md).
"${WINE_BIN}wine" reg import "$(dirname "$0")/mrt-single-module.reg" >/dev/null 2>&1

# The same split hits every DLL installed here: the generated class map points at the absolute
# extraction path while bare-name loads resolve to the prefix copy, so the process ends up with two
# copies that have separate data and separate lifetimes. Pin the activatable classes of those DLLs
# to the prefix copy. Only byte-identical copies are pinned, so Wine builtins keep serving their
# own classes.
PY=${PY:-$( [ -x "${UWP_BIN}/venv/bin/python" ] && echo "${UWP_BIN}/venv/bin/python" || echo python3 )}
# Classic in-process COM classes the UWP stack CoCreates by CLSID (WindowServerFactory and friends).
# Needed once combase really marshals: a custom marshaler names its unmarshal class by CLSID.
CLSID_REG=${WIN11}/wine-uwp-clsid.reg
if command -v "$PY" >/dev/null; then
  "$PY" "$(dirname "$0")/gen-clsid-reg.py" "$SRC/config/SOFTWARE" "$SRC" "$CLSID_REG" | tail -1
  # the marker is per prefix: one Windows tree can serve several prefixes (a bottle gets a copy of it)
  if ! cmp -s "$CLSID_REG" "$WINEPREFIX/.wine-uwp-clsid.reg.imported"; then
    "${WINE_BIN}wine" reg import "$CLSID_REG" >/dev/null 2>&1 && cp -f --reflink=auto "$CLSID_REG" "$WINEPREFIX/.wine-uwp-clsid.reg.imported"
  fi
fi
CLASSES_REG=${CLASSES_REG:-${WIN11}/wine-uwp-classes.reg}
SINGLE_REG=${WIN11}/wine-uwp-single-module.reg
if [ -f "$CLASSES_REG" ] && command -v "$PY" >/dev/null; then
  "$PY" "$(dirname "$0")/gen-single-module-reg.py" "$CLASSES_REG" "$SRC" "$SYS32" "$SINGLE_REG" | tail -1
  if ! cmp -s "$SINGLE_REG" "$WINEPREFIX/.wine-uwp-single-module.reg.imported"; then
    "${WINE_BIN}wine" reg import "$SINGLE_REG" >/dev/null 2>&1 &&
      cp -f --reflink=auto "$SINGLE_REG" "$WINEPREFIX/.wine-uwp-single-module.reg.imported"
  fi
fi

# wineuwp.reg carries one absolute host path (threadpoolwinrt.dll in the Windows image, $WIN11). A
# .reg file cannot expand a variable, so the placeholder is substituted at import time - otherwise
# the path is baked to whoever generated it, which is exactly what broke every app when this tree
# moved out of the home directory. It is imported after the registrations generated from the
# Windows hive, because it overrides some of them ({02844640-...} is wineuwp's, not twinapi's).
WIN11_WINPATH="Z:$(echo "$WIN11" | sed 's|/|\\\\|g')"
# The replacement has to be escaped *again* for sed, which eats a backslash pair in a replacement.
# Without this the imported value came out as "Z:homebobbyProjects..." - reg import then dropped
# every unrecognised \<letter> escape - and the app died with "couldn't load in-process dll",
# "no apartment found for ipid" and do_activate hr 0x80004005, which looks nothing like a path bug.
WIN11_WINPATH_SED=$(printf '%s' "$WIN11_WINPATH" | sed 's|\\|\\\\|g')
sed "s|@WIN11_WINPATH@|${WIN11_WINPATH_SED}|g" "$(dirname "$0")/wineuwp.reg" > "$WINEPREFIX/.wineuwp.reg.tmp"
"${WINE_BIN}wine" reg import "$WINEPREFIX/.wineuwp.reg.tmp" >/dev/null 2>&1
rm -f "$WINEPREFIX/.wineuwp.reg.tmp"

# Windows.Internal.StateRepository.* stand-in. Those classes are service-activated in the Windows
# hive (ActivationType=1, Server=StateRepository) with no DllPath, so gen-classes-reg.py skips them
# and RoGetActivationFactory returns REGDB_E_CLASSNOTREG. See shims/staterepository.
SR_SHIM=${SHIM_DIR}/wineuwp_staterepository.dll
if [ -f "$SR_SHIM" ]; then
  SR_DST=$WINEPREFIX/drive_c/windows/system32/wineuwp_staterepository.dll
  if ! cmp -s "$SR_SHIM" "$SR_DST"; then cp -f --reflink=auto "$SR_SHIM" "$SR_DST"; fi
  for cls in User Application MrtApplication Package PackageFamily MrtPackage PackageUser TargetDeviceFamily; do
    K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.Internal.StateRepository.$cls"
    "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_staterepository.dll' /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 1 /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  done
  echo "installed: Windows.Internal.StateRepository stand-in"
fi

# Windows.Security.Authorization.AppCapabilityAccess.AppCapability stand-in. The registered
# DllPath is the native CapabilityAccessManagerClient.dll, which RPCs to a service the prefix does
# not have, so every CheckAccess is denied and Media Player paints "can't access your music
# library" instead of the library. See shims/appcapability.
AC_SHIM=${SHIM_DIR}/wineuwp_appcapability.dll
if [ -f "$AC_SHIM" ]; then
  AC_DST=$WINEPREFIX/drive_c/windows/system32/wineuwp_appcapability.dll
  if ! cmp -s "$AC_SHIM" "$AC_DST"; then cp -f --reflink=auto "$AC_SHIM" "$AC_DST"; fi
  K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.Security.Authorization.AppCapabilityAccess.AppCapability"
  "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_appcapability.dll' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 1 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  echo "installed: AppCapability stand-in"
fi

# Windows.Storage.StorageLibrary stand-in. Wine's windows.storage does not implement the class
# (RoGetActivationFactory says "not found in windows.storage.dll"), and it cannot easily: Wine's
# include/windows.storage.idl declares neither StorageLibrary nor KnownLibraryId. Media Player's
# library pages need it once AppCapability reports musicLibrary as Allowed. See shims/storagelibrary.
SL_SHIM=${SHIM_DIR}/wineuwp_storagelibrary.dll
if [ -f "$SL_SHIM" ]; then
  SL_DST=$WINEPREFIX/drive_c/windows/system32/wineuwp_storagelibrary.dll
  if ! cmp -s "$SL_SHIM" "$SL_DST"; then cp -f --reflink=auto "$SL_SHIM" "$SL_DST"; fi
  K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.Storage.StorageLibrary"
  "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_storagelibrary.dll' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 1 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  echo "installed: StorageLibrary stand-in"
fi

# Windows.Storage.Pickers.FileOpenPicker stand-in. The registered DllPath is the native
# twinui.appcore.dll, whose picker is a shell UI in a broker process the prefix does not have, so
# PickSingleFileAsync never completes and an app's "open file" button does nothing. The stand-in
# returns the file named by WINE_UWP_PICKER_FILE. See shims/filepicker.
FP_SHIM=${SHIM_DIR}/wineuwp_filepicker.dll
if [ -f "$FP_SHIM" ]; then
  FP_DST=$WINEPREFIX/drive_c/windows/system32/wineuwp_filepicker.dll
  if ! cmp -s "$FP_SHIM" "$FP_DST"; then cp -f --reflink=auto "$FP_SHIM" "$FP_DST"; fi
  K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.Storage.Pickers.FileOpenPicker"
  "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_filepicker.dll' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 1 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  echo "installed: FileOpenPicker stand-in"
fi

# Windows.System.UserPicker stand-in. Registered on Windows as ActivationType=1 with
# Server=UserManager - an out-of-process class with no DllPath - so RoGetActivationFactory fails
# outright and Xbox Live auth (XAL) reports E_FAIL. Solitaire logs that as its first two lines.
# See shims/userpicker.
UP_SHIM=${SHIM_DIR}/wineuwp_userpicker.dll
if [ -f "$UP_SHIM" ]; then
  UP_DST=$WINEPREFIX/drive_c/windows/system32/wineuwp_userpicker.dll
  if ! cmp -s "$UP_SHIM" "$UP_DST"; then cp -f --reflink=auto "$UP_SHIM" "$UP_DST"; fi
  K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.System.UserPicker"
  "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_userpicker.dll' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 1 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  echo "installed: UserPicker stand-in"
fi

# MemoryManager's native rmclient event registration requires an absent Windows service.
# Solitaire throws out of its page constructor when registration returns 0x800706d9.
MM_SHIM=${SHIM_DIR}/wineuwp_memorymanager.dll
if [ -f "$MM_SHIM" ]; then
  MM_DST=$WINEPREFIX/drive_c/windows/system32/wineuwp_memorymanager.dll
  if ! cmp -s "$MM_SHIM" "$MM_DST"; then cp -f --reflink=auto "$MM_SHIM" "$MM_DST"; fi
  K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.System.MemoryManager"
  "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_memorymanager.dll' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 1 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  echo "installed: MemoryManager stand-in"
fi

# Windows.Devices.Enumeration.DeviceInformation: Wine implements every CreateWatcher overload
# except the DeviceClass one, which returns E_NOTIMPL. Solitaire is C++/CX, so that becomes a
# thrown Platform::Exception that abandons AppHost::EngineInitialize - including the render-window
# event subscription it needs to resize its swap chain. See shims/deviceinformation.
DI_SHIM=${SHIM_DIR}/wineuwp_deviceinformation.dll
if [ -f "$DI_SHIM" ]; then
  DI_DST=$WINEPREFIX/drive_c/windows/system32/wineuwp_deviceinformation.dll
  if ! cmp -s "$DI_SHIM" "$DI_DST"; then cp -f --reflink=auto "$DI_SHIM" "$DI_DST"; fi
  K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.Devices.Enumeration.DeviceInformation"
  "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_deviceinformation.dll' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 1 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  echo "installed: DeviceInformation CreateWatcher(DeviceClass) shim"
fi

# msxml6's COM classes. Installing msxml6.dll into system32 is not enough: nothing registers its
# CLSIDs, so CoCreateInstance returns REGDB_E_CLASSNOTREG (0x80040154). That matters because
# libHttpClient - which is what XSAPI and PlayFab make every HTTP call through on UWP - uses
# **IXMLHTTPRequest2**, i.e. CLSID_FreeThreadedXMLHTTP60. Without it Solitaire's PlayFab login is
# issued and never completes, the process never opens a single TCP socket, and the app waits on its
# splash screen forever while Wine's own NetworkListManager reports full internet connectivity.
# `wine regsvr32 msxml6.dll` silently does nothing here, so the three XMLHTTP classes are
# registered directly. The paths and ThreadingModel values are copied from the Windows hive
# (fasthive.py on Classes\CLSID\{...}\InprocServer32), not guessed - {88d96a09} is "Free Threaded
# XML HTTP Request class 6.0" and is the only one of the three with ThreadingModel Both.
if [ -f "$SYS32/msxml6.dll" ]; then
  msxml_reg() {
    local clsid=$1 model=$2
    "${WINE_BIN}wine" reg add "HKCR\\CLSID\\$clsid\\InprocServer32" /ve \
        /d 'C:\windows\system32\msxml6.dll' /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "HKCR\\CLSID\\$clsid\\InprocServer32" /v ThreadingModel \
        /d "$model" /f >/dev/null 2>&1
  }
  msxml_reg '{88d96a09-f192-11d4-a65f-0040963251e5}' Both        # Free Threaded XML HTTP 6.0
  msxml_reg '{88d96a0a-f192-11d4-a65f-0040963251e5}' Apartment   # XML HTTP 6.0
  msxml_reg '{88d96a0b-f192-11d4-a65f-0040963251e5}' Apartment   # Server XML HTTP 6.0
  echo "registered: msxml6 XMLHTTP classes"
fi

# wineuwp.dll is ours and is rebuilt often. A prefix update copies Wine's PE builtins into
# system32, and that copy shadows the build tree - so a stale one silently reinstates the previous
# build and a fresh fix looks like it had no effect. That is exactly what happened to the
# GetPackageFamilyNameFromToken import repair: the prefix held a copy three hours older than the
# build. Keep it in step.
UWPDLL=$(wine_pe wineuwp)
if [ -f "$UWPDLL" ] && [ -f "$SYS32/wineuwp.dll" ] && ! cmp -s "$UWPDLL" "$SYS32/wineuwp.dll"; then
  cp -f --reflink=auto "$UWPDLL" "$SYS32/wineuwp.dll"
  echo "refreshed: stale wineuwp.dll in the prefix"
fi

# api-ms-win-appmodel-runtime-l1-1-1.dll: forwards the apiset to kernelbase plus the one function
# Wine lacks. Resolved by name at load time, so it only has to exist in system32.
AMR=${SHIM_DIR}/api-ms-win-appmodel-runtime-l1-1-1.dll
if [ -f "$AMR" ]; then
  AMR_DST=$SYS32/api-ms-win-appmodel-runtime-l1-1-1.dll
  if ! cmp -s "$AMR" "$AMR_DST"; then cp -f --reflink=auto "$AMR" "$AMR_DST"; fi
  echo "installed: appmodel-runtime apiset stand-in"
fi

# NetworkInformation stand-in (see shims/networkstatus): fires NetworkStatusChanged once.
NS=${SHIM_DIR}/wineuwp_networkstatus.dll
if [ -f "$NS" ]; then
  if ! cmp -s "$NS" "$SYS32/wineuwp_networkstatus.dll"; then cp -f --reflink=auto "$NS" "$SYS32/wineuwp_networkstatus.dll"; fi
  K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.Networking.Connectivity.NetworkInformation"
  "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_networkstatus.dll' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 1 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  echo "installed: NetworkInformation stand-in"
fi

# SpatialAudioDeviceConfiguration stand-in (see shims/spatialaudio).
SA=${SHIM_DIR}/wineuwp_spatialaudio.dll
if [ -f "$SA" ]; then
  if ! cmp -s "$SA" "$SYS32/wineuwp_spatialaudio.dll"; then cp -f --reflink=auto "$SA" "$SYS32/wineuwp_spatialaudio.dll"; fi
  K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.Media.Audio.SpatialAudioDeviceConfiguration"
  "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_spatialaudio.dll' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 1 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  K2="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.Media.Audio.SpatialAudioFormatSubtype"
  "${WINE_BIN}wine" reg add "$K2" /v DllPath /d 'C:\windows\system32\wineuwp_spatialaudio.dll' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K2" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K2" /v Threading /t REG_DWORD /d 1 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K2" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  echo "installed: SpatialAudioDeviceConfiguration + FormatSubtype stand-ins"
fi

# UserActivityChannel.GetDefault needs cdp.dll in the native implementation. Its failure
# unwinds out of Solitaire's startup subscriber and terminates the engine thread.
UA_SHIM=${SHIM_DIR}/wineuwp_useractivitychannel.dll
if [ -f "$UA_SHIM" ]; then
  UA_DST=$SYS32/wineuwp_useractivitychannel.dll
  if ! cmp -s "$UA_SHIM" "$UA_DST"; then cp -f --reflink=auto "$UA_SHIM" "$UA_DST"; fi
  K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.ApplicationModel.UserActivities.UserActivityChannel"
  "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_useractivitychannel.dll' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 1 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  echo "installed: UserActivityChannel stand-in"
fi

# The notification service is absent; expose its disabled availability to apps.
TN_SHIM=${SHIM_DIR}/wineuwp_toastnotifier.dll
if [ -f "$TN_SHIM" ]; then
  TN_DST=$SYS32/wineuwp_toastnotifier.dll
  if ! cmp -s "$TN_SHIM" "$TN_DST"; then cp -f --reflink=auto "$TN_SHIM" "$TN_DST"; fi
  K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.UI.Notifications.ToastNotificationManager"
  "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_toastnotifier.dll' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 1 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  echo "installed: ToastNotifier availability stand-in"
fi

# Windows.Services.Store.StoreContext: traced stand-in (see shims/storecontext). The real one
# needs ContainerHostActivation {22f5b1df-...}, which is not registered under Wine.
SC_SHIM=${SHIM_DIR}/wineuwp_storecontext.dll
if [ -f "$SC_SHIM" ]; then
  SC_DST=$SYS32/wineuwp_storecontext.dll
  if ! cmp -s "$SC_SHIM" "$SC_DST"; then cp -f --reflink=auto "$SC_SHIM" "$SC_DST"; fi
  K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.Services.Store.StoreContext"
  "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_storecontext.dll' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 1 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  echo "installed: StoreContext traced stand-in"
fi

# Windows.System.User: the local user (see shims/user). Windows serves it from the UserManager service.
US_SHIM=${SHIM_DIR}/wineuwp_user.dll
if [ -f "$US_SHIM" ]; then
  US_DST=$SYS32/wineuwp_user.dll
  if ! cmp -s "$US_SHIM" "$US_DST"; then cp -f --reflink=auto "$US_SHIM" "$US_DST"; fi
  K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.System.User"
  "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_user.dll' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  echo "installed: Windows.System.User stand-in"
fi

# Microsoft.GamePlatform.Services.*: the Gaming Services client classes (see shims/gameplatform). They
# replace the package's own Microsoft.GamePlatform.Services.dll (OverridesPackage=1, read by combase),
# whose NT services cannot run under Wine.
GP_SHIM=${SHIM_DIR}/wineuwp_gameplatform.dll
if [ -f "$GP_SHIM" ]; then
  GP_DST=$SYS32/wineuwp_gameplatform.dll
  if ! cmp -s "$GP_SHIM" "$GP_DST"; then cp -f --reflink=auto "$GP_SHIM" "$GP_DST"; fi
  # every class of the client that has statics (gen-gameplatform.py lists them in shims/gameplatform/slots.h)
  for cls in PackageRequest Package PackagePreview PackageQueue PackageUpdate UploadQueue Storage GameFolder \
              GameProfile GameSave GameStreaming GameMetrics Licensing SupportedFeatures PlatformStatus \
              ProcessMonitor Store Customization ContentAccess BackgroundConsent; do
    K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Microsoft.GamePlatform.Services.$cls"
    "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_gameplatform.dll' /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 0 /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v OverridesPackage /t REG_DWORD /d 1 /f >/dev/null 2>&1
  done
  # ...and its package identity, so PackageManager reports Gaming Services as installed (appxdeploymentclient
  # standin.c). The identity is the one gamingservices-signed.appxbundle carries (Xbox app 2608.1001.17.0).
  K="HKLM\\Software\\Wine\\WineUWP\\StandInPackages\\Microsoft.GamingServices_8wekyb3d8bbwe"
  "${WINE_BIN}wine" reg add "$K" /v Name /d Microsoft.GamingServices /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Publisher /d 'CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v PublisherId /d 8wekyb3d8bbwe /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Version /d 37.114.26001.0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Architecture /t REG_DWORD /d 9 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v FullName /d Microsoft.GamingServices_37.114.26001.0_x64__8wekyb3d8bbwe /f >/dev/null 2>&1
  # The packages the Xbox app lists under Settings > App > Dependencies, registered as installed with the identities
  # of a Windows 11 install's copies (2026-10). Identity only: none of them is staged, so anything the app launches
  # in them (the Store, Purchase App, Game Bar) does not open.
  for dep in Microsoft.DesktopAppInstaller:1.29.379.0 Microsoft.WindowsFeedbackHub:1.2401.20253.0 \
             Microsoft.XboxGamingOverlay:7.326.8061.0 Microsoft.XboxIdentityProvider:12.130.16001.0 \
             Microsoft.Xbox.TCUI:1.24.10001.0 Microsoft.WindowsStore:22608.1401.3.0 Microsoft.StorePurchaseApp:22607.1401.4.0; do
    dn=${dep%%:*}; dv=${dep#*:}
    K="HKLM\\Software\\Wine\\WineUWP\\StandInPackages\\${dn}_8wekyb3d8bbwe"
    "${WINE_BIN}wine" reg add "$K" /v Name /d "$dn" /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v Publisher /d 'CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US' /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v PublisherId /d 8wekyb3d8bbwe /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v Version /d "$dv" /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v Architecture /t REG_DWORD /d 9 /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v FullName /d "${dn}_${dv}_x64__8wekyb3d8bbwe" /f >/dev/null 2>&1
  done
  # ...and its two NT services, as stand-ins that start, report RUNNING and stop (shims/gamingservice)
  GS_SVC=${SHIM_DIR}/wineuwp_gamingservice.exe
  if [ -f "$GS_SVC" ]; then
    if ! cmp -s "$GS_SVC" "$SYS32/wineuwp_gamingservice.exe"; then cp -f --reflink=auto "$GS_SVC" "$SYS32/wineuwp_gamingservice.exe"; fi
    for svc in GamingServices GamingServicesNet; do
      K="HKLM\\System\\CurrentControlSet\\Services\\$svc"
      "${WINE_BIN}wine" reg add "$K" /v ImagePath /d 'C:\windows\system32\wineuwp_gamingservice.exe' /f >/dev/null 2>&1
      "${WINE_BIN}wine" reg add "$K" /v Type /t REG_DWORD /d 16 /f >/dev/null 2>&1
      "${WINE_BIN}wine" reg add "$K" /v Start /t REG_DWORD /d 3 /f >/dev/null 2>&1
      "${WINE_BIN}wine" reg add "$K" /v ErrorControl /t REG_DWORD /d 1 /f >/dev/null 2>&1
      "${WINE_BIN}wine" reg add "$K" /v ObjectName /d LocalSystem /f >/dev/null 2>&1
      "${WINE_BIN}wine" reg add "$K" /v DisplayName /d "$svc" /f >/dev/null 2>&1
    done
  fi
  echo "installed: GamePlatform (Gaming Services client) stand-in"
fi

# Microsoft.GamePlatform.Services.Identity: Xbox sign-in (see shims/xboxauth). On Windows it is Gaming Services'
# token broker; this one signs in with a Microsoft account device code in the user's browser and talks to Xbox
# Live itself. The sign-in is kept, DPAPI-protected, in %LOCALAPPDATA%\wineuwp\xboxauth.bin.
XA_SHIM=${SHIM_DIR}/wineuwp_xboxauth.dll
if [ -f "$XA_SHIM" ]; then
  XA_DST=$SYS32/wineuwp_xboxauth.dll
  if ! cmp -s "$XA_SHIM" "$XA_DST"; then cp -f --reflink=auto "$XA_SHIM" "$XA_DST"; fi
  for cls in XboxAuthManager XboxAuthInitializationArgs; do
    K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Microsoft.GamePlatform.Services.Identity.$cls"
    "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_xboxauth.dll' /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 0 /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
    "${WINE_BIN}wine" reg add "$K" /v OverridesPackage /t REG_DWORD /d 1 /f >/dev/null 2>&1
  done
  # WebAuthenticationCoreManager from the same DLL: the Microsoft account provider (https://login.microsoft.com,
  # "consumers") and its WebAccount are the account the Xbox sign-in holds, which is what the Store account is on
  # Windows. Other providers are not found. (It replaced shims/webauthcore, which found no provider at all.)
  K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager"
  "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_xboxauth.dll' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  # StoreConfiguration likewise: which WebAccount is the Store's. Windows' own needs StoreConfigurationServer.
  K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.ApplicationModel.Store.Preview.StoreConfiguration"
  "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_xboxauth.dll' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  echo "installed: Xbox sign-in (XboxAuthManager, WebAuthenticationCoreManager, StoreConfiguration) stand-in"
fi

# api-ms-win-appmodel-advertisingid-l1-1-0.dll. No registry entry: this is a plain DLL resolved by
# name at load time, so it only has to exist in system32 for twinapi.appcore's imports to bind to
# it instead of Wine's raising stub.
ADID=${SHIM_DIR}/api-ms-win-appmodel-advertisingid-l1-1-0.dll
if [ -f "$ADID" ]; then
  ADID_DST=$SYS32/api-ms-win-appmodel-advertisingid-l1-1-0.dll
  if ! cmp -s "$ADID" "$ADID_DST"; then cp -f --reflink=auto "$ADID" "$ADID_DST"; fi
  echo "installed: advertising-id apiset stand-in"
fi

# The WebView2 runtime (Edge's EBWebView, from a Windows ISO's install.wim: Program Files (x86)/Microsoft/EdgeWebView),
# with shims/webview2 in front of its EmbeddedBrowserWebView.dll. The Xbox app's purchase dialog and other React
# Native WebViews need it; without a runtime the app crashes on Get. The host turns composition-hosted WebViews
# into a windowed browser in a popup (Wine does not show another process's child windows). Optional: only when
# the harvested runtime is there.
WV2_ROOT=${WEBVIEW2_RUNTIME:-${UWP_BIN}/webview2/rt/Program Files (x86)/Microsoft/EdgeWebView}
WV2_HOST=${SHIM_DIR}/EmbeddedBrowserWebView.dll
WV2_APP=$(ls -d "$WV2_ROOT"/Application/[0-9]*.[0-9]*.[0-9]*.[0-9]* 2>/dev/null | tail -1)
if [ -n "$WV2_APP" ] && [ -f "$WV2_HOST" ]; then
  WV2_VER=$(basename "$WV2_APP")
  WV2_DST="$WINEPREFIX/drive_c/Program Files (x86)/Microsoft/EdgeWebView/Application"
  mkdir -p "$WV2_DST"
  [ -d "$WV2_DST/$WV2_VER" ] || cp -a --reflink=auto "$WV2_APP" "$WV2_DST/"
  WV2_X="$WV2_DST/$WV2_VER/EBWebView/x64"
  [ -f "$WV2_X/EmbeddedBrowserWebView.real.dll" ] || mv "$WV2_X/EmbeddedBrowserWebView.dll" "$WV2_X/EmbeddedBrowserWebView.real.dll"
  cmp -s "$WV2_HOST" "$WV2_X/EmbeddedBrowserWebView.dll" || cp -f "$WV2_HOST" "$WV2_X/EmbeddedBrowserWebView.dll"
  K='HKLM\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate'
  G='{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}'
  "${WINE_BIN}wine" reg add "$K\\ClientState\\$G" /v EBWebView /d "C:\\Program Files (x86)\\Microsoft\\EdgeWebView\\Application\\$WV2_VER" /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K\\Clients\\$G" /v pv /d "$WV2_VER" /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K\\Clients\\$G" /v name /d "Microsoft Edge WebView2 Runtime" /f >/dev/null 2>&1
  echo "installed: WebView2 runtime $WV2_VER with the wine-uwp host"
fi

# Windows.UI.Shell.TaskbarManager: the real implementation reaches for ImmersiveShell
# ({c2f03a33-...}, not registered under Wine) and, when that fails, never completes the
# IAsyncOperation the caller is waiting on. Solitaire blocks there forever and its render loop
# never starts. See shims/taskbarmanager.
TB_SHIM=${SHIM_DIR}/wineuwp_taskbarmanager.dll
if [ -f "$TB_SHIM" ]; then
  TB_DST=$WINEPREFIX/drive_c/windows/system32/wineuwp_taskbarmanager.dll
  if ! cmp -s "$TB_SHIM" "$TB_DST"; then cp -f --reflink=auto "$TB_SHIM" "$TB_DST"; fi
  K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.UI.Shell.TaskbarManager"
  "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_taskbarmanager.dll' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 1 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  echo "installed: TaskbarManager stand-in"
fi

# Windows.Storage.AccessCache.StorageApplicationPermissions stand-in. An app adds a picked file to
# its future-access list so it can reopen it later; the class does not exist in Wine, so the
# activation throws straight out of the pick with 0x80040111. See shims/accesscache.
AC2_SHIM=${SHIM_DIR}/wineuwp_accesscache.dll
if [ -f "$AC2_SHIM" ]; then
  AC2_DST=$WINEPREFIX/drive_c/windows/system32/wineuwp_accesscache.dll
  if ! cmp -s "$AC2_SHIM" "$AC2_DST"; then cp -f --reflink=auto "$AC2_SHIM" "$AC2_DST"; fi
  K="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.Storage.AccessCache.StorageApplicationPermissions"
  "${WINE_BIN}wine" reg add "$K" /v DllPath /d 'C:\windows\system32\wineuwp_accesscache.dll' /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v Threading /t REG_DWORD /d 1 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$K" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
  echo "installed: StorageApplicationPermissions stand-in"
fi

# ICU data: the genuine icu.dll (used by the native windows.globalization.dll) loads
# Windows\Globalization\ICU\icudtl.dat and the time-zone .res files next to it, and fails with
# E_FAIL without them.
GLOB_SRC=${WIN11}/Windows/Globalization
if [ -d "$GLOB_SRC/ICU" ]; then
  for sub in ICU "Time Zone"; do
    lsub=$(echo "$sub" | tr "A-Z" "a-z"); mkdir -p "$WINEPREFIX/drive_c/windows/globalization/$lsub"
    for f in "$GLOB_SRC/$sub"/*; do
      b=$(basename "$f"); d="$WINEPREFIX/drive_c/windows/globalization/$lsub/$b"
      [ -f "$d" ] && cmp -s "$f" "$d" || cp -f --reflink=auto "$f" "$d"
    done
  done
  echo "installed: Globalization data"
fi

# --- shim registrations are not final until the prefix stops updating -------------------------
# Copying DLLs into system32 above makes the next `wine` invocation run a prefix update, and that
# update re-registers Wine's own builtin WinRT classes - rewriting "DllPath" for every class it
# owns, including ones a shim block above just claimed. This is silent: each `reg add` reports
# success, and the value is Wine's again by the time the app asks for it. It cost a full round of
# debugging on Solitaire, where the DeviceInformation shim was installed, exported correctly and
# never once loaded. The tell was Windows.Devices.Geolocation.Geolocator being rewritten with the
# same timestamp - nothing here touches Geolocator.
#
# So: let any pending update finish, then assert the shim classes again and *verify* by reading
# system.reg, rather than trusting the write.
reassert_activatable() {
  # Two statements, not one: within a single `local`, "$cls" in key= still expands to the
  # *caller's* cls. That wrote every reassert to whatever class an earlier loop left in cls
  # (ActivatableClassId\PackageUser), so a new shim never registered on its first run.
  local cls="$1" dll="$2" threading="${3:-1}"
  local key="HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\$cls"
  "${WINE_BIN}wine" reg add "$key" /v DllPath /d "C:\\windows\\system32\\$dll" /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$key" /v ActivationType /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$key" /v Threading /t REG_DWORD /d "$threading" /f >/dev/null 2>&1
  "${WINE_BIN}wine" reg add "$key" /v TrustLevel /t REG_DWORD /d 0 /f >/dev/null 2>&1
}

# Read the live DllPath straight out of system.reg. The section header is escaped-backslash form.
activatable_dllpath() {
  # `wine reg query` is asked first because it is the only authoritative source: a key written
  # moments ago still lives in wineserver's memory and is not in system.reg until the server
  # flushes, which made the first version of this check report a false failure for the
  # TaskbarManager shim it had just installed correctly. system.reg is the fallback. A query is
  # itself a `wine` invocation and can trigger the very update that clobbers the value - which is
  # fine, because the caller re-asserts and re-checks in a loop.
  local cls=$1 out
  out=$("${WINE_BIN}wine" reg query \
        "HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\$cls" /v DllPath 2>/dev/null \
        | tr -d '\r' | awk '/DllPath/ {print $NF}' | head -1)
  if [ -z "$out" ]; then
    out=$(awk -v key="[Software\\\\\\\\Microsoft\\\\\\\\WindowsRuntime\\\\\\\\ActivatableClassId\\\\\\\\$cls]" '
            index($0, key)==1 {inkey=1; next}
            inkey && /^\[/ {exit}
            inkey && /^"DllPath"=/ {sub(/^"DllPath"="/,""); sub(/"$/,""); gsub(/\\\\/,"\\"); print; exit}
          ' "$WINEPREFIX/system.reg" 2>/dev/null)
  fi
  printf '%s' "$out" | tr 'A-Z' 'a-z'
}

SHIM_CLASSES=""
[ -f "$SYS32/wineuwp_userpicker.dll" ]       && SHIM_CLASSES="$SHIM_CLASSES Windows.System.UserPicker:wineuwp_userpicker.dll"
[ -f "$SYS32/wineuwp_memorymanager.dll" ]    && SHIM_CLASSES="$SHIM_CLASSES Windows.System.MemoryManager:wineuwp_memorymanager.dll"
[ -f "$SYS32/wineuwp_deviceinformation.dll" ] && SHIM_CLASSES="$SHIM_CLASSES Windows.Devices.Enumeration.DeviceInformation:wineuwp_deviceinformation.dll"
[ -f "$SYS32/wineuwp_taskbarmanager.dll" ]    && SHIM_CLASSES="$SHIM_CLASSES Windows.UI.Shell.TaskbarManager:wineuwp_taskbarmanager.dll"
[ -f "$SYS32/wineuwp_storecontext.dll" ]      && SHIM_CLASSES="$SHIM_CLASSES Windows.Services.Store.StoreContext:wineuwp_storecontext.dll"
[ -f "$SYS32/wineuwp_xboxauth.dll" ]          && SHIM_CLASSES="$SHIM_CLASSES Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager:wineuwp_xboxauth.dll"
[ -f "$SYS32/wineuwp_xboxauth.dll" ]          && SHIM_CLASSES="$SHIM_CLASSES Windows.ApplicationModel.Store.Preview.StoreConfiguration:wineuwp_xboxauth.dll"
[ -f "$SYS32/wineuwp_spatialaudio.dll" ]      && SHIM_CLASSES="$SHIM_CLASSES Windows.Media.Audio.SpatialAudioDeviceConfiguration:wineuwp_spatialaudio.dll"
[ -f "$SYS32/wineuwp_spatialaudio.dll" ]      && SHIM_CLASSES="$SHIM_CLASSES Windows.Media.Audio.SpatialAudioFormatSubtype:wineuwp_spatialaudio.dll"
[ -f "$SYS32/wineuwp_networkstatus.dll" ]     && SHIM_CLASSES="$SHIM_CLASSES Windows.Networking.Connectivity.NetworkInformation:wineuwp_networkstatus.dll"
[ -f "$SYS32/wineuwp_useractivitychannel.dll" ] && SHIM_CLASSES="$SHIM_CLASSES Windows.ApplicationModel.UserActivities.UserActivityChannel:wineuwp_useractivitychannel.dll"
[ -f "$SYS32/wineuwp_toastnotifier.dll" ] && SHIM_CLASSES="$SHIM_CLASSES Windows.UI.Notifications.ToastNotificationManager:wineuwp_toastnotifier.dll"
# ext-ms-win-devmgmt-policy-l1-1-1 (see shims/devmgmtpolicy): the ink stack delay-loads it.
DP=${SHIM_DIR}/ext-ms-win-devmgmt-policy-l1-1-1.dll
[ -f "$DP" ] && { cmp -s "$DP" "$SYS32/ext-ms-win-devmgmt-policy-l1-1-1.dll" || cp -f --reflink=auto "$DP" "$SYS32/ext-ms-win-devmgmt-policy-l1-1-1.dll"; }
# api-ms-win-net-isolation-l1-1-1 (see shims/netisolation): not in Wine's apiset schema, so the file
# in system32 under that name is what the delay-load finds.
NI=${SHIM_DIR}/api-ms-win-net-isolation-l1-1-1.dll
[ -f "$NI" ] && { cmp -s "$NI" "$SYS32/api-ms-win-net-isolation-l1-1-1.dll" || cp -f --reflink=auto "$NI" "$SYS32/api-ms-win-net-isolation-l1-1-1.dll"; }
# Windows.Storage.Pickers.FileSavePicker (see shims/filepicker): the native constructor fails
# without the shell broker, and Shapr3D builds one with its dashboard, straight after sign-in.
if [ -f "$SYS32/wineuwp_filepicker.dll" ]; then
  reassert_activatable Windows.Storage.Pickers.FileSavePicker wineuwp_filepicker.dll 1
  SHIM_CLASSES="$SHIM_CLASSES Windows.Storage.Pickers.FileSavePicker:wineuwp_filepicker.dll:1"
  reassert_activatable Windows.Storage.Pickers.FolderPicker wineuwp_filepicker.dll 1
  SHIM_CLASSES="$SHIM_CLASSES Windows.Storage.Pickers.FolderPicker:wineuwp_filepicker.dll:1"
fi
# Windows.Security.Cryptography.Certificates.Certificate (see shims/certificate): certenroll.dll is
# missing, and without the class Windows.Web.Http abandons every HTTPS request.
CERT_SHIM=${SHIM_DIR}/wineuwp_certificate.dll
if [ -f "$CERT_SHIM" ]; then
  cmp -s "$CERT_SHIM" "$SYS32/wineuwp_certificate.dll" || cp -f --reflink=auto "$CERT_SHIM" "$SYS32/wineuwp_certificate.dll"
  reassert_activatable Windows.Security.Cryptography.Certificates.Certificate wineuwp_certificate.dll 0
  SHIM_CLASSES="$SHIM_CLASSES Windows.Security.Cryptography.Certificates.Certificate:wineuwp_certificate.dll:0"
fi
# Windows.System.Launcher (see shims/launcher): the genuine one goes through the shell broker and
# fail-fasts the app; this one hands the URI to ShellExecute, i.e. the host's browser.
LAUNCHER_SHIM=${SHIM_DIR}/wineuwp_launcher.dll
if [ -f "$LAUNCHER_SHIM" ]; then
  cmp -s "$LAUNCHER_SHIM" "$SYS32/wineuwp_launcher.dll" || cp -f --reflink=auto "$LAUNCHER_SHIM" "$SYS32/wineuwp_launcher.dll"
  reassert_activatable Windows.System.Launcher wineuwp_launcher.dll 0
  SHIM_CLASSES="$SHIM_CLASSES Windows.System.Launcher:wineuwp_launcher.dll:0"
fi

# Windows.Graphics.dll's classes that Wine's builtin windows.graphics does not implement, served by
# the genuine DLL. It is installed under another file name because a module is found by name and
# the builtin, which owns DisplayInformation (and our composition work behind it), must keep its
# own. Imaging sits on WIC, which Wine has; Shapr3D's engine throws, and aborts, when
# BitmapTransform cannot be activated. Threading is Both (0), as the Windows registry has it.
WG_SRC=$(ls "$SRC" | grep -i '^windows.graphics.dll$' | head -1)
if [ -n "$WG_SRC" ]; then
  cmp -s "$SRC/$WG_SRC" "$SYS32/windows.graphics.native.dll" || cp -f --reflink=auto "$SRC/$WG_SRC" "$SYS32/windows.graphics.native.dll"
  for cls in Windows.Graphics.Imaging.BitmapDecoder Windows.Graphics.Imaging.BitmapEncoder \
             Windows.Graphics.Imaging.BitmapPropertySet Windows.Graphics.Imaging.BitmapTransform \
             Windows.Graphics.Imaging.BitmapTypedValue Windows.Devices.Display.DisplayMonitor \
             Windows.Devices.Display.LogicalDisplayReference; do
    reassert_activatable "$cls" windows.graphics.native.dll 0
    SHIM_CLASSES="$SHIM_CLASSES $cls:windows.graphics.native.dll:0"
  done
  echo "installed: genuine Windows.Graphics.dll for imaging and display-monitor classes"
fi

if [ -n "$SHIM_CLASSES" ]; then
  # Let any update already in flight finish. Forcing one with `wineboot -u` is deliberately not
  # done here: it starts a fresh services.exe/explorer.exe set on every launch, and run.sh already
  # warns that those leftovers make the activation race fail. The retry loop below copes with a
  # clobber whenever it lands, which is the whole point of verifying rather than asserting once.
  "${WINE_BIN}wineserver" -w 2>/dev/null || true

  # Four checks around three repairs: the last repair is verified too, rather than reported as a
  # failure on the strength of the check before it.
  for attempt in 1 2 3 4; do
    bad=""
    for entry in $SHIM_CLASSES; do
      cls=${entry%%:*}; dll=${entry#*:}; dll=${dll%%:*}
      want="c:\\windows\\system32\\$dll"
      [ "$(activatable_dllpath "$cls")" = "$want" ] || bad="$bad $entry"
    done
    [ -n "$bad" ] && [ $attempt -lt 4 ] || break
    for entry in $bad; do
      rest=${entry#*:}; dll=${rest%%:*}; thr=; [ "$rest" != "$dll" ] && thr=${rest#*:}
      reassert_activatable "${entry%%:*}" "$dll" $thr
    done
    "${WINE_BIN}wineserver" -w 2>/dev/null || true
  done

  if [ -n "$bad" ]; then
    echo "WARNING: shim registration did not stick for:$bad" >&2
  else
    echo "verified: shim ActivatableClassId registrations"
  fi
fi


echo "$stamp" > "$STAMP"
