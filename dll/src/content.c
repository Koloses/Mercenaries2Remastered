/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Ported console content: patch-wad redirect + Lua bindings that exist only in the Xbox 360 TU4 build.
 *
 * The PC loader already mounts "%s\%s-patch.wad" and "%s\loading-patch.wad" from the patch directory
 * (global 0xED1F10, normally the data folder). We point those three sprintf sites at our content folder,
 * so converted vz-patch.wad / shell-patch.wad are mounted without touching the game's files.
 */
#include "m2r.h"
#include "d3dhook.h"
#include <stdio.h>
#include <string.h>

static char g_contentDir[MAX_PATH];
static int g_crashArmed;
const char *ContentDir(void) { return g_contentDir; }

/* ---- Lua 5.1 (float numbers) stack access, PC build layout ---- */
typedef struct { DWORD v; DWORD tt; } TV;
#define LTOP(L)  (*(TV **)((BYTE *)(L) + 8))
#define LBASE(L) (*(TV **)((BYTE *)(L) + 0xC))
enum { T_NIL = 0, T_BOOL = 1, T_LUD = 2, T_NUM = 3, T_STR = 4 };
static int  NArgs(void *L) { return (int)(LTOP(L) - LBASE(L)); }
static TV  *Arg(void *L, int i) { return i < NArgs(L) ? LBASE(L) + i : NULL; }
static void Push(void *L, DWORD v, DWORD tt) { TV *t = LTOP(L); t->v = v; t->tt = tt; LTOP(L) = t + 1; }
static void PushBool(void *L, int b) { Push(L, b ? 1 : 0, T_BOOL); }
static void PushNum(void *L, float f) { DWORD d; memcpy(&d, &f, 4); Push(L, d, T_NUM); }
static void PushNil(void *L) { Push(L, 0, T_NIL); }
static int  ToBool(TV *v) { return v && !(v->tt == T_NIL || (v->tt == T_BOOL && v->v == 0)); }
static float ToNum(TV *v, float def) { float f; if (!v || v->tt != T_NUM) return def; memcpy(&f, &v->v, 4); return f; }
/* lua_pushstring at 0x85D9F0: esi = L, [esp+4] = string */
static void __attribute__((noinline)) PushStr(void *L, const char *s)
{
    __asm__ volatile("push %%esi\n\tmov %0, %%esi\n\tpush %1\n\tmov $0x85D9F0, %%eax\n\tcall *%%eax\n\tadd $4, %%esp\n\tpop %%esi"
                     :: "r"(L), "r"(s) : "eax", "ecx", "edx", "memory");
}
/* component table lookup 0x5857E0: ecx = table, eax = guid */
static void *CompLookup(DWORD table, DWORD guid)
{
    void *r;
    __asm__ volatile("mov $0x5857E0, %%edx\n\tcall *%%edx" : "=a"(r) : "c"(table), "a"(guid) : "edx", "memory");
    return r;
}

/* ---- the missing bindings ---- */
static int g_cheatsUsed;
static float g_mapCorr[3];
static int __cdecl L_WereCheatsUsed(void *L) { PushBool(L, g_cheatsUsed); return 1; }
static int __cdecl L_SetCheatsUsed(void *L) { g_cheatsUsed = ToBool(Arg(L, 0)); Log("Lua: SetCheatsUsed(%d)", g_cheatsUsed); return 0; }
static int __cdecl L_GetInfiniteAmmo(void *L)
{
    TV *a = Arg(L, 0);
    if (!a || a->tt != T_LUD) { PushBool(L, 0); return 1; }
    PushBool(L, CompLookup(0xDF9B10, a->v) != NULL); return 1;
}
static const char *TStr(TV *v);
static int __cdecl L_Nop(void *L) { (void)L; return 0; }
/* engine name hash (asset / record names) */
static DWORD EngHash(const char *s)
{
    DWORD h = 0x811C9DC5;
    for (; *s; s++) h = (h ^ ((BYTE)*s | 0x20)) * 0x1000193;
    return (h ^ 0x2A) * 0x1000193;
}
/* asset request (as Sys.AddStringDb does): 0x872F40 stdcall(asset manager, {hash, type}, 3) */
__attribute__((used)) DWORD g_reqAsset[2], g_reqMgr;
static void RequestAsset(DWORD hash, DWORD type)
{
    g_reqMgr = *(DWORD *)0x1176630; if (!g_reqMgr) return;
    g_reqAsset[0] = hash; g_reqAsset[1] = type;
    /* globals only: after pushal an esp-relative operand would point elsewhere */
    __asm__ volatile("pushal\n\tpushl $3\n\tpushl $_g_reqAsset\n\tpushl _g_reqMgr\n\tmov $0x872F40, %%eax\n\tcall *%%eax\n\tpopal" ::: "memory");
}
/* Human.SetChatterSet(human, "Obama_Obama01") - Xbox TU4 binding (0x82324F38): sets the object's entry in the
 * ChatterSet component table (PC 0x17BD498, read by the chatter player 0x4F07E0) to the chatter set's name hash.
 * The DLC outfits use it so the player speaks with the Obama / Palin voice sets. */
