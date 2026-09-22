#!/usr/bin/env python
"""Log a line from the app's own logger when an address executes, without stopping it.

  tramp.py PE HOOK_RVA NBYTES 'MESSAGE' [CAVE_RVA]

A `ud2` probe answers "did this run" but ends the run. This patches a jmp to a code cave that
calls `OutputDebugStringA`, executes the displaced bytes and jumps back, so the run continues.
Read the messages with `WINEDEBUG=+debugstr`.

**Do not call the app's own logger (rva 0x66940c) from a trampoline.** It prints the line and then
kills the process a few instructions later - every run stopped dead right after the message, which
reads exactly like "the thing I probed is where it hangs". `--nolog` (displace and jump, no call)
is the control that proved it: same hook, same cave, 913 log lines and a normal run. It is
variadic and takes its va_list from the caller's home area, so it is not callable with an
improvised frame.

NBYTES must be >= 5 and land on an instruction boundary; the displaced bytes are re-executed in
the cave, so they must not be rip-relative, a jump or a call. .text has one 297-byte zero pad at
0xdc9ad7 in this build (past VirtualSize, inside SizeOfRawData) - the only cave, so two probes at
once is the practical limit. The trampoline saves the volatile GPRs, the flags **and xmm0-5**:
without the xmm saves the logger clobbers live float arguments and the app dies a few
milliseconds later, which reads exactly like an unrelated stall.



Patches the file in place. Keep a pristine copy and `cmp` after the run - see AGENTS.md.
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pefile

ODS_IAT_RVA = 0xdcba28   # api-ms-win-core-debug-l1-1-0.dll!OutputDebugStringA

def main():
    argv = [a for a in sys.argv[1:] if a != "--nolog"]
    nolog = "--nolog" in sys.argv
    path, hook, nbytes, msg = argv[0], int(argv[1], 16), int(argv[2], 0), argv[3]
    cave = int(argv[4], 16) if len(argv) > 4 else 0xdc9ae0
    if nbytes < 5: raise SystemExit("NBYTES must be at least 5")

    pe = pefile.PE(path)
    off = lambda rva: pe.get_offset_from_rva(rva)
    data = bytearray(open(path, "rb").read())
    displaced = bytes(data[off(hook):off(hook) + nbytes])

    code = bytearray()
    if nolog:
        code += displaced
        jmp_at = len(code)
        code += b"\xe9" + b"\0\0\0\0"
        struct.pack_into("<i", code, jmp_at + 1, (hook + nbytes) - (cave + jmp_at + 5))
        if any(data[off(cave):off(cave) + len(code)]): raise SystemExit("cave not zero")
        data[off(cave):off(cave) + len(code)] = code
        data[off(hook):off(hook) + nbytes] = b"\xe9" + struct.pack("<i", cave - (hook + 5)) + b"\x90" * (nbytes - 5)
        open(path, "wb").write(data)
        print("  hooked %#x (%d bytes) -> cave %#x, no logger call" % (hook, nbytes, cave))
        return
    code += bytes.fromhex("50 51 52 4150 4151 4152 4153 9c".replace(" ", ""))   # save volatiles + flags
    code += bytes.fromhex("4889e0" "4883e4f0" "4881ec80000000")                  # align, xmm save area + shadow
    for i in range(6):                                                           # movaps [rsp+0x20+16i], xmmi
        code += bytes.fromhex("0f29") + bytes([0x44 + i * 8, 0x24, 0x20 + i * 0x10])
    lea_at = len(code)
    code += b"\x48\x8d\x0d" + b"\0\0\0\0"                                        # lea rcx, [rip+msg]
    call_at = len(code)
    code += b"\xff\x15" + b"\0\0\0\0"                                            # call [rip+OutputDebugStringA]
    for i in range(6):                                                           # movaps xmmi, [rsp+0x20+16i]
        code += bytes.fromhex("0f28") + bytes([0x44 + i * 8, 0x24, 0x20 + i * 0x10])
    code += bytes.fromhex("4889c4" "9d" "415b 415a 4159 4158 5a 59 58".replace(" ", ""))
    code += displaced
    jmp_at = len(code)
    code += b"\xe9" + b"\0\0\0\0"                                                # jmp back
    fmt_at = len(code)
    code += msg.encode() + b"\0"

    struct.pack_into("<i", code, lea_at + 3, fmt_at - (lea_at + 7))
    struct.pack_into("<i", code, call_at + 2, ODS_IAT_RVA - (cave + call_at + 6))
    struct.pack_into("<i", code, jmp_at + 1, (hook + nbytes) - (cave + jmp_at + 5))

    if any(data[off(cave):off(cave) + len(code)]):
        raise SystemExit("cave %#x is not zero for %d bytes" % (cave, len(code)))
    data[off(cave):off(cave) + len(code)] = code

    patch = b"\xe9" + struct.pack("<i", cave - (hook + 5)) + b"\x90" * (nbytes - 5)
    data[off(hook):off(hook) + nbytes] = patch

    open(path, "wb").write(data)
    print("  hooked %#x (%d bytes) -> cave %#x, %d bytes, msg %r" % (hook, nbytes, cave, len(code), msg))

main()
