/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Shader pre-warming. The driver turns a vertex/pixel shader pair into GPU code the first time it is drawn with
 * it (with the vertex layout, render target and texture formats bound at that moment), so the first frames of a
 * new place hitch for tens to hundreds of milliseconds (entering the mansion: frames of 350 - 410 ms with 20 - 40
 * new shader pairs). Every combination the game draws is recorded (shader bytecode, vertex declaration, render
 * target format, the type and format of each bound texture, alpha test / sRGB write / fog) in
 * Mercenaries2Remastered\shaders.bin. In later sessions, as soon as the shaders and declaration of a recorded combination
 * exist, it is drawn once - one triangle into a 16x16 target, with stand-in textures of the same type and format -
 * during a light frame (menu, loading screen), so the driver has built it before the game needs it.
 * [Performance] ShaderWarm. */
#define COBJMACROS
#include "m2r.h"
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include "d3dhook.h"

void *VtSwapPublic(void *obj, int idx, void *hook);
void StateCacheReset(void);

#define NSLOT 20                 /* pixel samplers 0..15, vertex samplers 0..3 */
#define MAXOBJ 32768
#define MAXENT 16384
#define MAXFMT 128

typedef unsigned long long u64;

/* ------------------------------------------------------------------ small hash maps */
typedef struct { u64 key; DWORD val; } Slot;
typedef struct { Slot *s; DWORD cap, n; } Map;          /* key 0 = empty */
static int MapInit(Map *m, DWORD cap) { m->s = (Slot *)VirtualAlloc(NULL, cap * sizeof(Slot), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); m->cap = cap; m->n = 0; return m->s != NULL; }
static u64 Mix(u64 x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x; }
static DWORD *MapFind(Map *m, u64 key)
{
    DWORD i = (DWORD)Mix(key) & (m->cap - 1);
    for (DWORD k = 0; k < m->cap; k++, i = (i + 1) & (m->cap - 1)) {
        if (m->s[i].key == key) return &m->s[i].val;
        if (!m->s[i].key) return NULL;
    }
    return NULL;
}
static int MapPut(Map *m, u64 key, DWORD val)        /* insert or replace */
{
    if (m->n * 4 >= m->cap * 3) return 0;
    DWORD i = (DWORD)Mix(key) & (m->cap - 1);
    for (;; i = (i + 1) & (m->cap - 1)) {
        if (m->s[i].key == key) { m->s[i].val = val; return 1; }
        if (!m->s[i].key) { m->s[i].key = key; m->s[i].val = val; m->n++; return 1; }
    }
}
static u64 Fnv(const void *p, size_t n, u64 h)
{
    const BYTE *b = (const BYTE *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ULL; }
    return h ? h : 1;
}

/* ------------------------------------------------------------------ objects and recorded combinations */
typedef struct { void *p; u64 hash; BYTE stride[8]; BYTE kind; } Obj;  /* kind 1 vs, 2 ps, 3 decl */
static Obj g_obj[MAXOBJ]; static volatile LONG g_nobj;
static Map g_byPtr, g_byHash;                           /* object pointer -> id, (kind, bytecode hash) -> id */
static CRITICAL_SECTION g_cs;

typedef struct { BYTE type; DWORD fmt; } Fmt;          /* texture type (D3DRTYPE) and format */
static Fmt g_fmt[MAXFMT]; static int g_nfmt = 1;        /* 0 = nothing bound */
static Map g_texCode;                                   /* texture pointer -> format code */

#pragma pack(push, 1)
typedef struct {
    u64 vs, ps, decl;
    DWORD rt, states;
    DWORD mrt[3], blend;                                 /* render targets 1..3, src | dst << 8 | colour write << 16 */
    BYTE ttype[NSLOT]; DWORD tfmt[NSLOT];
} Ent;
#pragma pack(pop)
static Ent g_ent[MAXENT]; static BYTE g_done[MAXENT]; static volatile LONG g_nent; static LONG g_nsaved;
static Map g_entSet, g_seen;                            /* entry hash -> index, draw key -> 1 */

