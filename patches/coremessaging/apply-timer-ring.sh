#!/bin/bash
# Re-apply the due-now timer ring to CoreMessaging.dll 26100.8972 (see docs/Findings-M4.md,
# "The blank-window flake"). Input: the previously patched DLL with the single-slot mailbox
# (CoreMessaging.dll.bak-mailbox). Writes the ring into the free cave at 0xd4ab0, keeps its data in
# HotPatchSpareGlobals (+0x10dee0), and re-points the enqueue call (0xd4a1b) and the drain
# entry (0xd49e0). Copies the result into the prefix and checks both copies match.
set -e
cd "$(dirname "$0")/../.."; source env.sh
D=$UWP_BIN/win11/Windows/System32/CoreMessaging.dll
T=$(mktemp -d)
as patches/coremessaging/timer-ring.s -o $T/r.o
ld -Ttext=0x1800d4ab0 --defsym=LOCK=0x18010dee0 --defsym=TID=0x18010dee4 --defsym=HEAD=0x18010dee8 \
   --defsym=TAIL=0x18010deec --defsym=ENQ=0x18010def0 --defsym=DEQ=0x18010def4 --defsym=DROP=0x18010def8 \
   --defsym=RING=0x18010df00 --defsym=ONTIMER=0x1800551f0 -e enq $T/r.o -o $T/r.elf 2>/dev/null
DEQ=$(nm $T/r.elf | awk '$3=="deq"{print $1}')
objcopy -O binary -j .text $T/r.elf $T/r.bin
"$UWP_BIN/venv/bin/python" - "$D" "$T/r.bin" "$DEQ" <<'P'
import struct, sys
D, ringf, deq = sys.argv[1], sys.argv[2], int(sys.argv[3], 16) - 0x180000000
b = bytearray(open(D + '.bak-mailbox', 'rb').read())
ring = open(ringf, 'rb').read()
assert all(x == 0 for x in b[0xd4ab0:0xd4ab0 + len(ring)]) and 0xd4ab0 + len(ring) < 0xd5000
b[0xd4ab0:0xd4ab0 + len(ring)] = ring
assert b[0xd4a1b] == 0xe8 and b[0xd49e0] == 0xe9
b[0xd4a1c:0xd4a20] = struct.pack('<i', 0xd4ab0 - (0xd4a1b + 5))
b[0xd49e1:0xd49e5] = struct.pack('<i', deq - (0xd49e0 + 5))
open(D, 'wb').write(b)
P
cp "$D" "$WINEPREFIX/drive_c/windows/system32/coremessaging.dll"
cmp "$D" "$WINEPREFIX/drive_c/windows/system32/coremessaging.dll" && echo "timer ring applied"
rm -rf $T