static int __cdecl L_SetChatterSet(void *L)
{
    TV *o = Arg(L, 0); const char *n = TStr(Arg(L, 1));
    if (!o || o->tt != T_LUD || !n || !n[0]) return 0;
    DWORD h = EngHash(n);
    RequestAsset(h, 0xFA0B8DBC);
    DWORD *p = (DWORD *)CompLookup(0x17BD498, o->v);
    if (p) { Log("Human.SetChatterSet(%08lX, %s): %08lX -> %08lX", o->v, n, *p, h); *p = h; }
    else Log("Human.SetChatterSet(%08lX, %s): object has no ChatterSet entry", o->v, n);
    return 0;
}
static int __cdecl L_SetMapCorrectionOffset(void *L)
{
    for (int i = 0; i < 3; i++) g_mapCorr[i] = ToNum(Arg(L, i), 0.f);
    return 0;
}
static int __cdecl L_GetMapCorrectionOffset(void *L)
{
    for (int i = 0; i < 3; i++) PushNum(L, g_mapCorr[i]);
    return 3;
}
static int __cdecl L_LeaderboardPushScore(void *L) { (void)L; return 0; }
static int __cdecl L_LeaderboardGetScore(void *L) { PushNil(L); return 1; }
static int __cdecl L_LeaderboardGetFriends(void *L) { PushNil(L); return 1; }
static char g_dlcMount[MAX_PATH];
void ContentSetDlcMount(const char *p) { lstrcpynA(g_dlcMount, p ? p : "", sizeof g_dlcMount); }
static int __cdecl L_DLCGetMountPoint(void *L) { PushStr(L, g_dlcMount); return 1; }
static int __cdecl L_GetUserName(void *L)
{
    static char name[64];
    const char *h = (const char *)0x17C0BCC;
    if (h[0]) lstrcpynA(name, h, sizeof name);
    else { DWORD n = sizeof name; if (!GetUserNameA(name, &n)) lstrcpyA(name, "Mercenary"); }
    PushStr(L, name); return 1;
}
typedef int (__cdecl *lfn)(void *);
static lfn g_markerAddDisc;
static int __cdecl L_MarkerAddDiscDLC(void *L) { return g_markerAddDisc ? g_markerAddDisc(L) : 0; }

/* ---- Lua diagnostics: Debug.Printf & runtime errors into Mercenaries2Remastered.log ---- */
static const char *TStr(TV *v) { return (v && v->tt == T_STR) ? (const char *)(v->v + 16) : NULL; }
static int __cdecl L_Printf(void *L)
{
    char buf[1024]; int n = 0; buf[0] = 0;
    for (int i = 0; i < NArgs(L) && n < (int)sizeof buf - 64; i++) {
        TV *a = Arg(L, i); const char *s = TStr(a);
        if (s) n += _snprintf(buf + n, sizeof buf - n - 1, "%s", s);
        else if (a->tt == T_NUM) n += _snprintf(buf + n, sizeof buf - n - 1, "%g", ToNum(a, 0));
        else n += _snprintf(buf + n, sizeof buf - n - 1, "<%u>", (unsigned)a->tt);
    }
    buf[sizeof buf - 1] = 0;
    Log("lua: %s", buf);
    return 0;
}
__attribute__((used)) void __cdecl LuaErrLog(void *L)
{
    TV *t = LTOP(L) - 1; const char *s = TStr(t);
    Log("LUA ERROR: %s", s ? s : "(non-string error)");
}
__attribute__((naked)) static void ErrMsgStub(void)
{
    __asm__ volatile(
        "pushal\n\tpush %eax\n\tcall _LuaErrLog\n\tadd $4, %esp\n\tpopal\n\t"
        "push %esi\n\tmov %eax, %esi\n\tmov 0x60(%esi), %eax\n\t"
        "push $0x85EEA6\n\tret");
}

