/* m2r:no-retarget (1.1 addresses, built only for the 1.1 DLL)
 * msm_port.c - native reimplementation of the SecuROM-protected PgModelStateMachine
 *              functions of Mercenaries 2 PC exe 1.1 (+ logging / comparison harness).
 *
 * Ported functions (1.1 addresses; 1.0 = 1.1 - 0x190):
 *   0x4D4980  MsmFindTransition   (thunk jmp [0x24DC2C0]; real body = spliced code at 0x67C210)
 *             Xbox base 0x82231950
 *   0x4D4760  MsmSetState         (thunk jmp [0x24DD0EC]; real body = spliced code at 0x5A0290)
 *             Xbox base 0x82231708
 *   0x4D1C90  MsmRunActions       (plain code, 1 stolen instruction; ported only to log actions)
 *             Xbox base 0x8222ED78
 * Called unchanged (plain PC code):
 *   0x4D41A0  MsmEnterState (inner apply, Xbox 0x82231380)   eax=smo, stdcall(guid, layerIdx, target, script)
 *   0x587D80  component lookup with template fallback       eax=guid, ecx=map -> eax=record|0
 *
 * See PORT.md for evidence, layouts, install list and verification plan.
 *
 * Build check:  i686-w64-mingw32-gcc -O2 -c -Wall msm_port.c
 * In the DLL:   #define MSM_NO_STUBS, include <windows.h> first, provide Log() and A11().
 */
#include <stdio.h>
#include <stddef.h>
#define M2R_DLL
#ifdef M2R_DLL
#include "m2r.h"
#define MSM_NO_STUBS
#endif
#if defined(M2R_GAME11) || !defined(M2R_DLL)

#ifndef MSM_NO_STUBS
typedef unsigned long DWORD;
void Log(const char *fmt, ...);
#define A11(x) (x)
__declspec(dllimport) int   __stdcall VirtualProtect(void *addr, unsigned long size, DWORD prot, DWORD *old);
__declspec(dllimport) int   __stdcall FlushInstructionCache(void *proc, const void *addr, unsigned long size);
__declspec(dllimport) void *__stdcall GetCurrentProcess(void);
__declspec(dllimport) DWORD __stdcall GetTickCount(void);
#endif

#define MSM_USED __attribute__((used))

/* ------------------------------------------------------------------------------------------
 * Data layouts (PC 1.1, little-endian, all 32-bit fields). Offsets checked against PC code:
 *   lookup body 0x67C210, apply body 0x5A0290, inner apply 0x4D41A0, executor 0x4D1C90,
 *   SMO init 1.0 0x4D3F00, poller 0x4CFA10.
 * ------------------------------------------------------------------------------------------ */

typedef struct MsmTransition {      /* 12 bytes, def+0x18 array */
    DWORD layerKey;                 /* +0 compared with arg2 (event field +4)               */
    DWORD eventHash;                /* +4 compared with arg3 (event hash)                   */
    DWORD targetState;              /* +8 returned                                          */
} MsmTransition;

typedef struct MsmState {           /* 20 bytes, layer->states (stride 0x14 in 0x4D41A0)   */
    DWORD  key;                     /* +0x00                                                */
    int    enterLen;                /* +0x04 dword count of enter list (loader)             */
    DWORD *enterList;               /* +0x08 run AFTER cur = target                         */
    int    exitLen;                 /* +0x0C                                                */
    DWORD *exitList;                /* +0x10 run BEFORE cur = target                        */
} MsmState;

typedef struct MsmLayer {           /* 12 bytes, def+8 array (stride 0xC)                   */
    DWORD     key;                  /* +0 (layer key; = transition.layerKey)                */
    int       stateCount;           /* +4                                                   */
    MsmState *states;               /* +8                                                   */
} MsmLayer;

typedef struct MsmDef {             /* resident "PgModelStateMachine" data                  */
    DWORD           unk00;
    int             layerCount;     /* +0x04                                                */
    MsmLayer       *layers;         /* +0x08                                                */
    DWORD           unk0C, unk10, unk14;
    MsmTransition  *trans;          /* +0x18                                                */
    int             transCount;     /* +0x1C  (<8 -> linear scan, else hash)                */
    DWORD           hashCap;        /* +0x20  open-addressing capacity                      */
    DWORD           hashUnk24;      /* +0x24                                                */
    MsmTransition **hashValues;     /* +0x28  first transition of the (layer,event) run      */
    DWORD          *hashKeys;       /* +0x2C  key = combine(layer,event), 0 = empty         */
    MsmTransition  *hashMiss;       /* +0x30  slot used when the key is absent (expected 0) */
} MsmDef;