static DWORD Find(void *p) { EnterCriticalSection(&g_cs); DWORD *v = MapFind(&g_byPtr, (u64)(DWORD)p); DWORD id = v ? *v : 0; LeaveCriticalSection(&g_cs); return id; }
static void Track(void *p, BYTE kind, u64 hash, const BYTE *stride)
{
    if (!p) return;
    EnterCriticalSection(&g_cs);
    DWORD *v = MapFind(&g_byHash, hash ^ kind);
    DWORD id;
    if (v) id = *v;                                     /* the same code created again: keep the first object */
    else if (g_nobj + 1 < MAXOBJ) {
        id = (DWORD)g_nobj + 1;
        g_obj[id].p = p; g_obj[id].hash = hash; g_obj[id].kind = kind;
        if (stride) memcpy(g_obj[id].stride, stride, 8);
        IUnknown_AddRef((IUnknown *)p);                 /* kept alive for warming */
        g_nobj = id;
        MapPut(&g_byHash, hash ^ kind, id);
    } else { LeaveCriticalSection(&g_cs); return; }
    MapPut(&g_byPtr, (u64)(DWORD)p, id);
    LeaveCriticalSection(&g_cs);
}
static DWORD CodeSize(const DWORD *c)
{
    const DWORD *p = c + 1;
    int sm2 = (c[0] & 0xFF00) >= 0x0200;
    for (int guard = 0; guard < 65536; guard++) {
        DWORD t = *p;
        if (t == 0x0000FFFF) return (DWORD)((p + 1 - c) * 4);
        if ((t & 0xFFFF) == 0xFFFE) { p += 1 + (t >> 16); continue; }
        if (sm2) p += 1 + ((t >> 24) & 0xF);
        else { p++; while (*p & 0x80000000) p++; }
    }
    return 0;
}