static int  g_dlcOn;        /* DLC package installed/enabled: offered in Extras > Downloadable Content */
static int  g_dlcActive;    /* the current session was started from the DLC menu */
static int  g_dlcPending;
static int  g_dlcFlow;   /* DLC selected in the shell, waiting for the level start */
static const char g_dlcScript[] = "DLC01", g_dlcLevel[] = "DLC01";
/* engine globals: 0x1175AB8 level name, 0x1175B78 master script, 0x1175BB8 saved master (all char[64]) */
static void SetLevelGlobals(const char *lvl, const char *master)
{
    lstrcpynA((char *)0x1175AB8, lvl, 64); lstrcpynA((char *)0x1175B78, master, 64); lstrcpynA((char *)0x1175BB8, master, 64);
}
static int __cdecl L_GetMasterScriptName(void *L)
{
    const char *m = (const char *)0x1175B78;
    if (g_dlcActive && !lstrcmpiA(m, "vz")) {
        /* the engine later starts LevelBootstrap.LoadLevel(level, <stored master script>) */
        lstrcpynA((char *)0x1175B78, g_dlcScript, 64); lstrcpynA((char *)0x1175BB8, g_dlcScript, 64);   /* current + saved master script (0x1175AB8 is the level name) */
        Log("DLC: master script set to %s", g_dlcScript);
        PushStr(L, g_dlcScript); return 1;
    }
    PushStr(L, m); return 1;
}
static lfn g_origSetMaster;
static int __cdecl L_SetMasterScriptName(void *L)
{
    int r = g_origSetMaster ? g_origSetMaster(L) : 0;
    char *m = (char *)0x1175B78;
    if (g_dlcActive && !lstrcmpiA(m, "vz")) { lstrcpynA(m, g_dlcScript, 64); Log("DLC: SetMasterScriptName(vz) -> %s", g_dlcScript); }
    return r;
}
static lfn g_origRequiredAsset;
static int __cdecl L_RequiredAsset(void *L)
{
    /* LevelBootstrap.LoadLevel: Sys.RequiredAsset(master, "script") -> the DLC master script in a DLC session */
    const char *a = TStr(Arg(L, 0)), *b = TStr(Arg(L, 1));
    if (g_dlcActive && a && b && !lstrcmpiA(a, "vz") && !lstrcmpiA(b, "script")) {
        PushStr(L, g_dlcScript);
        *LBASE(L) = *(LTOP(L) - 1); LTOP(L) -= 1;
        Log("DLC: RequiredAsset(vz, script) -> %s", g_dlcScript);
    }
    return g_origRequiredAsset ? g_origRequiredAsset(L) : 0;
}
/* engine bug (0x4644B0, AddStringDb loader): the new table's hash is stored at slot [count] instead of
 * [index] and the slot of the table is cleared, so of all added string tables only the last one is ever
 * searched. With patch01 + dlc01 one of them was lost. Store it at its own index. */
__attribute__((used)) DWORD g_strDbIdx;
__attribute__((naked)) static void StrDbStub(void)
{
    __asm__ volatile("mov %ecx, _g_strDbIdx\n\tsub $0x88, %esp\n\tpush $0x4644B6\n\tret");
}
static void FixStringDbs(void)
{
    if (Expect(0x4644B0, "81 EC 88 00 00 00 83 F9 07") && Expect(0x464504, "8B 0D B8 59 17 01")) {
        WriteJmp(0x4644B0, StrDbStub, 6);
        WritePtr(0x464506, &g_strDbIdx);          /* mov ecx, [count] -> mov ecx, [index] */
        Log("Content: string table slot fix on");
    } else Log("Content: string table code mismatch, fix skipped");
}
/* campaign: also load the DLC string tables (costume names [SHELL.Costumes.*]) next to TU4's patch01 */
static lfn g_origAddStringDb;
static int __cdecl L_AddStringDb(void *L)
{
    const char *a = TStr(Arg(L, 0));
    if (g_origAddStringDb && a && !lstrcmpiA(a, "patch01") && !g_dlcActive && g_dlcOn) {
        /* engine loader (Sys.AddStringDb -> 0x464540, cdecl, copies the name) */
        ((void (__cdecl *)(const char *))0x464540)("dlc01");
        Log("DLC: string table dlc01 added for the campaign (costume names)");
    }
    return g_origAddStringDb ? g_origAddStringDb(L) : 0;
}
/* campaign: when the game opens its VO stream (vo_stream.<lang>.pws), also open the DLC VO stream so the
 * Obama / Palin costume chatter (vo_stream_DLCTest bank) can play outside DLC sessions */
static lfn g_origOpenStream, g_origCloseStream;
/* engine stream calls behind Sound.OpenStreamFile / CloseStreamFile (0x5E4020 / 0x5E40D0):
 *   0x606B40  edi = sound system, stdcall(path, alias)      0x606C00  edx = alias, stdcall(sound system) */
