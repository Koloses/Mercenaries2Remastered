/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Asset manager tables without a hard limit.
 *
 * Four asset managers (scrub models 0x600B904E among them; vtables 0xBB0CE8, 0xBB0E08, 0xBB0F28, 0xBB0FDC)
 * keep their loaded assets in a fixed open-addressing table of 256 entries: count at +4, values at +0xC, keys
 * at +0x40C. Inserting into a full table (FUN_00632250) probes forever, so the game froze when more than 256
 * assets of one kind were resident - e.g. scrub after a helicopter transfer, with the old and the new area
 * loaded at once. Entries beyond 256 now go to an overflow list; the managers' find / get-or-create / release /
 * count / by-index methods (vtable slots 2-6) see both. */
#include "m2r.h"

#define OVF 2048
typedef struct { BYTE *mgr; DWORD key; void *obj; } Ovf;
static Ovf g_ovf[OVF];
static int g_novf, g_logged;
static CRITICAL_SECTION g_lock;

typedef void *(__attribute__((thiscall)) *Find_t)(BYTE *, DWORD *);
typedef void  (__attribute__((thiscall)) *Rel_t)(BYTE *, void *);
typedef DWORD (__attribute__((thiscall)) *Type_t)(BYTE *);
static Find_t o_find = (Find_t)0x4CB5D0, o_get = (Find_t)0x4CA8D0;
static Rel_t  o_rel = (Rel_t)0x4CA950;
static DWORD (*o_byIndex)(void) = (void *)0x4CBA20;

#define VT(m) (*(DWORD **)(m))
static DWORD Type(BYTE *m) { return ((Type_t)VT(m)[1])(m); }
static int Count(BYTE *m) { return *(int *)(m + 4); }
static int InTable(BYTE *m, DWORD key)
{
    DWORD *keys = (DWORD *)(m + 0x40C);
    if (!key) return 0;
    for (int i = 0, j = key & 0xFF; i < 256; i++, j = (j + 1) & 0xFF) {
        if (keys[j] == key) return 1;
        if (!keys[j]) return 0;
    }
    return 0;
}
static int OvfFind(BYTE *m, DWORD key) { for (int i = 0; i < g_novf; i++) if (g_ovf[i].mgr == m && g_ovf[i].key == key) return i; return -1; }

static void *__attribute__((thiscall)) Find(BYTE *m, DWORD *desc)
{
    void *r = o_find(m, desc);
    if (r || !g_novf || desc[1] != Type(m)) return r;
    EnterCriticalSection(&g_lock);
    int i = OvfFind(m, desc[0]);
    if (i >= 0) { r = g_ovf[i].obj; *(WORD *)((BYTE *)r + 0x16) |= 0x4000; }
    LeaveCriticalSection(&g_lock);
    return r;
}

static void *__attribute__((thiscall)) GetOrCreate(BYTE *m, DWORD *desc)
{
    if (desc[1] != Type(m)) return o_get(m, desc);
    void *r = NULL;
    if (g_novf) {                                            /* already in the overflow list */
        EnterCriticalSection(&g_lock);
        int i = OvfFind(m, desc[0]);
        if (i >= 0) r = g_ovf[i].obj;
        LeaveCriticalSection(&g_lock);
        if (r) return r;
    }
    if (Count(m) < 256 || InTable(m, desc[0])) return o_get(m, desc);
    r = ((Find_t)VT(m)[7])(m, desc);                      /* create */
    if (!r) return r;
    EnterCriticalSection(&g_lock);
    if (g_novf < OVF) {
        g_ovf[g_novf].mgr = m; g_ovf[g_novf].key = desc[0]; g_ovf[g_novf].obj = r; g_novf++;
        if (!g_logged) { g_logged = 1; Log("assets: manager %08lX (type %08lX) has more than 256 assets, overflow list in use", (DWORD)m, desc[1]); }
    } else Log("assets: overflow list full");
    LeaveCriticalSection(&g_lock);
    return r;
}

static void __attribute__((thiscall)) Release(BYTE *m, void *obj)
{
    int i;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < g_novf && !(g_ovf[i].mgr == m && g_ovf[i].obj == obj); i++) ;
    int ovf = i < g_novf;
    if (ovf) g_ovf[i] = g_ovf[--g_novf];
    LeaveCriticalSection(&g_lock);
    if (ovf) ((Rel_t)VT(m)[8])(m, obj);                    /* destroy */
    else o_rel(m, obj);
}

static DWORD __attribute__((thiscall)) Capacity(BYTE *m) { (void)m; return 256 + OVF; }

static void *__attribute__((thiscall)) ByIndex(BYTE *m, int idx)
{
    if (idx < 256) return ((void *(__attribute__((thiscall)) *)(BYTE *, int))o_byIndex)(m, idx);
    void *r = NULL;
    EnterCriticalSection(&g_lock);
    for (int i = 0, k = 256; i < g_novf; i++)
        if (g_ovf[i].mgr == m && k++ == idx) { r = g_ovf[i].obj; break; }
    LeaveCriticalSection(&g_lock);
    return r;
}

void AssetTableInit(void)
{
    static const DWORD vts[] = {0xBB0CE8, 0xBB0E08, 0xBB0F28, 0xBB0FDC};
    static const DWORD orig[5] = {0x4CB5D0, 0x4CA8D0, 0x4CA950, 0x4CA980, 0x4CBA20};
    void *mine[5] = {(void *)Find, (void *)GetOrCreate, (void *)Release, (void *)Capacity, (void *)ByIndex};
    InitializeCriticalSection(&g_lock);
    int n = 0;
    for (unsigned v = 0; v < 4; v++) {
        DWORD *slot = (DWORD *)(vts[v] + 8);
        int ok = 1;
        for (int s = 0; s < 5; s++) if (slot[s] != orig[s]) ok = 0;
        if (!ok) { Log("assets: vtable %08lX unexpected", vts[v]); continue; }
        for (int s = 0; s < 5; s++) WritePtr((DWORD)&slot[s], mine[s]);
        n++;
    }
    Log("assets: %d asset managers without the 256-entry limit", n);
}
