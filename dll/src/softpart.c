/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Soft particles. The particle blend passes bind the scene depth (DepthBufferCopy, G32R32F, .r = z/w of the depth
 * pre-pass) to sampler 1 on both platforms, but the PC particle pixel shader (PgFXFP_3) never reads it, so smoke
 * and fire cut hard lines into the ground and walls. PgFXFP_3 is replaced at creation by the Xbox 360 PgFXFP
 * behaviour (shaders.bin): fade = saturate((sceneViewZ - particleViewZ) * 0.9), colour clamp [0, 32]. The particle
 * vertex shader stays original (replacing it hid every particle): the projection terms the fade needs come from the
 * VS ViewProj rows c9/c10 and are passed in c221; c220 = (1/fade distance, enable, 1/width, 1/height). c220 is a
 * register the engine never writes, so while the DLL has not enabled it the shader draws like the original.
 * The depth copy is read back once and checked (0 < z/w <= 1, sane view depths); if not, the fade stays off.
 * The Xbox near-camera fade (vertex TEXCOORD3.y) is not ported: the PC fade word is not reliable.
 * [Graphics] SoftParticles, SoftParticleDistance. */
#define COBJMACROS
#include "m2r.h"
#include <d3d9.h>
#include "d3dhook.h"
#include "softpart_shaders.h"

