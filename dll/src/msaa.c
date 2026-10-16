/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* MSAA for the 3D scene ([Fixes] AAFix).
 *
 * The game's Anti-Aliasing option only multisamples the back buffer: the scene is drawn into full-size render
 * target textures (A16B16G16R16F colour, G32R32F linear depth from the depth pre-pass) that cannot be
 * multisampled, and with the option on the renderer even creates a separate non-multisampled depth buffer for
 * them (0x74A1EB). Only the HUD got the samples, and in a window (borderless) the option is dropped altogether
 * (0x754EC9 / 0x7552C1 set PresentParameters.MultiSampleType to 0, the level index stays at 0xDFC33C).
 *
 * Here every full-size render target texture of those formats gets a multisampled twin surface, and so does the
 * full-size depth buffer bound with them. While the game draws into such targets with that depth buffer, the
 * twins are bound instead; the depth pre-pass, the scene, water and particles all draw multisampled. A twin is
 * resolved into its texture (StretchRect) when the texture is read: bound to a sampler for a draw, the source of
 * a StretchRect / GetRenderTargetData, or drawn to without the depth buffer. GetRenderTarget /
 * GetDepthStencilSurface return the game's surfaces. The back buffer stays single-sampled (SMAA works on it).
 *
 * The level is the game's Anti-Aliasing option (captured in FUN_007554B0 before the windowed check).
 * [Fixes] AAFix=0 leaves the game's own behaviour. These hooks are installed first, below all the other modules'
 * device hooks, so they all see the game's surfaces. */
#define COBJMACROS
#include "m2r.h"
#include <d3d9.h>
#include <string.h>
#include "d3dhook.h"

void *VtSwapPublic(void *obj, int idx, void *hook);

typedef HRESULT (STDMETHODCALLTYPE *SRT_t)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
typedef HRESULT (STDMETHODCALLTYPE *GRT_t)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 **);
typedef HRESULT (STDMETHODCALLTYPE *SDS_t)(IDirect3DDevice9 *, IDirect3DSurface9 *);
typedef HRESULT (STDMETHODCALLTYPE *GDS_t)(IDirect3DDevice9 *, IDirect3DSurface9 **);
typedef HRESULT (STDMETHODCALLTYPE *Clear_t)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *STX_t)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
typedef HRESULT (STDMETHODCALLTYPE *SR_t)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE);
typedef HRESULT (STDMETHODCALLTYPE *GRTD_t)(IDirect3DDevice9 *, IDirect3DSurface9 *, IDirect3DSurface9 *);
typedef HRESULT (STDMETHODCALLTYPE *RS_t)(IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *DP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DIP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DPUP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DIPUP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);
typedef HRESULT (STDMETHODCALLTYPE *CRT_t)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **, HANDLE *);

static SRT_t o_SRT; static GRT_t o_GRT; static SDS_t o_SDS; static GDS_t o_GDS; static Clear_t o_Clear;
static STX_t o_STX; static SR_t o_SR; static GRTD_t o_GRTD; static RS_t o_RS;
static DP_t o_DP; static DIP_t o_DIP; static DPUP_t o_DPUP; static DIPUP_t o_DIPUP;
static CRT_t r_CreateRT, r_CreateDS;          /* the runtime's own, below gfx.c's single-sample override */

/* ---- twins ---- */
#define MAXE 24
typedef struct Ent {
    IUnknown unk;                              /* private data object on the game's surface (owns the twin) */
    LONG ref;
    IDirect3DSurface9 *orig, *twin;
    IDirect3DBaseTexture9 *tex;                /* container (not referenced) */
    D3DMULTISAMPLE_TYPE ms;
    int depth, dirty, stale, clear, used;
} Ent;
static Ent g_e[MAXE];
static int g_dirty;                            /* number of dirty twins */
static const GUID kGuid = {0x6d2a1f3c, 0x5b7e, 0x4c11, {0x9a, 0x3e, 0x51, 0x8c, 0x0d, 0x27, 0x44, 0x9b}};

