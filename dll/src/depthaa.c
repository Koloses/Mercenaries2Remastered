/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* m2r:no-retarget
 * Xbox 360 anti-aliasing. The Xbox renders without MSAA and runs a depth-aware edge blur (PgAntiAliasingFP) on the
 * finished frame after tone mapping: per axis (vertical, horizontal, both diagonals) an edge where the linear depth
 * of the two opposite neighbours differs by >= 3.5 % of the centre depth; the pixel becomes lerp(centre, average
 * of the colour pairs along those edges, 0.8). The PC kept the shader as a plain copy and its pass as an empty
 * function (renderer vtable +0x38). Here the pass runs right after the tone-mapping draw: copy of the target -> s0,
 * DepthBufferCopy (the tone mapper's s6, G32R32F z/w) -> s1, c200 = (0.035, 0.8), c201 = projection (A, B) from the
 * view's projection matrix, c202 = 1/size.
 * [Graphics] DepthAA: -1 = only when the mod's MSAA is off (default, the Xbox look without MSAA), 1 = always, 0 = off. */
#define COBJMACROS
#include "m2r.h"
#include <d3d9.h>
#include "d3dhook.h"
#include "depthaa_shaders.h"

#ifdef M2R_GAME11
#define DA_RENDERER 0xDFB2F8u
#else
#define DA_RENDERER 0xDFC2F8u
#endif
#define DA_TM_LEN 2880u
#define DA_TM_CRC 0x76197FAAu        /* PgToneMappingFP_3 (original) */

void *VtSwapPublic(void *obj, int idx, void *hook);
int MsaaLevel(void);
typedef HRESULT (STDMETHODCALLTYPE *CreatePS_t)(IDirect3DDevice9 *, const DWORD *, IDirect3DPixelShader9 **);
typedef HRESULT (STDMETHODCALLTYPE *SetPS_t)(IDirect3DDevice9 *, IDirect3DPixelShader9 *);
typedef HRESULT (STDMETHODCALLTYPE *DIP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *UP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
static CreatePS_t o_CreatePS; static SetPS_t o_SetPS; static DIP_t o_DIP; static DP_t o_DP; static UP_t o_UP;
static int g_mode, g_done, g_inPass, g_fail;
static IDirect3DPixelShader9 *g_tm[4], *g_cur, *g_ps; static int g_ntm;
static IDirect3DVertexShader9 *g_vs;
static IDirect3DVertexDeclaration9 *g_decl;
static IDirect3DTexture9 *g_copy; static UINT g_cw, g_ch; static D3DFORMAT g_cf;
static IDirect3DStateBlock9 *g_sb;
static LONG g_runs;

static DWORD Crc32(const BYTE *p, DWORD n)
{
    DWORD c = 0xFFFFFFFF;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & -(c & 1)); }
    return ~c;
}
static DWORD CodeLen(const DWORD *c)
{
    DWORD i = 1;
    for (int g = 0; g < 100000; g++) {
        DWORD t = c[i];
        if (t == 0x0000FFFF) return (i + 1) * 4;
        if ((t & 0xFFFF) == 0xFFFE) i += 1 + (t >> 16);
        else i += 1 + ((t >> 24) & 0xF);
    }
    return 0;
}
static HRESULT STDMETHODCALLTYPE hk_CreatePS(IDirect3DDevice9 *d, const DWORD *c, IDirect3DPixelShader9 **o)
{
    HRESULT hr = o_CreatePS(d, c, o);
    if (SUCCEEDED(hr) && o && *o && c && !IsBadReadPtr(c, 8) && g_ntm < 4 && CodeLen(c) == DA_TM_LEN && Crc32((const BYTE *)c, DA_TM_LEN) == DA_TM_CRC)
        g_tm[g_ntm++] = *o;
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_SetPS(IDirect3DDevice9 *d, IDirect3DPixelShader9 *s) { g_cur = s; return o_SetPS(d, s); }

static int Projection(float *a, float *b)
{
    BYTE *r = *(BYTE **)DA_RENDERER;
    if (!r || IsBadReadPtr(r + 0x2B92, 2)) return 0;
    WORD idx = *(WORD *)(r + 0x2B92);
    if (idx > 15) return 0;
    BYTE *v = r + idx * 0xE80;
    if (IsBadReadPtr(v + 0xB58, 0x14)) return 0;
    *a = *(float *)(v + 0xB58); *b = *(float *)(v + 0xB68);
    return *a > 0.5f && *a < 2.f && *b < 0.f && *b > -1000.f;
}
static void Release(void)
{
    if (g_copy) { IDirect3DTexture9_Release(g_copy); g_copy = NULL; }
    if (g_sb) { IDirect3DStateBlock9_Release(g_sb); g_sb = NULL; }
    g_cw = g_ch = 0;
}
static void Fail(const char *what, HRESULT hr)
{
    if (!g_fail++) Log("depth AA: %s failed (%08lX), pass off", what, hr);
}
static void RunAA(IDirect3DDevice9 *d)
{
    float A, B;
    if (!Projection(&A, &B)) return;
    IDirect3DSurface9 *rt = NULL, *dst = NULL; IDirect3DBaseTexture9 *dt = NULL;
    D3DSURFACE_DESC rd, dd;
    if (FAILED(IDirect3DDevice9_GetRenderTarget(d, 0, &rt)) || !rt) return;
    IDirect3DSurface9_GetDesc(rt, &rd);
    if (FAILED(IDirect3DDevice9_GetTexture(d, 6, &dt)) || !dt || IDirect3DBaseTexture9_GetType(dt) != D3DRTYPE_TEXTURE ||
        FAILED(IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)dt, 0, &dd)) || dd.Format != D3DFMT_G32R32F || dd.Width != rd.Width || dd.Height != rd.Height)
        goto out;
    HRESULT hr;
    if (!g_vs && FAILED(hr = IDirect3DDevice9_CreateVertexShader(d, (const DWORD *)kDaaVS, &g_vs))) { g_vs = NULL; Fail("vertex shader", hr); goto out; }
    if (!g_ps && FAILED(hr = IDirect3DDevice9_CreatePixelShader(d, (const DWORD *)kDaaPS, &g_ps))) { g_ps = NULL; Fail("pixel shader", hr); goto out; }
    if (!g_decl) {
        static const D3DVERTEXELEMENT9 e[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
        if (FAILED(hr = IDirect3DDevice9_CreateVertexDeclaration(d, e, &g_decl))) { g_decl = NULL; Fail("declaration", hr); goto out; }
    }
    if (!g_copy || g_cw != rd.Width || g_ch != rd.Height || g_cf != rd.Format) {
        if (g_copy) { IDirect3DTexture9_Release(g_copy); g_copy = NULL; }
        if (FAILED(hr = IDirect3DDevice9_CreateTexture(d, rd.Width, rd.Height, 1, D3DUSAGE_RENDERTARGET, rd.Format, D3DPOOL_DEFAULT, &g_copy, NULL))) { g_copy = NULL; Fail("colour copy", hr); goto out; }
        g_cw = rd.Width; g_ch = rd.Height; g_cf = rd.Format;
        Log("depth AA: on, %ux%u format %d", rd.Width, rd.Height, rd.Format);
    }
    if (!g_sb && FAILED(hr = IDirect3DDevice9_CreateStateBlock(d, D3DSBT_ALL, &g_sb))) { g_sb = NULL; Fail("state block", hr); goto out; }
    if (FAILED(IDirect3DTexture9_GetSurfaceLevel(g_copy, 0, &dst))) goto out;
    if (FAILED(hr = IDirect3DDevice9_StretchRect(d, rt, NULL, dst, NULL, D3DTEXF_NONE))) { Fail("copy", hr); goto out; }
    IDirect3DStateBlock9_Capture(g_sb);
    DWORD srgb = 0; IDirect3DDevice9_GetRenderState(d, D3DRS_SRGBWRITEENABLE, &srgb);
    IDirect3DDevice9_SetVertexShader(d, g_vs);
    IDirect3DDevice9_SetPixelShader(d, g_ps);
    IDirect3DDevice9_SetVertexDeclaration(d, g_decl);
    IDirect3DDevice9_SetTexture(d, 0, (IDirect3DBaseTexture9 *)g_copy);
    IDirect3DDevice9_SetTexture(d, 1, dt);
    for (DWORD s = 0; s < 2; s++) {
        IDirect3DDevice9_SetSamplerState(d, s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(d, s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(d, s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetSamplerState(d, s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(d, s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(d, s, D3DSAMP_SRGBTEXTURE, s == 0 ? srgb : 0);
    }
    IDirect3DDevice9_SetRenderState(d, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_COLORWRITEENABLE, 0xF);
    float c[12] = {0.035f, 0.8f, 0, 0,  A, B, 0, 0,  1.f / rd.Width, 1.f / rd.Height, 0, 0};
    IDirect3DDevice9_SetPixelShaderConstantF(d, 200, c, 3);
    static const float q[16] = {-1, -1, 0, 1,  -1, 1, 0, 1,  1, -1, 0, 1,  1, 1, 0, 1};
    g_inPass = 1;
    hr = IDirect3DDevice9_DrawPrimitiveUP(d, D3DPT_TRIANGLESTRIP, 2, q, 16);
    g_inPass = 0;
    IDirect3DStateBlock9_Apply(g_sb);
    if (FAILED(hr)) Fail("draw", hr);
    else if (!g_runs++) Log("depth AA: first pass (A %.5f B %.4f)", A, B);
out:
    if (dst) IDirect3DSurface9_Release(dst);
    if (dt) IDirect3DBaseTexture9_Release(dt);
    IDirect3DSurface9_Release(rt);
}
static int Want(void)
{
    if (g_done || g_inPass || g_fail || !g_cur) return 0;
    if (g_mode < 0 && MsaaLevel() != 0) return 0;
    for (int i = 0; i < g_ntm; i++) if (g_tm[i] == g_cur) return 1;
    return 0;
}
static HRESULT STDMETHODCALLTYPE hk_DIP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE pt, INT bv, UINT mi, UINT nv, UINT si, UINT pc)
{
    HRESULT hr = o_DIP(d, pt, bv, mi, nv, si, pc);
    if (Want()) { g_done = 1; RunAA(d); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_DP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE pt, UINT sv, UINT pc)
{
    HRESULT hr = o_DP(d, pt, sv, pc);
    if (Want()) { g_done = 1; RunAA(d); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_UP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE pt, UINT pc, const void *v, UINT st)
{
    HRESULT hr = o_UP(d, pt, pc, v, st);
    if (Want()) { g_done = 1; RunAA(d); }
    return hr;
}
static void OnFrame(IDirect3DDevice9 *d) { (void)d; g_done = 0; }
void DepthAADevice(IDirect3DDevice9 *d)
{
    if (o_DP) return;
    g_mode = IniInt("Graphics", "DepthAA", -1);
    if (!g_mode) return;
    void *p;
    if ((p = VtSwapPublic(d, 106, (void *)hk_CreatePS))) o_CreatePS = (CreatePS_t)p;
    if ((p = VtSwapPublic(d, 107, (void *)hk_SetPS)))    o_SetPS = (SetPS_t)p;
    if ((p = VtSwapPublic(d, 82, (void *)hk_DIP)))       o_DIP = (DIP_t)p;
    if ((p = VtSwapPublic(d, 81, (void *)hk_DP)))        o_DP = (DP_t)p;
    if ((p = VtSwapPublic(d, 83, (void *)hk_UP)))        o_UP = (UP_t)p;
    D3DAddFrameCallback(OnFrame);
    D3DAddResetCallback(Release);
    Log("depth AA: %s", g_mode < 0 ? "Xbox edge blur when MSAA is off" : "Xbox edge blur always");
}
