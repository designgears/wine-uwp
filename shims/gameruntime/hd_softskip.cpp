/* Runtime HD/module EOF soft-skip for Halo Infinite.
 *
 * Parent game/HaloInfinite.exe is patched on disk (repro20). CampaignS1's
 * HaloInfinite.exe keeps .text encrypted on disk (entropy ~8), so the same
 * byte patches cannot be applied statically. This scanner runs inside the
 * process after load, finds the decrypted instruction sites, and applies:
 *   1) NOP the HD capability jae (stops ModuleSizeOnDisk:0 from a false HD bit)
 *   2) Redirect the EOF ja into a cave that undoes the 0x50 enqueue and marks done
 *   3) NOP the halt call that would otherwise RaiseException on resource fail
 *
 * Safe on the already-patched parent: original jae bytes are gone, so step 1
 * is a no-op and later steps only fire when their own patterns match.
 */
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <cstdint>

static void ss_log(const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    fputs(buf, stderr);
    fflush(stderr);
    OutputDebugStringA(buf);
    HANDLE file = CreateFileW(L"C:\\users\\bobby\\AppData\\Local\\Temp\\gdk-content.log",
                              FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD got = 0;
        WriteFile(file, buf, (DWORD)strlen(buf), &got, nullptr);
        CloseHandle(file);
    }
}

static bool patch_bytes(void *addr, const void *src, size_t n)
{
    DWORD old = 0;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(addr, src, n);
    FlushInstructionCache(GetCurrentProcess(), addr, n);
    DWORD ign = 0;
    VirtualProtect(addr, n, old, &ign);
    return true;
}

