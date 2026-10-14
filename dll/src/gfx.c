/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/*
 * Graphics upgrades ([Graphics] in Mercenaries2Remastered.ini)
 *
 *  Anisotropic  forced anisotropic filtering: every sampler the game sets to linear minification gets
 *               D3DTEXF_ANISOTROPIC (render targets / depth textures stay as they are).
 *  MSAA         the game's FSAA only multisampled the back buffer (tone-mapped copy and HUD); with [Fixes] AAFix
 *               the back buffer stays single-sampled and msaa.c multisamples the 3D scene instead.
 *  SMAA         SMAA 1x (Jimenez et al., third_party/smaa) on the finished frame before Present: luma edge
 *               detection, blending weights, neighbourhood blending. Shaders are compiled at startup with
 *               D3DXCompileShader from the game's own d3dx9_36.dll.
 */
#include "m2r.h"
#define COBJMACROS
#include <d3d9.h>
#include "d3dhook.h"
#include "smaa_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

typedef HRESULT (STDMETHODCALLTYPE *SetSamplerState_t)(IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *SetTexture_t)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
typedef HRESULT (STDMETHODCALLTYPE *CreateRT_t)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **, HANDLE *);
typedef HRESULT (STDMETHODCALLTYPE *CreateDS_t)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **, HANDLE *);
static SetSamplerState_t o_SetSamplerState;
static SetTexture_t      o_SetTexture;
static CreateRT_t        o_CreateRenderTarget;
static CreateDS_t        o_CreateDepthStencil;

static int g_af, g_smaa;
static volatile int g_inGfx;

void *VtSwapPublic(void *obj, int idx, void *hook);

/* ------------------------------------------------------------ anisotropic */
static DWORD g_minWant[16]; static BYTE g_stageRT[16];
/* render-target textures, looked up on every SetTexture: small open-addressing set. Texture memory is reused:
 * a render target that was released and an ordinary texture created at its address is taken out again (marked
 * with a tombstone so later entries stay reachable), or that texture would lose anisotropic filtering. */
