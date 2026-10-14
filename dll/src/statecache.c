/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Redundant state filter. The game calls SetRenderState, SetStreamSource, SetStreamSourceFreq, SetIndices,
 * SetVertexDeclaration, the shader setters and whole shader constant blocks for every draw without checking what is already set (its render
 * state wrapper 0x484310 and stream wrapper 0x4841E5 have no cache; only textures are cached, 0x484280). The
 * device is created with D3DCREATE_MULTITHREADED, so each of those calls also takes the runtime's lock; in towns
 * that is tens of thousands of calls a frame on the render thread. A call that sets the value already set is
 * dropped here; of a constant upload only the registers that changed are sent. The cache follows every call that reaches the device through its vtable (the game's, D3DX's and
 * this mod's) and is cleared on Reset, on state block Apply / BeginStateBlock (which change state without going
 * through these calls or must record them) and after the user-pointer draws (which unset stream 0 and the
 * indices), so it never holds a value the device does not.
 * [Performance] StateFilter. */
#define COBJMACROS
#include "m2r.h"
#include <d3d9.h>
#include <string.h>

void *VtSwapPublic(void *obj, int idx, void *hook);

typedef HRESULT (STDMETHODCALLTYPE *SetRS_t)(IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *SetSS_t)(IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *SetSrc_t)(IDirect3DDevice9 *, UINT, IDirect3DVertexBuffer9 *, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *SetFreq_t)(IDirect3DDevice9 *, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *SetIdx_t)(IDirect3DDevice9 *, IDirect3DIndexBuffer9 *);
typedef HRESULT (STDMETHODCALLTYPE *SetDecl_t)(IDirect3DDevice9 *, IDirect3DVertexDeclaration9 *);
typedef HRESULT (STDMETHODCALLTYPE *SetVS_t)(IDirect3DDevice9 *, IDirect3DVertexShader9 *);
typedef HRESULT (STDMETHODCALLTYPE *SetPS_t)(IDirect3DDevice9 *, IDirect3DPixelShader9 *);
typedef HRESULT (STDMETHODCALLTYPE *SetFVF_t)(IDirect3DDevice9 *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *SetCF_t)(IDirect3DDevice9 *, UINT, const float *, UINT);
typedef HRESULT (STDMETHODCALLTYPE *Begin_t)(IDirect3DDevice9 *);
typedef HRESULT (STDMETHODCALLTYPE *End_t)(IDirect3DDevice9 *, IDirect3DStateBlock9 **);
typedef HRESULT (STDMETHODCALLTYPE *Create_t)(IDirect3DDevice9 *, D3DSTATEBLOCKTYPE, IDirect3DStateBlock9 **);
typedef HRESULT (STDMETHODCALLTYPE *Apply_t)(IDirect3DStateBlock9 *);
typedef HRESULT (STDMETHODCALLTYPE *DrawUP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawIUP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);

static SetRS_t o_RS; static SetSS_t o_SS; static SetSrc_t o_Src; static SetFreq_t o_Freq; static SetIdx_t o_Idx;
static SetCF_t o_VSF, o_PSF; static SetDecl_t o_Decl; static SetVS_t o_VS; static SetPS_t o_PS; static SetFVF_t o_FVF;
static Begin_t o_Begin; static End_t o_End; static Create_t o_Create; static Apply_t o_Apply;
static DrawUP_t o_UP; static DrawIUP_t o_IUP;

#define NRS 256
static DWORD g_rs[NRS]; static BYTE g_rsOk[NRS];
static DWORD g_ss[20][16]; static BYTE g_ssOk[20][16];
static struct { void *vb; UINT off, stride; BYTE ok; UINT freq; BYTE fok; } g_src[16];
static void *g_idx, *g_decl, *g_vs, *g_ps; static BYTE g_idxOk, g_declOk, g_vsOk, g_psOk;
static float g_vc[256][4], g_pc[224][4]; static BYTE g_vcOk[256], g_pcOk[224];
static int g_recording;
static DWORD g_thread;

static void Clear(void)
{
    memset(g_rsOk, 0, sizeof g_rsOk); memset(g_ssOk, 0, sizeof g_ssOk);
    for (int i = 0; i < 16; i++) g_src[i].ok = g_src[i].fok = 0;
    g_idxOk = g_declOk = g_vsOk = g_psOk = 0;
    memset(g_vcOk, 0, sizeof g_vcOk); memset(g_pcOk, 0, sizeof g_pcOk);
}
/* only the render thread's calls are filtered; any other thread touching state clears the cache */
static int Mine(void)
{
    if (g_recording) { return 0; }
    if (GetCurrentThreadId() == g_thread) return 1;
   
    Clear();
    return 0;
}

