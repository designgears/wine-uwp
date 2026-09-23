/* CoreMessaging.dll 26100.8972 due-now message-timer ring; see apply-timer-ring.sh and
 * docs/Findings-M4.md "The blank-window flake". Data lives in HotPatchSpareGlobals. */
        .intel_syntax noprefix
        .text
        .globl enq, deq
/* rcx = Timeout* (due-now). Spin-locked ring; drop (and count) when full. */
enq:
        lea     r9, [rip + LOCK]
1:      mov     eax, 1
        xchg    eax, [r9]
        test    eax, eax
        jz      2f
        pause
        jmp     1b
2:      mov     eax, [rip + TAIL]
        mov     edx, eax
        sub     edx, [rip + HEAD]
        cmp     edx, 16
        jae     3f
        and     eax, 15
        lea     r10, [rip + RING]
        mov     [r10 + rax*8], rcx
        inc     dword ptr [rip + TAIL]
        lock inc dword ptr [rip + ENQ]
        jmp     4f
3:      lock inc dword ptr [rip + DROP]
4:      mov     dword ptr [r9], 0
        ret
/* Drain every queued timer on the bound thread (0 = any thread). */
deq:
        push    rbx
        push    rsi
        sub     rsp, 0x28
        mov     esi, 16                 /* at most one ring's worth per drain */
        mov     eax, gs:[0x48]
        mov     ebx, [rip + TID]
        test    ebx, ebx
        jz      5f
        cmp     ebx, eax
        jne     9f
5:      dec     esi
        js      9f
        lea     r9, [rip + LOCK]
6:      mov     eax, 1
        xchg    eax, [r9]
        test    eax, eax
        jz      7f
        pause
        jmp     6b
7:      mov     eax, [rip + HEAD]
        cmp     eax, [rip + TAIL]
        jne     8f
        mov     dword ptr [r9], 0
        jmp     9f
8:      and     eax, 15
        lea     r10, [rip + RING]
        mov     rbx, [r10 + rax*8]
        inc     dword ptr [rip + HEAD]
        mov     dword ptr [r9], 0
        lock inc dword ptr [rip + DEQ]
        mov     rcx, rbx
        xor     edx, edx
        mov     r8, [rbx + 0x38]
        call    ONTIMER
        jmp     5b
9:      add     rsp, 0x28
        pop     rsi
        pop     rbx
        ret
