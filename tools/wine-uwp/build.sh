#!/usr/bin/env bash
# Configure + build probes and shims with clang-cl against the xwin SDK.
set -euo pipefail
cd "$(dirname "$0")"
cmake -S . -B "$HOME/uwp-bin/build-probes" -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE="$PWD/clang-cl-x64.cmake" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$HOME/uwp-bin/build-probes" -j
