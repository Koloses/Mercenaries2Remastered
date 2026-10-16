/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Developer builds: fire spread probes. The per-frame ignitor update (FUN_004B82D0) asks the protection's
 * virtualized query (0x4B7B00) which objects overlap the ignitor's Havok phantom, then checks each one
 * (0x4B7B50, flammable) and sets it on fire (0x4B7BB0). The Xbox build does the query inline: the phantom's
 * overlap list at +0xC0 (count +0xC4), object = owner of each collidable (+signed byte at +0x10) + 0xC.
 * Logged every 10 s: query results vs the phantom's own overlap count, flammable / ignite results, live
 * ignitors. [Debug] NativeIgnitorQuery=1 replaces the query result with the Xbox logic. */
#include "m2r.h"
#include <stdio.h>
#include <string.h>

static volatile LONG g_q, g_qHits, g_qOverlap, g_qZeroButOverlap, g_fl, g_flYes, g_ig, g_igYes, g_native;
static int g_useNative;

/* ---- generic probe: run fn(regs) before the instructions at addr (len >= 5 bytes of whole, position
 * independent instructions); regs = pushad block: [0]edi [1]esi [2]ebp [3]esp(original) [4]ebx [5]edx [6]ecx [7]eax */
typedef void (__cdecl *ProbeFn)(DWORD *regs);
static BYTE *g_tramp; static int g_trampUsed;
__attribute__((unused)) static int ProbeAt(DWORD addr, int len, ProbeFn fn)
{
    if (!g_tramp) g_tramp = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_tramp || g_trampUsed + 64 > 4096 || len < 5 || len > 16) return 0;
    BYTE *t = g_tramp + g_trampUsed, *p = t;
    *p++ = 0x60;                                  /* pushad */
    *p++ = 0x9C;                                  /* pushfd */
    *p++ = 0x8D; *p++ = 0x44; *p++ = 0x24; *p++ = 0x04;   /* lea eax,[esp+4] (pushad block) */
    *p++ = 0x50;                                  /* push eax */
    *p++ = 0xE8; *(DWORD *)p = (DWORD)fn - (DWORD)(p + 4); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;        /* add esp,4 */
    *p++ = 0x9D;                                  /* popfd */
    *p++ = 0x61;                                  /* popad */
    memcpy(p, (void *)addr, len); p += len;
    *p++ = 0xE9; *(DWORD *)p = (addr + len) - (DWORD)(p + 4); p += 4;
    g_trampUsed += (int)(p - t + 15) & ~15;
    WriteJmp(addr, t, len);
    return 1;
}
#ifdef M2R_GAME11
/* inside ignite (1.1 0x4B7CA0) */
static volatile LONG g_igPass, g_igDmg, g_igF0, g_igFpos;
typedef struct { DWORD mat; LONG n; float f; } MatStat;
static MatStat g_mat[64]; static CRITICAL_SECTION g_matLock;
static volatile LONG g_thr, g_ignNew, g_trBurn, g_trBurnOk, g_tr, g_trOk;
static DWORD g_fxSwap;
static void __cdecl PrThr(DWORD *r) { (void)r; InterlockedIncrement(&g_thr); }
static void __cdecl PrIgnNew(DWORD *r) { (void)r; InterlockedIncrement(&g_ignNew); }
static DWORD g_trS[8][4]; static volatile LONG g_ntr;
static void __cdecl PrTrans(DWORD *r)          /* after the transition lookup: eax = result, [esp..esp+8] = args */
{
    DWORD *a = (DWORD *)r[3];
    if (!IsBadReadPtr(a, 12)) { LONG i = InterlockedIncrement(&g_ntr) - 1; if (i < 8) { g_trS[i][0] = a[0]; g_trS[i][1] = a[1]; g_trS[i][2] = a[2]; g_trS[i][3] = r[7]; } }
    int burn = !IsBadReadPtr(a, 12) && (a[0] == 0xC6507EE1 || a[1] == 0xC6507EE1 || a[2] == 0xC6507EE1);
    InterlockedIncrement(&g_tr); if (r[7]) InterlockedIncrement(&g_trOk);
    if (burn) { InterlockedIncrement(&g_trBurn); if (r[7]) InterlockedIncrement(&g_trBurnOk); }
}
/* whole-object health damage (1.1 0x522180), entry: [esp+4] health runtime, +0x10 amount, +0x14 type */
static volatile LONG g_hType[8], g_hFire, g_hFireDead, g_hFireNoHp;
typedef struct { DWORD guid; float max, cur, thr, dmg; BYTE c, d; } HSample;
static HSample g_hs[6], g_hs2[6]; static volatile LONG g_nhs, g_nhs2;
static void __cdecl PrHealth(DWORD *r)
{
    DWORD *a = (DWORD *)r[3];
    if (IsBadReadPtr(a, 0x24)) return;
    BYTE *h = (BYTE *)a[1]; DWORD type = a[3]; float dmg = *(float *)&a[2];
    InterlockedIncrement(&g_hType[type < 8 ? type : 7]);
    if (type != 2 && !IsBadReadPtr(h, 0x48)) {          /* other damage: a few samples (what kills the bushes) */
        float *hp = *(float **)(h + 4);
        LONG i = InterlockedIncrement(&g_nhs2) - 1;
        if (i < 6 && hp && !IsBadReadPtr(hp, 12)) { HSample *q = &g_hs2[i]; q->guid = *(DWORD *)h; q->max = hp[0]; q->cur = hp[1]; q->thr = hp[2]; q->dmg = dmg; q->c = (BYTE)type; q->d = h[0xD]; }
    }
    if (type != 2 || IsBadReadPtr(h, 0x48)) return;
    InterlockedIncrement(&g_hFire);
    if (h[0xC]) InterlockedIncrement(&g_hFireDead);
    float *hp = *(float **)(h + 4);
    if (!hp || IsBadReadPtr(hp, 12)) { InterlockedIncrement(&g_hFireNoHp); return; }
    LONG i = InterlockedIncrement(&g_nhs) - 1;
    if (i < 6) { HSample *q = &g_hs[i]; q->guid = *(DWORD *)h; q->max = hp[0]; q->cur = hp[1]; q->thr = hp[2]; q->dmg = dmg; q->c = h[0xC]; q->d = h[0xD]; }
}
/* damage message consumer (1.1 0x5229C0) */
#ifdef M2R_GAME11
static DWORD CompLookup11(DWORD map, DWORD guid)       /* 1.1 0x587D80: eax = guid, ecx = component map -> eax = data */
{
    DWORD r;
    __asm__ volatile("call *%3" : "=a"(r) : "a"(guid), "c"(map), "r"(A11(0x587D80)) : "edx", "memory", "cc");
    return r;
}
static DWORD Template11(DWORD guid) { DWORD so = CompLookup11(A11(0x17BF2D8), guid); return (so && !IsBadReadPtr((void *)(so + 0x14), 4)) ? *(DWORD *)(so + 0x14) : 0; }
#endif
typedef struct { DWORD guid; LONG msg, hp; } GStat;
static GStat g_gs[32]; static DWORD g_lastGuid;
static GStat *GFind(DWORD g)
{
    for (int i = 0; i < 32; i++) { if (g_gs[i].guid == g) return &g_gs[i]; if (!g_gs[i].guid) { g_gs[i].guid = g; return &g_gs[i]; } }
    return NULL;
}

