/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* FPS counter in the top left corner ([Display] FpsCounter, toggle FpsKey, default F9). Drawn after the
 * colour pass so it stays crisp. */
#include "m2r.h"
#define COBJMACROS
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct XFont XFont;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(XFont *, REFIID, void **); ULONG (WINAPI *AddRef)(XFont *); ULONG (WINAPI *Release)(XFont *);
    void *GetDevice, *GetDescA, *GetDescW, *GetTextMetricsA, *GetTextMetricsW, *GetDC, *GetGlyphData,
         *PreloadCharacters, *PreloadGlyphs, *PreloadTextA, *PreloadTextW;
    INT (WINAPI *DrawTextA)(XFont *, void *sprite, LPCSTR, INT, LPRECT, DWORD, D3DCOLOR);
    void *DrawTextW;
    HRESULT (WINAPI *OnLostDevice)(XFont *); HRESULT (WINAPI *OnResetDevice)(XFont *);
} XFontVt;
struct XFont { const XFontVt *vt; };
typedef HRESULT (WINAPI *CF_t)(IDirect3DDevice9 *, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD, LPCSTR, XFont **);

static XFont *g_font; static int g_on, g_key = VK_F9, g_failed, g_lost;
static double g_t0; static int g_frames; static float g_fps, g_ms;
static LARGE_INTEGER g_qpf, g_last;

void FpsReset(void) { if (g_font) { g_font->vt->OnLostDevice(g_font); g_lost = 1; } }

void FpsPresent(IDirect3DDevice9 *d)
{
    if (GetAsyncKeyState(g_key) & 1) { g_on = !g_on; }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    double t = (double)now.QuadPart / g_qpf.QuadPart;
    if (g_last.QuadPart) { float ms = (float)((now.QuadPart - g_last.QuadPart) * 1000.0 / g_qpf.QuadPart); g_ms = g_ms * 0.9f + ms * 0.1f; }
    g_last = now;
    g_frames++;
    if (t - g_t0 >= 0.5) { g_fps = (float)(g_frames / (t - g_t0)); g_frames = 0; g_t0 = t; }
    if (!g_on || g_failed) return;
    if (!g_font) {
        HMODULE x = GetModuleHandleA("d3dx9_36.dll"); if (!x) x = LoadLibraryA("d3dx9_36.dll");
        CF_t cf = x ? (CF_t)GetProcAddress(x, "D3DXCreateFontA") : NULL;
        if (!cf || FAILED(cf(d, 20, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, "Consolas", &g_font))) { g_failed = 1; Log("fps: font failed"); return; }
    }
    if (g_lost) { g_font->vt->OnResetDevice(g_font); g_lost = 0; }
    char s[48]; int n = sprintf(s, "%.0f FPS  %.1f ms", g_fps, g_ms);
    if (FAILED(IDirect3DDevice9_BeginScene(d))) return;
    RECT r = {11, 9, 400, 40};
    g_font->vt->DrawTextA(g_font, NULL, s, n, &r, DT_LEFT | DT_TOP | DT_NOCLIP, 0xC0000000);
    RECT r2 = {10, 8, 400, 40};
    D3DCOLOR c = g_fps >= 55.f ? 0xFF7CFC00 : g_fps >= 30.f ? 0xFFFFD700 : 0xFFFF4040;
    g_font->vt->DrawTextA(g_font, NULL, s, n, &r2, DT_LEFT | DT_TOP | DT_NOCLIP, c);
    IDirect3DDevice9_EndScene(d);
}

void FpsInit(void)
{
    g_on = IniInt("Display", "FpsCounter", 0);
    char b[16]; IniStr("Display", "FpsKey", "0x78", b, sizeof b); g_key = (int)strtol(b, NULL, 0); if (!g_key) g_key = VK_F9;
    Log("fps: counter %s (toggle key 0x%02X)", g_on ? "on" : "off", g_key);
}