static HRESULT STDMETHODCALLTYPE E_QI(IUnknown *u, REFIID r, void **o) { (void)r; *o = u; u->lpVtbl->AddRef(u); return S_OK; }
static ULONG STDMETHODCALLTYPE E_AddRef(IUnknown *u) { return (ULONG)InterlockedIncrement(&((Ent *)u)->ref); }
static ULONG STDMETHODCALLTYPE E_Release(IUnknown *u)
{
    Ent *e = (Ent *)u;
    LONG r = InterlockedDecrement(&e->ref);
    if (r == 0) {                               /* the game's surface is gone (or reset) */
        if (e->dirty) g_dirty--;
        if (e->twin) IDirect3DSurface9_Release(e->twin);
        memset(e, 0, sizeof *e);
    }
    return (ULONG)r;
}
static IUnknownVtbl kEntVt = {E_QI, E_AddRef, E_Release};

static D3DMULTISAMPLE_TYPE g_level;           /* in effect, 0 = off */
int g_aaFix = 1;                               /* [Fixes] AAFix */
int g_gameMsaa;                                /* the game's Anti-Aliasing option (multisample type) */
int MsaaLevel(void);
static int g_okFmt;                            /* both scene formats support g_level */
static UINT g_bbW, g_bbH;
static IDirect3DSurface9 *g_rt[4], *g_ds;      /* the game's bindings (referenced, like the device would) */
static IDirect3DSurface9 *g_art[4], *g_ads;    /* what is really bound (not referenced) */
static int g_mode, g_sync = 1;
static IDirect3DBaseTexture9 *g_tx[20];        /* bound textures, pixel 0..15, vertex 16..19 */
static IDirect3DDevice9 *g_d;

static Ent *Find(IDirect3DSurface9 *s)
{
    if (!s) return NULL;
    for (int i = 0; i < MAXE; i++) if (g_e[i].used && g_e[i].orig == s) return &g_e[i];
    return NULL;
}
static void Resolve(Ent *e)
{
    if (!e->dirty) return;
    o_SR(g_d, e->twin, NULL, e->orig, NULL, D3DTEXF_NONE);
    e->dirty = 0; g_dirty--;
}
static void Dirty(Ent *e) { if (!e->dirty) { e->dirty = 1; g_dirty++; } e->stale = 0; }

/* the twin for a game surface, NULL if it does not take part */
static Ent *Twin(IDirect3DSurface9 *s, int depth)
{
    Ent *e = Find(s);
    if (e && e->ms == g_level) return e;
    if (e) { IDirect3DSurface9_FreePrivateData(s, &kGuid); e = NULL; }
    D3DSURFACE_DESC sd;
    if (FAILED(IDirect3DSurface9_GetDesc(s, &sd)) || sd.MultiSampleType != D3DMULTISAMPLE_NONE ||
        sd.Width != g_bbW || sd.Height != g_bbH || sd.Pool != D3DPOOL_DEFAULT) return NULL;
    IDirect3DBaseTexture9 *tex = NULL;
    if (!depth) {
        if (!(sd.Usage & D3DUSAGE_RENDERTARGET) || (sd.Format != D3DFMT_A16B16G16R16F && sd.Format != D3DFMT_G32R32F)) return NULL;
        if (FAILED(IDirect3DSurface9_GetContainer(s, &IID_IDirect3DBaseTexture9, (void **)&tex)) || !tex) return NULL;
        IDirect3DBaseTexture9_Release(tex);
    }
    int i; for (i = 0; i < MAXE && g_e[i].used; i++) ;
    if (i == MAXE) return NULL;
    IDirect3DSurface9 *t = NULL;
    HRESULT hr = depth ? r_CreateDS(g_d, sd.Width, sd.Height, sd.Format, g_level, 0, FALSE, &t, NULL)
                       : r_CreateRT(g_d, sd.Width, sd.Height, sd.Format, g_level, 0, FALSE, &t, NULL);
    if (FAILED(hr) || !t) { Log("msaa: %ux%u format %d x%d twin failed (0x%08lx)", sd.Width, sd.Height, sd.Format, (int)g_level, hr); return NULL; }
    e = &g_e[i];
    memset(e, 0, sizeof *e);
    e->unk.lpVtbl = &kEntVt; e->ref = 1; e->used = 1;
    e->orig = s; e->twin = t; e->tex = tex; e->ms = g_level; e->depth = depth;
    e->stale = !depth; e->clear = depth;
    IDirect3DSurface9_SetPrivateData(s, &kGuid, &e->unk, sizeof(IUnknown *), D3DSPD_IUNKNOWN);
    E_Release(&e->unk);                                 /* now owned by the game's surface */
    if (!e->used) return NULL;
    Log("msaa: x%d twin for %s %ux%u format %d", (int)g_level, depth ? "depth buffer" : "target", sd.Width, sd.Height, sd.Format);
    return e;
}