static volatile LONG g_mType[8], g_mFireSkip, g_mFireHp, g_lastType; static DWORD g_mFireGuid[4]; static volatile LONG g_nmg;
static void __cdecl PrMsg(DWORD *r)            /* 0x522A36: edi = message */
{
    DWORD *m = (DWORD *)r[0];
    if (IsBadReadPtr(m, 0x40)) { g_lastType = -1; return; }
    LONG t = (m[0x3C / 4] >> 3) & 7; g_lastType = t;
    InterlockedIncrement(&g_mType[t]);
    if (t == 2) { LONG i = InterlockedIncrement(&g_nmg) - 1; if (i < 4) g_mFireGuid[i] = m[0]; GStat *q = GFind(m[0]); if (q) q->msg++; }
    g_lastGuid = m[0];
}
static void __cdecl PrSkip(DWORD *r) { if ((r[7] & 0xFF) && g_lastType == 2) InterlockedIncrement(&g_mFireSkip); }
static void __cdecl PrHp(DWORD *r) { (void)r; if (g_lastType == 2) { InterlockedIncrement(&g_mFireHp); GStat *q = GFind(g_lastGuid); if (q) q->hp++; } }
/* burnt look vs flames: scrub instances, ColorAnimation (tint to black), RedEffect (flames), fade */
#define NSCRUB 8192
static DWORD g_scrub[NSCRUB]; static volatile LONG g_nScrub;
static int IsScrub(DWORD g) { for (LONG i = 0; i < g_nScrub && i < NSCRUB; i++) if (g_scrub[i] == g) return 1; return 0; }
static void __cdecl PrScrub(DWORD *r) { DWORD g = ((DWORD *)r[3])[1]; LONG i = InterlockedIncrement(&g_nScrub) - 1; if (i < NSCRUB) g_scrub[i] = g; }
static DWORD g_redG[12]; static volatile LONG g_nredG;
static volatile LONG g_col, g_colScrub, g_red, g_redFail, g_fade; static DWORD g_colG[4], g_redId[8]; static LONG g_redIdN[8];
static void __cdecl PrColor(DWORD *r)
{
    DWORD g = *(DWORD *)(r[2] + 8); LONG i = InterlockedIncrement(&g_col) - 1;
    if (IsScrub(g)) InterlockedIncrement(&g_colScrub);
    if (i < 4) g_colG[i] = g;
}
static void __cdecl PrRedOut(DWORD *r)        /* eax = created effect, [ebx] = effect id, edi = guid */
{
    InterlockedIncrement(&g_red); if (!r[7]) InterlockedIncrement(&g_redFail);
    { LONG i = InterlockedIncrement(&g_nredG) - 1; if (i < 12) g_redG[i] = r[0]; }
    DWORD id = IsBadReadPtr((void *)r[4], 4) ? 0 : *(DWORD *)r[4];
    for (int i = 0; i < 8; i++) { if (g_redId[i] == id) { g_redIdN[i]++; break; } if (!g_redId[i]) { g_redId[i] = id; g_redIdN[i] = 1; break; } }
}
static void __cdecl PrFade(DWORD *r) { (void)r; InterlockedIncrement(&g_fade); }
/* debris manager (1.1 0x4B4590): damage-channel events, node-death bindings, chunk spawns */
static DWORD g_evH[12]; static LONG g_evN[12]; static volatile LONG g_ndEv, g_k2, g_chunk; static DWORD g_ndG[4], g_k2T[4], g_chT[4];
static void __cdecl PrEvt(DWORD *r)
{
    DWORD *sp = (DWORD *)r[3]; if (IsBadReadPtr(sp, 0x60)) return;
    DWORD g = sp[0x54 / 4], h = sp[0x5C / 4];
    for (int i = 0; i < 12; i++) { if (g_evH[i] == h) { g_evN[i]++; break; } if (!g_evH[i]) { g_evH[i] = h; g_evN[i] = 1; break; } }
    if (h == 0x87D3B9A1) { LONG i = InterlockedIncrement(&g_ndEv) - 1; if (i < 4) g_ndG[i] = g; }
}
static void __cdecl PrK2(DWORD *r) { LONG i = InterlockedIncrement(&g_k2) - 1; if (i < 4 && !IsBadReadPtr((void *)r[6], 0x10)) g_k2T[i] = *(DWORD *)(r[6] + 0xC); }
static void __cdecl PrChunk(DWORD *r) { DWORD *sp = (DWORD *)r[3]; LONG i = InterlockedIncrement(&g_chunk) - 1; if (i < 4 && !IsBadReadPtr(sp, 0x18)) g_chT[i] = sp[2]; }
/* state action 0x3347914F (effect spawn, 1.1 0x4D2A50 cdecl(guid, node, effect hash)): how far does it get */
typedef struct { DWORD guid, node, hash, obj; int stage; float pos[3]; DWORD eff, id; } SpawnRec;
static SpawnRec g_sp[24], g_spCur; static volatile LONG g_nsp;
static void EffTrackAdd(void);
static void SpawnFlush(void) { if (g_spCur.stage >= 5) EffTrackAdd(); if (g_spCur.guid || g_spCur.hash) { LONG i = InterlockedIncrement(&g_nsp) - 1; if (i < 24) g_sp[i] = g_spCur; } memset(&g_spCur, 0, sizeof g_spCur); }
static DWORD ResolveFx(DWORD h) { DWORD r; __asm__ volatile("call *%2" : "=a"(r) : "a"(h), "r"((DWORD)A11(0x672EB0)) : "ecx", "edx", "memory", "cc"); return r; }
static void __cdecl PrSp0(DWORD *r) { DWORD *a = (DWORD *)r[3];
    { static int once; if (!once) { once = 1; Log("fire: template of B5C59F0A = %08lX, 72AB2999 = %08lX, 602D65D7 = %08lX", ResolveFx(0xB5C59F0A), ResolveFx(0x72AB2999), ResolveFx(0x602D65D7)); } }
    SpawnFlush(); if (!IsBadReadPtr(a, 16)) { g_spCur.guid = a[1]; g_spCur.node = a[2]; g_spCur.hash = a[3]; } }