#define RT_GONE ((void *)1)
static void *g_rtSet[1024];
static int IsRT(void *t)
{
    DWORD h = ((DWORD)(UINT_PTR)t >> 4) * 2654435761u;
    for (int k = 0; k < 1024; k++) { void *e = g_rtSet[(h + k) & 1023]; if (e == t) return 1; if (!e) return 0; }
    return 0;
}
static void AddRT(void *t)
{
    DWORD h = ((DWORD)(UINT_PTR)t >> 4) * 2654435761u;
    void **free_ = NULL;
    for (int k = 0; k < 1024; k++) {
        void **e = &g_rtSet[(h + k) & 1023];
        if (*e == t) return;
        if (*e == RT_GONE && !free_) free_ = e;
        if (!*e) { *(free_ ? free_ : e) = t; return; }
    }
    if (free_) *free_ = t;
}
static void DropRT(void *t)
{
    DWORD h = ((DWORD)(UINT_PTR)t >> 4) * 2654435761u;
    for (int k = 0; k < 1024; k++) { void **e = &g_rtSet[(h + k) & 1023]; if (*e == t) { *e = RT_GONE; return; } if (!*e) return; }
}
void GfxTextureCreated(void *tex, DWORD usage)
{
    if (!(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) { if (IsRT(tex)) DropRT(tex); return; }
    if (!IsRT(tex)) AddRT(tex);
}
static HRESULT ApplyMin(IDirect3DDevice9 *d, DWORD s)
{
    DWORD want = g_minWant[s];
    if (want == D3DTEXF_LINEAR && !g_stageRT[s]) {
        o_SetSamplerState(d, s, D3DSAMP_MAXANISOTROPY, (DWORD)g_af);
        return o_SetSamplerState(d, s, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
    }
    return o_SetSamplerState(d, s, D3DSAMP_MINFILTER, want);
}
static HRESULT STDMETHODCALLTYPE hk_SetSamplerState(IDirect3DDevice9 *d, DWORD s, D3DSAMPLERSTATETYPE t, DWORD v)
{
    if (!g_af || g_inGfx || s >= 16) return o_SetSamplerState(d, s, t, v);
    if (t == D3DSAMP_MINFILTER) { g_minWant[s] = v; return ApplyMin(d, s); }
    if (t == D3DSAMP_MAXANISOTROPY && v < (DWORD)g_af && g_minWant[s] == D3DTEXF_LINEAR && !g_stageRT[s]) v = (DWORD)g_af;
    return o_SetSamplerState(d, s, t, v);
}
static HRESULT STDMETHODCALLTYPE hk_SetTexture(IDirect3DDevice9 *d, DWORD s, IDirect3DBaseTexture9 *tex)
{
    HRESULT hr = o_SetTexture(d, s, tex);
    if (g_af && !g_inGfx && s < 16) {
        BYTE rt = tex && IsRT(tex);
        if (rt != g_stageRT[s]) { g_stageRT[s] = rt; if (g_minWant[s] == D3DTEXF_LINEAR) ApplyMin(d, s); }
    }
    return hr;
}

/* ------------------------------------------------------------------ back buffer single-sampled (msaa.c multisamples the scene) */
void MsaaPresentParams(const D3DPRESENT_PARAMETERS *pp);
extern int g_aaFix;
void GfxAdjustPP(IDirect3D9 *d3d, UINT ad, D3DPRESENT_PARAMETERS *pp)
{
    (void)d3d; (void)ad;
    if (!g_aaFix) return;
    MsaaPresentParams(pp);
    pp->MultiSampleType = D3DMULTISAMPLE_NONE; pp->MultiSampleQuality = 0;
}
static HRESULT STDMETHODCALLTYPE hk_CreateRenderTarget(IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms,
                                                       DWORD q, BOOL lock, IDirect3DSurface9 **s, HANDLE *sh)
{
    if (!g_aaFix) return o_CreateRenderTarget(d, w, h, f, ms, q, lock, s, sh);
    return o_CreateRenderTarget(d, w, h, f, D3DMULTISAMPLE_NONE, 0, lock, s, sh);
}
static HRESULT STDMETHODCALLTYPE hk_CreateDepthStencil(IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms,
                                                       DWORD q, BOOL disc, IDirect3DSurface9 **s, HANDLE *sh)
{
    if (!g_aaFix) return o_CreateDepthStencil(d, w, h, f, ms, q, disc, s, sh);
    return o_CreateDepthStencil(d, w, h, f, D3DMULTISAMPLE_NONE, 0, disc, s, sh);
}

/* ------------------------------------------------------------------ SMAA */
typedef struct ID3DXBuffer ID3DXBuffer;
typedef struct { HRESULT (STDMETHODCALLTYPE *QI)(void *, REFIID, void **); ULONG (STDMETHODCALLTYPE *AddRef)(void *);
                 ULONG (STDMETHODCALLTYPE *Release)(void *); void *(STDMETHODCALLTYPE *Ptr)(void *); DWORD (STDMETHODCALLTYPE *Size)(void *); } XBufVt;
struct ID3DXBuffer { const XBufVt *vt; };
typedef HRESULT (WINAPI *CompileShader_t)(LPCSTR, UINT, const void *, void *, LPCSTR, LPCSTR, DWORD, ID3DXBuffer **, ID3DXBuffer **, void **);

static const char g_smaaPre[] =
    "#define SMAA_HLSL_3\n#define SMAA_PRESET_HIGH\n#define mad(a, b, c) ((a) * (b) + (c))\nfloat4 rtMetrics : register(c0);\n#define SMAA_RT_METRICS rtMetrics\n";
static const char g_smaaPost[] =
    "\nsampler2D s0 : register(s0);\nsampler2D s1 : register(s1);\nsampler2D s2 : register(s2);\n"
    "void VS1(inout float4 p : POSITION, inout float2 t : TEXCOORD0, out float4 o[3] : TEXCOORD1) {\n"
    "  p.xy += float2(-1.0, 1.0) * rtMetrics.xy; SMAAEdgeDetectionVS(t, o); }\n"
    "float4 PS1(float2 t : TEXCOORD0, float4 o[3] : TEXCOORD1) : COLOR {\n"
    "  return float4(SMAALumaEdgeDetectionPS(t, o, s0), 0.0, 0.0); }\n"
    "void VS2(inout float4 p : POSITION, inout float2 t : TEXCOORD0, out float2 pc : TEXCOORD1, out float4 o[3] : TEXCOORD2) {\n"
    "  p.xy += float2(-1.0, 1.0) * rtMetrics.xy; SMAABlendingWeightCalculationVS(t, pc, o); }\n"
    "float4 PS2(float2 t : TEXCOORD0, float2 pc : TEXCOORD1, float4 o[3] : TEXCOORD2) : COLOR {\n"
    "  return SMAABlendingWeightCalculationPS(t, pc, o, s0, s1, s2, 0.0); }\n"
    "void VS3(inout float4 p : POSITION, inout float2 t : TEXCOORD0, out float4 o : TEXCOORD1) {\n"
    "  p.xy += float2(-1.0, 1.0) * rtMetrics.xy; SMAANeighborhoodBlendingVS(t, o); }\n"
    "float4 PS3(float2 t : TEXCOORD0, float4 o : TEXCOORD1) : COLOR {\n"
    "  return SMAANeighborhoodBlendingPS(t, o, s0, s1); }\n";

static IDirect3DVertexShader9 *g_vs[3];
/* colour correction (single pass after SMAA) */
static const char g_ccSrc[] =
    "float4 rtMetrics : register(c0);\n"
    "float4 p1 : register(c1);  /* exposure (x2^), contrast, saturation, vibrance */\n"
    "float4 p2 : register(c2);  /* lift rgb, 1/gamma */\n"
    "float4 p3 : register(c3);  /* gain rgb, sharpen */\n"
    "float4 p4 : register(c4);  /* white balance rgb, vignette */\n"
    "sampler2D s0 : register(s0);\n"
    "void VSC(inout float4 p : POSITION, inout float2 t : TEXCOORD0) { p.xy += float2(-1.0, 1.0) * rtMetrics.xy; }\n"
    "float4 PSC(float2 t : TEXCOORD0) : COLOR {\n"
    "  float3 c = tex2D(s0, t).rgb;\n"
    "  float3 L = float3(0.2126, 0.7152, 0.0722);\n"
    "  float2 o = rtMetrics.xy;\n"
    "  float3 n = tex2D(s0, t + float2(o.x, 0)).rgb + tex2D(s0, t - float2(o.x, 0)).rgb\n"
    "           + tex2D(s0, t + float2(0, o.y)).rgb + tex2D(s0, t - float2(0, o.y)).rgb;\n"
    "  float d = dot(c - n * 0.25, L);\n"
    "  c = saturate(c + clamp(d, -0.05, 0.05) * p3.w * 4.0);\n"
    "  c *= p1.x * p4.rgb;\n"
    "  c = c * p3.rgb + p2.rgb * (1.0 - c);\n"
    "  c = pow(saturate(c), p2.w);\n"
    "  c = saturate((c - 0.5) * p1.y + 0.5);\n"
    "  float y = dot(c, L);\n"
    "  float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));\n"
    "  float s = p1.z * (1.0 + p1.w * (1.0 - (mx - mn)));\n"
    "  c = saturate(lerp(y.xxx, c, s));\n"
    "  float2 v = t - 0.5;\n"
    "  c *= 1.0 - p4.w * smoothstep(0.35, 0.85, length(v * float2(1.0, rtMetrics.w / rtMetrics.z)) * 1.4142);\n"
    "  return float4(c, 1.0);\n"
    "}\n";
static IDirect3DVertexShader9 *g_ccVs; static IDirect3DPixelShader9 *g_ccPs;
static int g_cc, g_ccReady, g_ccKey, g_ccOn = 1, g_ccKeyDown;
static float g_ccP[4][4];
static IDirect3DPixelShader9 *g_ps[3];
static IDirect3DVertexDeclaration9 *g_decl;
static IDirect3DTexture9 *g_area, *g_search;            /* managed */
static IDirect3DTexture9 *g_color, *g_edges, *g_blend;  /* default pool */
static IDirect3DStateBlock9 *g_sb;
static UINT g_w, g_h;
static int g_smaaReady;                                 /* 0 = not tried, 1 = ok, -1 = failed */

static void ReleaseDefault(void)
{
    if (g_color) { IDirect3DTexture9_Release(g_color); g_color = NULL; }
    if (g_edges) { IDirect3DTexture9_Release(g_edges); g_edges = NULL; }
    if (g_blend) { IDirect3DTexture9_Release(g_blend); g_blend = NULL; }
    if (g_sb) { IDirect3DStateBlock9_Release(g_sb); g_sb = NULL; }
    g_w = g_h = 0;
}
void GfxReset(void) { ReleaseDefault(); }

static int Compile(IDirect3DDevice9 *d, CompileShader_t cs, const char *src, int n, int i)
{
    char e[8]; ID3DXBuffer *code = NULL, *err = NULL;
    _snprintf(e, sizeof e, "VS%d", i + 1);
    HRESULT hr = cs(src, n, NULL, NULL, e, "vs_3_0", 0, &code, &err, NULL);
    if (SUCCEEDED(hr)) { hr = IDirect3DDevice9_CreateVertexShader(d, (const DWORD *)code->vt->Ptr(code), &g_vs[i]); code->vt->Release(code); }
    else if (err) { Log("gfx: SMAA %s: %.400s", e, (const char *)err->vt->Ptr(err)); }
    if (err) { err->vt->Release(err); err = NULL; }
    if (FAILED(hr)) return 0;
    _snprintf(e, sizeof e, "PS%d", i + 1);
    hr = cs(src, n, NULL, NULL, e, "ps_3_0", 0, &code, &err, NULL);
    if (SUCCEEDED(hr)) { hr = IDirect3DDevice9_CreatePixelShader(d, (const DWORD *)code->vt->Ptr(code), &g_ps[i]); code->vt->Release(code); }
    else if (err) { Log("gfx: SMAA %s: %.400s", e, (const char *)err->vt->Ptr(err)); }
    if (err) err->vt->Release(err);
    return SUCCEEDED(hr);
}

static int SmaaInit(IDirect3DDevice9 *d)
{
    HMODULE x = GetModuleHandleA("d3dx9_36.dll"); if (!x) x = LoadLibraryA("d3dx9_36.dll");
    CompileShader_t cs = x ? (CompileShader_t)GetProcAddress(x, "D3DXCompileShader") : NULL;
    if (!cs) { Log("gfx: SMAA off - D3DXCompileShader not found"); return 0; }
    size_t n = sizeof g_smaaPre - 1 + sizeof g_smaaHlsl - 1 + sizeof g_smaaPost - 1;
    char *src = (char *)malloc(n + 1);
    memcpy(src, g_smaaPre, sizeof g_smaaPre - 1);
    memcpy(src + sizeof g_smaaPre - 1, g_smaaHlsl, sizeof g_smaaHlsl - 1);
    memcpy(src + sizeof g_smaaPre - 1 + sizeof g_smaaHlsl - 1, g_smaaPost, sizeof g_smaaPost);
    int ok = 1;
    for (int i = 0; i < 3 && ok; i++) ok = Compile(d, cs, src, (int)n, i);
    free(src);
    if (!ok) { Log("gfx: SMAA off - shader compile failed"); return 0; }
    static const D3DVERTEXELEMENT9 el[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()};
    if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(d, el, &g_decl))) return 0;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DDevice9_CreateTexture(d, 160, 560, 1, 0, D3DFMT_A8L8, D3DPOOL_MANAGED, &g_area, NULL))) return 0;
    if (SUCCEEDED(IDirect3DTexture9_LockRect(g_area, 0, &lr, NULL, 0))) {
        for (int y = 0; y < 560; y++) memcpy((BYTE *)lr.pBits + y * lr.Pitch, g_smaaArea + y * 320, 320);
        IDirect3DTexture9_UnlockRect(g_area, 0);
    }
    if (FAILED(IDirect3DDevice9_CreateTexture(d, 64, 16, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED, &g_search, NULL))) return 0;
    if (SUCCEEDED(IDirect3DTexture9_LockRect(g_search, 0, &lr, NULL, 0))) {
        for (int y = 0; y < 16; y++) memcpy((BYTE *)lr.pBits + y * lr.Pitch, g_smaaSearch + y * 64, 64);
        IDirect3DTexture9_UnlockRect(g_search, 0);
    }
    Log("gfx: SMAA ready");
    return 1;
}