static void Sync(void)
{
    /* after device creation / Reset: the device's own bindings */
    g_sync = 0;
    for (int i = 0; i < 4; i++) {
        if (g_rt[i]) IDirect3DSurface9_Release(g_rt[i]);
        g_rt[i] = NULL;
        if (FAILED(o_GRT(g_d, i, &g_rt[i]))) g_rt[i] = NULL;
        g_art[i] = g_rt[i];
    }
    if (g_ds) IDirect3DSurface9_Release(g_ds);
    g_ds = NULL;
    if (FAILED(o_GDS(g_d, &g_ds))) g_ds = NULL;
    g_ads = g_ds; g_mode = 0;
    IDirect3DSurface9 *bb = NULL; D3DSURFACE_DESC sd;
    if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(g_d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        if (SUCCEEDED(IDirect3DSurface9_GetDesc(bb, &sd))) { g_bbW = sd.Width; g_bbH = sd.Height; }
        IDirect3DSurface9_Release(bb);
    }
    int want = g_gameMsaa;
    if (want > 16) want = 16;
    g_level = D3DMULTISAMPLE_NONE; g_okFmt = 0;
    IDirect3D9 *d3d = NULL; D3DDEVICE_CREATION_PARAMETERS cp;
    if (want >= 2 && SUCCEEDED(IDirect3DDevice9_GetDirect3D(g_d, &d3d)) && d3d && SUCCEEDED(IDirect3DDevice9_GetCreationParameters(g_d, &cp))) {
        for (int l = want; l >= 2 && !g_level; l--)
            if (SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(d3d, cp.AdapterOrdinal, cp.DeviceType, D3DFMT_A16B16G16R16F, TRUE, (D3DMULTISAMPLE_TYPE)l, NULL)) &&
                SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(d3d, cp.AdapterOrdinal, cp.DeviceType, D3DFMT_G32R32F, TRUE, (D3DMULTISAMPLE_TYPE)l, NULL)) &&
                SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(d3d, cp.AdapterOrdinal, cp.DeviceType, D3DFMT_D24S8, TRUE, (D3DMULTISAMPLE_TYPE)l, NULL)))
                g_level = (D3DMULTISAMPLE_TYPE)l;
        IDirect3D9_Release(d3d);
    }
    g_okFmt = g_level != D3DMULTISAMPLE_NONE;
    if (g_level) o_RS(g_d, D3DRS_MULTISAMPLEANTIALIAS, TRUE);
    static int last = -1;
    if (last != (int)g_level || want != (int)g_level) Log("msaa: scene x%d (game option x%d)", (int)g_level, want);
    last = (int)g_level;
}