static BYTE *find_pat(BYTE *hay, size_t hay_len, const BYTE *needle, size_t nlen)
{
    if (nlen == 0 || hay_len < nlen) return nullptr;
    for (size_t i = 0; i + nlen <= hay_len; i++)
    {
        if (memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    }
    return nullptr;
}

static double text_entropy(BYTE *hay, size_t hay_len)
{
    unsigned hist[256] = {};
    size_t sample = hay_len < (1u << 20) ? hay_len : (1u << 20);
    for (size_t i = 0; i < sample; i++) hist[hay[i]]++;
    double ent = 0.0;
    for (int b = 0; b < 256; b++)
    {
        if (!hist[b]) continue;
        double p = (double)hist[b] / (double)sample;
        ent -= p * (log(p) / log(2.0));
    }
    return ent;
}

/* cmp r64, [reg+0x58]; jae rel8 — accept any cmp reg (rax-rdi via REX.W 3B). */
static BYTE *find_hd_jae(BYTE *hay, size_t hay_len, int *hit_count)
{
    BYTE *best = nullptr;
    *hit_count = 0;
    for (size_t i = 0; i + 6 <= hay_len; i++)
    {
        /* 48/49 3B ModRM disp8=0x58 ; 73 xx */
        if ((hay[i] != 0x48 && hay[i] != 0x49) || hay[i + 1] != 0x3B) continue;
        BYTE modrm = hay[i + 2];
        if ((modrm >> 6) != 1) continue;       /* mod = disp8 */
        if (hay[i + 3] != 0x58) continue;      /* +0x58 */
        if (hay[i + 4] != 0x73 && hay[i + 4] != 0x90) continue;
        (*hit_count)++;
        /* Prefer jae followed by dec/test/jg shape (41 FF CA or FF CA). */
        BYTE *jae = hay + i + 4;
        if (jae[0] == 0x73 && i + 10 < hay_len)
        {
            if ((hay[i + 6] == 0x41 && hay[i + 7] == 0xFF && hay[i + 8] == 0xCA) ||
                (hay[i + 6] == 0xFF && hay[i + 7] == 0xC8) /* dec eax */ ||
                (hay[i + 6] == 0xFF && hay[i + 7] == 0xC9) /* dec ecx */ ||
                (hay[i + 6] == 0xFF && (hay[i + 7] & 0xF8) == 0xC8))
                return jae;
            if (!best) best = jae;
        }
        else if (jae[0] == 0x90 && !best)
            best = jae;
    }
    return best;
}

/* Fallback: mov r32, 2 ; ... within 48 bytes ... cmp ..,+0x58 ; jae */
static BYTE *find_hd_jae_via_mov2(BYTE *hay, size_t hay_len)
{
    for (size_t i = 0; i + 8 <= hay_len; i++)
    {
        /* B? 02 00 00 00  (mov r32, 2) or 41 B8..BF 02 00 00 00 */
        size_t mov_at = i;
        if (hay[i] >= 0xB8 && hay[i] <= 0xBF &&
            hay[i + 1] == 0x02 && hay[i + 2] == 0 && hay[i + 3] == 0 && hay[i + 4] == 0)
        { /* ok */ }
        else if (hay[i] == 0x41 && hay[i + 1] >= 0xB8 && hay[i + 1] <= 0xBF &&
                 hay[i + 2] == 0x02 && hay[i + 3] == 0 && hay[i + 4] == 0 && hay[i + 5] == 0)
            mov_at = i;
        else
            continue;

        for (size_t j = mov_at + 5; j + 6 < mov_at + 64 && j + 6 < hay_len; j++)
        {
            if ((hay[j] == 0x48 || hay[j] == 0x49) && hay[j + 1] == 0x3B &&
                hay[j + 3] == 0x58 && hay[j + 4] == 0x73)
                return hay + j + 4;
            /* also 3B without REX for 32-bit cmp eax,[reg+58]; jae */
            if (hay[j] == 0x3B && ((hay[j + 1] >> 6) == 1) && hay[j + 2] == 0x58 &&
                hay[j + 3] == 0x73)
                return hay + j + 3;
        }
    }
    return nullptr;
}

/* and dword [reg+0x10], 0xfffffffb — clears capability bit 2 in parent epilogue */
static int count_and_plus10_fb(BYTE *hay, size_t hay_len)
{
    int n = 0;
    for (size_t i = 0; i + 4 <= hay_len; i++)
    {
        if (hay[i] == 0x83 && hay[i + 2] == 0x10 && hay[i + 3] == 0xFB)
            n++;
    }
    return n;
}

static BYTE *find_eof_ja(BYTE *hay, size_t hay_len, BYTE **enqueue_insn, BYTE **mark_done)
{
    *enqueue_insn = nullptr;
    *mark_done = nullptr;
    for (size_t i = 0; i + 16 <= hay_len; i++)
    {
        /* add qword [reg+8], 0x50 */
        if (hay[i] != 0x48 || hay[i + 1] != 0x83) continue;
        BYTE modrm = hay[i + 2];
        if (((modrm >> 3) & 7) != 0) continue;
        if ((modrm >> 6) != 1) continue;
        if (hay[i + 3] != 0x08 || hay[i + 4] != 0x50) continue;

        for (size_t j = i + 5; j + 7 < i + 48 && j + 7 < hay_len; j++)
        {
            /* cmp rax, r12..r15 ; ja near  OR  cmp rax, r64 via 4C 3B */
            bool is_cmp = false;
            size_t ja_at = 0;
            if (hay[j] == 0x49 && hay[j + 1] == 0x3B &&
                hay[j + 2] >= 0xC4 && hay[j + 2] <= 0xC7 &&
                hay[j + 3] == 0x0F && hay[j + 4] == 0x87)
            {
                is_cmp = true;
                ja_at = j + 3;
            }
            else if (hay[j] == 0x4C && hay[j + 1] == 0x3B && /* cmp r8+, r/m */
                     hay[j + 3] == 0x0F && hay[j + 4] == 0x87)
            {
                is_cmp = true;
                ja_at = j + 3;
            }
            else if (hay[j] == 0x48 && hay[j + 1] == 0x3B &&
                     hay[j + 3] == 0x0F && hay[j + 4] == 0x87)
            {
                is_cmp = true;
                ja_at = j + 3;
            }
            if (!is_cmp) continue;

            *enqueue_insn = hay + i;
            BYTE *ja = hay + ja_at;
            for (size_t k = ja_at + 6; k + 5 < ja_at + 0x140 && k + 5 < hay_len; k++)
            {
                if (hay[k] == 0x66 && hay[k + 1] == 0x83 &&
                    (hay[k + 2] & 0xC7) == 0x41 && /* mod=01 reg=0?/1 rm varies — or word */
                    hay[k + 3] == 0x68 && hay[k + 4] == 0x10)
                {
                    /* 66 83 /1 : or. Prefer rcx (ModRM 0x49). */
                    if (((hay[k + 2] >> 3) & 7) != 1) continue;
                    if (k >= 3 && hay[k - 3] == 0x49 && hay[k - 2] == 0x8B && hay[k - 1] == 0x0E)
                        *mark_done = hay + k - 3;
                    else if (!*mark_done)
                        *mark_done = hay + k;
                }
            }
            return ja;
        }
    }
    return nullptr;
}

static BYTE *find_halt_call(BYTE *hay, size_t hay_len)
{
    static const BYTE after[] = { 0x49, 0x8B, 0x3E, 0x83, 0x7F, 0x58, 0xFF, 0x74, 0x09 };
    BYTE *hit = find_pat(hay, hay_len, after, sizeof(after));
    if (!hit || hit < hay + 5) return nullptr;
    if (hit[-5] != 0xE8 && hit[-5] != 0x90) return nullptr;
    return hit - 5;
}


/*
 * BG NET WORKER ClientSetup crash (fingerprint 0x41A20E35CB21FE14):
 *   mov r14, [r14] / mov r8, [r8] linked-list walk with count > actual nodes
 *   -> null deref at 0x1405C8DFD (and twin at 0x1405C9074).
 * Pattern is the walk body + the following mov rax,[rXX]; mov [rbx],rax.
 */
static BYTE *find_zero_cave(BYTE *hay, size_t hay_len, size_t need);
static void emit_abs_jmp(BYTE *dst, void *target);

static BYTE *find_bgnet_walk_r14(BYTE *hay, size_t hay_len)
{
    /* 4D 8B 36  48 83 E9 01  75 F7  49 8B 06  48 89 03 */
    static const BYTE pat[] = {
        0x4D, 0x8B, 0x36, 0x48, 0x83, 0xE9, 0x01, 0x75, 0xF7,
        0x49, 0x8B, 0x06, 0x48, 0x89, 0x03
    };
    return find_pat(hay, hay_len, pat, sizeof(pat));
}

static BYTE *find_bgnet_walk_r8(BYTE *hay, size_t hay_len)
{
    /* 4D 8B 00  48 83 E9 01  75 F7  49 8B 00  48 89 03 */
    static const BYTE pat[] = {
        0x4D, 0x8B, 0x00, 0x48, 0x83, 0xE9, 0x01, 0x75, 0xF7,
        0x49, 0x8B, 0x00, 0x48, 0x89, 0x03
    };
    return find_pat(hay, hay_len, pat, sizeof(pat));
}


static BYTE *find_bgnet_walk_r8_subr9(BYTE *hay, size_t hay_len)
{
    /* 4D 8B 00  4C 2B C9  75 F7  — mov r8,[r8]; sub rcx,r9; jne (seen at 0x1405C8C86) */
    static const BYTE pat[] = {
        0x4D, 0x8B, 0x00, 0x4C, 0x2B, 0xC9, 0x75, 0xF7,
        0x49, 0x8B, 0x00, 0x48, 0x89, 0x03
    };
    BYTE *hit = find_pat(hay, hay_len, pat, sizeof(pat));
    if (hit) return hit;
    /* jne imm may not be F7; scan flexibly */
    for (size_t i = 0; i + 14 <= hay_len; i++)
    {
        if (hay[i]==0x4D && hay[i+1]==0x8B && hay[i+2]==0x00 &&
            hay[i+3]==0x4C && hay[i+4]==0x2B && hay[i+5]==0xC9 &&
            hay[i+6]==0x75 &&
            hay[i+8]==0x49 && hay[i+9]==0x8B && hay[i+10]==0x00 &&
            hay[i+11]==0x48 && hay[i+12]==0x89 && hay[i+13]==0x03)
            return hay + i;
    }
    return nullptr;
}

/* Absolute jmp: mov rax, imm64; jmp rax (12 bytes). */
static void emit_abs_jmp(BYTE *dst, void *target)
{
    dst[0] = 0x48; dst[1] = 0xB8;
    memcpy(dst + 2, &target, 8);
    dst[10] = 0xFF; dst[11] = 0xE0;
}

/* Null-safe list walk for r14 version. Overwrites the 9-byte walk loop with
 * jmp-to-cave; cave clears the queue on null and resumes after the pop. */
static int patch_bgnet_walk_r14(BYTE *walk, BYTE *text, size_t text_len)
{
    /* walk points at mov r14,[r14] (0x1405C8DFD).
     * Full or partial pop: resume at mov rax,[r14] (walk+9) with edi=actual M.
     * Empty list: resume at test rsi,rsi (walk+0x5A) with rsi=0. */
    BYTE *resume_ok = walk + 9;          /* mov rax, [r14] */
    BYTE *resume_abort = walk + 0x5A;    /* test rsi,rsi -> empty batch */

    if (walk[0] == 0xE9)
    {
        ss_log("hd-softskip: BGNET r14 walk already patched at %p\n", walk);
        return 0;
    }

    BYTE *cave = find_zero_cave(text, text_len, 64);
    int cave_remote = 0;
    if (!cave)
    {
        cave = (BYTE *)VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        cave_remote = 1;
    }
    if (!cave)
    {
        ss_log("hd-softskip: no cave for BGNET r14 walk\n");
        return 0;
    }

    /*
     * Partial-pop repair:
     *   peek next; if NULL, r14 is last real node — set edi=M=N-rcx and resume_ok
     *   so unlink uses real count. Empty list → zero bucket and take empty-batch path.
     */
    BYTE code[128];
    int o = 0;

    code[o++] = 0x4D; code[o++] = 0x85; code[o++] = 0xF6;           /* test r14,r14 */
    code[o++] = 0x74; code[o++] = 0x00;                               /* jz empty */
    int jz_empty_at = o - 1;

    int loop_at = o;
    code[o++] = 0x49; code[o++] = 0x8B; code[o++] = 0x06;           /* mov rax,[r14] */
    code[o++] = 0x48; code[o++] = 0x85; code[o++] = 0xC0;           /* test rax,rax */
    code[o++] = 0x74; code[o++] = 0x00;                               /* jz short */
    int jz_short_at = o - 1;
    code[o++] = 0x49; code[o++] = 0x89; code[o++] = 0xC6;           /* mov r14,rax */
    code[o++] = 0x48; code[o++] = 0x83; code[o++] = 0xE9; code[o++] = 0x01; /* sub rcx,1 */
    code[o++] = 0x75; code[o++] = 0x00;                               /* jnz loop */
    int jnz_at = o - 1;
    /* jmp resume_ok (full) */
    if (cave_remote) { emit_abs_jmp(code + o, resume_ok); o += 12; }
    else {
        code[o++] = 0xE9;
        INT32 rel = (INT32)(resume_ok - (cave + o + 4));
        memcpy(code + o, &rel, 4); o += 4;
    }

    int short_at = o;
    code[jz_short_at] = (BYTE)(short_at - (jz_short_at + 1));
    code[o++] = 0x89; code[o++] = 0xF8;                               /* mov eax,edi */
    code[o++] = 0x29; code[o++] = 0xC8;                               /* sub eax,ecx */
    code[o++] = 0x89; code[o++] = 0xC7;                               /* mov edi,eax (M) */
    if (cave_remote) { emit_abs_jmp(code + o, resume_ok); o += 12; }
    else {
        code[o++] = 0xE9;
        INT32 rel = (INT32)(resume_ok - (cave + o + 4));
        memcpy(code + o, &rel, 4); o += 4;
    }

    int empty_at = o;
    code[jz_empty_at] = (BYTE)(empty_at - (jz_empty_at + 1));
    code[jnz_at] = (BYTE)(loop_at - (jnz_at + 1));
    code[o++] = 0x48; code[o++] = 0xC7; code[o++] = 0x03;
    code[o++] = 0x00; code[o++] = 0x00; code[o++] = 0x00; code[o++] = 0x00;
    code[o++] = 0x66; code[o++] = 0xC7; code[o++] = 0x43; code[o++] = 0x08;
    code[o++] = 0x00; code[o++] = 0x00;
    code[o++] = 0x45; code[o++] = 0x31; code[o++] = 0xF6;
    code[o++] = 0x31; code[o++] = 0xF6;
    if (cave_remote) { emit_abs_jmp(code + o, resume_abort); o += 12; }
    else {
        code[o++] = 0xE9;
        INT32 rel = (INT32)(resume_abort - (cave + o + 4));
        memcpy(code + o, &rel, 4); o += 4;
    }

    if (!patch_bytes(cave, code, (size_t)o))
    {
        if (cave_remote) VirtualFree(cave, 0, MEM_RELEASE);
        return 0;
    }

    /* Overlay walk loop (9 bytes). Near: rel32 jmp. Remote: need 12-byte abs jmp —
     * walk site is only 9 bytes, so for remote use a 5-byte jmp into a small stub
     * placed in a zero cave, or steal more bytes. Prefer: if remote, require a tiny
     * near trampoline in .text zero cave (5 bytes for jmp to VirtualAlloc cave). */
    if (cave_remote)
    {
        BYTE *near_stub = find_zero_cave(text, text_len, 16);
        if (!near_stub)
        {
            ss_log("hd-softskip: BGNET r14 remote cave %p but no near trampoline\n", cave);
            VirtualFree(cave, 0, MEM_RELEASE);
            return 0;
        }
        BYTE stub[16];
        emit_abs_jmp(stub, cave);
        memset(stub + 12, 0xCC, 4);
        if (!patch_bytes(near_stub, stub, 16))
        {
            VirtualFree(cave, 0, MEM_RELEASE);
            return 0;
        }
        BYTE hook[9];
        hook[0] = 0xE9;
        INT32 hrel = (INT32)(near_stub - (walk + 5));
        memcpy(hook + 1, &hrel, 4);
        hook[5] = 0xCC; hook[6] = 0xCC; hook[7] = 0xCC; hook[8] = 0xCC;
        if (!patch_bytes(walk, hook, 9))
        {
            VirtualFree(cave, 0, MEM_RELEASE);
            return 0;
        }
        ss_log("hd-softskip: BGNET r14 walk %p -> near %p -> remote %p (abort %p ok %p)\n",
               walk, near_stub, cave, resume_abort, resume_ok);
        return 1;
    }

    BYTE hook[9];
    hook[0] = 0xE9;
    INT32 hrel = (INT32)(cave - (walk + 5));
    memcpy(hook + 1, &hrel, 4);
    hook[5] = 0xCC; hook[6] = 0xCC; hook[7] = 0xCC; hook[8] = 0xCC;
    if (!patch_bytes(walk, hook, 9))
        return 0;

    ss_log("hd-softskip: BGNET r14 walk %p -> cave %p (abort %p ok %p)\n",
           walk, cave, resume_abort, resume_ok);
    return 1;
}

static int patch_bgnet_walk_r8(BYTE *walk, BYTE *text, size_t text_len)
{
    /* Twin site using r8 (0x1405C9074). Same empty-batch abort offset as r14. */
    BYTE *resume_ok = walk + 9;
    BYTE *resume_abort = walk + 0x5A; /* test rsi,rsi → empty batch */

    if (walk[0] == 0xE9)
    {
        ss_log("hd-softskip: BGNET r8 walk already patched at %p\n", walk);
        return 0;
    }

    BYTE *cave = find_zero_cave(text, text_len, 64);
    int cave_remote = 0;
    if (!cave)
    {
        cave = (BYTE *)VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        cave_remote = 1;
    }
    if (!cave)
    {
        ss_log("hd-softskip: no cave for BGNET r8 walk\n");
        return 0;
    }

    BYTE code[96];
    int o = 0;
    code[o++] = 0x4D; code[o++] = 0x85; code[o++] = 0xC0;           /* test r8,r8 */
    code[o++] = 0x74; code[o++] = 0x00;
    int jz_imm_at = o - 1;
    code[o++] = 0x4D; code[o++] = 0x8B; code[o++] = 0x00;           /* mov r8,[r8] */
    code[o++] = 0x48; code[o++] = 0x83; code[o++] = 0xE9; code[o++] = 0x01;
    code[o++] = 0x75; code[o++] = 0x00;
    int jnz_imm_at = o - 1;
    if (cave_remote)
    {
        emit_abs_jmp(code + o, resume_ok); o += 12;
    }
    else
    {
        code[o++] = 0xE9;
        INT32 rel = (INT32)(resume_ok - (cave + o + 4));
        memcpy(code + o, &rel, 4); o += 4;
    }
    int abort_at = o;
    code[jz_imm_at] = (BYTE)(abort_at - (jz_imm_at + 1));
    code[jnz_imm_at] = (BYTE)(0 - (jnz_imm_at + 1));

    code[o++] = 0x48; code[o++] = 0xC7; code[o++] = 0x03;
    code[o++] = 0x00; code[o++] = 0x00; code[o++] = 0x00; code[o++] = 0x00;
    code[o++] = 0x66; code[o++] = 0xC7; code[o++] = 0x43; code[o++] = 0x08;
    code[o++] = 0x00; code[o++] = 0x00;
    code[o++] = 0x45; code[o++] = 0x31; code[o++] = 0xC0;           /* xor r8d,r8d */
    code[o++] = 0x31; code[o++] = 0xD2;                               /* xor edx,edx */
    if (cave_remote)
    {
        emit_abs_jmp(code + o, resume_abort); o += 12;
    }
    else
    {
        code[o++] = 0xE9;
        INT32 rel = (INT32)(resume_abort - (cave + o + 4));
        memcpy(code + o, &rel, 4); o += 4;
    }

    if (!patch_bytes(cave, code, (size_t)o))
    {
        if (cave_remote) VirtualFree(cave, 0, MEM_RELEASE);
        return 0;
    }

    if (cave_remote)
    {
        BYTE *near_stub = find_zero_cave(text, text_len, 16);
        if (!near_stub)
        {
            ss_log("hd-softskip: BGNET r8 remote cave %p but no near trampoline\n", cave);
            VirtualFree(cave, 0, MEM_RELEASE);
            return 0;
        }
        BYTE stub[16];
        emit_abs_jmp(stub, cave);
        memset(stub + 12, 0xCC, 4);
        if (!patch_bytes(near_stub, stub, 16))
        {
            VirtualFree(cave, 0, MEM_RELEASE);
            return 0;
        }
        BYTE hook[9];
        hook[0] = 0xE9;
        INT32 hrel = (INT32)(near_stub - (walk + 5));
        memcpy(hook + 1, &hrel, 4);
        hook[5] = 0xCC; hook[6] = 0xCC; hook[7] = 0xCC; hook[8] = 0xCC;
        if (!patch_bytes(walk, hook, 9))
        {
            VirtualFree(cave, 0, MEM_RELEASE);
            return 0;
        }
        ss_log("hd-softskip: BGNET r8 walk %p -> near %p -> remote %p\n", walk, near_stub, cave);
        return 1;
    }

    BYTE hook[9];
    hook[0] = 0xE9;
    INT32 hrel = (INT32)(cave - (walk + 5));
    memcpy(hook + 1, &hrel, 4);
    hook[5] = 0xCC; hook[6] = 0xCC; hook[7] = 0xCC; hook[8] = 0xCC;
    if (!patch_bytes(walk, hook, 9))
        return 0;

    ss_log("hd-softskip: BGNET r8 walk %p -> cave %p\n", walk, cave);
    return 1;
}

static BYTE *find_zero_cave(BYTE *hay, size_t hay_len, size_t need)
{
    size_t run = 0, run_at = 0;
    for (size_t i = 0; i < hay_len; i++)
    {
        if (hay[i] == 0x00 || hay[i] == 0xCC)
        {
            if (run == 0) run_at = i;
            if (++run >= need) return hay + run_at;
        }
        else
            run = 0;
    }
    return nullptr;
}

static int patch_bgnet_walk_r8_subr9(BYTE *walk, BYTE *text, size_t text_len)
{
    /* walk at mov r8,[r8] (0x1405C8C86). Loop is 8 bytes (sub rcx,r9 + jne).
     * resume_ok = walk+8 (mov rax,[r8]).
     * Empty: after count update is messy; use walk+8 with r8/rsi cleared via
     * partial-pop: peek next in r8, short/full resume_ok, empty -> zero + jmp past unlink.
     * After unlink block: movzx at walk+18. For empty jump to walk+0x2E approx —
     * safer: zero list, xor r8d, set ebp=0 so sub ax,bp is no-op-ish, resume at movzx.
     */
    BYTE *resume_ok = walk + 8;       /* mov rax, [r8] */
    BYTE *resume_after_count = walk + 0x1D; /* cmp ax,[rbx+0xa] at 0x1405C8CA3 — after count store */

    if (walk[0] == 0xE9)
    {
        ss_log("hd-softskip: BGNET r8-subr9 walk already patched at %p\n", walk);
        return 0;
    }

    BYTE *cave = find_zero_cave(text, text_len, 64);
    int cave_remote = 0;
    if (!cave)
    {
        cave = (BYTE *)VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        cave_remote = 1;
    }
    if (!cave)
    {
        ss_log("hd-softskip: no cave for BGNET r8-subr9 walk\n");
        return 0;
    }

    BYTE code[128];
    int o = 0;
    code[o++] = 0x4D; code[o++] = 0x85; code[o++] = 0xC0;           /* test r8,r8 */
    code[o++] = 0x74; code[o++] = 0x00;
    int jz_empty_at = o - 1;

    int loop_at = o;
    code[o++] = 0x49; code[o++] = 0x8B; code[o++] = 0x00;           /* mov rax,[r8] */
    code[o++] = 0x48; code[o++] = 0x85; code[o++] = 0xC0;           /* test rax,rax */
    code[o++] = 0x74; code[o++] = 0x00;
    int jz_short_at = o - 1;
    code[o++] = 0x49; code[o++] = 0x89; code[o++] = 0xC0;           /* mov r8,rax */
    code[o++] = 0x49; code[o++] = 0x2B; code[o++] = 0xC9;           /* sub rcx,r9 */
    code[o++] = 0x75; code[o++] = 0x00;
    int jnz_at = o - 1;
    if (cave_remote) { emit_abs_jmp(code + o, resume_ok); o += 12; }
    else {
        code[o++] = 0xE9;
        INT32 rel = (INT32)(resume_ok - (cave + o + 4));
        memcpy(code + o, &rel, 4); o += 4;
    }

    int short_at = o;
    code[jz_short_at] = (BYTE)(short_at - (jz_short_at + 1));
    /* M = rbp - rcx; this variant uses ebp as N and rcx as remaining*(r9=1 steps).
     * Initial rcx = N-1 = rbp-1. After k peeks with advance, rcx = N-1-k.
     * M = N - rcx = ebp - ecx. */
    code[o++] = 0x89; code[o++] = 0xE8;                               /* mov eax, ebp */
    code[o++] = 0x29; code[o++] = 0xC8;                               /* sub eax, ecx */
    code[o++] = 0x89; code[o++] = 0xC5;                               /* mov ebp, eax (M) */
    if (cave_remote) { emit_abs_jmp(code + o, resume_ok); o += 12; }
    else {
        code[o++] = 0xE9;
        INT32 rel = (INT32)(resume_ok - (cave + o + 4));
        memcpy(code + o, &rel, 4); o += 4;
    }

    int empty_at = o;
    code[jz_empty_at] = (BYTE)(empty_at - (jz_empty_at + 1));
    code[jnz_at] = (BYTE)(loop_at - (jnz_at + 1));
    code[o++] = 0x48; code[o++] = 0xC7; code[o++] = 0x03;
    code[o++] = 0x00; code[o++] = 0x00; code[o++] = 0x00; code[o++] = 0x00;
    code[o++] = 0x66; code[o++] = 0xC7; code[o++] = 0x43; code[o++] = 0x08;
    code[o++] = 0x00; code[o++] = 0x00;
    code[o++] = 0x45; code[o++] = 0x31; code[o++] = 0xC0;           /* xor r8d,r8d */
    code[o++] = 0x31; code[o++] = 0xED;                               /* xor ebp,ebp so sub ax,bp keeps count */
    /* skip unlink: jump to cmp ax,[rbx+0xa] after count already zeroed */
    if (cave_remote) { emit_abs_jmp(code + o, resume_after_count); o += 12; }
    else {
        code[o++] = 0xE9;
        INT32 rel = (INT32)(resume_after_count - (cave + o + 4));
        memcpy(code + o, &rel, 4); o += 4;
    }

    if (!patch_bytes(cave, code, (size_t)o))
    {
        if (cave_remote) VirtualFree(cave, 0, MEM_RELEASE);
        return 0;
    }

    /*
     * Steal 14 bytes (8-byte loop + mov rax,[r8] + mov [rbx],rax) so a 12-byte
     * abs jmp fits without a .text near trampoline (those get exhausted).
     * Cave already ends by jmp resume_ok/after_count; for the abs hook path,
     * resume_ok in the cave targets walk+8 which we overwrite — retarget cave
     * resumes to walk+14 (after stolen movs) by rebuilding is complex.
     * Simpler: abs-jmp to cave; cave on full/short paths emits the two stolen
     * instructions then jmp walk+14.
     */
    BYTE *after_stolen = walk + 14; /* after mov [rbx],rax */
    /* Patch cave's resume_ok targets: we stored resume_ok=walk+8 in jmps already.
     * Re-emit is too late. Instead change approach: make resume_ok = after_stolen
     * and duplicate the two instructions at the start of resume paths in cave.
     * Easiest fix that works now: prefer near rel32 when possible; for remote,
     * write abs jmp over 14 bytes and append stolen bytes before each resume_ok jmp.
     * Since cave is already built with old resume_ok, rebuild with after_stolen.
     */
    (void)after_stolen;
    if (cave_remote)
    {
        /* Rebuild is heavy; use push-ret style 14-byte abs via mov rax + jmp rax into
         * walk, and keep cave resume_ok at walk+8 — but walk+8 is inside the hook.
         * Change resume_ok to walk+14 and prepend stolen insns onto cave before use.
         * Easiest: VirtualFree and fail over to a second build — for now, prepend:
         */
        VirtualFree(cave, 0, MEM_RELEASE);
        /* Fall through to rebuild below by recursive-style inline rebuild */
    }

    /* Unified hook: always use abs jmp in a VirtualAlloc cave with stolen tail. */
    {
        BYTE *rcave = (BYTE *)VirtualAlloc(nullptr, 192, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!rcave)
        {
            ss_log("hd-softskip: BGNET r8-subr9 VirtualAlloc failed\n");
            return 0;
        }
        BYTE *rok = walk + 14;
        BYTE *rafter = walk + 0x1D;
        BYTE code2[160];
        int p = 0;
        /* stolen: mov rax,[r8]; mov [rbx],rax — executed on full/short resume paths */
        auto emit_stolen = [&]() {
            code2[p++] = 0x49; code2[p++] = 0x8B; code2[p++] = 0x00;
            code2[p++] = 0x48; code2[p++] = 0x89; code2[p++] = 0x03;
        };
        auto ej = [&](BYTE *t) { emit_abs_jmp(code2 + p, t); p += 12; };

        code2[p++] = 0x4D; code2[p++] = 0x85; code2[p++] = 0xC0;
        code2[p++] = 0x74; code2[p++] = 0x00;
        int jz_e = p - 1;
        int loop = p;
        code2[p++] = 0x49; code2[p++] = 0x8B; code2[p++] = 0x00;
        code2[p++] = 0x48; code2[p++] = 0x85; code2[p++] = 0xC0;
        code2[p++] = 0x74; code2[p++] = 0x00;
        int jz_s = p - 1;
        code2[p++] = 0x49; code2[p++] = 0x89; code2[p++] = 0xC0;
        code2[p++] = 0x49; code2[p++] = 0x2B; code2[p++] = 0xC9;           /* sub rcx,r9 */
        code2[p++] = 0x75; code2[p++] = 0x00;
        int jnz = p - 1;
        emit_stolen(); ej(rok);

        int short_at = p;
        code2[jz_s] = (BYTE)(short_at - (jz_s + 1));
        code2[p++] = 0x89; code2[p++] = 0xE8;
        code2[p++] = 0x29; code2[p++] = 0xC8;
        code2[p++] = 0x89; code2[p++] = 0xC5;
        emit_stolen(); ej(rok);

        int empty_at = p;
        code2[jz_e] = (BYTE)(empty_at - (jz_e + 1));
        code2[jnz] = (BYTE)(loop - (jnz + 1));
        code2[p++] = 0x48; code2[p++] = 0xC7; code2[p++] = 0x03;
        code2[p++] = 0x00; code2[p++] = 0x00; code2[p++] = 0x00; code2[p++] = 0x00;
        code2[p++] = 0x66; code2[p++] = 0xC7; code2[p++] = 0x43; code2[p++] = 0x08;
        code2[p++] = 0x00; code2[p++] = 0x00;
        code2[p++] = 0x45; code2[p++] = 0x31; code2[p++] = 0xC0;
        code2[p++] = 0x31; code2[p++] = 0xED;
        ej(rafter);

        if (!patch_bytes(rcave, code2, (size_t)p))
        {
            VirtualFree(rcave, 0, MEM_RELEASE);
            return 0;
        }
        BYTE hook[14];
        emit_abs_jmp(hook, rcave);
        hook[12] = 0xCC; hook[13] = 0xCC;
        if (!patch_bytes(walk, hook, 14))
        {
            VirtualFree(rcave, 0, MEM_RELEASE);
            return 0;
        }
        ss_log("hd-softskip: BGNET r8-subr9 walk %p -> abs cave %p (steal 14)\n", walk, rcave);
        return 1;
    }
}



static void dump_va(BYTE *module, size_t module_span, ULONGLONG image_base, ULONGLONG va, size_t n, HANDLE out)
{
    char hdr[128];
    int hlen = snprintf(hdr, sizeof(hdr), "\nVA %llx: ", (unsigned long long)va);
    DWORD got = 0;
    WriteFile(out, hdr, (DWORD)hlen, &got, nullptr);
    if (va < image_base)
    {
        WriteFile(out, "(below base)\n", 13, &got, nullptr);
        return;
    }
    size_t off = (size_t)(va - image_base);
    if (off + n > module_span)
    {
        WriteFile(out, "(out of span)\n", 14, &got, nullptr);
        return;
    }
    BYTE *p = module + off;
    for (size_t i = 0; i < n; i++)
    {
        char b[4];
        snprintf(b, sizeof(b), "%02x ", p[i]);
        WriteFile(out, b, 3, &got, nullptr);
    }
}

static void write_diagnostics(BYTE *module, size_t module_span, BYTE *text, size_t text_len, ULONGLONG image_base)
{
    HANDLE out = CreateFileW(L"C:\\users\\bobby\\AppData\\Local\\Temp\\hd-softskip-diag.txt",
                             GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return;

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "module=%p text=%p len=%zu span=%zu base=0x%llx ent=%.3f\n",
                     module, text, text_len, module_span, (unsigned long long)image_base,
                     text_entropy(text, text_len));
    DWORD got = 0;
    WriteFile(out, buf, (DWORD)n, &got, nullptr);

    /* Crash-stack anchors from CampaignS1 ModuleSizeOnDisk fatal */
    static const ULONGLONG vas[] = {
        0x1406012E5ULL, 0x14051960EULL, 0x140E287A0ULL, 0x1404C9C9AULL,
        0x14043EA1CULL, 0x14043F204ULL, 0x1411CA810ULL, 0x141AAFA33ULL,
        0x140AE5FB3ULL, 0x140AE5E89ULL, 0x1410DFD62ULL,
    };
    for (size_t i = 0; i < sizeof(vas) / sizeof(vas[0]); i++)
        dump_va(module, module_span, image_base, vas[i], 64, out);

    /* Count interesting opcodes */
    int cmp58 = 0, add50 = 0, ja_near = 0, mov2 = 0;
    for (size_t i = 0; i + 6 < text_len; i++)
    {
        if ((text[i] == 0x48 || text[i] == 0x49) && text[i + 1] == 0x3B &&
            (text[i + 2] >> 6) == 1 && text[i + 3] == 0x58)
            cmp58++;
        if (text[i] == 0x48 && text[i + 1] == 0x83 &&
            ((text[i + 2] >> 3) & 7) == 0 && (text[i + 2] >> 6) == 1 &&
            text[i + 3] == 0x08 && text[i + 4] == 0x50)
            add50++;
        if (text[i] == 0x0F && text[i + 1] == 0x87)
            ja_near++;
        if (text[i] >= 0xB8 && text[i] <= 0xBF && text[i + 1] == 2 &&
            text[i + 2] == 0 && text[i + 3] == 0 && text[i + 4] == 0)
            mov2++;
    }
    n = snprintf(buf, sizeof(buf),
                 "\ncounts: cmp_[reg+58]=%d add_[reg+8],0x50=%d ja_near=%d mov_imm2=%d and_+10_fb=%d\n",
                 cmp58, add50, ja_near, mov2, count_and_plus10_fb(text, text_len));
    WriteFile(out, buf, (DWORD)n, &got, nullptr);

    /* Dump first few cmp [reg+0x58] sites with context */
    int shown = 0;
    WriteFile(out, "cmp+58 sites:\n", 14, &got, nullptr);
    for (size_t i = 0; i + 16 < text_len && shown < 12; i++)
    {
        if ((text[i] == 0x48 || text[i] == 0x49) && text[i + 1] == 0x3B &&
            (text[i + 2] >> 6) == 1 && text[i + 3] == 0x58)
        {
            n = snprintf(buf, sizeof(buf), "  %p: ", text + i);
            WriteFile(out, buf, (DWORD)n, &got, nullptr);
            for (int k = 0; k < 24; k++)
            {
                snprintf(buf, sizeof(buf), "%02x ", text[i + k]);
                WriteFile(out, buf, 3, &got, nullptr);
            }
            WriteFile(out, "\n", 1, &got, nullptr);
            shown++;
        }
    }

    shown = 0;
    WriteFile(out, "add+8,50 sites:\n", 16, &got, nullptr);
    for (size_t i = 0; i + 24 < text_len && shown < 12; i++)
    {
        if (text[i] == 0x48 && text[i + 1] == 0x83 &&
            ((text[i + 2] >> 3) & 7) == 0 && (text[i + 2] >> 6) == 1 &&
            text[i + 3] == 0x08 && text[i + 4] == 0x50)
        {
            n = snprintf(buf, sizeof(buf), "  %p: ", text + i);
            WriteFile(out, buf, (DWORD)n, &got, nullptr);
            for (int k = 0; k < 32; k++)
            {
                snprintf(buf, sizeof(buf), "%02x ", text[i + k]);
                WriteFile(out, buf, 3, &got, nullptr);
            }
            WriteFile(out, "\n", 1, &got, nullptr);
            shown++;
        }
    }

    CloseHandle(out);
    ss_log("hd-softskip: wrote hd-softskip-diag.txt\n");
}

/*
 * [DEBUG-bgnet] freelist instrumentation (log only — does not soft-skip the null walk).
 *
 * Halo ClientSetup crashes in freelist pop-N (0x1405C8D88) when [list+8] count claims
 * >= N (512 for sizeclass 7) but the intrusive chain is shorter (~73). Hook pop-N entry
 * and the push `inc word [list+8]` path to log count vs walked chain length.
 */
/* Reject NULL, low junk, and poisoned sentinels like (void*)-1 before deref. */
static int bgnet_ptr_readable(void *p)
{
    UINT64 u = (UINT64)(uintptr_t)p;
    if (u < 0x10000) return 0;
    /* Canonical user-space on Wine/Linux x86-64; excludes 0xFFF… and kernel. */
    if (u >= 0x0000700000000000ULL) return 0;
    return 1;
}

static size_t bgnet_walk_chain(void *head, size_t limit)
{
    size_t n = 0;
    void *p = head;
    while (bgnet_ptr_readable(p) && n < limit)
    {
        void *next = *(void **)p;
        n++;
        if (next == p) return n + 100000; /* cycle marker */
        if (next && !bgnet_ptr_readable(next))
            return n + 200000; /* poisoned next; stop before deref */
        p = next;
    }
    return n;
}

static void __attribute__((ms_abi)) bgnet_log_pop(void * /*ctx*/, void *list,
                                                   unsigned long long sizeclass, unsigned int n)
{
    if (!list) return;
    UINT16 count = *(UINT16 *)((BYTE *)list + 8);
    void *head = *(void **)list;
    size_t walked = bgnet_walk_chain(head, 8192);
    int desync = (walked < (size_t)count) || (walked < (size_t)n) || walked >= 100000;
    static volatile LONG seq;
    LONG id = InterlockedIncrement(&seq);
    if (sizeclass == 7 || desync || n >= 128 || (id & 63) == 1)
    {
        ss_log("[DEBUG-bgnet] pop#%ld list=%p class=%llu N=%u count=%u chain=%zu head=%p%s\n",
               (long)id, list, sizeclass, n, (unsigned)count, walked, head,
               desync ? " DESYNC" : "");
    }
}

static void __attribute__((ms_abi)) bgnet_log_push(void *list, unsigned long long sizeclass)
{
    if (!list) return;
    if (sizeclass != 7 && (sizeclass & 0x7f) != 7) return;
    UINT16 count = *(UINT16 *)((BYTE *)list + 8);
    UINT16 watermark = *(UINT16 *)((BYTE *)list + 0xc);
    void *head = *(void **)list;
    size_t walked = bgnet_walk_chain(head, 8192);
    int desync = (walked < (size_t)count) || walked >= 100000;
    static volatile LONG seq;
    LONG id = InterlockedIncrement(&seq);
    if (desync || count >= 64 || (id & 31) == 1)
    {
        ss_log("[DEBUG-bgnet] push#%ld list=%p class=%llu count=%u chain=%zu wm=%u head=%p%s\n",
               (long)id, list, sizeclass & 0x7f, (unsigned)count, walked, (unsigned)watermark, head,
               desync ? " DESYNC" : "");
    }
}

static int hook_bgnet_pop_n(BYTE *entry)
{
    /* Already hooked? */
    if (entry[0] == 0x48 && entry[1] == 0xB8) return 0;

    /* Steal 15-byte prologue:
     *   48 8B C4             mov rax, rsp
     *   48 89 58 10          mov [rax+10h], rbx
     *   48 89 68 18          mov [rax+18h], rbp
     *   48 89 70 20          mov [rax+20h], rsi
     */
    static const BYTE expect[] = {
        0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20
    };
    if (memcmp(entry, expect, sizeof(expect)) != 0)
    {
        ss_log("[DEBUG-bgnet] pop-N prologue mismatch at %p\n", entry);
        return 0;
    }

    BYTE *cave = (BYTE *)VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave)
    {
        ss_log("[DEBUG-bgnet] VirtualAlloc failed %lu\n", GetLastError());
        return 0;
    }

    /*
     * cave:
     *   pushfq / push rax-r11 (volatiles + rax)
     *   sub rsp, 0x20
     *   ; rcx/rdx/r8/r9 already hold ctx/list/class/N — call logger
     *   mov rax, imm64(logger) ; call rax
     *   add rsp, 0x20
     *   pop r11..rax / popfq
     *   <stolen 15 bytes>
     *   abs jmp entry+15
     *
     * At entry rsp%16==8. pushfq + 8 pushes = 9*8 = 72 → rsp%16==8.
     * sub 0x20 → rsp%16==8, correct for `call`.
     */
    int o = 0;
    cave[o++] = 0x9C; /* pushfq */
    static const BYTE pushes[] = {
        0x50,                         /* push rax */
        0x51,                         /* push rcx */
        0x52,                         /* push rdx */
        0x41, 0x50,                   /* push r8 */
        0x41, 0x51,                   /* push r9 */
        0x41, 0x52,                   /* push r10 */
        0x41, 0x53,                   /* push r11 */
        0x56,                         /* push rsi — keep 8 pushes of GP for easier pop order? */
    };
    /* Use exactly: rax rcx rdx r8 r9 r10 r11 = 7 GP + pushfq = 8 slots → after pushes rsp%16 == 8+0 = 8. */
    (void)pushes;
    cave[o++] = 0x50;
    cave[o++] = 0x51;
    cave[o++] = 0x52;
    cave[o++] = 0x41; cave[o++] = 0x50;
    cave[o++] = 0x41; cave[o++] = 0x51;
    cave[o++] = 0x41; cave[o++] = 0x52;
    cave[o++] = 0x41; cave[o++] = 0x53;
    /* 1+7=8 pushes; rsp%16==8. */
    cave[o++] = 0x48; cave[o++] = 0x83; cave[o++] = 0xEC; cave[o++] = 0x20; /* sub rsp, 0x20 */

    /* call bgnet_log_pop — args still in rcx, rdx, r8, r9 */
    cave[o++] = 0x48; cave[o++] = 0xB8;
    {
        UINT64 fn = (UINT64)(void *)&bgnet_log_pop;
        memcpy(cave + o, &fn, 8); o += 8;
    }
    cave[o++] = 0xFF; cave[o++] = 0xD0; /* call rax */

    cave[o++] = 0x48; cave[o++] = 0x83; cave[o++] = 0xC4; cave[o++] = 0x20; /* add rsp, 0x20 */
    cave[o++] = 0x41; cave[o++] = 0x5B; /* pop r11 */
    cave[o++] = 0x41; cave[o++] = 0x5A; /* pop r10 */
    cave[o++] = 0x41; cave[o++] = 0x59; /* pop r9 */
    cave[o++] = 0x41; cave[o++] = 0x58; /* pop r8 */
    cave[o++] = 0x5A; /* pop rdx */
    cave[o++] = 0x59; /* pop rcx */
    cave[o++] = 0x58; /* pop rax */
    cave[o++] = 0x9D; /* popfq */

    memcpy(cave + o, expect, sizeof(expect));
    o += (int)sizeof(expect);
    emit_abs_jmp(cave + o, entry + sizeof(expect));
    o += 12;

    BYTE hook[15];
    emit_abs_jmp(hook, cave);
    memset(hook + 12, 0xCC, 3);
    if (!patch_bytes(entry, hook, sizeof(hook)))
    {
        VirtualFree(cave, 0, MEM_RELEASE);
        ss_log("[DEBUG-bgnet] pop-N hook patch failed at %p\n", entry);
        return 0;
    }
    ss_log("[DEBUG-bgnet] pop-N instrumented at %p -> cave %p (%d bytes)\n", entry, cave, o);
    return 1;
}

