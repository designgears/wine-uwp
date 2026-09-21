#!/usr/bin/env bash
# What is stopping an app right now? Reads a run log and reports the fatal gap, in the order that
# matters - the checks are ordered by how the failure kills the app, not by where it appears in the log.
#
# Usage: tools/wine-uwp/next-gap.sh [log]        default: /tmp/wine-uwp-last.log
#        tools/wine-uwp/next-gap.sh /tmp/*.log   summarises several runs
set -uo pipefail

report_one() {
  local log=$1 found=0
  printf '== %s\n' "$log"

  # A delay-load that cannot be resolved aborts the process, and Wine's hook runs before the
  # "unimplemented function" message, so this is the first thing to look for.
  while read -r line; do
    printf '   delay-load abort : %s\n' "$line"; found=1
  done < <(grep -oE 'failed to delay load [^ ]+' "$log" | sed 's/failed to delay load //' | sort -u)

  # A .spec stub also aborts. Implementing the function as a *failing* call is usually enough.
  while read -r line; do
    printf '   spec stub abort  : %s\n' "$line"; found=1
  done < <(grep -oE 'unimplemented function [^ ,]+' "$log" | sed 's/unimplemented function //' | sort -u)

  # A missing DLL: the loader gives up on the whole import chain.
  while read -r line; do
    printf '   module missing   : %s\n' "$line"; found=1
  done < <(grep -oE 'Library [^ ]+ \(which is needed by [^)]*\) not found' "$log" | sort -u)

  # An unregistered or unimplemented WinRT class.
  while read -r line; do
    printf '   winrt class      : %s\n' "$line"; found=1
  done < <(grep -oE 'RoGetActivationFactory (Class|Failed to find library for) L"[^"]*"' "$log" \
           | sed 's/.*L"//; s/"$//' | sort -u)

  # Whatever the app itself gave up on, last, because it is usually a consequence of the above.
  while read -r line; do
    printf '   app error        : %s\n' "$line"; found=1
  done < <(grep -oE 'unhandled error details: hr [^ ]+ desc L"[^"]*"' "$log" \
           | sed 's/unhandled error details: //' | sort -u)

  [ "$found" = 1 ] || printf '   nothing fatal found\n'
}

if [ $# -eq 0 ]; then report_one /tmp/wine-uwp-last.log
else for f in "$@"; do report_one "$f"; done
fi