typedef struct MsmObj {             /* 0x38 bytes = element of component map 0xDF8310       */
    DWORD          unk00;           /* +0x00 copied from source record [0]                  */
    MsmDef        *def;             /* +0x04                                                */
    unsigned char *renderInst;      /* +0x08 byte +0x0D = "dirty" -> sets flags bit0        */
    DWORD         *curState;        /* +0x0C one current state key per layer                */
    unsigned char  flags;           /* +0x10 bit0 = poller calls 0x4D46D0(smo)              */
    unsigned char  pad11[3];
    int            bitCap;          /* +0x14 = 0x100                                        */
    DWORD          enabled[8];      /* +0x18 transition-enable bits, bit i = 1u<<(i&31)     */
} MsmObj;

typedef struct MsmActionTable {     /* 0x208 bytes; MSM table at 0x16CDED8                  */
    int   count;                    /* +0x000 number of registered actions (35)             */
    void *missHandler;              /* +0x004 called for unknown hashes (NULL at runtime!)  */
    void *handlers[64];             /* +0x008                                               */
    DWORD keys[64];                 /* +0x108 action hash, 0 = empty, slot = hash%64 probe  */
} MsmActionTable;

typedef struct MsmCtx {             /* 0x414 bytes, on the stack of 0x4D41A0 (esp+0x24)     */
    int             pc;             /* +0x000                                               */
    int             depth;          /* +0x004                                               */
    DWORD           stack[256];     /* +0x008 argument stack (handlers pop by depth -= n)   */
    MsmObj         *smo;            /* +0x408                                               */
    DWORD           layerKey;       /* +0x40C                                               */
    MsmActionTable *table;          /* +0x410 = 0x16CDED8 for state actions                 */
} MsmCtx;

_Static_assert(offsetof(MsmDef, trans) == 0x18, "def.trans");
_Static_assert(offsetof(MsmDef, hashCap) == 0x20, "def.hashCap");
_Static_assert(offsetof(MsmDef, hashValues) == 0x28, "def.hashValues");
_Static_assert(offsetof(MsmDef, hashKeys) == 0x2C, "def.hashKeys");
_Static_assert(offsetof(MsmDef, hashMiss) == 0x30, "def.hashMiss");
_Static_assert(offsetof(MsmObj, curState) == 0x0C, "smo.cur");
_Static_assert(offsetof(MsmObj, flags) == 0x10, "smo.flags");
_Static_assert(offsetof(MsmObj, enabled) == 0x18, "smo.enabled");
_Static_assert(sizeof(MsmObj) == 0x38, "smo size");
_Static_assert(sizeof(MsmState) == 0x14, "state size");
_Static_assert(sizeof(MsmLayer) == 0x0C, "layer size");
_Static_assert(sizeof(MsmTransition) == 0x0C, "transition size");
_Static_assert(offsetof(MsmActionTable, keys) == 0x108, "table.keys");
_Static_assert(offsetof(MsmCtx, smo) == 0x408, "ctx.smo");
_Static_assert(offsetof(MsmCtx, table) == 0x410, "ctx.table");

/* ------------------------------------------------------------------------------------------
 * Game addresses (1.1)
 * ------------------------------------------------------------------------------------------ */
#define MSM_ADDR_LOOKUP_THUNK   0x4D4980u
#define MSM_ADDR_APPLY_THUNK    0x4D4760u
#define MSM_ADDR_INNER_APPLY    0x4D41A0u
#define MSM_ADDR_EXEC           0x4D1C90u
#define MSM_ADDR_COMP_LOOKUP    0x587D80u
#define MSM_MAP_OBJECTSCRIPT    0x17BCD58u   /* name ptr at +0x3C -> "ObjectScript" */

/* ------------------------------------------------------------------------------------------
 * Configuration / state (all globals are plain ints so a console / ini can poke them)
 * ------------------------------------------------------------------------------------------ */
enum { MSM_LOOKUP_ORIGINAL = 0, MSM_LOOKUP_NATIVE = 1, MSM_LOOKUP_COMPARE = 2 };
enum { MSM_APPLY_ORIGINAL = 0, MSM_APPLY_NATIVE = 1 };
enum { MSM_EXEC_ORIGINAL = 0, MSM_EXEC_NATIVE = 1 };

int   g_msmLogAll          = 0;   /* 1 = log every lookup/apply/action                       */
int   g_msmLogPerSecond    = 20;  /* otherwise: first N (non-watched) log lines per second    */
int   g_msmLogMisses       = 0;   /* also log lookups that returned 0 (very frequent)        */
DWORD g_msmWatch[16];             /* guids always logged (0 = empty slot)                    */
int   g_msmLookupMode      = MSM_LOOKUP_NATIVE;   /* the original is a protected thunk: not called from here */
int   g_msmCompareUseNative = 0;  /* compare mode: 1 = return native result, 0 = original    */
int   g_msmApplyMode       = MSM_APPLY_NATIVE;
int   g_msmExecMode        = MSM_EXEC_NATIVE;

