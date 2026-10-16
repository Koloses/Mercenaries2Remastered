/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* SecuROM detours on hot paths. The protection moved single instructions out of game functions: at the original
 * place there is now `jmp [cell]`, and the cell leads to a trampoline that runs the moved instruction and then
 * returns into the function through an obfuscated computed return:
 *     <instruction> / push r / pushfd / mov r,[a] / not r / xor r,[b] / xchg [esp+4],r / popfd / ret
 * (target = ~[a] ^ [b]). pushfd/popfd and the unpredictable return cost tens of cycles each time, which matters
 * where it sits inside a per-item loop. The worst one is the forest instance decoder 0x49F700, called close to a
 * million times a second in towns: its moved instruction (mov edx,[eax+ecx*4+0x90], trampoline 0x5603B6,
 * continuing at 0x49F71B) is put back in place at load.
 * Other cells are checked periodically while the game runs: when a cell has been resolved to such a trampoline
 * whose return lands right after its jmp, the cell is pointed (one aligned 4-byte write) at a copy of the moved
 * instruction followed by a direct jump back. [Performance] SecuromFix. */
#include "m2r.h"
#include <string.h>
#include <stdio.h>

#define TEXT_LO 0x401000
#define TEXT_HI 0xA60000
#define CELL_LO 0x23E9000
#define CELL_HI 0x3700000

static int g_fixed, g_seen;
static BYTE *g_stubs; static int g_stubUsed;
static DWORD *g_sites; static int g_nsites;

static int Readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (DWORD)p + n <= (DWORD)mbi.BaseAddress + mbi.RegionSize;
}
/* trampoline at t: returns the moved instruction's length and the computed return target, or 0 */
static int Tramp(const BYTE *t, DWORD *ret)
{
    if (!Readable(t, 48)) return 0;
    for (int L = 1; L <= 15; L++) {
        const BYTE *p = t + L;
        if (p[0] < 0x50 || p[0] > 0x57 || p[1] != 0x9C || p[2] != 0x8B) continue;
        int r = p[0] - 0x50;
        if (p[3] != (BYTE)(0x05 | r << 3) || p[8] != 0xF7 || p[9] != (BYTE)(0xD0 | r) || p[10] != 0x33 ||
            p[11] != (BYTE)(0x05 | r << 3) || p[16] != 0x87 || p[17] != (BYTE)(0x44 | r << 3) || p[18] != 0x24 ||
            p[19] != 0x04 || p[20] != 0x9D || p[21] != 0xC3) continue;
        const DWORD *a = *(const DWORD **)(p + 4), *b = *(const DWORD **)(p + 12);
        if (!Readable(a, 4) || !Readable(b, 4)) return 0;
        BYTE op = t[0];
        if (op == 0xE8 || op == 0xE9 || op == 0xEB || (op >= 0x70 && op <= 0x7F) || op == 0xC3 || op == 0xC2 ||
            (op == 0x0F && (t[1] & 0xF0) == 0x80) || op == 0xFF || op == 0x9C || op == 0x9D) return 0;
        *ret = ~*a ^ *b;
        return L;
    }
    return 0;
}
static void Scan(void)
{
    int fixedNow = 0;
    for (int i = 0; i < g_nsites; i++) {
        DWORD site = g_sites[i];
        if (!site) continue;
        const BYTE *sp = (const BYTE *)site;
        DWORD c = *(const DWORD *)(sp + 2);
        /* the protection rewrites some of its code while the game runs: only an unchanged jmp [cell] counts */
        if (sp[0] != 0xFF || sp[1] != 0x25 || c < CELL_LO || c >= CELL_HI || (c & 3) || !Readable((void *)c, 4)) { g_sites[i] = 0; continue; }
        DWORD *cell = (DWORD *)c;
        DWORD t = *cell, ret;
        int L = Tramp((const BYTE *)t, &ret);
        if (!L) continue;
        if (ret <= site + 5 || ret > site + 12 || g_stubUsed + L + 5 > 65536) { g_sites[i] = 0; continue; }
        BYTE *s = g_stubs + g_stubUsed;
        memcpy(s, (const void *)t, L);
        s[L] = 0xE9; *(DWORD *)(s + L + 1) = ret - (DWORD)(s + L + 5);
        g_stubUsed += (L + 5 + 15) & ~15;
        FlushInstructionCache(GetCurrentProcess(), s, L + 5);
        DWORD old;
        if (VirtualProtect(cell, 4, PAGE_READWRITE, &old)) {
            InterlockedExchange((LONG *)cell, (LONG)(DWORD)s);
            VirtualProtect(cell, 4, old, &old);
            fixedNow++;
        }
        g_sites[i] = 0;
    }
    if (fixedNow) { g_fixed += fixedNow; Log("securom: %d more detours replaced (%d in all)", fixedNow, g_fixed); }
}
static DWORD WINAPI Watch(LPVOID u)
{
    (void)u;
    for (int n = 0; n < 40; n++) { Sleep(15000); Scan(); }   /* the first 10 minutes cover what gameplay resolves */
    return 0;
}