static int CcInit(IDirect3DDevice9 *d)
{
    HMODULE x = GetModuleHandleA("d3dx9_36.dll"); if (!x) x = LoadLibraryA("d3dx9_36.dll");
    CompileShader_t cs = x ? (CompileShader_t)GetProcAddress(x, "D3DXCompileShader") : NULL;
    if (!cs) return 0;
    ID3DXBuffer *code = NULL, *err = NULL; int n = (int)sizeof g_ccSrc - 1;
    HRESULT hr = cs(g_ccSrc, n, NULL, NULL, "VSC", "vs_3_0", 0, &code, &err, NULL);
    if (SUCCEEDED(hr)) { hr = IDirect3DDevice9_CreateVertexShader(d, (const DWORD *)code->vt->Ptr(code), &g_ccVs); code->vt->Release(code); }
    else if (err) Log("gfx: colour VS: %.400s", (const char *)err->vt->Ptr(err));
    if (err) { err->vt->Release(err); err = NULL; }
    if (FAILED(hr)) return 0;
    hr = cs(g_ccSrc, n, NULL, NULL, "PSC", "ps_3_0", 0, &code, &err, NULL);
    if (SUCCEEDED(hr)) { hr = IDirect3DDevice9_CreatePixelShader(d, (const DWORD *)code->vt->Ptr(code), &g_ccPs); code->vt->Release(code); }
    else if (err) Log("gfx: colour PS: %.400s", (const char *)err->vt->Ptr(err));
    if (err) err->vt->Release(err);
    if (FAILED(hr)) return 0;
    if (!g_decl) {
        static const D3DVERTEXELEMENT9 el[] = {
            {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()};
        if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(d, el, &g_decl))) return 0;
    }
    Log("gfx: colour correction ready");
    return 1;
}