struct MsmStats {
    DWORD lookups, lookupHits, mismatches;
    DWORD applies, applyNoLayer, applySame;
    DWORD actions, actionUnknown, actionNullHandler, stackOverflow;
} g_msmStats;

/* function pointers used by the asm glue (set in MsmInit so A11() may be a runtime fn) */
MSM_USED void *g_msmPfnLookupOrig;
MSM_USED void *g_msmPfnApplyOrig;
MSM_USED void *g_msmPfnInnerApply;
MSM_USED void *g_msmPfnExecOrig;
MSM_USED void *g_msmPfnCompLookup;

/* ------------------------------------------------------------------------------------------
 * asm glue (MinGW i686, cdecl symbols carry a leading underscore)
 * ------------------------------------------------------------------------------------------ */
/* entry stubs (targets of the redirected E8 rel32) */
void msm_LookupEntry(void);   /* stdcall(smo, layerKey, eventHash) -> eax; edi = guid at 0x4CFB17 */
void msm_ApplyEntry(void);    /* stdcall(smo, layerKey, target), edi = guid (register arg)       */
void msm_ExecEntry(void);     /* stdcall(list, guid), edi = MsmCtx*                              */
/* outgoing calls into the game */
DWORD msm_CallApplyOrig(DWORD guid, MsmObj *smo, DWORD layerKey, DWORD target);
void  msm_CallInnerApply(MsmObj *smo, DWORD guid, int layerIdx, DWORD target, void *script);
void  msm_CallExecOrig(MsmCtx *ctx, DWORD *list, DWORD guid);
void *msm_CallCompLookup(DWORD map, DWORD guid);
void  msm_CallHandler(void *fn, void *slot, MsmCtx *ctx, DWORD guid,
                      void *ebxv, void *esiv, DWORD *ebpv);

__asm__(
".intel_syntax noprefix\n"
".text\n"
".globl _msm_LookupEntry\n"
"_msm_LookupEntry:\n"
"    mov  eax, [esp]\n"                 /* caller return address (for the log)            */
"    push dword ptr [esp+12]\n"         /* eventHash                                      */
"    push dword ptr [esp+12]\n"         /* layerKey                                       */
"    push dword ptr [esp+12]\n"         /* smo                                            */
"    push edi\n"                        /* guid (valid at the only caller 0x4CFB17)       */
"    push eax\n"
"    call _msm_LookupC\n"               /* cdecl, preserves ebx/esi/edi/ebp               */
"    add  esp, 20\n"
"    ret  12\n"

".globl _msm_ApplyEntry\n"
"_msm_ApplyEntry:\n"
"    mov  eax, [esp]\n"
"    push dword ptr [esp+12]\n"         /* target                                         */
"    push dword ptr [esp+12]\n"         /* layerKey                                       */
"    push dword ptr [esp+12]\n"         /* smo                                            */
"    push edi\n"                        /* guid                                           */
"    push eax\n"
"    call _msm_ApplyC\n"
"    add  esp, 20\n"
"    ret  12\n"

".globl _msm_ExecEntry\n"
"_msm_ExecEntry:\n"
"    mov  eax, [esp]\n"
"    push dword ptr [esp+8]\n"          /* guid                                           */
"    push dword ptr [esp+8]\n"          /* list                                           */
"    push edi\n"                        /* ctx                                            */
"    push eax\n"
"    call _msm_ExecC\n"
"    add  esp, 16\n"
"    ret  8\n"

/* DWORD msm_CallApplyOrig(guid, smo, layerKey, target) */
".globl _msm_CallApplyOrig\n"
"_msm_CallApplyOrig:\n"
"    push edi\n"
"    mov  edi, [esp+8]\n"               /* guid -> edi                                    */
"    push dword ptr [esp+20]\n"         /* target                                         */
"    push dword ptr [esp+20]\n"         /* layerKey                                       */
"    push dword ptr [esp+20]\n"         /* smo                                            */
"    call dword ptr [_g_msmPfnApplyOrig]\n"  /* stdcall, pops 12                          */
"    pop  edi\n"
"    ret\n"

/* void msm_CallInnerApply(smo, guid, layerIdx, target, script) */
".globl _msm_CallInnerApply\n"
"_msm_CallInnerApply:\n"
"    push ebx\n"
"    push esi\n"
"    push edi\n"
"    push ebp\n"
"    mov  eax, [esp+20]\n"              /* smo in eax                                     */
"    mov  edi, [esp+24]\n"              /* edi = guid, like the original caller           */
"    push dword ptr [esp+36]\n"         /* script                                         */
"    push dword ptr [esp+36]\n"         /* target                                         */
"    push dword ptr [esp+36]\n"         /* layerIdx                                       */
"    push dword ptr [esp+36]\n"         /* guid                                           */
"    call dword ptr [_g_msmPfnInnerApply]\n" /* ret 0x10                                   */
"    pop  ebp\n"
"    pop  edi\n"
"    pop  esi\n"
"    pop  ebx\n"
"    ret\n"

/* void msm_CallExecOrig(ctx, list, guid) */
".globl _msm_CallExecOrig\n"
"_msm_CallExecOrig:\n"
"    push edi\n"
"    mov  edi, [esp+8]\n"
"    push dword ptr [esp+16]\n"         /* guid                                           */
"    push dword ptr [esp+16]\n"         /* list                                           */
"    call dword ptr [_g_msmPfnExecOrig]\n"   /* ret 8                                      */
"    pop  edi\n"
"    ret\n"

/* void *msm_CallCompLookup(map, guid) */
".globl _msm_CallCompLookup\n"
"_msm_CallCompLookup:\n"
"    push ebx\n"
"    push esi\n"
"    push edi\n"
"    push ebp\n"
"    mov  ecx, [esp+20]\n"
"    mov  eax, [esp+24]\n"
"    call dword ptr [_g_msmPfnCompLookup]\n"
"    pop  ebp\n"
"    pop  edi\n"
"    pop  esi\n"
"    pop  ebx\n"
"    ret\n"

/* void msm_CallHandler(fn, slot, ctx, guid, ebxv, esiv, ebpv) : cdecl handler(ctx, guid)
   with the register state of the original executor (eax=&slot, edi=ctx, ebx=table,
   esi=table->keys, ebp=list). Handlers only read stack args, the registers are insurance. */
".globl _msm_CallHandler\n"
"_msm_CallHandler:\n"
"    push ebx\n"
"    push esi\n"
"    push edi\n"
"    push ebp\n"
"    mov  ecx, [esp+20]\n"              /* fn                                             */
"    mov  edi, [esp+28]\n"              /* ctx                                            */
"    mov  ebx, [esp+36]\n"
"    mov  esi, [esp+40]\n"
"    mov  ebp, [esp+44]\n"
"    mov  eax, [esp+24]\n"              /* slot                                           */
"    push dword ptr [esp+32]\n"         /* guid                                           */
"    push edi\n"                        /* ctx                                            */
"    call ecx\n"
"    add  esp, 8\n"
"    pop  ebp\n"
"    pop  edi\n"
"    pop  esi\n"
"    pop  ebx\n"
"    ret\n"
".att_syntax prefix\n"
);