/* bind what the game's bindings call for */
static void Apply(int rt0Set)
{
    int mode = 0;
    Ent *et[4] = {0}, *ed = NULL;
    if (g_okFmt && g_ds && g_rt[0] && (ed = Twin(g_ds, 1))) {
        mode = 1;
        for (int i = 0; i < 4 && mode; i++) if (g_rt[i] && !(et[i] = Twin(g_rt[i], 0))) mode = 0;
    }
    D3DVIEWPORT9 vp; RECT sc; int saved = 0;
    IDirect3DSurface9 *want0 = mode ? et[0]->twin : g_rt[0];
    if (!rt0Set && want0 != g_art[0]) {
        saved = 1;
        IDirect3DDevice9_GetViewport(g_d, &vp); IDirect3DDevice9_GetScissorRect(g_d, &sc);
    }
    for (int i = 0; i < 4; i++) {
        Ent *e = mode ? et[i] : Find(g_rt[i]);
        IDirect3DSurface9 *w = mode ? (et[i] ? et[i]->twin : NULL) : g_rt[i];
        if (!mode && e) Resolve(e);                     /* drawn to directly from now on: bring it up to date */
        if (w != g_art[i] || (i == 0 && rt0Set)) { if (i == 0 || w || g_art[i]) o_SRT(g_d, i, w); g_art[i] = w; }
        if (mode && e && e->stale) {                     /* the texture was changed directly */
            if (FAILED(o_SR(g_d, e->orig, NULL, e->twin, NULL, D3DTEXF_NONE))) {
                static int nl; if (nl++ < 3) Log("msaa: copy into the twin failed (format change?)");
            }
            e->stale = 0;
        }
    }
    IDirect3DSurface9 *wd = mode ? ed->twin : g_ds;
    if (wd != g_ads) { o_SDS(g_d, wd); g_ads = wd; }
    if (mode && ed->clear) {                             /* new depth twin: start empty */
        D3DVIEWPORT9 v0, full = {0, 0, g_bbW, g_bbH, 0.f, 1.f};
        IDirect3DDevice9_GetViewport(g_d, &v0);
        IDirect3DDevice9_SetViewport(g_d, &full);
        o_Clear(g_d, 0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.f, 0);
        IDirect3DDevice9_SetViewport(g_d, &v0);
        ed->clear = 0;
    }
    if (saved) { IDirect3DDevice9_SetViewport(g_d, &vp); IDirect3DDevice9_SetScissorRect(g_d, &sc); }
    if (mode != g_mode) {
        static int nl; if (nl < 4 && mode) { nl++; Log("msaa: scene pass multisampled (x%d)", (int)g_level); }
        g_mode = mode;
    }
}

/* before a draw: textures it reads are resolved, the targets it writes marked */
static void PreDraw(void)
{
    if (g_sync) return;
    if (g_dirty) {
        for (int i = 0; i < MAXE; i++) {
            Ent *e = &g_e[i];
            if (!e->used || !e->dirty || e->twin == g_art[0]) continue;
            for (int s = 0; s < 20; s++) if (g_tx[s] == e->tex) { Resolve(e); break; }
        }
    }
    for (int i = 0; i < 4; i++) {
        if (!g_rt[i]) continue;
        Ent *e = Find(g_rt[i]);
        if (!e) continue;
        if (g_mode) Dirty(e); else e->stale = 1;
    }
}