/* ------------------------------------------------------------------ device hooks */
typedef HRESULT (STDMETHODCALLTYPE *CreateVS_t)(IDirect3DDevice9 *, const DWORD *, IDirect3DVertexShader9 **);
typedef HRESULT (STDMETHODCALLTYPE *CreatePS_t)(IDirect3DDevice9 *, const DWORD *, IDirect3DPixelShader9 **);
typedef HRESULT (STDMETHODCALLTYPE *CreateDecl_t)(IDirect3DDevice9 *, const D3DVERTEXELEMENT9 *, IDirect3DVertexDeclaration9 **);
typedef HRESULT (STDMETHODCALLTYPE *SetPtr_t)(IDirect3DDevice9 *, void *);
typedef HRESULT (STDMETHODCALLTYPE *SetFVF_t)(IDirect3DDevice9 *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *SetTex_t)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
typedef HRESULT (STDMETHODCALLTYPE *SetRT_t)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
typedef HRESULT (STDMETHODCALLTYPE *SetRS_t)(IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *DP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DIP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
static CreateVS_t o_CreateVS; static CreatePS_t o_CreatePS; static CreateDecl_t o_CreateDecl;
static SetPtr_t o_SetVS, o_SetPS, o_SetDecl; static SetFVF_t o_SetFVF; static SetTex_t o_SetTex; static SetRT_t o_SetRT;
static SetRS_t o_SetRS; static DP_t o_DP; static DIP_t o_DIP;

static DWORD g_vs, g_ps, g_decl, g_rtFmt, g_states, g_mrt[3], g_src = 2, g_dst = 1, g_cw = 0xF; static BYTE g_tex[NSLOT]; static u64 g_texSig;
static DWORD g_thread; static int g_draws, g_prevDraws, g_on;

static HRESULT STDMETHODCALLTYPE hk_CreateVS(IDirect3DDevice9 *d, const DWORD *c, IDirect3DVertexShader9 **o)
{
    HRESULT hr = o_CreateVS(d, c, o);
    DWORD n;
    if (SUCCEEDED(hr) && o && *o && c && (n = CodeSize(c))) Track(*o, 1, Fnv(c, n, 0xcbf29ce484222325ULL), NULL);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_CreatePS(IDirect3DDevice9 *d, const DWORD *c, IDirect3DPixelShader9 **o)
{
    HRESULT hr = o_CreatePS(d, c, o);
    DWORD n;
    if (SUCCEEDED(hr) && o && *o && c && (n = CodeSize(c))) Track(*o, 2, Fnv(c, n, 0xcbf29ce484222325ULL), NULL);
    return hr;
}
static const BYTE g_typeSize[] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 8, 4, 8};
static HRESULT STDMETHODCALLTYPE hk_CreateDecl(IDirect3DDevice9 *d, const D3DVERTEXELEMENT9 *e, IDirect3DVertexDeclaration9 **o)
{
    HRESULT hr = o_CreateDecl(d, e, o);
    if (SUCCEEDED(hr) && o && *o && e) {
        int n = 0; BYTE stride[8] = {0};
        while (n < 64 && e[n].Stream != 0xFF) {
            if (e[n].Stream < 8 && e[n].Type < sizeof g_typeSize) {
                int end = e[n].Offset + g_typeSize[e[n].Type];
                if (end > stride[e[n].Stream]) stride[e[n].Stream] = (BYTE)(end > 255 ? 255 : end);
            }
            n++;
        }
        Track(*o, 3, Fnv(e, (n + 1) * sizeof *e, 0xcbf29ce484222325ULL), stride);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_SetVS(IDirect3DDevice9 *d, void *p) { g_vs = p ? Find(p) : 0; return o_SetVS(d, p); }
static HRESULT STDMETHODCALLTYPE hk_SetPS(IDirect3DDevice9 *d, void *p) { g_ps = p ? Find(p) : 0; return o_SetPS(d, p); }
static HRESULT STDMETHODCALLTYPE hk_SetDecl(IDirect3DDevice9 *d, void *p) { g_decl = p ? Find(p) : 0; return o_SetDecl(d, p); }
static HRESULT STDMETHODCALLTYPE hk_SetFVF(IDirect3DDevice9 *d, DWORD f) { g_decl = 0; return o_SetFVF(d, f); }

static BYTE TexCode(IDirect3DBaseTexture9 *t)
{
    DWORD *v = MapFind(&g_texCode, (u64)(DWORD)t);
    if (v) return (BYTE)*v;
    Fmt f = {0, 0};
    D3DRESOURCETYPE ty = IDirect3DBaseTexture9_GetType(t);
    if (ty == D3DRTYPE_TEXTURE) { D3DSURFACE_DESC s; if (SUCCEEDED(IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)t, 0, &s))) { f.type = 3; f.fmt = s.Format; } }
    else if (ty == D3DRTYPE_CUBETEXTURE) { D3DSURFACE_DESC s; if (SUCCEEDED(IDirect3DCubeTexture9_GetLevelDesc((IDirect3DCubeTexture9 *)t, 0, &s))) { f.type = 5; f.fmt = s.Format; } }
    else if (ty == D3DRTYPE_VOLUMETEXTURE) { D3DVOLUME_DESC s; if (SUCCEEDED(IDirect3DVolumeTexture9_GetLevelDesc((IDirect3DVolumeTexture9 *)t, 0, &s))) { f.type = 4; f.fmt = s.Format; } }
    int c;
    for (c = 1; c < g_nfmt; c++) if (g_fmt[c].type == f.type && g_fmt[c].fmt == f.fmt) break;
    if (c == g_nfmt) { if (g_nfmt >= MAXFMT || !f.type) return 0; g_fmt[g_nfmt++] = f; }
    MapPut(&g_texCode, (u64)(DWORD)t, (DWORD)c);
    return (BYTE)c;
}
static int SlotOf(DWORD s) { return s < 16 ? (int)s : (s >= D3DVERTEXTEXTURESAMPLER0 && s <= D3DVERTEXTEXTURESAMPLER3) ? 16 + (int)(s - D3DVERTEXTEXTURESAMPLER0) : -1; }
static HRESULT STDMETHODCALLTYPE hk_SetTex(IDirect3DDevice9 *d, DWORD s, IDirect3DBaseTexture9 *t)
{
    int i = SlotOf(s);
    if (i >= 0 && GetCurrentThreadId() == g_thread) {
        BYTE c = t ? TexCode(t) : 0;
        if (g_tex[i] != c) { g_tex[i] = c; g_texSig = Fnv(g_tex, NSLOT, 0xcbf29ce484222325ULL); }
    }
    return o_SetTex(d, s, t);
}
static HRESULT STDMETHODCALLTYPE hk_SetRT(IDirect3DDevice9 *d, DWORD i, IDirect3DSurface9 *s)
{
    if (i < 4) {
        D3DSURFACE_DESC sd; DWORD f = s && SUCCEEDED(IDirect3DSurface9_GetDesc(s, &sd)) ? (DWORD)sd.Format : 0;
        if (i == 0) g_rtFmt = f; else g_mrt[i - 1] = f;
    }
    return o_SetRT(d, i, s);
}
static HRESULT STDMETHODCALLTYPE hk_SetRS(IDirect3DDevice9 *d, D3DRENDERSTATETYPE st, DWORD v)
{
    DWORD bit = st == D3DRS_ALPHATESTENABLE ? 1 : st == D3DRS_SRGBWRITEENABLE ? 2 : st == D3DRS_FOGENABLE ? 4 : st == D3DRS_ALPHABLENDENABLE ? 8 : 0;
    if (bit) g_states = v ? g_states | bit : g_states & ~bit;
    else if (st == D3DRS_SRCBLEND) g_src = v;
    else if (st == D3DRS_DESTBLEND) g_dst = v;
    else if (st == D3DRS_COLORWRITEENABLE) g_cw = v;
    return o_SetRS(d, st, v);
}

/* a draw: record its combination when new */
static void Seen(void)
{
    g_draws++;
    if (!g_vs || !g_ps || !g_decl || GetCurrentThreadId() != g_thread) return;
    DWORD blend = (g_states & 8) ? (g_src & 0xFF) | (g_dst & 0xFF) << 8 | (g_cw & 0xF) << 16 : (g_cw & 0xF) << 16;
    u64 key = Mix(((u64)g_vs << 40) ^ ((u64)g_ps << 20) ^ g_decl ^ ((u64)g_rtFmt << 52) ^ ((u64)g_states << 61)) ^ g_texSig ^
              Mix(((u64)blend << 32) ^ g_mrt[0] ^ ((u64)g_mrt[1] << 12) ^ ((u64)g_mrt[2] << 24));
    if (!key) key = 1;
    if (MapFind(&g_seen, key)) return;
    MapPut(&g_seen, key, 1);
    Ent e; memset(&e, 0, sizeof e);
    e.vs = g_obj[g_vs].hash; e.ps = g_obj[g_ps].hash; e.decl = g_obj[g_decl].hash;
    e.rt = g_rtFmt; e.states = g_states; e.blend = blend; memcpy(e.mrt, g_mrt, sizeof e.mrt);
    for (int i = 0; i < NSLOT; i++) { e.ttype[i] = g_fmt[g_tex[i]].type; e.tfmt[i] = g_fmt[g_tex[i]].fmt; }
    u64 h = Fnv(&e, sizeof e, 0xcbf29ce484222325ULL);
    DWORD *v = MapFind(&g_entSet, h);
    if (v) { g_done[*v] = 1; return; }
    if (g_nent >= MAXENT) return;
    LONG n = g_nent;
    g_ent[n] = e; g_done[n] = 1;
    MapPut(&g_entSet, h, (DWORD)n);
    InterlockedExchange(&g_nent, n + 1);
}
static HRESULT STDMETHODCALLTYPE hk_DP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT s, UINT n) { Seen(); return o_DP(d, t, s, n); }
static HRESULT STDMETHODCALLTYPE hk_DIP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT si, UINT pc) { Seen(); return o_DIP(d, t, b, mi, nv, si, pc); }