static int hook_bgnet_push_inc(BYTE *inc_site)
{
    /* inc_site points at 66 FF 47 08. Steal 5 bytes: 66 FF 47 08 0F — no, need full insn.
     * After: 0F B7 47 08 (movzx eax,[rdi+8]). Steal inc (4) + movzx (4) = 8, then abs jmp needs 12 —
     * not enough. Use a nearby 12+ byte window:
     *   66 FF 47 08       inc word [rdi+8]     ; 4
     *   0F B7 47 08       movzx eax,[rdi+8]    ; 4
     *   48 89 37          mov [rdi], rsi       ; 3  → 11 total, still short
     *   0F B7 57 0C       movzx edx,[rdi+0xc]  ; 4  → 15
     */
    static const BYTE expect[] = {
        0x66, 0xFF, 0x47, 0x08, 0x0F, 0xB7, 0x47, 0x08, 0x48, 0x89, 0x37, 0x0F, 0xB7, 0x57, 0x0C
    };
    if (memcmp(inc_site, expect, sizeof(expect)) != 0) return 0;
    if (inc_site[0] == 0x48 && inc_site[1] == 0xB8) return 0;

    BYTE *cave = (BYTE *)VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) return 0;

    /* Run the original push (inc count + link) first, then log post-inc count vs chain. */
    int o = 0;
    memcpy(cave + o, expect, sizeof(expect));
    o += (int)sizeof(expect);

    cave[o++] = 0x9C;
    cave[o++] = 0x50;
    cave[o++] = 0x51;
    cave[o++] = 0x52;
    cave[o++] = 0x41; cave[o++] = 0x50;
    cave[o++] = 0x41; cave[o++] = 0x51;
    cave[o++] = 0x41; cave[o++] = 0x52;
    cave[o++] = 0x41; cave[o++] = 0x53;
    cave[o++] = 0x48; cave[o++] = 0x83; cave[o++] = 0xEC; cave[o++] = 0x20;

    /* ms_abi: rcx=list, rdx=sizeclass. */
    cave[o++] = 0x48; cave[o++] = 0x89; cave[o++] = 0xF9; /* mov rcx, rdi */
    cave[o++] = 0x4C; cave[o++] = 0x89; cave[o++] = 0xC2; /* mov rdx, r8 */
    cave[o++] = 0x48; cave[o++] = 0xB8;
    {
        UINT64 fn = (UINT64)(void *)&bgnet_log_push;
        memcpy(cave + o, &fn, 8); o += 8;
    }
    cave[o++] = 0xFF; cave[o++] = 0xD0;

    cave[o++] = 0x48; cave[o++] = 0x83; cave[o++] = 0xC4; cave[o++] = 0x20;
    cave[o++] = 0x41; cave[o++] = 0x5B;
    cave[o++] = 0x41; cave[o++] = 0x5A;
    cave[o++] = 0x41; cave[o++] = 0x59;
    cave[o++] = 0x41; cave[o++] = 0x58;
    cave[o++] = 0x5A;
    cave[o++] = 0x59;
    cave[o++] = 0x58;
    cave[o++] = 0x9D;

    emit_abs_jmp(cave + o, inc_site + sizeof(expect));
    o += 12;

    BYTE hook[15];
    emit_abs_jmp(hook, cave);
    memset(hook + 12, 0xCC, 3);
    if (!patch_bytes(inc_site, hook, sizeof(hook)))
    {
        VirtualFree(cave, 0, MEM_RELEASE);
        return 0;
    }
    ss_log("[DEBUG-bgnet] push-inc instrumented at %p -> cave %p\n", inc_site, cave);
    return 1;
}

