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

LOGGER_RVA  = int(os.environ.get("TRAMP_CALL", "0x66940c"), 16)  # Solitaire logger: ecx=level, edx=cat, r8=fmt
ODS_IAT_RVA = 0xdcba28   # api-ms-win-core-debug-l1-1-0.dll!OutputDebugStringA

def add_pdata_entry(pe, data, begin, end, unwind):
    """Give the cave a RUNTIME_FUNCTION so the unwinder can walk through it.

    Without one, RtlLookupFunctionEntry returns NULL for a return address in the cave and any
    exception raised below - by the logger's own try/catch, or by Wine's OutputDebugStringA -
    derails. .pdata is a sorted array searched by binary chop, and the cave sits above every
    real function, so appending keeps it sorted. The section has spare raw bytes past its
    VirtualSize to grow into.
    """
    d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    sec = next(x for x in pe.sections if x.VirtualAddress == d.VirtualAddress)
    if d.Size + 12 > sec.SizeOfRawData: raise SystemExit(".pdata has no room for another entry")
    off = pe.get_offset_from_rva(d.VirtualAddress) + d.Size
    data[off:off+12] = struct.pack("<III", begin, end, unwind)
    # the directory size and the section's virtual size both have to cover the new entry
    dir_off = pe.OPTIONAL_HEADER.get_file_offset() + 112 + 3 * 8 + 4
    struct.pack_into("<I", data, dir_off, d.Size + 12)
    if sec.Misc_VirtualSize < d.Size + 12:
        struct.pack_into("<I", data, sec.get_file_offset() + 8, d.Size + 12)

def main():
    argv = [a for a in sys.argv[1:] if a not in ("--nolog", "--applog", "--noop")]
    nolog = "--nolog" in sys.argv
    applog = "--applog" in sys.argv
    noop = "--noop" in sys.argv        # full save/restore, no call: isolates prologue from call
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
    # rbx is callee-saved, so it survives the call and can hold the pre-alignment rsp. Using rax
    # for that - the obvious choice - is a silent disaster: rax is the return-value register, the
    # callee overwrites it, and `mov rsp, rax` on the way out sets rsp to garbage. The pops then
    # restore rubbish into rcx and the app faults on the *first instruction after the hook*, which
    # points the blame at the hooked code instead of the probe.
    code += bytes.fromhex("50 51 52 4150 4151 4152 4153 53 9c".replace(" ", ""))
    code += bytes.fromhex("4889e3" "4883e4f0" "4881ec80000000")                  # rbx=rsp, align, xmm+shadow
    for i in range(6):                                                           # movaps [rsp+0x20+16i], xmmi
        code += bytes.fromhex("0f29") + bytes([0x44 + i * 8, 0x24, 0x20 + i * 0x10])
    lea_at = len(code)
    lea_reg = b"\x4c\x8d\x05" if applog else b"\x48\x8d\x0d"                      # r8 for the app logger, rcx for ODS
    code += lea_reg + b"\0\0\0\0"
    call_at = len(code)
    if noop:
        pass
    elif applog:
        code += bytes.fromhex("4531c9" "ba01000000" "b901000000")                 # r9=0, edx=1, ecx=1
        call_at = len(code)
        code += b"\xe8" + b"\0\0\0\0"                                            # call the app's logger
    else:
        code += b"\xff\x15" + b"\0\0\0\0"                                        # call [rip+OutputDebugStringA]
    for i in range(6):                                                           # movaps xmmi, [rsp+0x20+16i]
        code += bytes.fromhex("0f28") + bytes([0x44 + i * 8, 0x24, 0x20 + i * 0x10])
    code += bytes.fromhex("4889dc" "9d" "5b 415b 415a 4159 4158 5a 59 58".replace(" ", ""))
    code += displaced
    jmp_at = len(code)
    code += b"\xe9" + b"\0\0\0\0"                                                # jmp back
    fmt_at = len(code)
    code += msg.encode() + b"\0"
    while len(code) % 4: code += b"\0"
    unwind_at = len(code)
    code += bytes([1, 0, 0, 0])              # UNWIND_INFO: version 1, no prologue, no codes

    struct.pack_into("<i", code, lea_at + 3, fmt_at - (lea_at + 7))
    if noop:     pass
    elif applog: struct.pack_into("<i", code, call_at + 1, LOGGER_RVA - (cave + call_at + 5))
    else:        struct.pack_into("<i", code, call_at + 2, ODS_IAT_RVA - (cave + call_at + 6))
    struct.pack_into("<i", code, jmp_at + 1, (hook + nbytes) - (cave + jmp_at + 5))

    if any(data[off(cave):off(cave) + len(code)]):
        raise SystemExit("cave %#x is not zero for %d bytes" % (cave, len(code)))
    data[off(cave):off(cave) + len(code)] = code
    add_pdata_entry(pe, data, cave, cave + len(code), cave + unwind_at)

    patch = b"\xe9" + struct.pack("<i", cave - (hook + 5)) + b"\x90" * (nbytes - 5)
    data[off(hook):off(hook) + nbytes] = patch

    open(path, "wb").write(data)
    print("  hooked %#x (%d bytes) -> cave %#x, %d bytes, msg %r" % (hook, nbytes, cave, len(code), msg))

main()