__attribute__((used)) const char *g_sPath, *g_sAlias; __attribute__((used)) DWORD g_sObj;
static void SndOpen(const char *path, const char *alias)
{
    if (!*(BYTE *)0xDF67F4 || !*(DWORD *)0x1175F7C) return;
    g_sObj = *(DWORD *)(*(DWORD *)0x1175F7C + 0x40); g_sPath = path; g_sAlias = alias;
    __asm__ volatile("pushal\n\tmov _g_sObj, %%edi\n\tpushl _g_sAlias\n\tpushl _g_sPath\n\tmov $0x606B40, %%eax\n\tcall *%%eax\n\tpopal" ::: "memory");
}
static void SndClose(const char *alias)
{
    if (!*(BYTE *)0xDF67F4 || !*(DWORD *)0x1175F7C) return;
    g_sObj = *(DWORD *)(*(DWORD *)0x1175F7C + 0x40); g_sAlias = alias;
    __asm__ volatile("pushal\n\tmov _g_sAlias, %%edx\n\tpushl _g_sObj\n\tmov $0x606C00, %%eax\n\tcall *%%eax\n\tpopal" ::: "memory");
}
static int g_voOpen;
static int __cdecl L_OpenStreamFile(void *L)
{
    const char *path = TStr(Arg(L, 0)), *alias = TStr(Arg(L, 1));
    char p2[MAX_PATH] = "";
    if (!g_dlcActive && g_dlcOn && path && alias && !lstrcmpiA(alias, "vo_stream.pws")) {
        const char *l = strstr(path, "vo_stream."); if (!l) l = strstr(path, "VO_STREAM.");
        if (l) {
            char lang[32]; int n = 0; l += 10;
            while (l[n] && l[n] != '.' && n < 31) { lang[n] = l[n]; n++; }
            lang[n] = 0;
            _snprintf(p2, sizeof p2, "%s\\Data\\Audios\\vo_stream_dlctest.%s.pws", g_contentDir, lang); p2[MAX_PATH - 1] = 0;
            if (GetFileAttributesA(p2) == INVALID_FILE_ATTRIBUTES) p2[0] = 0;
        }
    }
    int r = g_origOpenStream ? g_origOpenStream(L) : 0;
    if (p2[0]) {
        if (g_voOpen) SndClose("vo_stream_DLCTest.pws");
        SndOpen(p2, "vo_stream_DLCTest.pws"); g_voOpen = 1;
        Log("DLC: opened %s for the campaign", p2);
    }
    return r;
}
static int __cdecl L_CloseStreamFile(void *L)
{
    const char *alias = TStr(Arg(L, 0));
    if (g_voOpen && alias && !lstrcmpiA(alias, "vo_stream.pws")) { SndClose("vo_stream_DLCTest.pws"); g_voOpen = 0; Log("DLC: closed vo_stream_DLCTest.pws"); }
    return g_origCloseStream ? g_origCloseStream(L) : 0;
}
/* ---- extend the engine's static binding tables ---- */
typedef struct { const char *name; void *fn; } LReg;
typedef struct { DWORD slot; DWORD table; const LReg *add; } TableExt;
static const LReg addSys[]  = {{"WereCheatsUsed", L_WereCheatsUsed}, {"SetCheatsUsed", L_SetCheatsUsed}, {0, 0}};
static const LReg addObj[]  = {{"GetInfiniteAmmo", L_GetInfiniteAmmo}, {0, 0}};
static const LReg addNet[]  = {{"LeaderboardPushScore", L_LeaderboardPushScore}, {"LeaderboardGetScore", L_LeaderboardGetScore},
                               {"LeaderboardGetFriends", L_LeaderboardGetFriends}, {"DLCGetMountPoint", L_DLCGetMountPoint},
                               {"GetUserName", L_GetUserName}, {0, 0}};
static const LReg addDbg[]  = {{"Printf", L_Printf}, {"LogError", L_Printf}, {"LogWarning", L_Printf}, {"LogInfo", L_Printf}, {0, 0}};
static const LReg addSysO[] = {{"GetMasterScriptName", L_GetMasterScriptName}, {"SetMasterScriptName", L_SetMasterScriptName}, {"RequiredAsset", L_RequiredAsset}, {"AddStringDb", L_AddStringDb}, {0, 0}};
static const LReg addSnd[]  = {{"OpenStreamFile", L_OpenStreamFile}, {"CloseStreamFile", L_CloseStreamFile}, {0, 0}};
static const LReg addAi[]   = {{"SetChatterSet", L_SetChatterSet}, {0, 0}};
static const LReg addHud[]  = {{"_MarkerAddDiscDLC", L_MarkerAddDiscDLC}, {"SetMapCorrectionOffset", L_SetMapCorrectionOffset},
                               {"GetMapCorrectionOffset", L_GetMapCorrectionOffset}, {"ReleaseLeftStickControl", L_Nop}, {0, 0}};