static int SmaaTargets(IDirect3DDevice9 *d, UINT w, UINT h)
{
    if (g_color && w == g_w && h == g_h) return 1;
    ReleaseDefault();
    if (FAILED(IDirect3DDevice9_CreateTexture(d, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_color, NULL)) ||
        FAILED(IDirect3DDevice9_CreateTexture(d, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_edges, NULL)) ||
        FAILED(IDirect3DDevice9_CreateTexture(d, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_blend, NULL)) ||
        FAILED(IDirect3DDevice9_CreateStateBlock(d, D3DSBT_ALL, &g_sb))) { ReleaseDefault(); return 0; }
    g_w = w; g_h = h;
    return 1;
}

static void Samp(IDirect3DDevice9 *d, DWORD s, IDirect3DTexture9 *t, DWORD filt)
{
    o_SetTexture(d, s, (IDirect3DBaseTexture9 *)t);
    o_SetSamplerState(d, s, D3DSAMP_MINFILTER, filt); o_SetSamplerState(d, s, D3DSAMP_MAGFILTER, filt);
    o_SetSamplerState(d, s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    o_SetSamplerState(d, s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); o_SetSamplerState(d, s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    o_SetSamplerState(d, s, D3DSAMP_SRGBTEXTURE, 0); o_SetSamplerState(d, s, D3DSAMP_MAXMIPLEVEL, 0);
}
static void Quad(IDirect3DDevice9 *d)
{
    static const float v[4][6] = {{-1, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 1, 0}, {-1, -1, 0, 1, 0, 1}, {1, -1, 0, 1, 1, 1}};
    IDirect3DDevice9_DrawPrimitiveUP(d, D3DPT_TRIANGLESTRIP, 2, v, sizeof v[0]);
}

static void PassState(IDirect3DDevice9 *d)
{
    IDirect3DDevice9_SetDepthStencilSurface(d, NULL);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_SRGBWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_COLORWRITEENABLE, 0xF);
    IDirect3DDevice9_SetRenderState(d, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_CLIPPLANEENABLE, 0);
    IDirect3DDevice9_SetVertexDeclaration(d, g_decl);
}

void GfxPresent(IDirect3DDevice9 *d)
{
    /* toggle key for the colour correction (compare on/off) */
    if (g_ccKey) {
        int down = (GetAsyncKeyState(g_ccKey) & 0x8000) != 0;
        if (down && !g_ccKeyDown) { g_ccOn = !g_ccOn; Log("gfx: colour correction %s", g_ccOn ? "on" : "off"); }
        g_ccKeyDown = down;
    }
    int smaa = g_smaa && g_smaaReady >= 0, cc = g_cc && g_ccOn && g_ccReady >= 0;
    if (!smaa && !cc) return;
    if (smaa && !g_smaaReady) { g_smaaReady = SmaaInit(d) ? 1 : -1; smaa = g_smaaReady > 0; }
    if (cc && !g_ccReady) { g_ccReady = CcInit(d) ? 1 : -1; cc = g_ccReady > 0; }
    if (!smaa && !cc) return;
    IDirect3DSurface9 *bb = NULL, *rt0 = NULL, *ds = NULL, *s = NULL;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC bd; IDirect3DSurface9_GetDesc(bb, &bd);
    if (!SmaaTargets(d, bd.Width, bd.Height)) {
        IDirect3DSurface9_Release(bb); Log("gfx: post-processing off - render targets"); g_smaaReady = g_ccReady = -1; return;
    }
    g_inGfx = 1;
    IDirect3DStateBlock9_Capture(g_sb);
    IDirect3DDevice9_GetRenderTarget(d, 0, &rt0);
    IDirect3DDevice9_GetDepthStencilSurface(d, &ds);
    if (SUCCEEDED(IDirect3DDevice9_BeginScene(d))) {
        IDirect3DTexture9_GetSurfaceLevel(g_color, 0, &s);
        IDirect3DDevice9_StretchRect(d, bb, NULL, s, NULL, D3DTEXF_NONE);
        IDirect3DSurface9_Release(s);
        PassState(d);
        float m[4] = {1.0f / bd.Width, 1.0f / bd.Height, (float)bd.Width, (float)bd.Height};
        IDirect3DDevice9_SetVertexShaderConstantF(d, 0, m, 1);
        IDirect3DDevice9_SetPixelShaderConstantF(d, 0, m, 1);
        IDirect3DTexture9 *src = g_color;
        if (smaa) {
            /* 1: edges */
            IDirect3DTexture9_GetSurfaceLevel(g_edges, 0, &s);
            IDirect3DDevice9_SetRenderTarget(d, 0, s); IDirect3DSurface9_Release(s);
            IDirect3DDevice9_Clear(d, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
            Samp(d, 0, g_color, D3DTEXF_LINEAR);
            IDirect3DDevice9_SetVertexShader(d, g_vs[0]); IDirect3DDevice9_SetPixelShader(d, g_ps[0]);
            Quad(d);
            /* 2: blending weights */
            IDirect3DTexture9_GetSurfaceLevel(g_blend, 0, &s);
            IDirect3DDevice9_SetRenderTarget(d, 0, s); IDirect3DSurface9_Release(s);
            IDirect3DDevice9_Clear(d, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
            Samp(d, 0, g_edges, D3DTEXF_LINEAR); Samp(d, 1, g_area, D3DTEXF_LINEAR); Samp(d, 2, g_search, D3DTEXF_POINT);
            IDirect3DDevice9_SetVertexShader(d, g_vs[1]); IDirect3DDevice9_SetPixelShader(d, g_ps[1]);
            Quad(d);
            /* 3: neighbourhood blending: into the back buffer, or into "edges" when colour correction follows */
            if (cc) { IDirect3DTexture9_GetSurfaceLevel(g_edges, 0, &s); IDirect3DDevice9_SetRenderTarget(d, 0, s); IDirect3DSurface9_Release(s); src = g_edges; }
            else IDirect3DDevice9_SetRenderTarget(d, 0, bb);
            Samp(d, 0, g_color, D3DTEXF_LINEAR); Samp(d, 1, g_blend, D3DTEXF_LINEAR);
            o_SetTexture(d, 2, NULL);
            IDirect3DDevice9_SetVertexShader(d, g_vs[2]); IDirect3DDevice9_SetPixelShader(d, g_ps[2]);
            Quad(d);
            o_SetTexture(d, 1, NULL);
        }
        if (cc) {
            IDirect3DDevice9_SetRenderTarget(d, 0, bb);
            Samp(d, 0, src, D3DTEXF_POINT);
            IDirect3DDevice9_SetPixelShaderConstantF(d, 1, &g_ccP[0][0], 4);
            IDirect3DDevice9_SetVertexShader(d, g_ccVs); IDirect3DDevice9_SetPixelShader(d, g_ccPs);
            Quad(d);
        }
        IDirect3DDevice9_EndScene(d);
    }
    IDirect3DDevice9_SetRenderTarget(d, 0, rt0);
    IDirect3DDevice9_SetDepthStencilSurface(d, ds);
    IDirect3DStateBlock9_Apply(g_sb);
    if (rt0) IDirect3DSurface9_Release(rt0);
    if (ds) IDirect3DSurface9_Release(ds);
    IDirect3DSurface9_Release(bb);
    g_inGfx = 0;
}

/* --------------------------------------------------------------- setup */
void GfxInit(void)
{
    g_af = IniInt("Graphics", "Anisotropic", 16);
    g_smaa = IniInt("Graphics", "SMAA", 1);
    if (g_af < 2) g_af = 0;
    if (g_af > 16) g_af = 16;
    Log("gfx: anisotropic %d, SMAA %d", g_af, g_smaa);
    { void MsaaInit(void); MsaaInit(); }

    g_cc = IniInt("Color", "Enabled", 1);
    float ev = IniFloat("Color", "Exposure", 0.f), con = IniFloat("Color", "Contrast", 1.08f);
    float sat = IniFloat("Color", "Saturation", 1.05f), vib = IniFloat("Color", "Vibrance", 0.15f);
    float gam = IniFloat("Color", "Gamma", 1.f), lift = IniFloat("Color", "Lift", 0.f), gain = IniFloat("Color", "Gain", 1.f);
    float temp = IniFloat("Color", "Temperature", 0.f), tint = IniFloat("Color", "Tint", 0.f);
    float sharp = IniFloat("Color", "Sharpen", 0.3f), vig = IniFloat("Color", "Vignette", 0.15f);
    char kb[32]; IniStr("Color", "ToggleKey", "0x79", kb, sizeof kb); g_ccKey = (int)strtol(kb, NULL, 0);
    if (gam < 0.2f) gam = 0.2f;
    float g[4][4] = {
        {powf(2.f, ev), con, sat, vib},
        {lift, lift, lift, 1.f / gam},
        {gain, gain, gain, sharp},
        /* white balance: warm (+) / cool (-), tint green (-) / magenta (+), kept at equal brightness */
        {1.f + 0.10f * temp + 0.05f * tint, 1.f - 0.10f * tint, 1.f - 0.10f * temp + 0.05f * tint, vig}};
    memcpy(g_ccP, g, sizeof g);
    if (g_cc) Log("gfx: colour correction: exposure %+.2f contrast %.2f saturation %.2f vibrance %.2f gamma %.2f lift %.2f gain %.2f temperature %+.2f tint %+.2f sharpen %.2f vignette %.2f (toggle key 0x%02X)",
                  ev, con, sat, vib, gam, lift, gain, temp, tint, sharp, vig, g_ccKey);
}

void GfxDevice(IDirect3DDevice9 *d)
{
    D3DCAPS9 caps;
    if (g_af && SUCCEEDED(IDirect3DDevice9_GetDeviceCaps(d, &caps))) {
        if ((int)caps.MaxAnisotropy < g_af) g_af = (int)caps.MaxAnisotropy;
        if (g_af < 2) g_af = 0;
    }
    void *p;
    if ((p = VtSwapPublic(d, 69, (void *)hk_SetSamplerState))) o_SetSamplerState = (SetSamplerState_t)p;
    if ((p = VtSwapPublic(d, 65, (void *)hk_SetTexture)))      o_SetTexture = (SetTexture_t)p;
    if ((p = VtSwapPublic(d, 28, (void *)hk_CreateRenderTarget))) o_CreateRenderTarget = (CreateRT_t)p;
    if ((p = VtSwapPublic(d, 29, (void *)hk_CreateDepthStencil))) o_CreateDepthStencil = (CreateDS_t)p;
    if (!o_SetSamplerState) o_SetSamplerState = (SetSamplerState_t)(*(void ***)d)[69];
    if (!o_SetTexture) o_SetTexture = (SetTexture_t)(*(void ***)d)[65];
    Log("gfx: device hooks on (anisotropic %dx)", g_af);
}