/* Ray collector constructor (thunk 0x6AEF70 -> jmp [0x245E84C] -> 0x3490000 in 1.0), used by every Havok ray cast of
 * FUN_00415670 / 0x413596 (line of sight, aiming, vehicles): base constructor, vtable, then one step that the
 * protection turned into virtual-machine bytecode run through its dispatcher (push 0x403056 / jmp 0x1AAFF10):
 * a quarter of the main thread's time in towns. The constructor only stores constants into the object, so they
 * are learned once by running it on scratch buffers with different fill patterns (bytes equal in both results
 * were written; a third run with another pattern must match the prediction exactly), and from then on the
 * constructor just stores them. Any difference keeps the original. */
#define CTOR_CELL 0x245E84C
#define CTOR_BASE 0x8CDE80                       /* the base constructor the protected code continues in */
#define CTOR_VT   0xB6CB7C                       /* the vtable it stores */
DWORD g_ctorOrig __attribute__((used));                         /* the protected code the cell leads to */
#define CTOR_SPAN 0x200
static volatile LONG g_ctorState;                 /* 0 not learned, 2 learning, 1 learned, -1 keep original */
static BYTE g_ctorVal[CTOR_SPAN], g_ctorSet[CTOR_SPAN];
static struct { WORD off; WORD n; } g_ctorRun[64]; static int g_nCtorRun;
__attribute__((naked)) static void CallCtor(void)  /* eax = buffer */
{
    __asm__ volatile(
        "push %%esi\n\t" "push %%edi\n\t" "push %%ebx\n\t" "push %%ebp\n\t"
        "mov %%eax, %%esi\n\t"
        "call *_g_ctorOrig\n\t"
        "pop %%ebp\n\t" "pop %%ebx\n\t" "pop %%edi\n\t" "pop %%esi\n\t"
        "ret\n\t" ::: "memory");
}
static void RunCtor(BYTE *b, BYTE fill) { memset(b, fill, CTOR_SPAN); __asm__ volatile("call *%1" : "+a"(b) : "r"(CallCtor) : "ecx", "edx", "memory"); }
static void CtorLearn2(void);
static void __cdecl CtorLearn(void)
{
    if (InterlockedCompareExchange(&g_ctorState, 2, 0) != 0) return;    /* one thread learns; others run the original */
    CtorLearn2();
    if (g_ctorState == 2) g_ctorState = -1;
}
static void CtorLearn2(void)
{
    static BYTE a[CTOR_SPAN] __attribute__((aligned(16))), b[CTOR_SPAN] __attribute__((aligned(16))), c[CTOR_SPAN] __attribute__((aligned(16)));
    RunCtor(a, 0xA5); RunCtor(b, 0x3C);
    for (int i = 0; i < CTOR_SPAN; i++) {
        if (a[i] == b[i]) { g_ctorSet[i] = 1; g_ctorVal[i] = a[i]; }
        else if (a[i] != 0xA5 || b[i] != 0x3C) { g_ctorState = -1; Log("securom: ray collector constructor stores data-dependent byte at +%X, kept", i); return; }
    }
    RunCtor(c, 0x00);
    for (int i = 0; i < CTOR_SPAN; i++)
        if (c[i] != (g_ctorSet[i] ? g_ctorVal[i] : 0)) { g_ctorState = -1; Log("securom: ray collector constructor check failed at +%X, kept", i); return; }
    int n = 0, last = -2, bytes = 0;
    for (int i = 0; i < CTOR_SPAN; i++) if (g_ctorSet[i]) {
        if (i == last + 1 && n) g_ctorRun[n - 1].n++;
        else { if (n == 64) { g_ctorState = -1; return; } g_ctorRun[n].off = (WORD)i; g_ctorRun[n].n = 1; n++; }
        last = i; bytes++;
    }
    g_nCtorRun = n;
    char line[256]; int len = 0;
    for (int k = 0; k < n && len < 200; k++) len += _snprintf(line + len, sizeof line - len, " +%X:%d", g_ctorRun[k].off, g_ctorRun[k].n);
    Log("securom: ray collector constructor learned (%d bytes in %d runs:%s), virtual machine step skipped", bytes, n, line);
    g_ctorState = 1;
}
static void __fastcall CtorFast(BYTE *o)
{
    for (int k = 0; k < g_nCtorRun; k++) memcpy(o + g_ctorRun[k].off, g_ctorVal + g_ctorRun[k].off, g_ctorRun[k].n);
}
/* entered by jmp [0x245E84C] with esi = object, returns eax = object */
__attribute__((naked)) static void RayCtor(void)
{
    __asm__ volatile(
        "cmpl $1, %0\n\t"
        "je 2f\n\t"
        "cmpl $0, %0\n\t"
        "jne 1f\n\t"
        "pushal\n\t" "call %P1\n\t" "popal\n\t"
        "1: jmp *_g_ctorOrig\n\t"
        "2: mov %%esi, %%ecx\n\t"
        "call %P2\n\t"
        "mov %%esi, %%eax\n\t"
        "ret\n\t" :: "m"(g_ctorState), "i"(CtorLearn), "i"(CtorFast));
}