static BYTE *find_bgnet_pop_n(BYTE *hay, size_t hay_len)
{
    static const BYTE pat[] = {
        0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20,
        0x48, 0x89, 0x48, 0x08, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20,
        0x48, 0x8B, 0xDA, 0x49, 0x63, 0xF9, 0x0F, 0xB7, 0x52, 0x08
    };
    return find_pat(hay, hay_len, pat, sizeof(pat));
}

static int is_halo_crash_helper_process(void)
{
    const wchar_t *cl = GetCommandLineW();
    if (!cl) return 0;
    /* Halo's DumpTool / -upload -cleanup helpers also load xgameruntime; hooking them
     * re-entered the freelist trampoline, faulted on poisoned next pointers, and spawned
     * a storm of upload processes. Skip freelist instr there. */
    if (wcsstr(cl, L"-upload") || wcsstr(cl, L"DumpTool") || wcsstr(cl, L"-cleanup"))
        return 1;
    return 0;
}

static int instrument_bgnet_freelist(BYTE *text, size_t text_len)
{
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) != 0)
        return 0;
    if (is_halo_crash_helper_process())
    {
        ss_log("[DEBUG-bgnet] skip freelist instr in crash-helper process\n");
        return 0;
    }

    int n = 0;
    BYTE *pop = find_bgnet_pop_n(text, text_len);
    if (pop)
        n += hook_bgnet_pop_n(pop);
    else
        ss_log("[DEBUG-bgnet] pop-N pattern not found in .text\n");

    /* Push path: may appear once per allocator variant; instrument every match (usually 1). */
    static const BYTE push_pat[] = {
        0x66, 0xFF, 0x47, 0x08, 0x0F, 0xB7, 0x47, 0x08, 0x48, 0x89, 0x37, 0x0F, 0xB7, 0x57, 0x0C
    };
    int push_hits = 0;
    for (size_t i = 0; i + sizeof(push_pat) <= text_len; i++)
    {
        if (memcmp(text + i, push_pat, sizeof(push_pat)) != 0) continue;
        /* Prefer sites that look like freelist push: preceding `mov [rsi], rax` (48 89 06) nearby. */
        bool near_link = false;
        for (size_t b = (i > 24 ? i - 24 : 0); b < i; b++)
        {
            if (text[b] == 0x48 && text[b + 1] == 0x89 && text[b + 2] == 0x06)
            {
                near_link = true;
                break;
            }
        }
        if (!near_link) continue;
        if (hook_bgnet_push_inc(text + i))
        {
            n++;
            push_hits++;
            if (push_hits >= 2) break;
        }
    }
    if (!push_hits)
        ss_log("[DEBUG-bgnet] push-inc pattern not found (or already hooked)\n");
    return n;
}