/* ------------------------------------------------------------------------------------------
 * logging policy
 * ------------------------------------------------------------------------------------------ */
static DWORD s_logSecond, s_logCount;

static int msm_IsWatched(DWORD guid)
{
    int i;
    if (!guid) return 0;
    for (i = 0; i < 16; ++i)
        if (g_msmWatch[i] == guid) return 1;
    return 0;
}

static int msm_ShouldLog(DWORD guid)
{
    DWORD sec;
    if (g_msmLogAll || msm_IsWatched(guid)) return 1;
    if (g_msmLogPerSecond <= 0) return 0;
    sec = GetTickCount() / 1000u;
    if (sec != s_logSecond) { s_logSecond = sec; s_logCount = 0; }
    if (s_logCount >= (DWORD)g_msmLogPerSecond) return 0;
    ++s_logCount;
    return 1;
}

int MsmWatchAdd(DWORD guid)
{
    int i;
    for (i = 0; i < 16; ++i) if (g_msmWatch[i] == guid) return i;
    for (i = 0; i < 16; ++i) if (!g_msmWatch[i]) { g_msmWatch[i] = guid; return i; }
    return -1;
}
void MsmWatchClear(void) { int i; for (i = 0; i < 16; ++i) g_msmWatch[i] = 0; }

static const char *msm_EventName(DWORD h)
{
    switch (h) {
    case 0xC6507EE1u: return "DamageThreshold";
    case 0x3D0D4C99u: return "Death(fire/explosion)";
    case 0x1ED7AD78u: return "Death";
    case 0x87D3B9A1u: return "NodeDeath";
    default:          return "";
    }
}

/* ------------------------------------------------------------------------------------------
 * open-addressing find, == 0x8244E0 (Xbox 0x82855778): ecx=key, esi=keys, [esp+4]=cap.
 * idx = key % cap (unsigned), linear probe with wrap-around; stop at match or empty (0).
 * key 0 -> -1.  (cap 0 would #DE in the original; we return -1.)
 * ------------------------------------------------------------------------------------------ */