static const TableExt g_ext[] = {
    {0xDFD48C + 8, 0xB99328, addSys},   /* LoadingStaticLayers ... WereCheatsUsed/SetCheatsUsed */
    {0xDFD498 + 8, 0xB99608, addObj},   /* GetParent ... GetInfiniteAmmo */
    {0xDFD534 + 8, 0xB998D0, addNet},   /* IsPlatformConnected ... Net bindings */
    {0xDFD4C8 + 8, 0xB99EF0, addAi},    /* DoAction ... SetChatterSet */
    {0xDFD4F8 + 8, 0xB9A398, addHud},   /* AddObjective ... markers */
    {0xDFD4D4 + 8, 0xB98828, addDbg},   /* Debug.* (stubs on PC) */
    {0xDFD480 + 8, 0xB98A78, addSysO},  /* Sys.GetMasterScriptName -> DLC master script */
    {0xDFD51C + 8, 0xB98C98, addSnd},   /* Sound.OpenStreamFile: DLC VO stream in the campaign */
};
static void ExtendTables(void)
{
    for (unsigned i = 0; i < sizeof g_ext / sizeof g_ext[0]; i++) {
        const TableExt *e = &g_ext[i];
        if (*(DWORD *)e->slot != e->table) { Log("Content: binding table slot %08X unexpected (%08X)", e->slot, *(DWORD *)e->slot); continue; }
        const LReg *orig = (const LReg *)e->table;
        int n = 0, m = 0;
        while (orig[n].name) n++;
        while (e->add[m].name) m++;
        LReg *nt = (LReg *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (n + m + 1) * sizeof(LReg));
        memcpy(nt, orig, n * sizeof(LReg));
        int k = n;
        for (int j = 0; j < m; j++) {
            int found = 0;
            for (int q = 0; q < n; q++) if (!strcmp(nt[q].name, e->add[j].name)) { if (e->add == addDbg || e->add == addSysO || e->add == addSnd) nt[q].fn = e->add[j].fn; found = 1; }
            if (!found) nt[k++] = e->add[j];
        }
        for (int j = 0; j < n; j++) if (!strcmp(orig[j].name, "_MarkerAddDisc")) g_markerAddDisc = (lfn)orig[j].fn;
        for (int j = 0; j < n; j++) if (!strcmp(orig[j].name, "SetMasterScriptName")) g_origSetMaster = (lfn)orig[j].fn;
        for (int j = 0; j < n; j++) if (!strcmp(orig[j].name, "RequiredAsset")) g_origRequiredAsset = (lfn)orig[j].fn;
        for (int j = 0; j < n; j++) if (!strcmp(orig[j].name, "AddStringDb")) g_origAddStringDb = (lfn)orig[j].fn;
        for (int j = 0; j < n; j++) if (!strcmp(orig[j].name, "OpenStreamFile")) g_origOpenStream = (lfn)orig[j].fn;
        for (int j = 0; j < n; j++) if (!strcmp(orig[j].name, "CloseStreamFile")) g_origCloseStream = (lfn)orig[j].fn;
        WritePtr(e->slot, nt);
        Log("Content: binding table %08X: %d + %d functions", e->table, n, k - n);
    }
}

/* ---- DLC: mount an extra level package through the loader's unused custom-wad slot ----
 * FUN_004BFC70 (resource state 8) loads "<obj+0x283C>" into slot +0xB78 when that string is set.
 * Nothing in the PC build writes it; we fill it while the "vz" level loads and DLC is enabled. */