/* ---- hooks ---- */
static HRESULT STDMETHODCALLTYPE hk_SRT(IDirect3DDevice9 *d, DWORD i, IDirect3DSurface9 *s)
{
    if (g_sync) Sync();
    if (i > 3) return o_SRT(d, i, s);
    if (i == 0 && !s) return D3DERR_INVALIDCALL;
    if (s) IDirect3DSurface9_AddRef(s);
    if (g_rt[i]) IDirect3DSurface9_Release(g_rt[i]);
    g_rt[i] = s;
    Apply(i == 0);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE hk_GRT(IDirect3DDevice9 *d, DWORD i, IDirect3DSurface9 **out)
{
    if (g_sync) Sync();
    if (i > 3 || !out) return o_GRT(d, i, out);
    *out = g_rt[i];
    if (!*out) return D3DERR_NOTFOUND;
    IDirect3DSurface9_AddRef(*out);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE hk_SDS(IDirect3DDevice9 *d, IDirect3DSurface9 *s)
{
    (void)d;
    if (g_sync) Sync();
    if (s) IDirect3DSurface9_AddRef(s);
    if (g_ds) IDirect3DSurface9_Release(g_ds);
    g_ds = s;
    Apply(0);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE hk_GDS(IDirect3DDevice9 *d, IDirect3DSurface9 **out)
{
    if (g_sync) Sync();
    if (!out) return o_GDS(d, out);
    *out = g_ds;
    if (!*out) return D3DERR_NOTFOUND;
    IDirect3DSurface9_AddRef(*out);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE hk_Clear(IDirect3DDevice9 *d, DWORD n, const D3DRECT *r, DWORD f, D3DCOLOR c, float z, DWORD st)
{
    if (!g_sync && (f & D3DCLEAR_TARGET)) {
        for (int i = 0; i < 4; i++) {
            Ent *e = g_rt[i] ? Find(g_rt[i]) : NULL;
            if (!e) continue;
            if (g_mode) Dirty(e);
            else { if (n) Resolve(e); else if (e->dirty) { e->dirty = 0; g_dirty--; } e->stale = 1; }
        }
    }
    return o_Clear(d, n, r, f, c, z, st);
}
static HRESULT STDMETHODCALLTYPE hk_STX(IDirect3DDevice9 *d, DWORD s, IDirect3DBaseTexture9 *t)
{
    int k = s < 16 ? (int)s : (s >= D3DVERTEXTEXTURESAMPLER0 && s <= D3DVERTEXTEXTURESAMPLER3) ? 16 + (int)(s - D3DVERTEXTEXTURESAMPLER0) : -1;
    if (k >= 0) g_tx[k] = t;
    return o_STX(d, s, t);
}
static HRESULT STDMETHODCALLTYPE hk_SR(IDirect3DDevice9 *d, IDirect3DSurface9 *s, const RECT *sr, IDirect3DSurface9 *t, const RECT *tr, D3DTEXTUREFILTERTYPE f)
{
    if (!g_sync) {
        Ent *e = Find(s);
        if (e) Resolve(e);
        if ((e = Find(t))) {
            if (tr) Resolve(e); else if (e->dirty) { e->dirty = 0; g_dirty--; }
            e->stale = 1;
        }
    }
    return o_SR(d, s, sr, t, tr, f);
}
static HRESULT STDMETHODCALLTYPE hk_GRTD(IDirect3DDevice9 *d, IDirect3DSurface9 *s, IDirect3DSurface9 *t)
{
    Ent *e = g_sync ? NULL : Find(s);
    if (e) Resolve(e);
    return o_GRTD(d, s, t);
}
static HRESULT STDMETHODCALLTYPE hk_RS(IDirect3DDevice9 *d, D3DRENDERSTATETYPE st, DWORD v)
{
    if (st == D3DRS_MULTISAMPLEANTIALIAS && g_level) v = TRUE;   /* the game turns it off when its own option is off */
    return o_RS(d, st, v);
}
static HRESULT STDMETHODCALLTYPE hk_DP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT b)
{ PreDraw(); return o_DP(d, t, a, b); }
static HRESULT STDMETHODCALLTYPE hk_DIP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT e, UINT f)
{ PreDraw(); return o_DIP(d, t, a, b, c, e, f); }
static HRESULT STDMETHODCALLTYPE hk_DPUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, const void *p, UINT s)
{ PreDraw(); return o_DPUP(d, t, a, p, s); }
static HRESULT STDMETHODCALLTYPE hk_DIPUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c, const void *i, D3DFORMAT f, const void *v, UINT s)
{ PreDraw(); return o_DIPUP(d, t, a, b, c, i, f, v, s); }

/* before Reset: the game's surfaces back in place, every twin released */
static void OnReset(void)
{
    if (!g_d) return;
    for (int i = 0; i < 4; i++) if (g_art[i] != g_rt[i] && (i == 0 ? g_rt[0] != NULL : 1)) o_SRT(g_d, i, g_rt[i]);
    if (g_ads != g_ds) o_SDS(g_d, g_ds);
    for (int i = 0; i < MAXE; i++) if (g_e[i].used) IDirect3DSurface9_FreePrivateData(g_e[i].orig, &kGuid);
    for (int i = 0; i < 4; i++) { if (g_rt[i]) IDirect3DSurface9_Release(g_rt[i]); g_rt[i] = g_art[i] = NULL; }
    if (g_ds) IDirect3DSurface9_Release(g_ds);
    g_ds = g_ads = NULL;
    memset(g_tx, 0, sizeof g_tx);
    g_mode = 0; g_sync = 1;
}