void *VtSwapPublic(void *obj, int idx, void *hook);
typedef HRESULT (STDMETHODCALLTYPE *CreateVS_t)(IDirect3DDevice9 *, const DWORD *, IDirect3DVertexShader9 **);
typedef HRESULT (STDMETHODCALLTYPE *CreatePS_t)(IDirect3DDevice9 *, const DWORD *, IDirect3DPixelShader9 **);
typedef HRESULT (STDMETHODCALLTYPE *SetPS_t)(IDirect3DDevice9 *, IDirect3DPixelShader9 *);
typedef HRESULT (STDMETHODCALLTYPE *DIP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
static CreateVS_t o_CreateVS; static CreatePS_t o_CreatePS; static SetPS_t o_SetPS; static DIP_t o_DIP; static DP_t o_DP;
static int g_cur;                                   /* a soft particle pixel shader is bound */
static IDirect3DPixelShader9 *g_softPs[8]; static int g_nSoftPs, g_vsSwapped, g_psSwapped;
static float g_k = 0.9f;
static int g_depthState;             /* 0 unchecked, 1 valid, -1 invalid: fade off */

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
static int Is(const DWORD *c, DWORD len, DWORD crc)
{
    if (!c || IsBadReadPtr(c, 8)) return 0;
    DWORD n = CodeLen(c);
    return n == len && Crc32((const BYTE *)c, n) == crc;
}
static HRESULT STDMETHODCALLTYPE hk_CreateVS(IDirect3DDevice9 *d, const DWORD *c, IDirect3DVertexShader9 **o)
{
    if (Is(c, kPgFXVP_len, kPgFXVP_crc32)) {
        HRESULT hr = o_CreateVS(d, (const DWORD *)kPgFXVP_3_soft, o);
        if (SUCCEEDED(hr)) { if (!g_vsSwapped++) Log("soft particles: particle vertex shader replaced"); return hr; }
        Log("soft particles: replacement vertex shader failed (%08lX), original kept", hr);
    }
    return o_CreateVS(d, c, o);
}
int g_softOwnsFx;   /* xbshader.c leaves PgFXFP_3 to this file (its CRC must match here) */
static int g_noZ;   /* dev: [Debug] ParticleNoZ=1 draws the particle shader with the depth test off (diagnosis) */
static HRESULT STDMETHODCALLTYPE hk_CreatePS(IDirect3DDevice9 *d, const DWORD *c, IDirect3DPixelShader9 **o)
{
    if (g_noZ) {
        HRESULT hr = o_CreatePS(d, c, o);
        if (SUCCEEDED(hr) && o && *o && Is(c, kPgFXFP_len, kPgFXFP_crc32) && g_nSoftPs < 8) { g_softPs[g_nSoftPs++] = *o; Log("particles: depth test off for the particle shader (dev)"); }
        return hr;
    }
    if (Is(c, kPgFXFP_len, kPgFXFP_crc32)) {
        HRESULT hr = o_CreatePS(d, (const DWORD *)kPgFXFP_3_soft, o);
        if (SUCCEEDED(hr) && o && *o) {
            if (g_nSoftPs < 8) g_softPs[g_nSoftPs++] = *o;
            if (!g_psSwapped++) Log("soft particles: particle pixel shader replaced");
            return hr;
        }
        Log("soft particles: replacement pixel shader failed (%08lX), original kept", hr);
    }
    return o_CreatePS(d, c, o);
}
/* one read-back of the depth copy: it must hold post-projection z/w in (0,1] (what the Xbox shader expects) */
static int CheckDepth(IDirect3DDevice9 *d, IDirect3DTexture9 *t, const D3DSURFACE_DESC *ds)
{
    IDirect3DSurface9 *rt = NULL, *sys = NULL; int ok = -1, zok = 0; float v[9] = {0}, r9[4] = {0}, r10[4] = {0}, A = 0, B = 0;
    if (ds->Format != D3DFMT_G32R32F && ds->Format != D3DFMT_R32F) { Log("soft particles: depth copy format %d unexpected, fade off", ds->Format); return -1; }
    /* the same projection terms the soft VS derives from ViewProj rows c9 (clip z) and c10 (clip w) */
    if (SUCCEEDED(IDirect3DDevice9_GetVertexShaderConstantF(d, 9, r9, 1)) && SUCCEEDED(IDirect3DDevice9_GetVertexShaderConstantF(d, 10, r10, 1))) {
        float ww = r10[0] * r10[0] + r10[1] * r10[1] + r10[2] * r10[2];
        if (ww > 0) { A = (r9[0] * r10[0] + r9[1] * r10[1] + r9[2] * r10[2]) / ww; B = r9[3] - A * r10[3]; }
    }
    if (FAILED(IDirect3DTexture9_GetSurfaceLevel(t, 0, &rt))) return -1;
    if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(d, ds->Width, ds->Height, ds->Format, D3DPOOL_SYSTEMMEM, &sys, NULL)) &&
        SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(d, rt, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
            int px = ds->Format == D3DFMT_G32R32F ? 8 : 4, n = 0, inside = 0;
            for (int y = 1; y <= 3; y++) for (int x = 1; x <= 3; x++) {
                float f = *(float *)((BYTE *)lr.pBits + (ds->Height * y / 4) * lr.Pitch + (ds->Width * x / 4) * px);
                v[n++] = f;
                if (f == f && f > 0.f && f <= 1.f) {
                    inside++;
                    float z = f - A != 0 ? B / (f - A) : -1;          /* scene view depth as the PS computes it */
                    if (z > 0.05f && z < 100000.f) zok++;
                }
            }
            IDirect3DSurface9_UnlockRect(sys);
            ok = inside >= 5 && zok >= 5 ? 1 : -1;
        }
    }
    if (sys) IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(rt);
    Log("soft particles: depth copy %ux%u fmt %d samples %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f, A %.5f B %.4f, view depths ok %d -> %s",
        ds->Width, ds->Height, ds->Format, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], A, B, zok, ok > 0 ? "soft fade on" : "fade off");
    return ok;
}
/* c220 = (k, enable, 1/w, 1/h), set at draw time (the engine binds the depth texture after the shader).
 * The engine never writes c220; while it is 0 the shader draws exactly like the original. */