static char g_dlcWad[MAX_PATH];
__attribute__((used)) void __cdecl DlcPrepare(BYTE *obj)
{
    const char *lvl = (const char *)(obj + 0x27FC);
    char *slot = (char *)(obj + 0x283C);
    if (g_dlcActive && !lstrcmpiA(lvl, "vz")) {
        int was = slot[0] != 0;
        lstrcpynA(slot, g_dlcWad, 256);
        g_crashArmed = 1;
        if (!was) Log("DLC: mounting %s for level %s, master script %s", g_dlcWad, lvl, g_dlcScript);
    } else slot[0] = 0;
}
__attribute__((naked)) static void DlcStub(void)
{
    __asm__ volatile(
        "pushal\n\tpush %esi\n\tcall _DlcPrepare\n\tadd $4, %esp\n\tpopal\n\t"
        "cmpb $0, 0x1175BFC\n\t"
        "push $0x4BFC77\n\tret");
}
/* 0x6CF640: start-level handler, arg = { char level[64]; char master[64]; } -> LevelBootstrap.LoadLevel(level, master) */
__attribute__((used)) void __cdecl DlcFixStart(char *st)
{
    if (!st) return;
    if ((g_dlcPending || g_dlcActive) && (!lstrcmpiA(st, "vz") || !lstrcmpiA(st, g_dlcLevel))) {
        /* Xbox: the DLC is its own level ("DLC01", blocks\dlc01\...) with its own master script */
        g_dlcPending = 0; g_dlcActive = 1;
        Log("DLC: level start %s/%s -> %s/%s (DLC session)", st, st + 0x40, g_dlcLevel, g_dlcScript);
        lstrcpynA(st, g_dlcLevel, 64); lstrcpynA(st + 0x40, g_dlcScript, 64);
    } else {
        if (g_dlcActive) Log("DLC: level start %s/%s, leaving DLC session", st, st + 0x40);
        g_dlcActive = 0; g_dlcPending = 0;
    }
}
__attribute__((naked)) static void StartStub(void)
{
    __asm__ volatile(
        "pushal\n\tpushl 36(%esp)\n\tcall _DlcFixStart\n\tadd $4, %esp\n\tpopal\n\t"
        "push %esi\n\tmov 8(%esp), %esi\n\t"
        "push $0x6CF645\n\tret");
}
/* ---- shell (Flash) side, as on Xbox 360 (FUN_8244C5B8 / FUN_8244BE00):
 * the PC shell movie still has the whole DLC flow (Extras > DLC list > details > character > _DLCStart),
 * the engine part was cut. We answer its FSCommands and feed it the package entry. */
static DWORD o_inv; static volatile LONG g_inInv; DWORD g_invName;
typedef struct { DWORD type, pad; union { const char *s; double d; BYTE b; } v; } GVal;   /* GFxValue, 16 bytes */
enum { GV_BOOL = 2, GV_NUM = 3, GV_STR = 4 };
/* 0x61C550: esi = movie wrapper, edi = method name, stdcall(args, nargs, result) -> al ok (takes the GFx lock) */
static int GfxInvoke(const char *name, GVal *args, int n)
{
    DWORD mgr = *(DWORD *)0x1176054; if (!mgr) return 0;
    DWORD w = *(DWORD *)(mgr + 0xA884); if (!w || !*(DWORD *)(w + 0x1E0)) return 0;
    int r; LONG prev = g_inInv; g_inInv = 1;
    static DWORD a_w, a_name, a_args, a_n;
    a_w = w; a_name = (DWORD)name; a_args = (DWORD)args; a_n = (DWORD)n;
    __asm__ volatile("push %%esi\n\tpush %%edi\n\t"
                     "push $0\n\tpushl %4\n\tpushl %3\n\tmov %1, %%esi\n\tmov %2, %%edi\n\t"
                     "mov $0x61C550, %%eax\n\tcall *%%eax\n\t"
                     "movzbl %%al, %0\n\tpop %%edi\n\tpop %%esi"
                     : "=a"(r) : "m"(a_w), "m"(a_name), "m"(a_args), "m"(a_n) : "ecx", "edx", "memory");
    g_inInv = prev;
    return r;
}
static char g_dlcName[] = "Blow It Up Again Pack", g_dlcDesc[] = "Blow it up again with four new explosive missions!";
static void DlcAnnounce(void)
{
    static DWORD last;
    DWORD t = GetTickCount();
    if (!g_dlcOn || t - last < 500) return;
    g_crashArmed = 1;
    last = t;
    GVal v = {GV_BOOL, 0, {0}}; v.v.b = 1;
    int ok = GfxInvoke("DLCAvailable", &v, 1);
    static int n; if (!n++) Log("DLC: DLCAvailable(true) -> %d", ok);
}
static void DlcList(void)
{
    GVal a[9]; memset(a, 0, sizeof a);
    static const char *const e = "";
    for (int i = 0; i < 8; i++) { a[i].type = GV_STR; a[i].v.s = e; }
    a[0].v.s = "0";          /* internalName = package index, sent back by onlineSelectDLC */
    a[4].v.s = g_dlcName;    /* mapName */
    a[7].v.s = g_dlcDesc;    /* missionDetails */
    a[8].type = GV_BOOL; a[8].v.b = 1;   /* isGood */
    int ok = GfxInvoke("addDLC", a, 9);
    Log("DLC: shell list -> addDLC(%s) %s", g_dlcName, ok ? "ok" : "failed");
}
typedef void (__fastcall *FsCmd_t)(void *self, void *edx, void *movie, const char *cmd, const char *arg);
static FsCmd_t o_FsCmd;
static void __fastcall hk_FsCmd(void *self, void *edx, void *movie, const char *cmd, const char *arg)
{
    int fr = -1;
    if (movie) { void **vt = *(void ***)movie; typedef int (__thiscall *gcf_t)(void *); fr = ((gcf_t)vt[2])(movie); }
    { static int n; if (n < 300 && cmd && strncmp(cmd, "Sound.", 6)) { n++; Log("DLC: fscommand %.60s(%.60s) frame %d", cmd ? cmd : "", arg ? arg : "", fr); } }
    /* PC shell: the mouse path of the DLC character screen (frames _DLCcharacterTrans.._DLCStart) falls into the
     * normal new-game flow; remember we came from there */
    if (fr >= 2253 && fr < 2357) g_dlcFlow = 1;
    else if (fr >= 874 && fr < 964) g_dlcFlow = 0;
    if (cmd && g_dlcOn) {
        if (!lstrcmpiA(cmd, "onlineListDLCMaps")) { DlcList(); return; }
        if (!lstrcmpiA(cmd, "onlineSelectDLC")) {
            int idx = arg ? atoi(arg) : -1;
            Log("DLC: shell selected package %d -> new game, master script %s", idx, g_dlcScript);
            /* same path the shell's own new game takes: save check -> saveCompleteContinue -> newGame */
            if (idx == 0) { g_dlcFlow = 1; o_FsCmd(self, edx, movie, "preoperation", "save,newGame"); }
            return;
        }
        if (!lstrcmpiA(cmd, "onlineDownloadDLC")) return;
        if (!lstrcmpiA(cmd, "newGame")) {
            if (g_dlcFlow) {
                g_dlcFlow = 0; g_dlcPending = 1; g_dlcActive = 1;
                /* Xbox: world package vz + the DLC data file (custom package slot), then
                 * LevelBootstrap.LoadLevel(DLC01, DLC01) -> layer DLC01_base + master script DLC01 */
                if (!lstrcmpiA((char *)0x1175AB8, g_dlcLevel)) SetLevelGlobals("vz", "vz");
                Log("DLC: new game from the DLC character screen -> vz + %s, level %s, master script %s", g_dlcWad, g_dlcLevel, g_dlcScript);
            } else {
                if (g_dlcActive || !lstrcmpiA((char *)0x1175AB8, g_dlcLevel)) Log("DLC: normal new game, DLC session off");
                if (!lstrcmpiA((char *)0x1175AB8, g_dlcLevel)) SetLevelGlobals("vz", "vz");
                g_dlcActive = 0; g_dlcPending = 0;
            }
        }
        if (!lstrcmpiA(cmd, "LoadSaveGame") || !lstrcmpiA(cmd, "joinGame")) {
            if (!lstrcmpiA((char *)0x1175AB8, g_dlcLevel)) SetLevelGlobals("vz", "vz");
            g_dlcActive = 0; g_dlcPending = 0; g_dlcFlow = 0;
        }
    }
    o_FsCmd(self, edx, movie, cmd, arg);
    if (g_dlcOn) DlcAnnounce();
}
/* engine -> Flash invokes go through 0x61C550 (jmp [0x2455570]); after each one on the shell movie
 * (same thread and lock state the engine uses) we re-assert DLCAvailable(true) */
