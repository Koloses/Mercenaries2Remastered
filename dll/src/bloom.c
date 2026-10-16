/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Linear HDR bloom, as on Xbox 360. The Xbox keeps the bloom chain (BlurH, BlurV, BloomCombiner targets) in
 * filterable FP16. The PC port (PgLtiBloomPc, 0x74DC90) creates them as 16-bit integer A16B16G16R16 and its six
 * bloom shaders squeeze HDR values into 0..1 (sat(x)*0.375 + pow(x-1,0.54)/255, unpacked after sampling). The
 * blur taps sample between texels, so the hardware blends packed values and most of the brightness above 1 of
 * small bright spots (fire, explosions, lights) is lost: little glow. The targets are made FP16 (A16B16G16R16F)
 * again and the six shaders are replaced by versions without the packing. [Graphics] LinearBloom. */
#define COBJMACROS
#include "m2r.h"
#include <d3d9.h>
#include "d3dhook.h"
#include "bloom_shaders.h"

void *VtSwapPublic(void *obj, int idx, void *hook);
typedef HRESULT (STDMETHODCALLTYPE *CreatePS_t)(IDirect3DDevice9 *, const DWORD *, IDirect3DPixelShader9 **);
static CreatePS_t o_CreatePS;
static int g_on, g_swapped;

typedef struct { const char *name; uint32_t len, crc; const uint32_t *code; } Rep;
static const Rep g_rep[] = {
    {"PgDownSampleFP2_3",   k_PgDownSampleFP2_3_origLen,   k_PgDownSampleFP2_3_origCrc,   k_PgDownSampleFP2_3_lin},
    {"PgDownSampleFP3_3",   k_PgDownSampleFP3_3_origLen,   k_PgDownSampleFP3_3_origCrc,   k_PgDownSampleFP3_3_lin},
    {"PgBlurHFP_3",         k_PgBlurHFP_3_origLen,         k_PgBlurHFP_3_origCrc,         k_PgBlurHFP_3_lin},
    {"PgBlurVFP_3",         k_PgBlurVFP_3_origLen,         k_PgBlurVFP_3_origCrc,         k_PgBlurVFP_3_lin},
    {"PgBloomCombinerFP_3", k_PgBloomCombinerFP_3_origLen, k_PgBloomCombinerFP_3_origCrc, k_PgBloomCombinerFP_3_lin},
    {"PgToneMappingFP_3",   k_PgToneMappingFP_3_origLen,   k_PgToneMappingFP_3_origCrc,   k_PgToneMappingFP_3_linmb},
};

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
    if (c && c[0] == 0xFFFF0300) {
        DWORD n = CodeLen(c), crc = 0;
        for (unsigned i = 0; i < sizeof g_rep / sizeof g_rep[0]; i++) if (g_rep[i].len == n) {
            if (!crc) crc = Crc32((const BYTE *)c, n);
            if (crc != g_rep[i].crc) continue;
            HRESULT hr = o_CreatePS(d, (const DWORD *)g_rep[i].code, o);
            if (SUCCEEDED(hr)) { g_swapped++; Log("bloom: %s replaced (linear)", g_rep[i].name); return hr; }
            Log("bloom: replacement %s failed (%08lX)", g_rep[i].name, hr);
            break;
        }
    }
    return o_CreatePS(d, c, o);
}
void BloomDevice(IDirect3DDevice9 *d)
{
    if (!g_on || o_CreatePS) return;
    void *p;
    if ((p = VtSwapPublic(d, 106, (void *)hk_CreatePS))) o_CreatePS = (CreatePS_t)p;
}
void BloomInit(void)
{
    if (!IniInt("Graphics", "LinearBloom", 1)) return;
    /* the three render targets of PgLtiBloomPc: push 0x24 (A16B16G16R16) -> push 0x71 (A16B16G16R16F) */
    if (!Expect(0x74DD2C, "6A 00 6A 24 6A 01 6A 01") || !Expect(0x74DECC, "6A 00 6A 24 6A 01 6A 01") ||
        !Expect(0x74E09E, "6A 00 6A 24 2B C2 6A 01")) { Log("bloom: code mismatch, skipped"); return; }
    static const BYTE f16 = 0x71;
    WriteMem(0x74DD2F, &f16, 1); WriteMem(0x74DECF, &f16, 1); WriteMem(0x74E0A1, &f16, 1);
    g_on = 1;
    Log("bloom: FP16 bloom targets, linear bloom shaders");
}