static int msm_HashFind(DWORD key, const DWORD *keys, DWORD cap)
{
    DWORD i, n, k;
    if (!key || !cap) return -1;
    i = key % cap;
    for (n = 0; n < cap; ++n) {
        k = keys[i];
        if (k == key) return (int)i;
        if (k == 0) return -1;
        if (++i == cap) i = 0;
    }
    return -1;
}

/* ------------------------------------------------------------------------------------------
 * 1) transition lookup  (0x4D4980 / Xbox 0x82231950)
 * ------------------------------------------------------------------------------------------ */
static int msm_Enabled(const MsmObj *smo, int i)
{
    return (smo->enabled[i >> 5] & (1u << (i & 31))) != 0;
}

DWORD MsmFindTransitionNative(MsmObj *smo, DWORD layerKey, DWORD eventHash)
{
    MsmDef *def;
    MsmTransition *t, *e;
    int n, i, idx;
    DWORD h;

    if (!smo || !(def = smo->def)) return 0;          /* original would fault here */
    t = def->trans;
    if (!t) return 0;
    n = def->transCount;
    if (n < 8) {                                      /* signed compare, like `cmp ebp,8; jl` */
        for (i = 0; i < n; ++i)
            if (t[i].layerKey == layerKey && t[i].eventHash == eventHash && msm_Enabled(smo, i))
                return t[i].targetState;
        return 0;
    }
    h = ((layerKey ^ 0x811C9DC5u) * 0x01000193u) ^ eventHash;   /* salt read from [0x24DD850] */
    if (!h) h = 1;                    /* Xbox 0x82855980; on PC this branch is a VM'd snippet */
    idx = msm_HashFind(h, def->hashKeys, def->hashCap);
    e = (idx >= 0) ? def->hashValues[idx] : def->hashMiss;
    if (!e) return 0;
    i = (int)(((char *)e - (char *)t) / 12);          /* signed /12 (imul 0x2AAAAAAB) */
    if (i >= n) return 0;
    for (; i < n; ++i, ++e) {
        if (e->layerKey != layerKey || e->eventHash != eventHash) return 0;
        if (msm_Enabled(smo, i)) return e->targetState;
    }
    return 0;
}

typedef DWORD (__stdcall *PFN_MsmLookup)(MsmObj *, DWORD, DWORD);

static void msm_DumpDef(DWORD guid, MsmObj *smo)
{
    static MsmDef *done[16]; int i, j, k;
    MsmDef *d = smo ? smo->def : 0; if (!d || IsBadReadPtr(d, 0x34)) return;
    for (i = 0; i < 16 && done[i]; ++i) if (done[i] == d) return;
    if (i < 16) done[i] = d; else return;
    Log("MSM DEF guid=%08lX def=%p layers=%d transitions=%d", guid, (void *)d, d->layerCount, d->transCount);
    for (i = 0; i < d->transCount && i < 256; ++i) Log("MSM DEF  trans %d: layer %08lX event %08lX -> %08lX", i, d->trans[i].layerKey, d->trans[i].eventHash, d->trans[i].targetState);
    for (i = 0; i < d->layerCount && i < 32; ++i) {
        MsmLayer *L = &d->layers[i];
        Log("MSM DEF  layer %08lX states %d", L->key, L->stateCount);
        for (j = 0; j < L->stateCount && j < 64; ++j) {
            MsmState *S = &L->states[j]; char b[3000]; int n;
            n = _snprintf(b, sizeof b, "MSM DEF   state %08lX enter[%d]:", S->key, S->enterLen);
            for (k = 0; k < S->enterLen && k < 300 && n < 2900; ++k) n += _snprintf(b + n, sizeof b - n, " %lX", S->enterList[k]);
            Log("%s", b);
            n = _snprintf(b, sizeof b, "MSM DEF   state %08lX exit[%d]:", S->key, S->exitLen);
            for (k = 0; k < S->exitLen && k < 300 && n < 2900; ++k) n += _snprintf(b + n, sizeof b - n, " %lX", S->exitList[k]);
            Log("%s", b);
        }
    }
}
MSM_USED DWORD msm_LookupC(void *caller, DWORD guid, MsmObj *smo, DWORD layerKey, DWORD eventHash)
{
    DWORD rOrig = 0, rNat = 0, r;
    ++g_msmStats.lookups;
    switch (g_msmLookupMode) {
    case MSM_LOOKUP_NATIVE:
        r = rNat = MsmFindTransitionNative(smo, layerKey, eventHash);
        break;
    case MSM_LOOKUP_COMPARE:
        rOrig = ((PFN_MsmLookup)g_msmPfnLookupOrig)(smo, layerKey, eventHash);
        rNat  = MsmFindTransitionNative(smo, layerKey, eventHash);
        if (rOrig != rNat) {
            ++g_msmStats.mismatches;
            Log("MSM LOOKUP MISMATCH guid=%08lX smo=%p layer=%08lX ev=%08lX orig=%08lX native=%08lX "
                "n=%d caller=%p\n", guid, (void *)smo, layerKey, eventHash, rOrig, rNat,
                (smo && smo->def) ? smo->def->transCount : -1, caller);
        }
        r = g_msmCompareUseNative ? rNat : rOrig;
        break;
    default:
        r = rOrig = ((PFN_MsmLookup)g_msmPfnLookupOrig)(smo, layerKey, eventHash);
        break;
    }
    if (r) ++g_msmStats.lookupHits;
    if (*msm_EventName(eventHash)) { MsmWatchAdd(guid); msm_DumpDef(guid, smo); }   /* damage/death events: follow this object */
    if ((r || g_msmLogMisses || *msm_EventName(eventHash) || msm_IsWatched(guid)) && msm_ShouldLog(guid))
        Log("MSM lookup guid=%08lX smo=%p layer=%08lX ev=%08lX %s -> %08lX%s\n",
            guid, (void *)smo, layerKey, eventHash, msm_EventName(eventHash), r,
            g_msmLookupMode == MSM_LOOKUP_COMPARE ? (rOrig == rNat ? " [cmp ok]" : " [cmp BAD]") : "");
    return r;
}

