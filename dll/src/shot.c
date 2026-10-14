/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Screenshots ([Display] ScreenshotKey, default F11 = 0x7A), saved as PNG in Screenshots\ in the game folder.
 *
 *   key          the finished frame (with the mod's post effects, without the FPS counter)
 *   Shift + key  a comparison of the same frame: _off.png before the mod's post effects (SMAA, colour grading,
 *                sharpening, vignette), _on.png after them, and _compare.png with the left half off and the
 *                right half on, split by a thin line
 *
 * The frame is copied in Present: once before the post effects run (comparison only) and once after them,
 * before the FPS counter is drawn. */
#include "m2r.h"
#define COBJMACROS
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>

typedef HRESULT (WINAPI *SaveSurf_t)(LPCSTR, DWORD, IDirect3DSurface9 *, const PALETTEENTRY *, const RECT *);
static SaveSurf_t g_save;
static int g_key, g_down, g_want;            /* g_want: 1 = screenshot, 2 = comparison (this frame) */
static IDirect3DSurface9 *g_off;             /* frame before the post effects (comparison) */

void ShotInit(void)
{
    char b[32]; IniStr("Display", "ScreenshotKey", "0x7A", b, sizeof b);
    g_key = (int)strtol(b, NULL, 0);
    if (g_key) Log("shot: screenshot key 0x%02X (Shift: comparison)", g_key);
}

/* copy of the back buffer in system memory */
static IDirect3DSurface9 *Grab(IDirect3DDevice9 *d)
{
    IDirect3DSurface9 *bb = NULL, *rt = NULL, *mem = NULL;
    D3DSURFACE_DESC sd;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return NULL;
    IDirect3DSurface9_GetDesc(bb, &sd);
    int ok = SUCCEEDED(IDirect3DDevice9_CreateRenderTarget(d, sd.Width, sd.Height, sd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL)) &&
             SUCCEEDED(IDirect3DDevice9_StretchRect(d, bb, NULL, rt, NULL, D3DTEXF_NONE)) &&
             SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(d, sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &mem, NULL)) &&
             SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(d, rt, mem));
    if (rt) IDirect3DSurface9_Release(rt);
    IDirect3DSurface9_Release(bb);
    if (!ok && mem) { IDirect3DSurface9_Release(mem); mem = NULL; }
    return mem;
}

static int Save(IDirect3DSurface9 *s, const char *path)
{
    HRESULT hr = g_save(path, 3 /* D3DXIFF_PNG */, s, NULL, NULL);
    Log("shot: %s %s", path, SUCCEEDED(hr) ? "saved" : "failed");
    return SUCCEEDED(hr);
}

/* left half of a, right half of b, a 2-pixel light line between them (32-bit formats) */
static IDirect3DSurface9 *Compose(IDirect3DDevice9 *d, IDirect3DSurface9 *a, IDirect3DSurface9 *b)
{
    D3DSURFACE_DESC sd; IDirect3DSurface9_GetDesc(a, &sd);
    if (sd.Format != D3DFMT_X8R8G8B8 && sd.Format != D3DFMT_A8R8G8B8) return NULL;
    IDirect3DSurface9 *c = NULL;
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(d, sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &c, NULL))) return NULL;
    D3DLOCKED_RECT la, lb, lc;
    if (FAILED(IDirect3DSurface9_LockRect(a, &la, NULL, D3DLOCK_READONLY))) { IDirect3DSurface9_Release(c); return NULL; }
    if (FAILED(IDirect3DSurface9_LockRect(b, &lb, NULL, D3DLOCK_READONLY))) { IDirect3DSurface9_UnlockRect(a); IDirect3DSurface9_Release(c); return NULL; }
    if (FAILED(IDirect3DSurface9_LockRect(c, &lc, NULL, 0))) { IDirect3DSurface9_UnlockRect(a); IDirect3DSurface9_UnlockRect(b); IDirect3DSurface9_Release(c); return NULL; }
    UINT mid = sd.Width / 2;
    for (UINT y = 0; y < sd.Height; y++) {
        const DWORD *pa = (const DWORD *)((const BYTE *)la.pBits + y * la.Pitch), *pb = (const DWORD *)((const BYTE *)lb.pBits + y * lb.Pitch);
        DWORD *pc = (DWORD *)((BYTE *)lc.pBits + y * lc.Pitch);
        for (UINT x = 0; x < sd.Width; x++) pc[x] = x + 1 >= mid && x <= mid ? 0xFFE6E6E6 : x < mid ? pa[x] : pb[x];
    }
    IDirect3DSurface9_UnlockRect(c); IDirect3DSurface9_UnlockRect(b); IDirect3DSurface9_UnlockRect(a);
    return c;
}

/* before the post effects (d3d.c Present) */
void ShotPre(IDirect3DDevice9 *d)
{
    g_want = 0;
    if (!g_key) return;
    int down = (GetAsyncKeyState(g_key) & 0x8000) != 0, go = down && !g_down;
    g_down = down;
    if (!go) return;
    if (!g_save) {
        HMODULE x = GetModuleHandleA("d3dx9_36.dll"); if (!x) x = LoadLibraryA("d3dx9_36.dll");
        if (x) g_save = (SaveSurf_t)GetProcAddress(x, "D3DXSaveSurfaceToFileA");
        if (!g_save) { Log("shot: D3DXSaveSurfaceToFileA not found"); return; }
    }
    g_want = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 2 : 1;
    if (g_want == 2 && !(g_off = Grab(d))) { Log("shot: capture failed"); g_want = 0; }
}

/* after the post effects, before the FPS counter */
void ShotPresent(IDirect3DDevice9 *d)
{
    if (!g_want) return;
    int want = g_want; g_want = 0;
    IDirect3DSurface9 *on = Grab(d);
    if (!on) { Log("shot: capture failed"); if (g_off) { IDirect3DSurface9_Release(g_off); g_off = NULL; } return; }
    char dir[MAX_PATH], base[MAX_PATH], p[MAX_PATH];
    _snprintf(dir, sizeof dir, "%s\\Screenshots", g_gameDir);
    CreateDirectoryA(dir, NULL);
    SYSTEMTIME st; GetLocalTime(&st);
    _snprintf(base, sizeof base, "%s\\Mercenaries2Remastered_%04d%02d%02d_%02d%02d%02d", dir,
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    {   /* several shots within one second: _2, _3, ... */
        char b2[MAX_PATH]; lstrcpynA(b2, base, sizeof b2);
        for (int n = 2; n < 100; n++) {
            _snprintf(p, sizeof p, "%s%s.png", base, want == 2 ? "_on" : "");
            if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) break;
            _snprintf(base, sizeof base, "%s_%d", b2, n);
        }
    }
    if (want == 1) {
        _snprintf(p, sizeof p, "%s.png", base); Save(on, p);
    } else {
        _snprintf(p, sizeof p, "%s_off.png", base); Save(g_off, p);
        _snprintf(p, sizeof p, "%s_on.png", base); Save(on, p);
        IDirect3DSurface9 *c = Compose(d, g_off, on);
        if (c) { _snprintf(p, sizeof p, "%s_compare.png", base); Save(c, p); IDirect3DSurface9_Release(c); }
        IDirect3DSurface9_Release(g_off); g_off = NULL;
    }
    IDirect3DSurface9_Release(on);
}
