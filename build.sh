#!/usr/bin/env bash
# Configure + build probes and shims with clang-cl against the xwin SDK.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]:-$0}")/./env.sh"
cd "$(dirname "$0")"
WV2SDK=""; [ -f "${UWP_BIN}/webview2/sdk/pkg/build/native/include/WebView2.h" ] && WV2SDK="${UWP_BIN}/webview2/sdk/pkg/build/native/include"
cmake -S . -B "${UWP_BIN}/build-probes" -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE="$PWD/clang-cl-x64.cmake" -DCMAKE_BUILD_TYPE=Release ${WV2SDK:+-DWEBVIEW2_SDK=$WV2SDK} >/dev/null
cmake --build "${UWP_BIN}/build-probes" -j