/* ------------------------------------------------------------------------------------------
 * 3) action executor (0x4D1C90 / Xbox 0x8222ED78) - native, logs every action
 * ------------------------------------------------------------------------------------------ */
#define MSM_PHASE_MAX 8
static int    s_phDepth;
static DWORD *s_phExit[MSM_PHASE_MAX], *s_phEnter[MSM_PHASE_MAX];
static DWORD  s_phGuid[MSM_PHASE_MAX];
static int    s_phLog[MSM_PHASE_MAX];

static const char *msm_ListPhase(const DWORD *list, DWORD guid, int *logIt)
{
    int d = s_phDepth - 1;
    *logIt = 0;
    if (d >= 0 && d < MSM_PHASE_MAX && s_phGuid[d] == guid) {
        *logIt = s_phLog[d];
        if (list == s_phExit[d])  return "exit";
        if (list == s_phEnter[d]) return "enter";
    }
    return "list";
}

MSM_USED void msm_ExecC(void *caller, MsmCtx *ctx, DWORD *list, DWORD guid)
{
    int logIt, before, after, i, idx;
    const char *phase;
    MsmActionTable *tab;
    void *fn, *slot;
    DWORD op, hash;

    if (g_msmExecMode != MSM_EXEC_NATIVE) { msm_CallExecOrig(ctx, list, guid); return; }
    if (!list) return;
    phase = msm_ListPhase(list, guid, &logIt);
    if (!logIt) logIt = g_msmLogAll || msm_IsWatched(guid);

    ctx->pc = 0;                      /* stolen instruction `mov dword ptr [edi],0` */
    ctx->depth = 0;
    for (;;) {
        op = list[ctx->pc];
        if (op == 1) {                /* push literal */
            ctx->pc++;
            if (ctx->depth >= 256) {  /* original has no check (would overwrite ctx->smo) */
                ++g_msmStats.stackOverflow;
                Log("MSM exec STACK OVERFLOW guid=%08lX list=%p - list aborted\n", guid, (void *)list);
                return;
            }
            ctx->stack[ctx->depth] = list[ctx->pc];
            ctx->depth++;
            ctx->pc++;
            continue;
        }
        if (op == 2) {                /* call action */
            tab = ctx->table;
            ctx->pc++;
            hash = list[ctx->pc];
            idx = msm_HashFind(hash, tab->keys, 0x40);
            if (idx < 0) { slot = &tab->missHandler; ++g_msmStats.actionUnknown; }
            else           slot = &tab->handlers[idx];
            fn = *(void **)slot;
            before = ctx->depth;
            ++g_msmStats.actions;
            if (!fn) {                /* original would call NULL and crash */
                ++g_msmStats.actionNullHandler;
                Log("MSM action NULL HANDLER guid=%08lX hash=%08lX - skipped\n", guid, hash);
                ctx->pc++;
                continue;
            }
            msm_CallHandler(fn, slot, ctx, guid, tab, tab->keys, list);
            after = ctx->depth;
            if (logIt) {
                /* args consumed by the handler are still in stack[after..before-1] */
                char buf[160]; int p = 0, k;
                for (i = (after < before ? after : before); i < before && p < 140; ++i) {
                    DWORD v = ctx->stack[i], x; char tmp[9]; int nd = 0;
                    buf[p++] = ' ';
                    for (k = 7; k >= 0; --k) { x = (v >> (k * 4)) & 15; tmp[nd++] = (char)(x < 10 ? '0' + x : 'A' + x - 10); }
                    for (k = 0; k < 8; ++k) buf[p++] = tmp[k];
                }
                buf[p] = 0;
                Log("MSM   action %s guid=%08lX hash=%08lX fn=%p%s args[%d]:%s depth %d->%d\n",
                    phase, guid, hash, fn, idx < 0 ? "(miss)" : "", before - after, buf, before, after);
            }
            ctx->pc++;                /* `add dword ptr [edi],1` after the handler (re-read) */
            continue;
        }
        if (op == 3 && ctx->depth == 0) return;      /* end */
        ctx->pc++;                    /* 0, 3 with depth != 0, anything else: skip */
    }
    (void)caller;
}