/* the cell leads to: mov ecx,esi / push T2 / push CTOR_BASE / ret, and T2: or eax,-1 / mov [esi],CTOR_VT / push ... */
static int CtorHook(void)
{
    DWORD t = *(volatile DWORD *)CTOR_CELL;
    if (!Expect(t, "8B CE 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? C3") || *(DWORD *)(t + 8) != CTOR_BASE) return 0;
    DWORD t2 = *(DWORD *)(t + 3);
    if (!Expect(t2, "83 C8 FF C7 06 ?? ?? ?? ?? ??") || *(DWORD *)(t2 + 5) != CTOR_VT) return 0;
    if (*(BYTE *)(t2 + 9) != 0x68) {   /* 1.1: the constructor is plain code (no virtual machine step): nothing to skip */
        Log("securom: ray collector constructor is plain code in this build (%08lX), no hook needed", t);
        return 1;
    }
    g_ctorOrig = t;
    DWORD old; void *stub = (void *)RayCtor;
    if (!VirtualProtect((void *)CTOR_CELL, 4, PAGE_READWRITE, &old)) return 0;
    LONG prev = InterlockedCompareExchange((LONG *)CTOR_CELL, (LONG)(DWORD)stub, (LONG)t);
    VirtualProtect((void *)CTOR_CELL, 4, old, &old);
    if (prev != (LONG)t) return 0;
    Log("securom: ray collector constructor hooked (%08lX)", t);
    return 1;
}
static DWORD g_ctorLazy;
static DWORD WINAPI CtorWatch(LPVOID u)
{
    (void)u;
    int changes = 0;
    for (int n = 0; n < 4 * 60 * 30; n++) {                 /* up to 30 minutes */
        Sleep(250);
        DWORD t = *(volatile DWORD *)CTOR_CELL;
        if (t == g_ctorLazy) continue;
        if (CtorHook()) return 0;
        Log("securom: ray collector constructor cell -> %08lX (unexpected code)", t);
        if (++changes == 3) { Log("securom: ray collector constructor kept"); return 0; }
        g_ctorLazy = t;
    }
    return 0;
}

void SecuromInit(void)
{
    if (!IniInt("Performance", "SecuromFix", 1)) return;
    /* forest instance decoder: put the moved instruction back */
    if (Expect(0x49F714, "FF 25 28 5B 45 02") && Expect(0x5603B6, "8B 94 88 90 00 00 00 57 9C")) {
        static const BYTE mov[7] = {0x8B, 0x94, 0x88, 0x90, 0x00, 0x00, 0x00};
        WriteMem(0x49F714, mov, 7);
        Log("securom: forest decoder detour removed");
    } else Log("securom: forest decoder code mismatch, skipped");
    /* Havok ray collector constructor: in 1.0 the cell is already resolved (VM step, hooked); in 1.1 the protection resolves
       it on the first ray cast (to plain code there, which needs no hook), so it is watched until then */
    if (Expect(0x6AEF70, "FF 25 4C E8 45 02")) {
        if (!CtorHook()) { g_ctorLazy = *(DWORD *)CTOR_CELL; CloseHandle(CreateThread(NULL, 0, CtorWatch, NULL, 0, NULL)); }
    } else Log("securom: ray collector constructor code mismatch, skipped");
    /* list every jmp [cell] in the game code whose cell lies in the protection's data */
    g_sites = (DWORD *)VirtualAlloc(NULL, 4 * 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_stubs = (BYTE *)VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_sites || !g_stubs) return;
    for (DWORD a = TEXT_LO; a + 6 <= TEXT_HI && g_nsites < 8192; a++) {
        const BYTE *p = (const BYTE *)a;
        if (p[0] == 0xFF && p[1] == 0x25) {
            DWORD c = *(const DWORD *)(p + 2);
            if (c >= CELL_LO && c < CELL_HI && !(c & 3)) g_sites[g_nsites++] = a;
        }
    }
    g_seen = g_nsites;
    Log("securom: %d protected jumps watched", g_nsites);
    CloseHandle(CreateThread(NULL, 0, Watch, NULL, 0, NULL));
}
