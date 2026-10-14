/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/*
 * water.c - water on native D3D9 drivers.
 *
 * The water simulation binds four 128x128 A32B32G32R32F render targets, clears them with one Clear and
 * then draws additively (ALPHABLENDENABLE) into what was RT3 (the wake map: terrain/seabed height + shore
 * mask, read by the water vertex shader). Native NVIDIA D3D9 drivers do not clear RT1..RT3 of that MRT set
 * completely: tile-sized blocks keep the previous frame's values and the additive pass piles up frame after
 * frame (seen: -29.4863 x 259 = -7637 in the wake map), so blocks of the water grid end up hundreds of
 * metres off - walls, holes, flicker. dgVoodoo/DXVK clear every bound target.
 *
 * Fix: on a target clear with several render targets bound, every extra target is cleared explicitly too.
 */
#include <windows.h>
#include <d3d9.h>

void Log(const char *fmt, ...);
void *VtSwapPublic(void *obj, int idx, void *hook);

typedef HRESULT (STDMETHODCALLTYPE *CLR_t)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
static CLR_t o_CLR;

static HRESULT STDMETHODCALLTYPE hk_CLR(IDirect3DDevice9 *d, DWORD n, const D3DRECT *r, DWORD fl, D3DCOLOR c, float z, DWORD st)
{
    if (fl & D3DCLEAR_TARGET) {
        for (DWORD i = 1; i < 4; i++) {
            IDirect3DSurface9 *rt = NULL;
            if (FAILED(IDirect3DDevice9_GetRenderTarget(d, i, &rt)) || !rt) continue;
            if (n && r) for (DWORD k = 0; k < n; k++) { RECT rc = {r[k].x1, r[k].y1, r[k].x2, r[k].y2}; IDirect3DDevice9_ColorFill(d, rt, &rc, c); }
            else IDirect3DDevice9_ColorFill(d, rt, NULL, c);
            IDirect3DSurface9_Release(rt);
        }
    }
    return o_CLR(d, n, r, fl, c, z, st);
}

void WaterDevice(IDirect3DDevice9 *d)
{
    void *p;
    if ((p = VtSwapPublic(d, 43, (void *)hk_CLR))) o_CLR = (CLR_t)p;
    Log("water: multiple render target clear fix on");
}