__attribute__((used)) void __cdecl DlcAfterInvoke(const char *name)
{
    static int n; if (n < 200 && name) { n++; Log("DLC: engine invoke %.60s", name); }
    DlcAnnounce();
}
__attribute__((naked)) static void InvStub(void)
{
    __asm__ volatile(
        "cmpl $0, _g_inInv\n\tjne 1f\n\t"
        "movl $1, _g_inInv\n\tmovl %edi, _g_invName\n\t"
        "pushl 12(%esp)\n\tpushl 12(%esp)\n\tpushl 12(%esp)\n\t"
        "call *_o_inv\n\t"
        "push %eax\n\tpushal\n\tpushl _g_invName\n\tcall _DlcAfterInvoke\n\tadd $4, %esp\n\tpopal\n\tpop %eax\n\t"
        "movl $0, _g_inInv\n\tret $12\n"
        "1:\tjmp *_o_inv");
}
static void DlcInit(void)
{
    _snprintf(g_dlcWad, sizeof g_dlcWad, "%s\\DLC01.wad", g_contentDir);
    g_dlcOn = GetFileAttributesA(g_dlcWad) != INVALID_FILE_ATTRIBUTES;
    lstrcpynA(g_dlcMount, g_contentDir, sizeof g_dlcMount);   /* scripts append "\\" + audio dir */
    if (!g_dlcOn) Log("DLC: %s not found, menu entry off", g_dlcWad);
    if (Expect(0x6CF640, "56 8B 74 24 08")) WriteJmp(0x6CF640, StartStub, 5);
    if (g_dlcOn && *(DWORD *)0xBBC0EC == 0x60DE80) {   /* shell FSCommand handler (vtable slot) */
        o_FsCmd = (FsCmd_t)0x60DE80; WritePtr(0xBBC0EC, (void *)hk_FsCmd);
        if (*(WORD *)0x61C550 == 0x25FF && *(DWORD *)0x61C552 == 0x2455570) { o_inv = *(DWORD *)0x2455570; WriteJmp(0x61C550, InvStub, 6); }
        Log("DLC: shell hooks on (invoke %08lX)", o_inv);
    }
    if (Expect(0x4BFC70, "80 3D FC 5B 17 01 00")) { WriteJmp(0x4BFC70, DlcStub, 7); Log("DLC: %s (%s, script %s)", g_dlcOn ? "on" : "off", g_dlcWad, g_dlcScript); }
}