/* g_fxSwap: [Debug] FxSwap: canopy flame effect 72AB2999 replaced by this effect hash (test) */
static void __cdecl PrSp1(DWORD *r) { DWORD *h = (DWORD *)(r[2] + 0x10); g_spCur.stage = 1; if (g_fxSwap && *h == 0x72AB2999) *h = g_fxSwap; }
static void __cdecl PrSp2(DWORD *r) { (void)r; g_spCur.stage = 2; }
static int g_nodeUp;   /* [Debug] NodeUpright: state-spawned effects on vegetation keep the node position but world-upright rotation (test) */
static float g_rot[9]; static volatile LONG g_nrot;
static void __cdecl PrSp3(DWORD *r)
{
    float *m = (float *)(r[3] + 0x70); g_spCur.stage = 3;
    if (IsBadReadPtr(m, 0x40)) return;
    g_spCur.pos[0] = m[12]; g_spCur.pos[1] = m[13]; g_spCur.pos[2] = m[14];
    if (g_spCur.hash == 0x72AB2999) {
        if (!g_nrot++) { g_rot[0] = m[0]; g_rot[1] = m[1]; g_rot[2] = m[2]; g_rot[3] = m[4]; g_rot[4] = m[5]; g_rot[5] = m[6]; g_rot[6] = m[8]; g_rot[7] = m[9]; g_rot[8] = m[10];
            Log("fire: canopy node rotation %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f (w %.3f %.3f %.3f %.3f)", m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[3], m[7], m[11], m[15]); }
        if (g_nodeUp) { for (int i = 0; i < 12; i++) m[i] = (i % 5 == 0) ? 1.0f : 0.0f; }
    }
}
static void __cdecl PrSp4(DWORD *r) { DWORD *sp = (DWORD *)r[3]; g_spCur.stage = 4; g_spCur.obj = IsBadReadPtr(sp, 0x1C) ? 0 : sp[0x18 / 4]; }
static void __cdecl PrSp5(DWORD *r) { g_spCur.stage = 5; g_spCur.eff = r[0]; g_spCur.id = IsBadReadPtr((void *)r[1], 4) ? 0 : *(DWORD *)r[1]; }
/* effect instance tracker: pool entry (0x810 bytes) of state-spawned effects, logged as changes every 250 ms for 8 s */
typedef struct { DWORD *eff, guid, fx; DWORD t0; volatile LONG nSet, nUpd, nUpdLive; DWORD prev[0x810 / 4]; } EffTrack;
#define NET 16
static EffTrack g_et[NET]; static volatile LONG g_etCanopy, g_etOther;
static volatile LONG g_setAll, g_updAll;
static void __cdecl PrEffSet(DWORD *r) { InterlockedIncrement(&g_setAll); for (int i = 0; i < NET; i++) if (g_et[i].eff && (DWORD)g_et[i].eff == r[6]) InterlockedIncrement(&g_et[i].nSet); }
static void __cdecl PrEffUpd(DWORD *r) { InterlockedIncrement(&g_updAll); for (int i = 0; i < NET; i++) if (g_et[i].eff && (DWORD)g_et[i].eff == r[4]) { InterlockedIncrement(&g_et[i].nUpd); if (*(BYTE *)(r[4] + 0x808) & 1) InterlockedIncrement(&g_et[i].nUpdLive); } }
static int g_effDiffLog;
static void EffTrackAdd(void)
{
    if (!g_spCur.eff || IsBadReadPtr((void *)g_spCur.eff, 0x810)) return;
    int canopy = g_spCur.hash == 0x72AB2999;
    if (canopy ? g_etCanopy >= 4 : (g_etOther >= 10 || g_spCur.guid < 0x40000000u)) return;
    for (int i = 0; i < NET; i++) if (!g_et[i].eff) {
        g_et[i].guid = g_spCur.guid; g_et[i].fx = g_spCur.hash; g_et[i].t0 = GetTickCount();
        memcpy(g_et[i].prev, (void *)g_spCur.eff, 0x810);
        g_et[i].nSet = g_et[i].nUpd = g_et[i].nUpdLive = 0;
        g_et[i].eff = (DWORD *)g_spCur.eff;
        if (canopy) g_etCanopy++; else g_etOther++;
        { char b[700]; int n = 0; DWORD *e = (DWORD *)g_spCur.eff, *d = (DWORD *)e[0x7E4 / 4];
          n += _snprintf(b + n, sizeof b - n, "fire: track eff %p owner %08lX (spawner %08lX fx %08lX id %08lX): state %ld guid %08lX pos %.1f %.1f %.1f def %p",
              (void *)e, e[0x10 / 4], g_spCur.guid, g_spCur.hash, g_spCur.id, (LONG)e[0], e[0x10 / 4],
              *(float *)&e[0x7C4 / 4], *(float *)&e[0x7C8 / 4], *(float *)&e[0x7CC / 4], (void *)d);
          if (d && !IsBadReadPtr(d, 0x98)) { n += _snprintf(b + n, sizeof b - n, " def:"); for (int k = 0; k < 0x98 / 4 && n < 660; k++) n += _snprintf(b + n, sizeof b - n, " %lX", d[k]); }
          Log("%s", b);
          if (d && !IsBadReadPtr(d, 0x98) && d[0x80 / 4] < 16 && !IsBadReadPtr((void *)d[0x84 / 4], d[0x80 / 4] * 0x210)) {
              for (DWORD k = 0; k < d[0x80 / 4]; k++) { DWORD *em = (DWORD *)(d[0x84 / 4] + k * 0x210); char c[1500]; int m = 0;
                  for (int j = 0; j < 0x210 / 4 && m < 1450; j++) m += _snprintf(c + m, sizeof c - m, " %lX", em[j]);
                  Log("fire:   emitter %lu:%s", k, c); }
          } }
        return;
    }
}
static DWORD WINAPI EffTrackThread(LPVOID u)
{
    (void)u;
    for (;;) {
        Sleep(250);
        for (int i = 0; i < NET; i++) {
            EffTrack *t = &g_et[i]; if (!t->eff) continue;
            DWORD age = GetTickCount() - t->t0;
            char b[1600]; int n = 0, c = 0;
            for (int k = 0; k < 0x810 / 4; k++) { DWORD v = t->eff[k]; if (v != t->prev[k]) { if (n < 1500) n += _snprintf(b + n, sizeof b - n, " %X:%lX", k * 4, v); t->prev[k] = v; c++; } }
            b[n] = 0;
            if (c && g_effDiffLog) Log("fire: eff %p (fx %08lX) +%lums attach-set %ld update %ld (attached %ld) | all set %ld upd %ld | %d changed:%s", (void *)t->eff, t->fx, age,
                InterlockedExchange(&t->nSet, 0), InterlockedExchange(&t->nUpd, 0), InterlockedExchange(&t->nUpdLive, 0), g_setAll, g_updAll, c, b);
            if (age > 8000) { Log("fire: eff %p tracking done", (void *)t->eff); t->eff = 0; }
        }
    }
    return 0;
}
static void __cdecl PrSp6(DWORD *r) { (void)r; g_spCur.stage = 6; }
/* particle render 1.1 0x494200: batches drawn only when near < key <= far (key = -[keys+i*4], mm) */
static volatile LONG g_prCalls, g_prIn, g_prBelow, g_prAbove, g_prEmpty;
static LONG g_rng[8][2]; static LONG g_rngN[8];
typedef struct { LONG key, lo, hi; DWORD b[12]; } PrSamp;
typedef struct { LONG n, in, key, lo, hi; DWORD b[12]; float s6, s7, m6, vy, a; LONG a0; DWORD last[12]; } PrTrk; static PrTrk g_trk[NET]; static volatile LONG g_kAll;
static PrSamp g_prS[10]; static volatile LONG g_nprS; static PrSamp g_prD[4]; static volatile LONG g_nprD;
/* final particle vertices (1.1 0x494ADA: ebp = particle record, [edi+0x21C] = its 4 vertices, 0x24 bytes each:
 * +0 +4 UV halves, +8 colour ARGB, +0C..14 pos, +18 size, +1C intensity half, +1E fade half, +20 corner) */