static int apply_softskip_once(int pass)
{
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) return -1;

    auto *dos = (IMAGE_DOS_HEADER *)exe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -2;
    auto *nt = (IMAGE_NT_HEADERS *)((BYTE *)exe + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return -3;
    ULONGLONG image_base = nt->OptionalHeader.ImageBase;

    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    BYTE *text = nullptr;
    size_t text_len = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        if (memcmp(sec[i].Name, ".text", 5) == 0)
        {
            text = (BYTE *)exe + sec[i].VirtualAddress;
            text_len = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
            break;
        }
    }
    if (!text || text_len < 64) return -4;

    double ent = text_entropy(text, text_len);
    if (ent > 7.5)
    {
        if (pass % 5 == 0)
            ss_log("hd-softskip: .text still encrypted (ent=%.2f), defer\n", ent);
        return 0;
    }

    size_t module_span = nt->OptionalHeader.SizeOfImage;
    if (pass == 0 || pass == 1)
        write_diagnostics((BYTE *)exe, module_span, text, text_len, image_base);

    int applied = 0;
    int jae_hits = 0;
    BYTE *jae = find_hd_jae(text, text_len, &jae_hits);
    /* Only use mov-imm2 fallback when no cmp/+0x58 site was found at all. */
    if (!jae)
        jae = find_hd_jae_via_mov2(text, text_len);

    if (jae && jae[0] == 0x73)
    {
        BYTE nops[2] = { 0x90, 0x90 };
        if (patch_bytes(jae, nops, 2))
        {
            ss_log("hd-softskip: jae-NOP at %p (hits=%d)\n", jae, jae_hits);
            applied++;
        }
    }
    else if (jae && jae[0] == 0x90)
        ss_log("hd-softskip: jae already NOP at %p\n", jae);
    else
        ss_log("hd-softskip: HD jae site not found (raw_hits=%d)\n", jae_hits);

    BYTE *enqueue = nullptr;
    BYTE *mark_done = nullptr;
    BYTE *eof_ja = find_eof_ja(text, text_len, &enqueue, &mark_done);
    if (eof_ja && mark_done)
    {
        INT32 cur_rel = 0;
        memcpy(&cur_rel, eof_ja + 2, 4);
        BYTE *cur_tgt = eof_ja + 6 + cur_rel;
        if (cur_tgt[0] == 0x48 && cur_tgt[1] == 0x83 && cur_tgt[2] == 0x69 &&
            cur_tgt[3] == 0x08 && cur_tgt[4] == 0x50)
        {
            ss_log("hd-softskip: EOF ja already soft-skipped at %p -> %p\n", eof_ja, cur_tgt);
        }
        else
        {
            BYTE *cave = find_zero_cave(text, text_len, 16);
            if (cave)
            {
                BYTE cave_code[11];
                cave_code[0] = 0x48; cave_code[1] = 0x83; cave_code[2] = 0x69;
                cave_code[3] = 0x08; cave_code[4] = 0x50;
                cave_code[5] = 0xE9;
                INT32 rel = (INT32)(mark_done - (cave + 10));
                memcpy(cave_code + 6, &rel, 4);
                if (patch_bytes(cave, cave_code, 11))
                {
                    BYTE ja_patch[6];
                    ja_patch[0] = 0x0F; ja_patch[1] = 0x87;
                    INT32 ja_rel = (INT32)(cave - (eof_ja + 6));
                    memcpy(ja_patch + 2, &ja_rel, 4);
                    if (patch_bytes(eof_ja, ja_patch, 6))
                    {
                        ss_log("hd-softskip: EOF ja %p -> cave %p -> mark_done %p (enq %p)\n",
                               eof_ja, cave, mark_done, enqueue);
                        applied++;
                    }
                }
            }
            else
                ss_log("hd-softskip: no zero cave for EOF undo\n");
        }
    }
    else
        ss_log("hd-softskip: EOF ja/mark_done not found (ja=%p mark=%p enq=%p)\n",
               eof_ja, mark_done, enqueue);

    BYTE *halt = find_halt_call(text, text_len);
    if (halt && halt[0] == 0xE8)
    {
        BYTE nops[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
        if (patch_bytes(halt, nops, 5))
        {
            ss_log("hd-softskip: halt-call NOP at %p\n", halt);
            applied++;
        }
    }
    else if (halt && halt[0] == 0x90)
        ss_log("hd-softskip: halt already NOP at %p\n", halt);
    else
        ss_log("hd-softskip: halt call not found\n");

    /*
     * BG NET WORKER ClientSetup freelist desync (RIP 0x1405C8DFD):
     * push watermark drain asks pop-N (N=512 sizeclass 7) but the intrusive chain is
     * shorter than [list+8]. Title-yes-before-Party did not stop it; UDP completions
     * looked clean; VirtualAlloc log trampolines that walked the chain crashed us.
     *
     * Repair (not a feature skip): null-safe walk; on early NULL, zero head+count so the
     * bucket is empty and later allocs refill from the OS path instead of AV. Root Wine
     * cause (who inflates count) is still open — this only makes the allocator survive.
     */
    BYTE *bg_r14 = find_bgnet_walk_r14(text, text_len);
    BYTE *bg_r8 = find_bgnet_walk_r8(text, text_len);
    BYTE *bg_r8b = find_bgnet_walk_r8_subr9(text, text_len);
    if (bg_r14)
        applied += patch_bgnet_walk_r14(bg_r14, text, text_len);
    else
        ss_log("[DEBUG-bgnet] r14 walk pattern not found\n");
    if (bg_r8)
        applied += patch_bgnet_walk_r8(bg_r8, text, text_len);
    else
        ss_log("[DEBUG-bgnet] r8 walk pattern not found\n");
    /* Runtime .text may not match on-disk pattern for 0x1405C8C86; locate via
     * fixed delta from the r14 walk at 0x1405C8DFD (delta 0x177). */
    if (!bg_r8b && bg_r14 && bg_r14 >= text + 0x177)
    {
        BYTE *cand = bg_r14 - 0x177;
        ss_log("[DEBUG-bgnet] r8-subr9 candidate %p bytes %02x %02x %02x %02x %02x %02x %02x %02x\n",
               cand, cand[0], cand[1], cand[2], cand[3], cand[4], cand[5], cand[6], cand[7]);
        if ((cand[0] == 0x4D || cand[0] == 0x49) && cand[1] == 0x8B)
        {
            bg_r8b = cand;
            ss_log("[DEBUG-bgnet] r8-subr9 forced via r14-0x177 at %p\n", bg_r8b);
        }
        else
            ss_log("[DEBUG-bgnet] r8-subr9 candidate looks wrong — not patching\n");
    }
    if (bg_r8b)
        applied += patch_bgnet_walk_r8_subr9(bg_r8b, text, text_len);
    else
        ss_log("[DEBUG-bgnet] r8-subr9 walk pattern not found\n");
    (void)instrument_bgnet_freelist; /* walk-based log trampoline deferred — crashed BG 00 */

    return applied > 0 ? 1 : -10; /* -10 = decrypted but nothing to patch / already done */
}