static HRESULT STDMETHODCALLTYPE hk_RS(IDirect3DDevice9 *d, D3DRENDERSTATETYPE s, DWORD v)
{
    if ((unsigned)s < NRS && Mine()) {
        if (g_rsOk[s] && g_rs[s] == v) return D3D_OK;
        HRESULT hr = o_RS(d, s, v);
        if (SUCCEEDED(hr)) { g_rs[s] = v; g_rsOk[s] = 1; } else g_rsOk[s] = 0;
        return hr;
    }
    if ((unsigned)s < NRS) g_rsOk[s] = 0;
    return o_RS(d, s, v);
}
static int SamplerSlot(DWORD st)
{
    if (st < 16) return (int)st;
    if (st >= D3DDMAPSAMPLER && st <= D3DVERTEXTEXTURESAMPLER3) return 16 + (int)(st - D3DDMAPSAMPLER);
    return -1;
}
static HRESULT STDMETHODCALLTYPE hk_SS(IDirect3DDevice9 *d, DWORD st, D3DSAMPLERSTATETYPE t, DWORD v)
{
    int i = SamplerSlot(st);
    if (i >= 0 && (unsigned)t < 16 && Mine()) {
        if (g_ssOk[i][t] && g_ss[i][t] == v) return D3D_OK;
        HRESULT hr = o_SS(d, st, t, v);
        if (SUCCEEDED(hr)) { g_ss[i][t] = v; g_ssOk[i][t] = 1; } else g_ssOk[i][t] = 0;
        return hr;
    }
    if (i >= 0 && (unsigned)t < 16) g_ssOk[i][t] = 0;
    return o_SS(d, st, t, v);
}
static HRESULT STDMETHODCALLTYPE hk_Src(IDirect3DDevice9 *d, UINT n, IDirect3DVertexBuffer9 *vb, UINT off, UINT stride)
{
    if (n < 16 && Mine()) {
        if (g_src[n].ok && g_src[n].vb == vb && g_src[n].off == off && g_src[n].stride == stride) return D3D_OK;
        HRESULT hr = o_Src(d, n, vb, off, stride);
        g_src[n].vb = vb; g_src[n].off = off; g_src[n].stride = stride; g_src[n].ok = SUCCEEDED(hr);
        return hr;
    }
    if (n < 16) g_src[n].ok = 0;
    return o_Src(d, n, vb, off, stride);
}
static HRESULT STDMETHODCALLTYPE hk_Freq(IDirect3DDevice9 *d, UINT n, UINT f)
{
    if (n < 16 && Mine()) {
        if (g_src[n].fok && g_src[n].freq == f) return D3D_OK;
        HRESULT hr = o_Freq(d, n, f);
        g_src[n].freq = f; g_src[n].fok = SUCCEEDED(hr);
        return hr;
    }
    if (n < 16) g_src[n].fok = 0;
    return o_Freq(d, n, f);
}
#define PTRSET(name, T, cur, ok, orig)                                                          \
    static HRESULT STDMETHODCALLTYPE name(IDirect3DDevice9 *d, T p)                             \
    {                                                                                           \
        if (Mine()) {                                                                           \
            if (ok && cur == (void *)p) return D3D_OK;                        \
            HRESULT hr = orig(d, p); cur = (void *)p; ok = SUCCEEDED(hr); return hr;    \
        }                                                                                       \
        ok = 0; return orig(d, p);                                                              \
    }