typedef struct { LONG drawn, dropped; float size, alpha, inten, fade; DWORD key, type, atlas; DWORD v[3][9]; int nv; DWORD rec[12]; } VxStat;
static VxStat g_vx[NET]; static DWORD g_lastDrawn; static volatile LONG g_vxAll, g_vxDropAll; static DWORD g_vxCurMax;
static float Half(WORD h) { DWORD e = (h >> 10) & 31, m = h & 1023, sgn = (h >> 15) & 1; float f;
    if (!e) f = m / 16777216.0f; else if (e == 31) f = 65504.0f; else { f = (1.0f + m / 1024.0f); int k = (int)e - 15; while (k > 0) { f *= 2; k--; } while (k < 0) { f /= 2; k++; } }
    return sgn ? -f : f; }
static int VxMatch(const float *pp)
{
    DWORD now = GetTickCount();
    for (int t = 0; t < NET; t++) { DWORD *e = g_et[t].eff; if (!e || now - g_et[t].t0 > 2500) continue;
        float *ep = (float *)&e[0x7C4 / 4]; float dx = pp[0] - ep[0], dy = pp[1] - ep[1], dz = pp[2] - ep[2];
        if (dx * dx + dy * dy + dz * dz < 25.0f) return t; }
    return -1;
}
static void __cdecl PrVxDone(DWORD *r)
{
    DWORD *rec = (DWORD *)r[2], ren = r[0]; InterlockedIncrement(&g_vxAll); g_lastDrawn = (DWORD)rec;
    if (IsBadReadPtr(rec, 0x30)) return;
    int t = VxMatch((float *)rec); if (t < 0) return;
    DWORD *vx = *(DWORD **)(ren + 0x21C); if (IsBadReadPtr(vx, 0x90)) return;
    VxStat *q = &g_vx[t]; q->drawn++;
    q->size += *(float *)&vx[6]; q->alpha += (float)(vx[2] >> 24); q->inten += Half((WORD)vx[7]); q->fade += Half((WORD)(vx[7] >> 16));
    DWORD type = rec[9]; q->type = type; if (!IsBadReadPtr((void *)type, 0x210)) { q->key = *(DWORD *)(type + 0x1EC); q->atlas = *(DWORD *)(type + 0x1F8); }
    if (q->nv < 3 && (q->drawn % 37) == 1) { memcpy(q->v[q->nv], vx, 0x24); if (!q->nv) memcpy(q->rec, rec, 0x30); q->nv++; }
}
static void __cdecl PrVxEnd(DWORD *r)
{
    DWORD *rec = (DWORD *)r[2];
    if ((DWORD)rec == g_lastDrawn) return;
    InterlockedIncrement(&g_vxDropAll);
    if (IsBadReadPtr(rec, 0x30)) return;
    int t = VxMatch((float *)rec); if (t >= 0) g_vx[t].dropped++;
}
static void PrVxLog(void)
{
    Log("fire: particle quads written %ld, dropped (vertex buffer full) %ld", InterlockedExchange(&g_vxAll, 0), InterlockedExchange(&g_vxDropAll, 0));
    for (int t = 0; t < NET; t++) { VxStat *q = &g_vx[t]; if (!q->drawn && !q->dropped) continue;
        LONG n = q->drawn ? q->drawn : 1;
        Log("fire:   eff fx %08lX: quads %ld dropped %ld | avg size %.3f alpha %.1f intensity %.3f fade %.3f | batch key %lu type %08lX atlas %08lX",
            g_et[t].fx, q->drawn, q->dropped, q->size / n, q->alpha / n, q->inten / n, q->fade / n, q->key, q->type, q->atlas);
        { char b[400]; int k = _snprintf(b, sizeof b, "fire:     record:"); for (int j = 0; j < 12; j++) k += _snprintf(b + k, sizeof b - k, " %lX", q->rec[j]); Log("%s", b); }
        for (int i = 0; i < q->nv; i++) { DWORD *v = q->v[i]; Log("fire:     vertex: uv %.3f %.3f %.3f %.3f colour %08lX pos %.1f %.1f %.1f size %.3f intensity %.3f fade %.3f corner %lu",
            Half((WORD)v[0]), Half((WORD)(v[0] >> 16)), Half((WORD)v[1]), Half((WORD)(v[1] >> 16)), v[2], *(float *)&v[3], *(float *)&v[4], *(float *)&v[5], *(float *)&v[6],
            Half((WORD)v[7]), Half((WORD)(v[7] >> 16)), v[8] & 0xFF); }
        memset(q, 0, sizeof *q); }
}
static void __cdecl PrRender(DWORD *r)
{
    BYTE *sp = (BYTE *)r[3]; DWORD edi = r[0], i = r[1];
    LONG lo = *(LONG *)(sp + 0x28), hi = *(LONG *)(sp + 0x2C);
    if (i == 0) { InterlockedIncrement(&g_prCalls);
        for (int k = 0; k < 8; k++) { if (g_rngN[k] && g_rng[k][0] == lo && g_rng[k][1] == hi) { g_rngN[k]++; break; } if (!g_rngN[k]) { g_rng[k][0] = lo; g_rng[k][1] = hi; g_rngN[k] = 1; break; } } }
    LONG key = -*(LONG *)(*(DWORD *)(edi + 0x1D748) + i * 4);
    DWORD *b = (DWORD *)(*(DWORD *)(*(DWORD *)(sp + 0x30) + 0x1D71C) + i * 0x30);
    if (IsBadReadPtr(b, 0x30)) return;
    if (!*(WORD *)((BYTE *)b + 0x28)) { InterlockedIncrement(&g_prEmpty); return; }
    { float *pp = (float *)b; LONG ki = InterlockedIncrement(&g_kAll) - 1; (void)ki;
      for (int t = 0; t < NET; t++) { DWORD *e = g_et[t].eff; if (!e) continue;
        float *ep = (float *)&e[0x7C4 / 4]; float dx = pp[0] - ep[0], dy = pp[1] - ep[1], dz = pp[2] - ep[2];
        if (dx * dx + dy * dy + dz * dz < 36.0f) { if (!g_trk[t].n++) { g_trk[t].key = key; g_trk[t].lo = lo; g_trk[t].hi = hi; memcpy(g_trk[t].b, b, 0x30); }
            if (key > lo && key <= hi) g_trk[t].in++;
            { PrTrk *q = &g_trk[t]; float f6 = pp[6], f7 = pp[7]; q->s6 += f6; q->s7 += f7; if (f6 > q->m6) q->m6 = f6; q->vy += pp[4];
              DWORD c = b[10]; q->a += (float)(c >> 24); if (!(c >> 24)) q->a0++; if ((q->n & 1023) == 512) memcpy(q->last, b, 0x30); }
            break; } } }
    if (key > lo && key <= hi) { InterlockedIncrement(&g_prIn); LONG n = InterlockedIncrement(&g_nprD) - 1; if (n < 4) { g_prD[n].key = key; g_prD[n].lo = lo; g_prD[n].hi = hi; memcpy(g_prD[n].b, b, 0x30); } return; }
    if (key <= lo) InterlockedIncrement(&g_prBelow); else InterlockedIncrement(&g_prAbove);
    LONG n = InterlockedIncrement(&g_nprS) - 1; if (n < 10) { g_prS[n].key = key; g_prS[n].lo = lo; g_prS[n].hi = hi; memcpy(g_prS[n].b, b, 0x30); }
}
static void PrRenderLog(void)
{
    char t[3000]; int n = 0;
    n += _snprintf(t + n, sizeof t - n, "fire: particle render calls %ld, batches drawn %ld, skipped near %ld far %ld, empty %ld | ranges",
        InterlockedExchange(&g_prCalls, 0), InterlockedExchange(&g_prIn, 0), InterlockedExchange(&g_prBelow, 0), InterlockedExchange(&g_prAbove, 0), InterlockedExchange(&g_prEmpty, 0));
    for (int k = 0; k < 8 && g_rngN[k]; k++) { n += _snprintf(t + n, sizeof t - n, " %ld..%ld:%ld", g_rng[k][0], g_rng[k][1], g_rngN[k]); g_rngN[k] = 0; }
    Log("%s", t);
    for (int k = 0; k < NET; k++) if (g_trk[k].n) { PrTrk *q = &g_trk[k]; n = _snprintf(t, sizeof t, "fire:   tracked eff %p (fx %08lX) particles within 6 m %ld drawn %ld, first key %ld range %ld..%ld:",
        (void *)g_et[k].eff, g_et[k].fx, q->n, q->in, q->key, q->lo, q->hi); for (int j = 0; j < 12; j++) n += _snprintf(t + n, sizeof t - n, " %lX", q->b[j]);
        n += _snprintf(t + n, sizeof t - n, " | avg [6] %.3f max %.3f avg [7] %.3f avg vy %.2f avg [10]>>24 %.1f zero %ld | mid sample:", q->s6 / q->n, q->m6, q->s7 / q->n, q->vy / q->n, q->a / q->n, q->a0);
        for (int j = 0; j < 12; j++) n += _snprintf(t + n, sizeof t - n, " %lX", q->last[j]); Log("%s", t); memset(q, 0, sizeof *q); }
    LONG ns = InterlockedExchange(&g_nprS, 0); if (ns > 10) ns = 10;
    for (int k = 0; k < ns; k++) { PrSamp *q = &g_prS[k]; n = _snprintf(t, sizeof t, "fire:   skipped key %ld (range %ld..%ld):", q->key, q->lo, q->hi);
        for (int j = 0; j < 12; j++) n += _snprintf(t + n, sizeof t - n, " %lX", q->b[j]); Log("%s", t); }
    LONG nd = InterlockedExchange(&g_nprD, 0); if (nd > 4) nd = 4;
    for (int k = 0; k < nd; k++) { PrSamp *q = &g_prD[k]; n = _snprintf(t, sizeof t, "fire:   drawn key %ld (range %ld..%ld):", q->key, q->lo, q->hi);
        for (int j = 0; j < 12; j++) n += _snprintf(t + n, sizeof t - n, " %lX", q->b[j]); Log("%s", t); }
}
static void __cdecl PrPass(DWORD *r) { (void)r; InterlockedIncrement(&g_igPass); }
static void __cdecl PrDmg(DWORD *r) { (void)r; InterlockedIncrement(&g_igDmg); }
static void __cdecl PrMat(DWORD *r)           /* at 0x4B8009: esi = material, eax = &Table[mat]["Fire"] */
{
    DWORD m = r[1]; float f = (r[7] && !IsBadReadPtr((void *)r[7], 4)) ? *(float *)r[7] : -999.f;
    if (f > 0) InterlockedIncrement(&g_igFpos); else InterlockedIncrement(&g_igF0);
    EnterCriticalSection(&g_matLock);
    int i; for (i = 0; i < 64 && g_mat[i].n && g_mat[i].mat != m; i++) ;
    if (i < 64) { g_mat[i].mat = m; g_mat[i].n++; g_mat[i].f = f; }
    LeaveCriticalSection(&g_matLock);
}
#endif