/* ------------------------------------------------------------------------------------------
 * 2) state apply (0x4D4760 / Xbox 0x82231708)
 * ------------------------------------------------------------------------------------------ */
MSM_USED void msm_ApplyC(void *caller, DWORD guid, MsmObj *smo, DWORD layerKey, DWORD target)
{
    void *script;
    MsmDef *def;
    MsmLayer *L;
    MsmState *st, *cur = 0, *tgt = 0;
    DWORD from;
    int n, i, j, logIt;

    ++g_msmStats.applies;
    logIt = msm_ShouldLog(guid);

    if (g_msmApplyMode != MSM_APPLY_NATIVE) {
        if (logIt)
            Log("MSM apply(orig) guid=%08lX smo=%p layer=%08lX -> %08lX caller=%p\n",
                guid, (void *)smo, layerKey, target, caller);
        msm_CallApplyOrig(guid, smo, layerKey, target);
        return;
    }

    /* the original looks the script component up first, before touching smo */
    script = msm_CallCompLookup((DWORD)A11(MSM_MAP_OBJECTSCRIPT), guid);
    if (!smo || !(def = smo->def)) {                 /* original would fault */
        Log("MSM apply NULL smo/def guid=%08lX smo=%p caller=%p\n", guid, (void *)smo, caller);
        return;
    }
    n = def->layerCount;
    L = def->layers;
    for (i = 0; i < n; ++i, ++L)
        if (L->key == layerKey) break;
    if (i >= n) {
        ++g_msmStats.applyNoLayer;
        if (logIt)
            Log("MSM apply guid=%08lX layer=%08lX NOT FOUND (target %08lX) caller=%p\n",
                guid, layerKey, target, caller);
        return;
    }

    from = smo->curState[i];
    if (from == target) ++g_msmStats.applySame;
    for (j = 0, st = L->states; j < L->stateCount; ++j, ++st) {   /* same scan as 0x4D41A0 */
        if (st->key == target) tgt = st;
        if (st->key == from)   cur = st;
    }
    if (logIt)
        Log("MSM apply guid=%08lX smo=%p layer=%08lX[%d] %08lX -> %08lX%s exit=%p(%d) enter=%p(%d) "
            "script=%p caller=%p\n", guid, (void *)smo, layerKey, i, from, target,
            from == target ? " (same, no-op)" : "",
            cur ? (void *)cur->exitList : 0, cur ? cur->exitLen : 0,
            tgt ? (void *)tgt->enterList : 0, tgt ? tgt->enterLen : 0, script, caller);

    if (s_phDepth < MSM_PHASE_MAX) {
        s_phExit[s_phDepth]  = cur ? cur->exitList : 0;
        s_phEnter[s_phDepth] = tgt ? tgt->enterList : 0;
        s_phGuid[s_phDepth]  = guid;
        s_phLog[s_phDepth]   = logIt;
    }
    ++s_phDepth;
    msm_CallInnerApply(smo, guid, i, target, script);     /* 0x4D41A0, plain code */
    --s_phDepth;
}

/* ------------------------------------------------------------------------------------------
 * install / uninstall: retarget E8 rel32 call sites
 * ------------------------------------------------------------------------------------------ */
typedef struct MsmSite { DWORD site; DWORD orig; int group; DWORD saved; int patched; } MsmSite;

enum { MSM_G_LOOKUP = 1, MSM_G_APPLY = 2, MSM_G_EXEC = 4 };