/* ------------------------------------------------------------------ warming */
typedef struct { DWORD fmt; IDirect3DTexture9 *tex; IDirect3DSurface9 *surf; int failed; } RtEnt;
static RtEnt g_rt[32]; static int g_nrt;
typedef struct { BYTE type; DWORD fmt; IDirect3DBaseTexture9 *tex; int failed, def; } TexEnt;
static TexEnt g_stand[MAXFMT]; static int g_nstand;
static IDirect3DSurface9 *g_ds; static IDirect3DVertexBuffer9 *g_vb; static IDirect3DStateBlock9 *g_sb;
static int g_warmed, g_skipped;

static int IsDepth(DWORD f) { return f == D3DFMT_D24S8 || f == D3DFMT_D24X8 || f == D3DFMT_D16 || f == D3DFMT_D32 || f == MAKEFOURCC('I','N','T','Z') || f == MAKEFOURCC('D','F','2','4') || f == MAKEFOURCC('D','F','1','6') || f == MAKEFOURCC('R','A','W','Z'); }
static IDirect3DSurface9 *RtFor(IDirect3DDevice9 *d, DWORD fmt)
{
    for (int i = 0; i < g_nrt; i++) if (g_rt[i].fmt == fmt) return g_rt[i].failed ? NULL : g_rt[i].surf;
    if (g_nrt >= 32) return NULL;
    RtEnt *r = &g_rt[g_nrt++]; memset(r, 0, sizeof *r); r->fmt = fmt;
    if (FAILED(IDirect3DDevice9_CreateTexture(d, 16, 16, 1, D3DUSAGE_RENDERTARGET, (D3DFORMAT)fmt, D3DPOOL_DEFAULT, &r->tex, NULL)) ||
        FAILED(IDirect3DTexture9_GetSurfaceLevel(r->tex, 0, &r->surf))) { r->failed = 1; return NULL; }
    return r->surf;
}
static IDirect3DBaseTexture9 *StandIn(IDirect3DDevice9 *d, BYTE type, DWORD fmt)
{
    if (!type) return NULL;
    for (int i = 0; i < g_nstand; i++) if (g_stand[i].type == type && g_stand[i].fmt == fmt) return g_stand[i].failed ? NULL : g_stand[i].tex;
    if (g_nstand >= MAXFMT) return NULL;
    TexEnt *t = &g_stand[g_nstand++]; memset(t, 0, sizeof *t); t->type = type; t->fmt = fmt;
    HRESULT hr = E_FAIL;
    if (type == 3) {
        IDirect3DTexture9 *x = NULL;
        if (IsDepth(fmt)) { hr = IDirect3DDevice9_CreateTexture(d, 4, 4, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)fmt, D3DPOOL_DEFAULT, &x, NULL); t->def = 1; }
        else {
            hr = IDirect3DDevice9_CreateTexture(d, 4, 4, 1, 0, (D3DFORMAT)fmt, D3DPOOL_MANAGED, &x, NULL);
            if (FAILED(hr)) { hr = IDirect3DDevice9_CreateTexture(d, 4, 4, 1, D3DUSAGE_RENDERTARGET, (D3DFORMAT)fmt, D3DPOOL_DEFAULT, &x, NULL); t->def = 1; }
        }
        t->tex = (IDirect3DBaseTexture9 *)x;
    } else if (type == 5) {
        IDirect3DCubeTexture9 *x = NULL;
        hr = IDirect3DDevice9_CreateCubeTexture(d, 4, 1, 0, (D3DFORMAT)fmt, D3DPOOL_MANAGED, &x, NULL);
        if (FAILED(hr)) { hr = IDirect3DDevice9_CreateCubeTexture(d, 4, 1, D3DUSAGE_RENDERTARGET, (D3DFORMAT)fmt, D3DPOOL_DEFAULT, &x, NULL); t->def = 1; }
        t->tex = (IDirect3DBaseTexture9 *)x;
    } else if (type == 4) {
        IDirect3DVolumeTexture9 *x = NULL;
        hr = IDirect3DDevice9_CreateVolumeTexture(d, 4, 4, 4, 1, 0, (D3DFORMAT)fmt, D3DPOOL_MANAGED, &x, NULL);
        t->tex = (IDirect3DBaseTexture9 *)x;
    }
    if (FAILED(hr) || !t->tex) { t->failed = 1; t->tex = NULL; }
    return t->tex;
}
static void ReleaseDefault(void)
{
    for (int i = 0; i < g_nrt; i++) { if (g_rt[i].surf) IDirect3DSurface9_Release(g_rt[i].surf); if (g_rt[i].tex) IDirect3DTexture9_Release(g_rt[i].tex); }
    g_nrt = 0;
    int k = 0;
    for (int i = 0; i < g_nstand; i++) {
        if (g_stand[i].def) { if (g_stand[i].tex) IDirect3DBaseTexture9_Release(g_stand[i].tex); }
        else g_stand[k++] = g_stand[i];
    }
    g_nstand = k;
    if (g_ds) { IDirect3DSurface9_Release(g_ds); g_ds = NULL; }
    if (g_sb) { IDirect3DStateBlock9_Release(g_sb); g_sb = NULL; }
}