static int NativeQuery(BYTE *ph, int *out)
{
    int n = *(int *)(ph + 0xC4), c = 0;
    BYTE **a = *(BYTE ***)(ph + 0xC0);
    if (n <= 0 || !a || IsBadReadPtr(a, n * 4)) return 0;
    for (int i = 0; i < n && c < 32; i++) {
        BYTE *col = a[i];
        if (!col || IsBadReadPtr(col, 0x14)) continue;
        BYTE *owner = col + *(signed char *)(col + 0x10);
        if (IsBadReadPtr(owner, 0x10)) continue;
        int obj = *(int *)(owner + 0xC);
        if (obj) out[c++] = obj;
    }
    return c;
}
/* after `call 0x4B7B00` (eax = count, [esp] = phantom, [esp+4] = out[32]) */
__attribute__((used)) int __cdecl ProbeQuery(int r, BYTE *ph, int *out)
{
    InterlockedIncrement(&g_q);
    int ov = (ph && !IsBadReadPtr(ph, 0xC8)) ? *(int *)(ph + 0xC4) : -1;
    if (r > 0) InterlockedExchangeAdd(&g_qHits, r);
    if (ov > 0) { InterlockedExchangeAdd(&g_qOverlap, ov); if (r <= 0) InterlockedIncrement(&g_qZeroButOverlap); }
    if (g_useNative && ph && out) {
        int n = NativeQuery(ph, out);
        if (n != r) InterlockedIncrement(&g_native);
        return n;
    }
    return r;
}
__attribute__((naked)) static void QueryStub(void)     /* replaces `xor ecx,ecx / add esp,8` */
{
    __asm__ volatile(
        "pushal\n\t"
        "pushl 40(%esp)\n\t"            /* out     (32 pushal + 4 ret + 4 phantom) */
        "pushl 40(%esp)\n\t"            /* phantom (one push later) */
        "pushl %eax\n\t"
        "call _ProbeQuery\n\t"
        "add $12, %esp\n\t"
        "movl %eax, 28(%esp)\n\t"       /* eax slot of pushal */
        "popal\n\t"
        "xor %ecx, %ecx\n\t"
        "ret $8");
}
__attribute__((used)) void __cdecl ProbeFl(int al) { InterlockedIncrement(&g_fl); if (al & 0xFF) InterlockedIncrement(&g_flYes); }
__attribute__((naked)) static void FlStub(void)        /* replaces `add esp,4 / test al,al` */
{
    __asm__ volatile("pushal\n\tpushl %eax\n\tcall _ProbeFl\n\tadd $4, %esp\n\tpopal\n\ttest %al, %al\n\tret $4");
}
__attribute__((used)) void __cdecl ProbeIg(int al) { InterlockedIncrement(&g_ig); if (al & 0xFF) InterlockedIncrement(&g_igYes); }
__attribute__((naked)) static void IgStub(void)        /* replaces `add esp,0x20 / test al,al` */
{
    __asm__ volatile("pushal\n\tpushl %eax\n\tcall _ProbeIg\n\tadd $4, %esp\n\tpopal\n\ttest %al, %al\n\tret $0x20");
}
static DWORD WINAPI ProbeThread(LPVOID u)
{
    (void)u;
    LONG maxIgn = 0;
    for (;;) {
        Sleep(10000);
        LONG ign = *(volatile LONG *)0x17BF3A8;
        { static int once; BYTE *dev = *(BYTE **)0x1176288;
          if (!once && dev && !IsBadReadPtr(dev + 0x5E4, 4)) { once = 1; Log("fire: device caps %08lX, ShaderLevel %d/%d", *(DWORD *)(dev + 0x5E4), *(BYTE *)0xDFC345, *(BYTE *)0xDFC346); } }
        if (ign > maxIgn) maxIgn = ign;
#ifdef M2R_GAME11
        { static int k; void MsmLogStats(void); if (++k % 3 == 0) MsmLogStats(); }
        { void EmitFixLog(void); EmitFixLog(); }
#endif
        if (!g_q && !g_fl) continue;
#ifdef M2R_GAME11
        {
            char b[600]; int n = 0;
            EnterCriticalSection(&g_matLock);
            for (int i = 0; i < 64 && g_mat[i].n && n < 520; i++) { n += _snprintf(b + n, sizeof b - n, " %08lX:%ld:%.3g", g_mat[i].mat, g_mat[i].n, g_mat[i].f); g_mat[i].n = 0; g_mat[i].mat = 0; }
            LeaveCriticalSection(&g_matLock);
            b[n] = 0;
            Log("fire: burn threshold crossed %ld, live tick damage %ld, new ignitors %ld | state transitions %ld/%ld found, burn event %ld/%ld found",
                InterlockedExchange(&g_thr, 0), *(volatile LONG *)A11(0x17BEC18), InterlockedExchange(&g_ignNew, 0),
                InterlockedExchange(&g_trOk, 0), InterlockedExchange(&g_tr, 0), InterlockedExchange(&g_trBurnOk, 0), InterlockedExchange(&g_trBurn, 0));
            {
                char hb[400]; int k = 0; LONG ns = InterlockedExchange(&g_nhs, 0); if (ns > 6) ns = 6;
                for (int i = 0; i < ns && k < 380; i++) k += _snprintf(hb + k, sizeof hb - k, " [%08lX hp %.1f/%.1f thr %.1f dmg %.3f c%d d%d]",
                    g_hs[i].guid, g_hs[i].cur, g_hs[i].max, g_hs[i].thr, g_hs[i].dmg, g_hs[i].c, g_hs[i].d);
                hb[k] = 0;
                LONG ng = InterlockedExchange(&g_nmg, 0); if (ng > 4) ng = 4;
                Log("fire: damage messages by type %ld/%ld/%ld/%ld/%ld/%ld | fire: target skipped %ld, has health %ld | fire guids %08lX %08lX %08lX %08lX",
                    InterlockedExchange(&g_mType[0], 0), InterlockedExchange(&g_mType[1], 0), InterlockedExchange(&g_mType[2], 0),
                    InterlockedExchange(&g_mType[3], 0), InterlockedExchange(&g_mType[4], 0), InterlockedExchange(&g_mType[5], 0),
                    InterlockedExchange(&g_mFireSkip, 0), InterlockedExchange(&g_mFireHp, 0),
                    ng > 0 ? g_mFireGuid[0] : 0, ng > 1 ? g_mFireGuid[1] : 0, ng > 2 ? g_mFireGuid[2] : 0, ng > 3 ? g_mFireGuid[3] : 0);
                { char gb[700]; int k2 = 0;
                  for (int i = 0; i < 32 && g_gs[i].guid && k2 < 660; i++) { k2 += _snprintf(gb + k2, sizeof gb - k2, " %08lX(template %08lX):%ld/%ld", g_gs[i].guid, Template11(g_gs[i].guid), g_gs[i].hp, g_gs[i].msg); }
                  gb[k2] = 0; memset(g_gs, 0, sizeof g_gs);
                  if (k2) Log("fire: fire messages per target (reached health/messages):%s", gb); }
                { char hb2[400]; int k3 = 0; LONG n2 = InterlockedExchange(&g_nhs2, 0); if (n2 > 6) n2 = 6;
                  for (int i = 0; i < n2 && k3 < 380; i++) k3 += _snprintf(hb2 + k3, sizeof hb2 - k3, " [%08lX type %d hp %.1f/%.1f thr %.1f dmg %.1f]",
                      g_hs2[i].guid, g_hs2[i].c, g_hs2[i].cur, g_hs2[i].max, g_hs2[i].thr, g_hs2[i].dmg);
                  hb2[k3] = 0; if (k3) Log("fire: other damage samples:%s", hb2); }
                { char rb[300]; int k4 = 0;
                  for (int i = 0; i < 8 && g_redId[i] && k4 < 280; i++) k4 += _snprintf(rb + k4, sizeof rb - k4, " %08lX:%ld", g_redId[i], g_redIdN[i]);
                  rb[k4] = 0; memset(g_redId, 0, sizeof g_redId); memset(g_redIdN, 0, sizeof g_redIdN);
                  LONG nc = InterlockedExchange(&g_col, 0); if (nc > 4) nc = 4;
                  Log("fire: black tint %ld (scrub %ld; %08lX %08lX %08lX %08lX) | flame effects %ld (failed %ld; ids%s) | fades %ld | scrub instances %ld",
                      nc, InterlockedExchange(&g_colScrub, 0), nc > 0 ? g_colG[0] : 0, nc > 1 ? g_colG[1] : 0, nc > 2 ? g_colG[2] : 0, nc > 3 ? g_colG[3] : 0,
                      InterlockedExchange(&g_red, 0), InterlockedExchange(&g_redFail, 0), rb, InterlockedExchange(&g_fade, 0), g_nScrub); }
                { char tb[600]; int k5 = 0; LONG n5 = InterlockedExchange(&g_ntr, 0); if (n5 > 8) n5 = 8;
                  for (int i = 0; i < n5 && k5 < 560; i++) k5 += _snprintf(tb + k5, sizeof tb - k5, " [%08lX %08lX %08lX -> %08lX]", g_trS[i][0], g_trS[i][1], g_trS[i][2], g_trS[i][3]);
                  tb[k5] = 0; if (k5) Log("fire: state transitions:%s", tb);
                  k5 = 0; n5 = InterlockedExchange(&g_nredG, 0); if (n5 > 12) n5 = 12;
                  for (int i = 0; i < n5 && k5 < 560; i++) k5 += _snprintf(tb + k5, sizeof tb - k5, " %08lX", g_redG[i]);
                  tb[k5] = 0; if (k5) Log("fire: flame effect owners:%s", tb); }
                { char eb[500]; int k6 = 0;
                  for (int i = 0; i < 12 && g_evH[i] && k6 < 470; i++) k6 += _snprintf(eb + k6, sizeof eb - k6, " %08lX:%ld", g_evH[i], g_evN[i]);
                  eb[k6] = 0; memset(g_evH, 0, sizeof g_evH); memset(g_evN, 0, sizeof g_evN);
                  LONG a1 = InterlockedExchange(&g_ndEv, 0), a2 = InterlockedExchange(&g_k2, 0), a3 = InterlockedExchange(&g_chunk, 0);
                  if (k6 || a2 || a3) Log("fire: debris events%s | node death %ld (%08lX %08lX) | node-death bindings %ld (debris %08lX %08lX) | chunks %ld (%08lX %08lX %08lX)",
                      eb, a1, g_ndG[0], g_ndG[1], a2, g_k2T[0], g_k2T[1], a3, g_chT[0], g_chT[1], g_chT[2]); }
                { char sb[3000]; int k7 = 0; LONG n7 = InterlockedExchange(&g_nsp, 0); if (n7 > 24) n7 = 24;
                  for (int i = 0; i < n7 && k7 < 2900; i++) k7 += _snprintf(sb + k7, sizeof sb - k7, " [%08lX node %08lX fx %08lX stage %d obj %08lX at %.1f %.1f %.1f eff %08lX id %08lX]",
                      g_sp[i].guid, g_sp[i].node, g_sp[i].hash, g_sp[i].stage, g_sp[i].obj, g_sp[i].pos[0], g_sp[i].pos[1], g_sp[i].pos[2], g_sp[i].eff, g_sp[i].id);
                  sb[k7] = 0; if (k7) Log("fire: state effect spawns (0 pool full, 1 pool ok, 2 effect found, 3 node found, 4 spawned, 5 effect data, 6 started):%s", sb); }
                PrRenderLog(); PrVxLog();
                Log("fire: health damage by type %ld/%ld/%ld/%ld/%ld | fire %ld (dead %ld, no hp %ld)%s",
                    InterlockedExchange(&g_hType[0], 0), InterlockedExchange(&g_hType[1], 0), InterlockedExchange(&g_hType[2], 0),
                    InterlockedExchange(&g_hType[3], 0), InterlockedExchange(&g_hType[4], 0),
                    InterlockedExchange(&g_hFire, 0), InterlockedExchange(&g_hFireDead, 0), InterlockedExchange(&g_hFireNoHp, 0), hb);
            }
            Log("fire: ignite past distance/strength %ld, Fire value >0 %ld, =0 %ld, damage posted %ld | materials (id:count:Fire)%s",
                InterlockedExchange(&g_igPass, 0), InterlockedExchange(&g_igFpos, 0), InterlockedExchange(&g_igF0, 0), InterlockedExchange(&g_igDmg, 0), b);
        }
#endif
        Log("fire: queries %ld (objects %ld, phantom overlaps %ld, empty result with overlaps %ld, native differs %ld) | flammable %ld/%ld | ignited %ld/%ld | ignitors now %ld max %ld",
            InterlockedExchange(&g_q, 0), InterlockedExchange(&g_qHits, 0), InterlockedExchange(&g_qOverlap, 0),
            InterlockedExchange(&g_qZeroButOverlap, 0), InterlockedExchange(&g_native, 0),
            InterlockedExchange(&g_flYes, 0), InterlockedExchange(&g_fl, 0), InterlockedExchange(&g_igYes, 0), InterlockedExchange(&g_ig, 0),
            ign, maxIgn);
    }
    return 0;
}
void FireProbeInit(void)
{
    if (!IniInt("Debug", "FireProbe", 1)) return;
    g_useNative = IniInt("Debug", "NativeIgnitorQuery", 0);
#ifdef M2R_GAME11
    g_nodeUp = IniInt("Debug", "NodeUpright", 0); Log("fire: node upright test %d", g_nodeUp);
    { g_fxSwap = IniInt("Debug", "FxSwap", 0) ? 0x602D65D7 : 0;
      Log("fire: canopy flame effect swap %08lX", g_fxSwap); }
#endif
    if (!Expect(0x4B836A, "E8 91 F7 FF FF 33 C9 83 C4 08 3B C1") || !Expect(0x4B83A2, "E8 A9 F7 FF FF 83 C4 04 84 C0") ||
        !Expect(0x4B83D6, "E8 D5 F7 FF FF 83 C4 20 84 C0")) { Log("fire: probe code mismatch, off"); return; }
    WriteCall(0x4B836F, (void *)QueryStub, 5);
    WriteCall(0x4B83A7, (void *)FlStub, 5);
    WriteCall(0x4B83DB, (void *)IgStub, 5);
#ifdef M2R_GAME11
    InitializeCriticalSection(&g_matLock);
    if (Expect(A11(0x4B7F7E), "8B 03 8B 50 3C") && Expect(A11(0x4B8009), "8B 00 8B 75 20") && Expect(A11(0x4B8027), "8B 55 0C 8B 45 08")) {
        ProbeAt(A11(0x4B7F7E), 5, PrPass); ProbeAt(A11(0x4B8009), 5, PrMat); ProbeAt(A11(0x4B8027), 6, PrDmg);
        Log("fire: ignite probes on");
    }
    if (Expect(A11(0x5224D9), "8B 45 00 B9 B8 D1 7B 01") && Expect(A11(0x66E570), "55 8B EC 83 E4 F8") && Expect(A11(0x4CFB1C), "85 C0 89 44 24 10")) {
        ProbeAt(A11(0x5224D9), 8, PrThr); ProbeAt(A11(0x66E570), 6, PrIgnNew); ProbeAt(A11(0x4CFB1C), 6, PrTrans);
        Log("fire: burn chain probes on");
        if (Expect(A11(0x522180), "83 EC 54 53 55")) ProbeAt(A11(0x522180), 5, PrHealth);
        if (Expect(A11(0x522A36), "8B 07 89 7D E8 89 45 FC") && Expect(A11(0x522A58), "83 C4 04 84 C0") && Expect(A11(0x522B70), "F3 0F 10 47 04")) {
            ProbeAt(A11(0x522A36), 8, PrMsg); ProbeAt(A11(0x522A58), 5, PrSkip); ProbeAt(A11(0x522B70), 5, PrHp);
            Log("fire: damage queue probes on");
        }
        if (Expect(A11(0x66C850), "55 8B EC 83 E4 F8") && Expect(A11(0x66E49A), "8B 5D 08 56 57") && Expect(A11(0x66E2B7), "0F 57 C0 83 C4 08") &&
            Expect(A11(0x469260), "83 EC 0C 53 56")) {
            ProbeAt(A11(0x66C850), 6, PrScrub); ProbeAt(A11(0x66E49A), 5, PrColor); ProbeAt(A11(0x66E2B7), 6, PrRedOut); ProbeAt(A11(0x469260), 5, PrFade);
            Log("fire: burnt look probes on");
        }
        if (Expect(A11(0x4B4964), "39 6C 24 5C 8B 44 24 54") && Expect(A11(0x4B4A2C), "A1 AC BB DF 00") && Expect(A11(0x4B3D90), "55 8B EC 83 E4 F0")) {
            ProbeAt(A11(0x4B4964), 8, PrEvt); ProbeAt(A11(0x4B4A2C), 5, PrK2); ProbeAt(A11(0x4B3D90), 6, PrChunk);
            Log("fire: debris probes on");
        }
        if (Expect(A11(0x4D2A50), "55 8B EC 83 E4 F0") && Expect(A11(0x4D2A8D), "8B 75 08 8B 4D 10") && Expect(A11(0x4D2ABA), "8B 4D 0C 51 8B C6") &&
            Expect(A11(0x4D2AD4), "8B 94 24 A8 00 00 00") && Expect(A11(0x4D2B1C), "8B 74 24 18 85 F6") && Expect(A11(0x4D2BD8), "F3 0F 10 46 1C") &&
            Expect(A11(0x4D2C1D), "D9 46 30 8B 46 28")) {
            ProbeAt(A11(0x4D2A50), 6, PrSp0); ProbeAt(A11(0x4D2A8D), 6, PrSp1); ProbeAt(A11(0x4D2ABA), 6, PrSp2); ProbeAt(A11(0x4D2AD4), 7, PrSp3);
            ProbeAt(A11(0x4D2B1C), 6, PrSp4); ProbeAt(A11(0x4D2BD8), 5, PrSp5); ProbeAt(A11(0x4D2C1D), 6, PrSp6);
            if (Expect(A11(0x4942B0), "8B 97 48 D7 01 00")) ProbeAt(A11(0x4942B0), 6, PrRender);
            if (Expect(A11(0x494ADA), "81 87 1C 02 00 00 90 00 00 00") && Expect(A11(0x494AEF), "5F 5E 5D 5B 83 C4 38")) {
                ProbeAt(A11(0x494ADA), 10, PrVxDone); ProbeAt(A11(0x494AEF), 7, PrVxEnd); Log("fire: particle vertex probes on"); }
            if (Expect(A11(0x676751), "80 89 08 08 00 00 01")) ProbeAt(A11(0x676751), 7, PrEffSet);
            if (Expect(A11(0x48926F), "8A 83 08 08 00 00")) ProbeAt(A11(0x48926F), 6, PrEffUpd);
            CreateThread(NULL, 0, EffTrackThread, NULL, 0, NULL);
            Log("fire: state effect spawn probes on");
        }
    } else Log("fire: ignite probe code mismatch");
#endif
#ifdef M2R_GAME11
    if (IniInt("Debug", "NativeStateMachine", 1)) { int MsmInstall(int); MsmInstall(7); }
#endif
    CloseHandle(CreateThread(NULL, 0, ProbeThread, NULL, 0, NULL));
    Log("fire: probes on (native ignitor query %d)", g_useNative);
    Log("fire: ShaderLevel %d/%d", *(BYTE *)0xDFC345, *(BYTE *)0xDFC346);   /* render device caps are logged by ProbeThread */
}