/* the game's Anti-Aliasing option, captured where FUN_007554B0 stores it (before the windowed check zeroes it) */
__attribute__((naked)) static void StoreLevel(void)
{
    __asm__ volatile(
        "mov %ecx, 0x594(%esi)\n\t"
        "mov %ecx, _g_gameMsaa\n\t"
        "ret");
}
__attribute__((naked)) static void StoreNone(void)
{
    __asm__ volatile(
        "movl $0, 0x594(%esi)\n\t"
        "movl $0, _g_gameMsaa\n\t"
        "ret");
}

/* the back buffer's multisample type the game asked for (when the option store above was not reached) */
void MsaaPresentParams(const D3DPRESENT_PARAMETERS *pp)
{
    if (pp->MultiSampleType > D3DMULTISAMPLE_NONMASKABLE && !g_gameMsaa) g_gameMsaa = (int)pp->MultiSampleType;
}

void MsaaInit(void)
{
    g_aaFix = IniInt("Fixes", "AAFix", 1) != 0;
    if (!g_aaFix) { Log("msaa: off ([Fixes] AAFix=0, the game's own anti-aliasing)"); return; }
    if (Expect(0x7554E5, "89 8E 94 05 00 00 5E C3 C7 86 94 05 00 00 00 00 00 00")) {
        WriteCall(0x7554E5, (void *)StoreLevel, 6);
        WriteCall(0x7554ED, (void *)StoreNone, 10);
    } else Log("msaa: option store site mismatch, game option not read");
}

void MsaaDevice(IDirect3DDevice9 *d)
{
    if (!g_aaFix) return;
    g_d = d;
    void **vt = *(void ***)d;
    r_CreateRT = (CRT_t)vt[28]; r_CreateDS = (CRT_t)vt[29];
    void *p;
    if ((p = VtSwapPublic(d, 37, (void *)hk_SRT)))   o_SRT = (SRT_t)p;
    if ((p = VtSwapPublic(d, 38, (void *)hk_GRT)))   o_GRT = (GRT_t)p;
    if ((p = VtSwapPublic(d, 39, (void *)hk_SDS)))   o_SDS = (SDS_t)p;
    if ((p = VtSwapPublic(d, 40, (void *)hk_GDS)))   o_GDS = (GDS_t)p;
    if ((p = VtSwapPublic(d, 43, (void *)hk_Clear))) o_Clear = (Clear_t)p;
    if ((p = VtSwapPublic(d, 65, (void *)hk_STX)))   o_STX = (STX_t)p;
    if ((p = VtSwapPublic(d, 34, (void *)hk_SR)))    o_SR = (SR_t)p;
    if ((p = VtSwapPublic(d, 32, (void *)hk_GRTD)))  o_GRTD = (GRTD_t)p;
    if ((p = VtSwapPublic(d, 57, (void *)hk_RS)))    o_RS = (RS_t)p;
    if ((p = VtSwapPublic(d, 81, (void *)hk_DP)))    o_DP = (DP_t)p;
    if ((p = VtSwapPublic(d, 82, (void *)hk_DIP)))   o_DIP = (DIP_t)p;
    if ((p = VtSwapPublic(d, 83, (void *)hk_DPUP)))  o_DPUP = (DPUP_t)p;
    if ((p = VtSwapPublic(d, 84, (void *)hk_DIPUP))) o_DIPUP = (DIPUP_t)p;
    D3DAddResetCallback(OnReset);
    g_sync = 1;
    Log("msaa: device hooks on");
}
int MsaaLevel(void) { return (int)g_level; }