/* ---- crash logger: first-chance access violations / illegal instructions (first 8) ---- */
static volatile LONG g_crashN;
static LONG CALLBACK CrashVeh(PEXCEPTION_POINTERS ep)
{
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c != EXCEPTION_ACCESS_VIOLATION && c != EXCEPTION_ILLEGAL_INSTRUCTION && c != EXCEPTION_INT_DIVIDE_BY_ZERO && c != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!g_crashArmed || InterlockedIncrement(&g_crashN) > 8) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *x = ep->ContextRecord;
    Log("EXCEPTION %08lX at %08lX (addr %08lX rw %lu) eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX",
        c, x->Eip, (DWORD)ep->ExceptionRecord->ExceptionInformation[1], (DWORD)ep->ExceptionRecord->ExceptionInformation[0],
        x->Eax, x->Ebx, x->Ecx, x->Edx, x->Esi, x->Edi, x->Ebp, x->Esp);
    char buf[512]; int n = 0; DWORD *sp = (DWORD *)x->Esp;
    for (int i = 0; i < 256 && n < 480; i++) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(sp + i, &mi, sizeof mi) || mi.State != MEM_COMMIT) break;
        DWORD v = sp[i];
        if (v >= 0x401000 && v < 0x1200000) n += _snprintf(buf + n, sizeof buf - n, " %08lX", v);
    }
    buf[n] = 0; Log("  stack:%s", buf);
    if (g_crashN == 1) {
        DWORD regs[4] = {x->Ebx, x->Ebp, x->Esi, x->Edi};
        for (int i = 0; i < 64 + 4; i++) {
            DWORD v = i < 64 ? sp[i] : regs[i - 64];
            MEMORY_BASIC_INFORMATION mi; char h[200]; int k = 0;
            MEMORY_BASIC_INFORMATION m2;
            if (v < 0x10000 || !VirtualQuery((void *)v, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !VirtualQuery((void *)(v + 47), &m2, sizeof m2) || m2.State != MEM_COMMIT || (m2.Protect & (PAGE_NOACCESS | PAGE_GUARD))) { Log("  [%02d] %08lX", i, v); continue; }
            for (int j = 0; j < 48; j++) k += _snprintf(h + k, sizeof h - k, "%02x", ((BYTE *)v)[j]);
            Log("  [%02d] %08lX -> %s", i, v, h);
        }
    }
    LogFlush();
    return EXCEPTION_CONTINUE_SEARCH;
}

void ContentInit(void)
{
    _snprintf(g_contentDir, sizeof g_contentDir, "%s\\Mercenaries2Remastered\\content", g_gameDir);
    int n = 0;
    static const DWORD sites[] = {0x4BFDB3, 0x4BFF0B, 0x4C004C};
    for (unsigned i = 0; i < 3; i++)
        if (Expect(sites[i], "68 10 1F ED 00")) { WritePtr(sites[i] + 1, g_contentDir); n++; }
    Log("Content: patch wads from %s (%d/3 sites)", g_contentDir, n);
    if (Expect(0x85EEA0, "56 8B F0 8B 46 60")) WriteJmp(0x85EEA0, ErrMsgStub, 6);
    ExtendTables();
    FixStringDbs();
    DlcInit();
    /* Promo outfits: each hero's second outfit (Chris Vacation, Jennifer Rebel, Mattias MetalHead) is offered in
       the wardrobe only when Net.HasPlayerUnlockedCode() (FUN_005CB6B0) is true, i.e. signed in to EA online with
       the promo code on the account; offline it pushes false. It now skips the online check (0x5CB6C0 je -> jmp)
       and pushes true (0x5CB6F3), so the outfits are always unlocked. */
    if (Expect(0x5CB6C0, "74 1C") && Expect(0x5CB6F3, "C7 00 00 00 00 00")) {
        static const BYTE j = 0xEB, t = 1;
        WriteMem(0x5CB6C0, &j, 1); WriteMem(0x5CB6F5, &t, 1);
        Log("Content: promo outfits unlocked");
    } else Log("Content: promo outfit check mismatch");
    AddVectoredExceptionHandler(0, CrashVeh);
}