static DWORD LiveId(u64 hash, BYTE kind) { EnterCriticalSection(&g_cs); DWORD *v = MapFind(&g_byHash, hash ^ kind); DWORD id = v ? *v : 0; LeaveCriticalSection(&g_cs); return id; }

static void Warm(IDirect3DDevice9 *d)
{
    LONG n = g_nent;
    int ready = 0;
    for (LONG i = 0; i < n && !ready; i++) if (!g_done[i] && LiveId(g_ent[i].vs, 1) && LiveId(g_ent[i].ps, 2) && LiveId(g_ent[i].decl, 3)) ready = 1;
    if (!ready) return;
    if (!g_vb) {
        if (FAILED(IDirect3DDevice9_CreateVertexBuffer(d, 4096, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &g_vb, NULL))) { g_on = 0; return; }
        void *p; if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(g_vb, 0, 0, &p, 0))) { memset(p, 0, 4096); IDirect3DVertexBuffer9_Unlock(g_vb); }
    }
    if (!g_ds && FAILED(IDirect3DDevice9_CreateDepthStencilSurface(d, 16, 16, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_ds, NULL))) return;
    if (!g_sb && FAILED(IDirect3DDevice9_CreateStateBlock(d, D3DSBT_ALL, &g_sb))) return;
    IDirect3DSurface9 *rt[4] = {0}, *ds = NULL;
    for (int i = 0; i < 4; i++) IDirect3DDevice9_GetRenderTarget(d, i, &rt[i]);
    IDirect3DDevice9_GetDepthStencilSurface(d, &ds);
    IDirect3DStateBlock9_Capture(g_sb);
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    int made = 0;
    if (SUCCEEDED(IDirect3DDevice9_BeginScene(d))) {
        for (int i = 1; i < 4; i++) o_SetRT(d, i, NULL);
        IDirect3DDevice9_SetDepthStencilSurface(d, g_ds);
        IDirect3DDevice9_SetRenderState(d, D3DRS_ZENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(d, D3DRS_CULLMODE, D3DCULL_NONE);
        for (int s = 0; s < 8; s++) IDirect3DDevice9_SetStreamSourceFreq(d, s, 1);
        for (LONG i = 0; i < n; i++) {
            if (g_done[i]) continue;
            Ent *e = &g_ent[i];
            DWORD vs = LiveId(e->vs, 1), ps = LiveId(e->ps, 2), dc = LiveId(e->decl, 3);
            if (!vs || !ps || !dc) continue;
            g_done[i] = 1;
            IDirect3DSurface9 *s = RtFor(d, e->rt);
            if (!s) { g_skipped++; continue; }
            o_SetRT(d, 0, s);
            for (int k = 0; k < 3; k++) o_SetRT(d, k + 1, e->mrt[k] ? RtFor(d, e->mrt[k]) : NULL);
            o_SetDecl(d, g_obj[dc].p); o_SetVS(d, g_obj[vs].p); o_SetPS(d, g_obj[ps].p);
            for (int k = 0; k < 8; k++)
                IDirect3DDevice9_SetStreamSource(d, k, g_obj[dc].stride[k] ? g_vb : NULL, 0, g_obj[dc].stride[k] ? g_obj[dc].stride[k] : 0);
            for (int k = 0; k < NSLOT; k++)
                o_SetTex(d, k < 16 ? (DWORD)k : D3DVERTEXTEXTURESAMPLER0 + (k - 16), StandIn(d, e->ttype[k], e->tfmt[k]));
            o_SetRS(d, D3DRS_ALPHATESTENABLE, e->states & 1); o_SetRS(d, D3DRS_SRGBWRITEENABLE, (e->states >> 1) & 1);
            o_SetRS(d, D3DRS_FOGENABLE, (e->states >> 2) & 1);
            o_SetRS(d, D3DRS_ALPHABLENDENABLE, (e->states >> 3) & 1);
            if (e->states & 8) { o_SetRS(d, D3DRS_SRCBLEND, e->blend & 0xFF); o_SetRS(d, D3DRS_DESTBLEND, (e->blend >> 8) & 0xFF); }
            o_SetRS(d, D3DRS_COLORWRITEENABLE, (e->blend >> 16) & 0xF);
            o_DP(d, D3DPT_TRIANGLELIST, 0, 1);
            made++;
            QueryPerformanceCounter(&t);
            if ((t.QuadPart - t0.QuadPart) * 1000 > f.QuadPart * 150) break;   /* at most 150 ms per frame */
        }
        IDirect3DDevice9_EndScene(d);
    }
    for (int i = 0; i < 4; i++) { o_SetRT(d, i, rt[i]); if (rt[i]) IDirect3DSurface9_Release(rt[i]); }
    IDirect3DDevice9_SetDepthStencilSurface(d, ds); if (ds) IDirect3DSurface9_Release(ds);
    IDirect3DStateBlock9_Apply(g_sb);
    StateCacheReset();
    if (made) {
        g_warmed += made;
        QueryPerformanceCounter(&t);
        Log("shaderwarm: %d combinations prepared in %.0f ms (%d in all, %d skipped)", made, (t.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart, g_warmed, g_skipped);
    }
}

static void OnFrame(IDirect3DDevice9 *d)
{
    g_thread = GetCurrentThreadId();
    g_prevDraws = g_draws; g_draws = 0;
    if (g_on && g_prevDraws < 100) Warm(d);
}
static void OnReset(void) { ReleaseDefault(); }

/* ------------------------------------------------------------------ file */
static char g_path[MAX_PATH];
#define MAGIC 0x32574853u                               /* "SHW2" */
static DWORD WINAPI Saver(LPVOID u)
{
    (void)u;
    for (;;) {
        Sleep(5000);
        LONG n = g_nent;
        if (n == g_nsaved) continue;
        char tmp[MAX_PATH]; _snprintf(tmp, sizeof tmp, "%s.tmp", g_path); tmp[MAX_PATH - 1] = 0;
        HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD hdr[2] = {MAGIC, (DWORD)sizeof(Ent)}, wr;
        BOOL ok = WriteFile(h, hdr, sizeof hdr, &wr, NULL) && WriteFile(h, g_ent, (DWORD)(n * sizeof(Ent)), &wr, NULL);
        CloseHandle(h);
        if (ok && MoveFileExA(tmp, g_path, MOVEFILE_REPLACE_EXISTING)) g_nsaved = n;
    }
    return 0;
}
static void Load(void)
{
    HANDLE h = CreateFileA(g_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD hdr[2], rd, sz = GetFileSize(h, NULL);
    if (ReadFile(h, hdr, sizeof hdr, &rd, NULL) && rd == sizeof hdr && hdr[0] == MAGIC && hdr[1] == sizeof(Ent)) {
        LONG n = (LONG)((sz - sizeof hdr) / sizeof(Ent));
        if (n > MAXENT) n = MAXENT;
        if (ReadFile(h, g_ent, (DWORD)(n * sizeof(Ent)), &rd, NULL) && rd == n * sizeof(Ent)) {
            LONG k = 0;
            for (LONG i = 0; i < n; i++) {
                u64 hs = Fnv(&g_ent[i], sizeof(Ent), 0xcbf29ce484222325ULL);
                if (MapFind(&g_entSet, hs)) continue;
                g_ent[k] = g_ent[i]; MapPut(&g_entSet, hs, (DWORD)k); k++;
            }
            g_nent = g_nsaved = k;
        }
    }
    CloseHandle(h);
}

void ShaderWarmDevice(IDirect3DDevice9 *d)
{
    if (!g_on || o_CreateVS) return;
    void *p;
    if ((p = VtSwapPublic(d, 91, (void *)hk_CreateVS)))   o_CreateVS = (CreateVS_t)p;
    if ((p = VtSwapPublic(d, 106, (void *)hk_CreatePS)))  o_CreatePS = (CreatePS_t)p;
    if ((p = VtSwapPublic(d, 86, (void *)hk_CreateDecl))) o_CreateDecl = (CreateDecl_t)p;
    if ((p = VtSwapPublic(d, 92, (void *)hk_SetVS)))      o_SetVS = (SetPtr_t)p;
    if ((p = VtSwapPublic(d, 107, (void *)hk_SetPS)))     o_SetPS = (SetPtr_t)p;
    if ((p = VtSwapPublic(d, 87, (void *)hk_SetDecl)))    o_SetDecl = (SetPtr_t)p;
    if ((p = VtSwapPublic(d, 89, (void *)hk_SetFVF)))     o_SetFVF = (SetFVF_t)p;
    if ((p = VtSwapPublic(d, 65, (void *)hk_SetTex)))     o_SetTex = (SetTex_t)p;
    if ((p = VtSwapPublic(d, 37, (void *)hk_SetRT)))      o_SetRT = (SetRT_t)p;
    if ((p = VtSwapPublic(d, 57, (void *)hk_SetRS)))      o_SetRS = (SetRS_t)p;
    if ((p = VtSwapPublic(d, 81, (void *)hk_DP)))         o_DP = (DP_t)p;
    if ((p = VtSwapPublic(d, 82, (void *)hk_DIP)))        o_DIP = (DIP_t)p;
    if (!o_CreateVS || !o_CreatePS || !o_CreateDecl || !o_SetVS || !o_SetPS || !o_SetDecl || !o_SetFVF || !o_SetTex || !o_SetRT ||
        !o_SetRS || !o_DP || !o_DIP) { Log("shaderwarm: some hooks were not installed, off"); g_on = 0; return; }
    D3DAddFrameCallback(OnFrame);
    D3DAddResetCallback(OnReset);
    Log("shaderwarm: %ld recorded shader combinations", g_nent);
}
void ShaderWarmInit(void)
{
    g_on = IniInt("Performance", "ShaderWarm", 1);
    if (!g_on) return;
    InitializeCriticalSection(&g_cs);
    if (!MapInit(&g_byPtr, 1 << 17) || !MapInit(&g_byHash, 1 << 17) || !MapInit(&g_texCode, 1 << 17) || !MapInit(&g_entSet, 1 << 16) ||
        !MapInit(&g_seen, 1 << 17)) { g_on = 0; return; }
    _snprintf(g_path, sizeof g_path, "%s\\Mercenaries2Remastered\\shaders.bin", g_gameDir); g_path[MAX_PATH - 1] = 0;
    Load();
    CloseHandle(CreateThread(NULL, 0, Saver, NULL, 0, NULL));
}