static DWORD WINAPI softskip_thread(void *)
{
    int saw_plain = 0;
    for (int i = 0; i < 90; i++)
    {
        int rc = apply_softskip_once(i);
        if (rc > 0)
        {
            ss_log("hd-softskip: applied (pass %d)\n", i);
            return 0;
        }
        if (rc == -10)
        {
            /* Decrypted; either already patched or patterns absent. Keep diag, stop. */
            ss_log("hd-softskip: plain .text but no new patches (pass %d)\n", i);
            return 3;
        }
        if (rc < 0 && rc != -10)
        {
            ss_log("hd-softskip: fatal %d\n", rc);
            return 1;
        }
        if (rc == 0) saw_plain = 0;
        else saw_plain++;
        Sleep(1000);
    }
    ss_log("hd-softskip: gave up after retries\n");
    return 2;
}

/* WINE_UWP_DUMP_IMAGE=<windows path>: once the exe's .text is decrypted, write its whole mapped
 * image (SizeOfImage bytes, unreadable pages as zeros) so the code can be read statically. A
 * measurement only; it changes nothing in the process. */
static DWORD WINAPI image_dump_thread(void *param)
{
    WCHAR *path = (WCHAR *)param;
    BYTE *exe = (BYTE *)GetModuleHandleW(nullptr);
    auto *nt = (IMAGE_NT_HEADERS *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    BYTE *text = nullptr; size_t text_len = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(sec[i].Name, ".text", 5)) { text = exe + sec[i].VirtualAddress; text_len = sec[i].Misc.VirtualSize; }
    for (int i = 0; i < 120 && text && text_entropy(text, text_len) > 7.5; i++) Sleep(500);
    size_t size = nt->OptionalHeader.SizeOfImage;
    size_t text_end = text ? (size_t)(text - exe) + text_len : 0;
    HANDLE out = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) { ss_log("image-dump: cannot create file %lu\n", GetLastError()); return 1; }
    static BYTE zero[0x1000];
    for (size_t off = 0; off < size; off += 0x1000)
    {
        MEMORY_BASIC_INFORMATION mbi;
        BOOL readable = VirtualQuery(exe + off, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
                        !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        /* The packer keeps most code pages no-access and decrypts each on its first touch, from
         * its own fault handler; a plain read makes it do that, then the page is copied. */
        if (!readable && mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_NOACCESS) && off < text_end)
        {
            (void)*(volatile BYTE *)(exe + off);
            readable = VirtualQuery(exe + off, &mbi, sizeof(mbi)) && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        }
        DWORD got;
        WriteFile(out, readable ? exe + off : zero, 0x1000, &got, nullptr);
    }
    CloseHandle(out);
    ss_log("image-dump: wrote %zu bytes, text entropy %.2f\n", size, text ? text_entropy(text, text_len) : 0.0);
    return 0;
}