static void SetConsts(IDirect3DDevice9 *d)
{
    static int logged;
    float c[4] = {g_k, 0, 0, 0};
    IDirect3DBaseTexture9 *t = NULL; D3DSURFACE_DESC ds = {0};
    if (SUCCEEDED(IDirect3DDevice9_GetTexture(d, 1, &t)) && t) {
        if (IDirect3DBaseTexture9_GetType(t) == D3DRTYPE_TEXTURE &&
            SUCCEEDED(IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)t, 0, &ds)) && ds.Width && ds.Height) {
            c[2] = 1.f / ds.Width; c[3] = 1.f / ds.Height;
            if (!g_depthState) g_depthState = CheckDepth(d, (IDirect3DTexture9 *)t, &ds);
        }
        IDirect3DBaseTexture9_Release(t);
    }
    if (!logged) { logged = 1; Log("soft particles: first particle draw, depth texture %s (%ux%u fmt %d)", c[2] != 0 ? "bound" : "NOT bound", ds.Width, ds.Height, ds.Format); }
    float r9[4], r10[4], ab[4] = {0, 0, 0, 0}; int abOk = 0;
    if (SUCCEEDED(IDirect3DDevice9_GetVertexShaderConstantF(d, 9, r9, 1)) && SUCCEEDED(IDirect3DDevice9_GetVertexShaderConstantF(d, 10, r10, 1))) {
        float ww = r10[0] * r10[0] + r10[1] * r10[1] + r10[2] * r10[2];
        if (ww > 0) { ab[0] = (r9[0] * r10[0] + r9[1] * r10[1] + r9[2] * r10[2]) / ww; ab[1] = r9[3] - ab[0] * r10[3]; abOk = ab[1] != 0; }
    }
    c[1] = (c[2] != 0 && g_depthState > 0 && abOk) ? 1.f : 0.f;
    IDirect3DDevice9_SetPixelShaderConstantF(d, 220, c, 1);
    IDirect3DDevice9_SetPixelShaderConstantF(d, 221, ab, 1);
}
static HRESULT STDMETHODCALLTYPE hk_SetPS(IDirect3DDevice9 *d, IDirect3DPixelShader9 *ps)
{
    g_cur = 0;
    static int logged;
    if (ps) for (int i = 0; i < g_nSoftPs; i++) if (g_softPs[i] == ps) { g_cur = 1; if (!logged) { logged = 1; Log("soft particles: particle shader bound"); } break; }
    return o_SetPS(d, ps);
}
static HRESULT NoZDraw(IDirect3DDevice9 *d, D3DPRIMITIVETYPE pt, INT bv, UINT mi, UINT nv, UINT si, UINT pc, int indexed)
{
    DWORD z = 0; IDirect3DDevice9_GetRenderState(d, D3DRS_ZFUNC, &z);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ZFUNC, D3DCMP_ALWAYS);
    HRESULT hr = indexed ? o_DIP(d, pt, bv, mi, nv, si, pc) : o_DP(d, pt, (UINT)bv, pc);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ZFUNC, z);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_DIP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE pt, INT bv, UINT mi, UINT nv, UINT si, UINT pc)
{
    if (g_cur && g_noZ) return NoZDraw(d, pt, bv, mi, nv, si, pc, 1);
    if (g_cur) SetConsts(d);
    return o_DIP(d, pt, bv, mi, nv, si, pc);
}
static HRESULT STDMETHODCALLTYPE hk_DP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE pt, UINT sv, UINT pc)
{
    if (g_cur && g_noZ) return NoZDraw(d, pt, (INT)sv, 0, 0, 0, pc, 0);
    if (g_cur) SetConsts(d);
    return o_DP(d, pt, sv, pc);
}
void SoftDevice(IDirect3DDevice9 *d)
{
    int def = 1;
#ifdef M2R_SHOT
    g_noZ = IniInt("Debug", "ParticleNoZ", 0);
#endif
    if (o_CreateVS || (!g_noZ && !IniInt("Graphics", "SoftParticles", def))) return;
    float dist = IniFloat("Graphics", "SoftParticleDistance", 1.0f / 0.9f);
    if (dist < 0.05f) dist = 0.05f;
    g_k = 1.f / dist;
    void *p;
    o_CreateVS = (CreateVS_t)1;   /* the particle VS stays original: A,B come from c221 (replacing it hid every particle) */
    (void)hk_CreateVS;
    if ((p = VtSwapPublic(d, 106, (void *)hk_CreatePS))) { o_CreatePS = (CreatePS_t)p; g_softOwnsFx = 1; }
    if ((p = VtSwapPublic(d, 107, (void *)hk_SetPS)))    o_SetPS = (SetPS_t)p;
    if ((p = VtSwapPublic(d, 82, (void *)hk_DIP)))       o_DIP = (DIP_t)p;
    if ((p = VtSwapPublic(d, 81, (void *)hk_DP)))        o_DP = (DP_t)p;
    Log("soft particles: on (fade distance %.2f m)", dist);
}