PTRSET(hk_Idx, IDirect3DIndexBuffer9 *, g_idx, g_idxOk, o_Idx)
PTRSET(hk_Decl, IDirect3DVertexDeclaration9 *, g_decl, g_declOk, o_Decl)
PTRSET(hk_VS, IDirect3DVertexShader9 *, g_vs, g_vsOk, o_VS)
PTRSET(hk_PS, IDirect3DPixelShader9 *, g_ps, g_psOk, o_PS)
/* shader constants: only the registers that differ from what is set are sent (one call per changed run) */
static HRESULT ConstF(IDirect3DDevice9 *d, SetCF_t orig, float (*mir)[4], BYTE *ok, UINT lim, UINT r, const float *v, UINT n)
{
    if (!v || r >= lim || r + n > lim || !Mine()) {
        if (r < lim) memset(ok + r, 0, (r + n > lim ? lim : r + n) - r);
        return orig(d, r, v, n);
    }
    UINT i = 0; HRESULT hr = D3D_OK;
    while (i < n) {
        while (i < n && ok[r + i] && !memcmp(mir[r + i], v + i * 4, 16)) i++;
        if (i >= n) break;
        UINT j = i;
        while (j < n && !(ok[r + j] && !memcmp(mir[r + j], v + j * 4, 16))) j++;
        HRESULT h = orig(d, r + i, v + i * 4, j - i);
        if (FAILED(h)) { hr = h; memset(ok + r + i, 0, j - i); }
        else { memcpy(mir[r + i], v + i * 4, (j - i) * 16); memset(ok + r + i, 1, j - i); }
        i = j;
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_VSF(IDirect3DDevice9 *d, UINT r, const float *v, UINT n) { return ConstF(d, o_VSF, g_vc, g_vcOk, 256, r, v, n); }
static HRESULT STDMETHODCALLTYPE hk_PSF(IDirect3DDevice9 *d, UINT r, const float *v, UINT n) { return ConstF(d, o_PSF, g_pc, g_pcOk, 224, r, v, n); }
/* the user-pointer draws leave stream 0 (and the index buffer) unset */
static HRESULT STDMETHODCALLTYPE hk_UP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT n, const void *v, UINT st)
{
    HRESULT hr = o_UP(d, t, n, v, st);
    g_src[0].ok = 0;
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_IUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT n, const void *ix, D3DFORMAT f,
                                        const void *v, UINT st)
{
    HRESULT hr = o_IUP(d, t, mi, nv, n, ix, f, v, st);
    g_src[0].ok = 0; g_idxOk = 0;
    return hr;
}
/* SetFVF replaces the vertex declaration */
static HRESULT STDMETHODCALLTYPE hk_FVF(IDirect3DDevice9 *d, DWORD f) { g_declOk = 0; return o_FVF(d, f); }

static HRESULT STDMETHODCALLTYPE hk_Apply(IDirect3DStateBlock9 *sb) { Clear(); HRESULT hr = o_Apply(sb); Clear(); return hr; }
static void HookBlock(IDirect3DStateBlock9 *sb)
{
    void *p;
    if (sb && !o_Apply && (p = VtSwapPublic(sb, 5, (void *)hk_Apply))) o_Apply = (Apply_t)p;
}
static HRESULT STDMETHODCALLTYPE hk_Begin(IDirect3DDevice9 *d)
{
    Clear();
    HRESULT hr = o_Begin(d);
    if (SUCCEEDED(hr)) g_recording = 1;                       /* a failed Begin records nothing */
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_End(IDirect3DDevice9 *d, IDirect3DStateBlock9 **out)
{
    HRESULT hr = o_End(d, out);
    g_recording = 0; Clear();
    if (SUCCEEDED(hr) && out) HookBlock(*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_Create(IDirect3DDevice9 *d, D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9 **out)
{
    HRESULT hr = o_Create(d, t, out);
    if (SUCCEEDED(hr) && out) HookBlock(*out);
    return hr;
}

static int g_on;
void StateCacheReset(void) { Clear(); }
/* at Present: follows the presenting (render) thread; no state block recording spans a frame */
void StateCachePresent(void)
{
    DWORD t = GetCurrentThreadId();
    if (g_thread != t || g_recording) { g_thread = t; g_recording = 0; Clear(); }
}
void StateCacheDevice(IDirect3DDevice9 *d)
{
    g_on = IniInt("Performance", "StateFilter", 1);
    if (!g_on || o_RS) return;
    void *p;
    if ((p = VtSwapPublic(d, 57, (void *)hk_RS)))    o_RS = (SetRS_t)p;
    if ((p = VtSwapPublic(d, 69, (void *)hk_SS)))    o_SS = (SetSS_t)p;
    if ((p = VtSwapPublic(d, 100, (void *)hk_Src)))  o_Src = (SetSrc_t)p;
    if ((p = VtSwapPublic(d, 102, (void *)hk_Freq))) o_Freq = (SetFreq_t)p;
    if ((p = VtSwapPublic(d, 104, (void *)hk_Idx)))  o_Idx = (SetIdx_t)p;
    if ((p = VtSwapPublic(d, 87, (void *)hk_Decl)))  o_Decl = (SetDecl_t)p;
    if ((p = VtSwapPublic(d, 89, (void *)hk_FVF)))   o_FVF = (SetFVF_t)p;
    if ((p = VtSwapPublic(d, 92, (void *)hk_VS)))    o_VS = (SetVS_t)p;
    if ((p = VtSwapPublic(d, 107, (void *)hk_PS)))   o_PS = (SetPS_t)p;
    if ((p = VtSwapPublic(d, 94, (void *)hk_VSF)))   o_VSF = (SetCF_t)p;
    if ((p = VtSwapPublic(d, 109, (void *)hk_PSF)))  o_PSF = (SetCF_t)p;
    if ((p = VtSwapPublic(d, 60, (void *)hk_Begin))) o_Begin = (Begin_t)p;
    if ((p = VtSwapPublic(d, 61, (void *)hk_End)))   o_End = (End_t)p;
    if ((p = VtSwapPublic(d, 59, (void *)hk_Create))) o_Create = (Create_t)p;
    if ((p = VtSwapPublic(d, 83, (void *)hk_UP)))    o_UP = (DrawUP_t)p;
    if ((p = VtSwapPublic(d, 84, (void *)hk_IUP)))   o_IUP = (DrawIUP_t)p;
    if (!o_RS || !o_UP || !o_IUP || !o_SS || !o_Src || !o_Freq || !o_Idx || !o_Decl || !o_FVF || !o_VS || !o_PS || !o_Begin || !o_End || !o_Create || !o_VSF || !o_PSF)
        Log("statecache: some hooks were not installed");
    Log("statecache: redundant state calls are dropped");
}