void wineuwp_start_hd_softskip(void)
{
    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) != 0)
        return;
    static WCHAR dump_path[MAX_PATH];
    if (GetEnvironmentVariableW(L"WINE_UWP_DUMP_IMAGE", dump_path, MAX_PATH) && !is_halo_crash_helper_process())
    {
        HANDLE thr = CreateThread(nullptr, 0, image_dump_thread, dump_path, 0, nullptr);
        if (thr) CloseHandle(thr);
    }
    /* Opt-in only. These patches NOP the game's own halt and EOF/HD checks, which turns a
     * clean stop into silent heap corruption that crashes somewhere unrelated later. */
    char on[8];
    if (!GetEnvironmentVariableA("WINE_UWP_HD_SOFTSKIP", on, sizeof(on)) || on[0] != '1')
    {
        ss_log("hd-softskip: disabled (set WINE_UWP_HD_SOFTSKIP=1 to enable)\n");
        return;
    }
    if (is_halo_crash_helper_process())
    {
        ss_log("hd-softskip: skip in crash-helper process\n");
        return;
    }
    HANDLE thr = CreateThread(nullptr, 0, softskip_thread, nullptr, 0, nullptr);
    if (thr) CloseHandle(thr);
    else ss_log("hd-softskip: CreateThread failed %lu\n", GetLastError());
}