static MsmSite s_sites[] = {
    { 0x4CFB17, MSM_ADDR_LOOKUP_THUNK, MSM_G_LOOKUP, 0, 0 },  /* poller                       */
    { 0x4B68A5, MSM_ADDR_APPLY_THUNK,  MSM_G_APPLY,  0, 0 },
    { 0x4CFC14, MSM_ADDR_APPLY_THUNK,  MSM_G_APPLY,  0, 0 },  /* poller: event transition     */
    { 0x4CFD27, MSM_ADDR_APPLY_THUNK,  MSM_G_APPLY,  0, 0 },  /* poller: delayed transition   */
    { 0x4D05CB, MSM_ADDR_APPLY_THUNK,  MSM_G_APPLY,  0, 0 },  /* temp smo, target 0x517FB5D1  */
    { 0x4D1E45, MSM_ADDR_APPLY_THUNK,  MSM_G_APPLY,  0, 0 },  /* SetState action 0x9B31491A   */
    { 0x4D1E56, MSM_ADDR_APPLY_THUNK,  MSM_G_APPLY,  0, 0 },  /* SetState action              */
    { 0x5D2DB7, MSM_ADDR_APPLY_THUNK,  MSM_G_APPLY,  0, 0 },  /* script binding               */
    { 0x4D424D, MSM_ADDR_EXEC,         MSM_G_EXEC,   0, 0 },  /* inner apply: exit actions    */
    { 0x4D4270, MSM_ADDR_EXEC,         MSM_G_EXEC,   0, 0 },  /* inner apply: enter actions   */
};
#define MSM_NSITES ((int)(sizeof(s_sites) / sizeof(s_sites[0])))

void MsmInit(void)
{
    g_msmPfnLookupOrig = (void *)A11(MSM_ADDR_LOOKUP_THUNK);
    g_msmPfnApplyOrig  = (void *)A11(MSM_ADDR_APPLY_THUNK);
    g_msmPfnInnerApply = (void *)A11(MSM_ADDR_INNER_APPLY);
    g_msmPfnExecOrig   = (void *)A11(MSM_ADDR_EXEC);
    g_msmPfnCompLookup = (void *)A11(MSM_ADDR_COMP_LOOKUP);
}

static int msm_WriteRel(DWORD site, DWORD newRel)
{
    DWORD old, tmp;
    unsigned char *p = (unsigned char *)site;
    if (!VirtualProtect(p, 5, 0x40 /*PAGE_EXECUTE_READWRITE*/, &old)) return 0;
    *(volatile DWORD *)(p + 1) = newRel;
    VirtualProtect(p, 5, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

/* groups: MSM_G_LOOKUP | MSM_G_APPLY | MSM_G_EXEC. Returns number of sites patched, or -1 if
   any selected site does not contain the expected `E8 rel32 -> orig` (nothing is patched). */
int MsmInstall(int groups)
{
    int i, n = 0;
    MsmInit();
    for (i = 0; i < MSM_NSITES; ++i) {
        MsmSite *s = &s_sites[i];
        unsigned char *p = (unsigned char *)A11(s->site);
        if (!(groups & s->group) || s->patched) continue;
        if (p[0] != 0xE8 || (DWORD)(*(DWORD *)(p + 1) + (DWORD)p + 5) != (DWORD)A11(s->orig)) {
            Log("MSM install: unexpected bytes at %p (%02X %08lX) - aborting\n",
                (void *)p, p[0], *(DWORD *)(p + 1));
            return -1;
        }
    }
    for (i = 0; i < MSM_NSITES; ++i) {
        MsmSite *s = &s_sites[i];
        unsigned char *p = (unsigned char *)A11(s->site);
        void *tgt;
        if (!(groups & s->group) || s->patched) continue;
        tgt = s->group == MSM_G_LOOKUP ? (void *)msm_LookupEntry
            : s->group == MSM_G_APPLY  ? (void *)msm_ApplyEntry : (void *)msm_ExecEntry;
        s->saved = *(DWORD *)(p + 1);
        if (msm_WriteRel((DWORD)p, (DWORD)tgt - ((DWORD)p + 5))) { s->patched = 1; ++n; }
    }
    Log("MSM installed %d call sites (groups %d)\n", n, groups);
    return n;
}

void MsmUninstall(void)
{
    int i;
    for (i = 0; i < MSM_NSITES; ++i) {
        MsmSite *s = &s_sites[i];
        if (!s->patched) continue;
        if (msm_WriteRel((DWORD)A11(s->site), s->saved)) s->patched = 0;
    }
}

void MsmLogStats(void)
{
    Log("MSM stats: lookups=%lu hits=%lu mismatches=%lu applies=%lu noLayer=%lu same=%lu "
        "actions=%lu unknown=%lu nullHandler=%lu stackOverflow=%lu\n",
        g_msmStats.lookups, g_msmStats.lookupHits, g_msmStats.mismatches, g_msmStats.applies,
        g_msmStats.applyNoLayer, g_msmStats.applySame, g_msmStats.actions,
        g_msmStats.actionUnknown, g_msmStats.actionNullHandler, g_msmStats.stackOverflow);
}
#endif /* M2R_GAME11 */
