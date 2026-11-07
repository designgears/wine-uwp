#!/usr/bin/env bash
# Build WineGDK's unchanged notification implementation separately from Wine-UWP.
# Usage: build-push-notifications.sh WINE_BUILD WINE_SOURCE OUTPUT
set -euo pipefail
wine_build=$(cd "${1:?Wine build directory required}" && pwd)
wine_source=$(cd "${2:?Wine source directory required}" && pwd)
mkdir -p "${3:?Output directory required}"
output=$(cd "$3" && pwd)
manifest="$(cd "$(dirname "$0")" && pwd)/winegdk-push.sha256"
commit=b53268302fd0b7479a4cb83d4c7719ba96eb28ad
source="$output/source"
mkdir -p "$source"
while read -r checksum relative; do
    target="$source/$relative"
    if [[ ! -f "$target" ]] || ! (cd "$source" && printf '%s  %s\n' "$checksum" "$relative" | sha256sum --status -c -); then
        mkdir -p "$(dirname "$target")"
        curl --fail --silent --show-error --location --retry 3 \
            "https://raw.githubusercontent.com/BedrockNative/WineGDK/$commit/$relative" -o "$target.download"
        printf '%s  %s\n' "$checksum" "$target.download" | sha256sum -c -
        mv "$target.download" "$target"
    fi
done < "$manifest"
(cd "$source" && sha256sum -c "$manifest")

cd "$wine_build"
module="$source/dlls/windows.networking.connectivity"
includes=(-I"$output" -I"$source/include" -I"$module" -Iinclude -I"$wine_source/include")
for name in windows.networking.pushnotifications async_private; do
    if [[ "$name" == async_private ]]; then idl="$module/$name.idl"; else idl="$source/include/$name.idl"; fi
    tools/widl/widl -o "$output/$name.h" -m64 --nostdinc "${includes[@]}" -D__WINESRC__ "$idl"
done
compiler=${PE_CC:-x86_64-w64-mingw32-gcc}
for name in main network_information async pushnotifications; do
    "$compiler" -c -o "$output/$name.o" "$module/$name.c" "${includes[@]}" \
        -I"$wine_source/include/msvcrt" -D_UCRT -D__WINESRC__ -D__WINE_PE_BUILD \
        -Wall -fno-strict-aliasing -ffunction-sections -mlong-double-64 -mcx16 -mcmodel=small -O2
done
tools/winegcc/winegcc -o "$output/wineuwp-push-notifications.dll" --wine-objdir . \
    --cc-cmd="$compiler" -b x86_64-w64-mingw32 -shared \
    "$module/windows.networking.connectivity.spec" -Wb,-F,wineuwp-push-notifications.dll \
    "$output/main.o" "$output/network_information.o" "$output/async.o" "$output/pushnotifications.o" \
    dlls/combase/x86_64-windows/libcombase.a dlls/propsys/x86_64-windows/libpropsys.a \
    libs/uuid/x86_64-windows/libuuid.a libs/winecrt0/x86_64-windows/libwinecrt0.a \
    libs/compiler-rt/x86_64-windows/libcompiler-rt.a dlls/ucrtbase/x86_64-windows/libucrtbase.a \
    dlls/kernel32/x86_64-windows/libkernel32.a dlls/ntdll/x86_64-windows/libntdll.a -Wl,--build-id
sha256sum "$output/wineuwp-push-notifications.dll"
