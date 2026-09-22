#!/usr/bin/env python
"""Log a line from the app's own logger when an address executes, without stopping it.

  tramp.py PE HOOK_RVA NBYTES 'MESSAGE' [CAVE_RVA] [--applog|--nolog|--noop]

"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pefile, subprocess

def check_boundary(img, hook, nbytes):
    """Refuse a displacement that splits an instruction, or copies one that cannot be relocated.

    A mid-instruction split silently corrupts the function and the app then dies somewhere
    unrelated - a whole run wasted on a probe that was never going to work. objdump is already a
    dependency (disas.py uses it), so check rather than guess.
    """
    open("/tmp/.tramp-check.bin", "wb").write(bytes(img[hook:hook + nbytes + 16]))
    out = subprocess.run(["objdump", "-D", "-b", "binary", "-m", "i386:x86-64", "-M", "intel",
                          "--adjust-vma=%#x" % hook, "/tmp/.tramp-check.bin"],
                         capture_output=True, text=True).stdout
    addrs, text = [], []
    for line in out.splitlines():
        if ":\t" in line:
            a, rest = line.split(":\t", 1)
            try: addrs.append(int(a.strip(), 16))
            except ValueError: continue
            text.append(rest)
    if hook + nbytes not in addrs:
        raise SystemExit("%d bytes at %#x splits an instruction; boundaries are %s"
                         % (nbytes, hook, " ".join("+%d" % (a - hook) for a in addrs if a > hook)))
    riprel = []
    for i, (a, t) in enumerate(zip(addrs, text)):
        if a >= hook + nbytes: break
        end = addrs[i + 1] if i + 1 < len(addrs) else hook + nbytes
        mnem = t.split("\t")[-1].split()[0] if t.split() else ""
        if mnem in ("call", "jmp", "je", "jne", "ja", "jb", "jg", "jl", "jle",
                    "jge", "jbe", "jae", "js", "jns", "loop"):
            raise SystemExit("displaced instruction at %#x is not relocatable: %s" % (a, t.strip()))
        if "rip+" in t or "rip-" in t:
            # rip-relative operands still work in the cave if the disp32 is rebased. Only safe for
            # instructions with no trailing immediate, where the disp32 is the last four bytes.
            op = img[a] if img[a] not in (0x48, 0x4c, 0x49, 0x4d, 0x66, 0xf2, 0xf3) else img[a + 1]
            if op not in (0x8d, 0x8b, 0x89, 0x03, 0x2b, 0x33, 0x3b, 0x85, 0x39, 0x63, 0x0f):
                raise SystemExit("displaced rip-relative instruction at %#x is not relocatable: %s"
                                 % (a, t.strip()))
            riprel.append((a - hook, end - hook - 4))    # (offset of insn, offset of its disp32)
    return riprel

LOGGER_RVA  = int(os.environ.get("TRAMP_CALL", "0x66940c"), 16)  # Solitaire logger: ecx=level, edx=cat, r8=fmt
ODS_IAT_RVA = 0xdcba28   # api-ms-win-core-debug-l1-1-0.dll!OutputDebugStringA

# Saved-register slots relative to rbx, which the trampoline sets to rsp after its nine pushes
# (rax rcx rdx r8 r9 r10 r11 rbx, then the flags).
SAVED = {"flags": 0, "rbx": 8, "r11": 16, "r10": 24, "r9": 32, "r8": 40,
         "rdx": 48, "rcx": 56, "rax": 64, "ret": 72}

def value_code(val):
    """Emit code leaving the value to print in r10.

    --val=rcx, --val=ret, --val=[rcx+0x30], --val=[[rcx+0x30]+0x14c] - brackets nest, so a chain
    of pointers can be followed in one probe.
    """
    derefs = []
    val = val.strip()
    while val.startswith("[") and val.endswith("]"):
        inner, depth, cut = val[1:-1], 0, None
        for i, ch in enumerate(inner):
            if ch == "[": depth += 1
            elif ch == "]": depth -= 1
            elif ch == "+" and depth == 0: cut = i
        base, disp = (inner, 0) if cut is None else (inner[:cut], int(inner[cut + 1:], 0))
        derefs.insert(0, disp)
        val = base.strip()
    if val not in SAVED:
        raise SystemExit("--val: base must be one of %s" % ", ".join(SAVED))
    code = b"\x4c\x8b\x53" + bytes([SAVED[val]])                    # mov r10, [rbx+slot]
    for d in derefs:
        code += b"\x4d\x8b\x92" + struct.pack("<i", d)              # mov r10, [r10+disp]
    return code

def hex_code(buf_rel):
    """Write r10 as 16 hex digits at [rip+buf_rel] (patched by the caller)."""
    return (b"\x4c\x8d\x1d" + struct.pack("<i", buf_rel) +          # lea r11, [rip+buf]
            b"\xba\x10\x00\x00\x00" +                              # mov edx, 16 (not ecx: rcx
                                                                     #  already holds the message)
            b"\x4c\x89\xd0"                                          # loop: mov rax, r10
            b"\x48\xc1\xe8\x3c"                                     #       shr rax, 60
            b"\x3c\x09"                                               #       cmp al, 9
            b"\x76\x02"                                               #       jbe +2
            b"\x04\x27"                                               #       add al, 0x27
            b"\x04\x30"                                               #       add al, '0'
            b"\x41\x88\x03"                                          #       mov [r11], al
            b"\x49\xff\xc3"                                          #       inc r11
            b"\x49\xc1\xe2\x04"                                     #       shl r10, 4
            b"\xff\xca"                                               #       dec edx
            b"\x75\xe3")                                              #       jnz loop

def add_cave_section(path, size=0x10000, name=b".tramp"):
    """Append an RWX section to hold trampolines, and return its RVA.

    The 297-byte pad at the end of .text fits two probes, which is not enough to instrument a
    whole vtable in one run. A fresh section costs a header slot and some raw data, and there is
    room before the first section's raw data for more headers in this binary.
    """
    pe = pefile.PE(path)
    opt, fh = pe.OPTIONAL_HEADER, pe.FILE_HEADER
    for sec in pe.sections:
        if sec.Name.rstrip(b"\0") == name:
            return sec.VirtualAddress
    last = max(pe.sections, key=lambda x: x.VirtualAddress)
    def up(v, a): return (v + a - 1) // a * a
    rva = up(last.VirtualAddress + last.Misc_VirtualSize, opt.SectionAlignment)
    raw_size = up(size, opt.FileAlignment)
    data = bytearray(open(path, "rb").read())
    raw_ptr = up(len(data), opt.FileAlignment)
    data += b"\0" * (raw_ptr - len(data)) + b"\0" * raw_size

    hdr = last.get_file_offset() + 40
    if hdr + 40 > min(x.PointerToRawData for x in pe.sections):
        raise SystemExit("no room in the PE header for another section")
    data[hdr:hdr + 40] = (name.ljust(8, b"\0") + struct.pack("<IIII", size, rva, raw_size, raw_ptr)
                          + struct.pack("<IIHHI", 0, 0, 0, 0, 0xE0000020))   # code, read, write, execute
    struct.pack_into("<H", data, fh.get_file_offset() + 2, fh.NumberOfSections + 1)
    struct.pack_into("<I", data, opt.get_file_offset() + 56, up(rva + size, opt.SectionAlignment))
    open(path, "wb").write(data)
    return rva

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
    argv = [a for a in sys.argv[1:]
            if a not in ("--nolog", "--applog", "--noop") and not a.startswith("--val=")]
    nolog = "--nolog" in sys.argv
    applog = "--applog" in sys.argv
    noop = "--noop" in sys.argv        # full save/restore, no call: isolates prologue from call
    val = next((a[6:] for a in sys.argv if a.startswith("--val=")), None)
    path, hook, nbytes, msg = argv[0], int(argv[1], 16), int(argv[2], 0), argv[3]
    cave = int(argv[4], 16) if len(argv) > 4 else None
    if nbytes < 5: raise SystemExit("NBYTES must be at least 5")

    if cave is None:
        base = add_cave_section(path)
        pe = pefile.PE(path)
        img = pe.get_memory_mapped_image()
        sec = next(x for x in pe.sections if x.VirtualAddress == base)
        cave = next((base + i for i in range(0, sec.Misc_VirtualSize - 256, 16)
                     if not any(img[base + i:base + i + 256])), None)
        if cave is None: raise SystemExit("the .tramp section is full")

    pe = pefile.PE(path)
    off = lambda rva: pe.get_offset_from_rva(rva)
    data = bytearray(open(path, "rb").read())
    riprel = check_boundary(pe.get_memory_mapped_image(), hook, nbytes)
    displaced = bytearray(data[off(hook):off(hook) + nbytes])

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
        # r9 is the logger's first vararg, but the formatter wants a typed argument descriptor,
        # not a raw value - a "{}" in the message comes out literally. Messages are constants.
        code += bytes.fromhex("4531c9" "ba01000000" "b901000000")                 # r9=0, edx=1, ecx=1
        call_at = len(code)
        code += b"\xe8" + b"\0\0\0\0"                                            # call the app's logger
    else:
        if val:
            code += value_code(val)
            hex_at = len(code)
            code += hex_code(0)                                                   # patched below
        call_at = len(code)          # must be re-taken: the value code moved it
        code += b"\xff\x15" + b"\0\0\0\0"                                        # call [rip+OutputDebugStringA]
    for i in range(6):                                                           # movaps xmmi, [rsp+0x20+16i]
        code += bytes.fromhex("0f28") + bytes([0x44 + i * 8, 0x24, 0x20 + i * 0x10])
    code += bytes.fromhex("4889dc" "9d" "5b 415b 415a 4159 4158 5a 59 58".replace(" ", ""))
    disp_at = len(code)
    code += displaced
    for _, d in riprel:                      # rebase each rip-relative displacement into the cave
        old = struct.unpack_from("<i", code, disp_at + d)[0]
        struct.pack_into("<i", code, disp_at + d, old + hook - (cave + disp_at))
    jmp_at = len(code)
    code += b"\xe9" + b"\0\0\0\0"                                                # jmp back
    fmt_at = len(code)
    code += (msg + (" 0000000000000000" if (val and not applog) else "")).encode() + b"\0"
    if val and not applog:
        buf_at = fmt_at + len(msg) + 1
        struct.pack_into("<i", code, hex_at + 3, buf_at - (hex_at + 7))
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
