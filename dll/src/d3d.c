/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/*
 * Direct3D 9 hooks: Present (frame limiter, per-frame updates) and a hook
 * on texture creation/unlock so other modules can find & modify game
 * textures (button prompt font).
 */
#include "m2r.h"
#define COBJMACROS
#define INITGUID
#include <d3d9.h>
#include "d3dhook.h"


typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);
typedef HRESULT (STDMETHODCALLTYPE *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT (STDMETHODCALLTYPE *Present_t)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT (STDMETHODCALLTYPE *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
typedef HRESULT (STDMETHODCALLTYPE *CreateTexture_t)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
typedef HRESULT (STDMETHODCALLTYPE *UnlockRect_t)(IDirect3DTexture9 *, UINT);
typedef HRESULT (STDMETHODCALLTYPE *LockRect_t)(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);

static Direct3DCreate9_t o_Direct3DCreate9;
static CreateDevice_t    o_CreateDevice;
static Present_t         o_Present;
static Reset_t           o_Reset;
static CreateTexture_t   o_CreateTexture;
static UnlockRect_t      o_UnlockRect;
static LockRect_t        o_LockRect;
typedef HRESULT (STDMETHODCALLTYPE *SLockRect_t)(IDirect3DSurface9 *, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *SUnlockRect_t)(IDirect3DSurface9 *);
typedef HRESULT (STDMETHODCALLTYPE *UpdateTexture_t)(IDirect3DDevice9 *, IDirect3DBaseTexture9 *, IDirect3DBaseTexture9 *);
static SLockRect_t       o_SLockRect;
static SUnlockRect_t     o_SUnlockRect;
static UpdateTexture_t   o_UpdateTexture;
static D3DUpdateCallback g_updCb[4];
static int g_nUpdCb;
void D3DSetUpdateCallback(D3DUpdateCallback cb) { if (g_nUpdCb < 4) g_updCb[g_nUpdCb++] = cb; }

IDirect3DDevice9 *g_dev;
static D3DTexCallback g_texCbs[4];
static int g_nTexCb;
static void g_texCb(IDirect3DTexture9 *t, const D3DSURFACE_DESC *d, void *bits, int pitch)
{
    for (int i = 0; i < g_nTexCb; i++) g_texCbs[i](t, d, bits, pitch);
}
static D3DFrameCallback g_frameCb[16];
static int g_nFrameCb;

void D3DSetTextureCallback(D3DTexCallback cb) { if (g_nTexCb < 4) g_texCbs[g_nTexCb++] = cb; }
void D3DAddFrameCallback(D3DFrameCallback cb) { if (g_nFrameCb < 16) g_frameCb[g_nFrameCb++] = cb; }

void *VtSwapPublic(void *obj, int idx, void *hook);
void GfxTextureCreated(void *tex, DWORD usage);
void GfxAdjustPP(IDirect3D9 *d3d, UINT ad, D3DPRESENT_PARAMETERS *pp);
void GfxReset(void);
void GfxPresent(IDirect3DDevice9 *d);
void GfxDevice(IDirect3DDevice9 *d);
void GfxInit(void);
void DeformDevice(IDirect3DDevice9 *d);
void PostDevice(IDirect3DDevice9 *d);
void PostReset(void);
/* every vtable slot we patch is remembered so a watchdog can put it back if something
   (native runtime vtable swap, overlays re-hooking Present) replaces it later */
typedef struct { void **vt; int idx; void *hook; } VtRec;
static VtRec g_vtr[512]; static int g_nvtr; static CRITICAL_SECTION g_vtcs; static int g_vtcsInit;
static void VtPatch(void **vt, int idx, void *v)
{
    DWORD old;
    VirtualProtect(&vt[idx], 4, PAGE_EXECUTE_READWRITE, &old);
    vt[idx] = v;
    VirtualProtect(&vt[idx], 4, old, &old);
}
static void *VtSwap(void *obj, int idx, void *hook)
{
    void **vt = *(void ***)obj;
    void *orig = vt[idx];
    if (orig == hook) return NULL;
    VtPatch(vt, idx, hook);
    if (!g_vtcsInit) { InitializeCriticalSection(&g_vtcs); g_vtcsInit = 1; }
    EnterCriticalSection(&g_vtcs);
    int found = 0;
    for (int i = 0; i < g_nvtr; i++) if (g_vtr[i].vt == vt && g_vtr[i].idx == idx) { g_vtr[i].hook = hook; found = 1; }
    if (!found && g_nvtr < 512) { g_vtr[g_nvtr].vt = vt; g_vtr[g_nvtr].idx = idx; g_vtr[g_nvtr].hook = hook; g_nvtr++; }
    LeaveCriticalSection(&g_vtcs);
    return orig;
}
static const char *ModOf(void *p, char *buf, int n)
{
    HMODULE m = NULL; buf[0] = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &m) && m) {
        char f[MAX_PATH]; GetModuleFileNameA(m, f, MAX_PATH); const char *b = strrchr(f, '\\'); lstrcpynA(buf, b ? b + 1 : f, n);
    } else lstrcpynA(buf, "?", n);
    return buf;
}
extern IDirect3DDevice9 *g_dev;
static DWORD WINAPI VtWatch(LPVOID unused)
{
    (void)unused;
    void **devVt = NULL; int logged = 0;
    for (;;) {
        Sleep(250);
        if (!g_dev || !g_vtcsInit) continue;
        void **cur = *(void ***)g_dev;
        EnterCriticalSection(&g_vtcs);
        if (!devVt) devVt = cur;
        if (cur != devVt) {
            /* the device object now uses another vtable: carry all our device hooks over */
            int n = 0;
            for (int i = 0; i < g_nvtr; i++) if (g_vtr[i].vt == devVt) {
                if (cur[g_vtr[i].idx] != g_vtr[i].hook) VtPatch(cur, g_vtr[i].idx, g_vtr[i].hook);
                g_vtr[i].vt = cur; n++;
            }
            { void StateCacheReset(void); StateCacheReset(); }
            if (logged++ < 20) Log("d3d: device vtable changed %p -> %p, %d hooks moved", (void *)devVt, (void *)cur, n);
            devVt = cur;
        }
        for (int i = 0; i < g_nvtr; i++) {
            VtRec *r = &g_vtr[i];
            if (r->vt[r->idx] != r->hook) {
                char m[64];
                if (logged++ < 40) Log("d3d: vtable %p slot %d overwritten by %p (%s), restoring our hook", (void *)r->vt, r->idx, r->vt[r->idx], ModOf(r->vt[r->idx], m, sizeof m));
                VtPatch(r->vt, r->idx, r->hook);
                { void StateCacheReset(void); StateCacheReset(); }
            }
        }
        LeaveCriticalSection(&g_vtcs);
    }
    return 0;
}

/* ---- texture tracking: remember the last LockRect of level 0 per texture */
static __thread IDirect3DTexture9 *t_lockTex;
static __thread D3DLOCKED_RECT t_lock;

static HRESULT STDMETHODCALLTYPE hk_LockRect(IDirect3DTexture9 *t, UINT lvl, D3DLOCKED_RECT *lr, const RECT *r, DWORD fl)
{
    HRESULT hr = o_LockRect(t, lvl, lr, r, fl);
    if (SUCCEEDED(hr) && lvl == 0 && !r && !(fl & D3DLOCK_READONLY)) { t_lockTex = t; t_lock = *lr; }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_UnlockRect(IDirect3DTexture9 *t, UINT lvl)
{
    if (lvl == 0 && t == t_lockTex && g_nTexCb) {
        D3DSURFACE_DESC d;
        if (SUCCEEDED(IDirect3DTexture9_GetLevelDesc(t, 0, &d)))
            g_texCb(t, &d, t_lock.pBits, t_lock.Pitch);
        t_lockTex = NULL;
    }
    return o_UnlockRect(t, lvl);
}

/* same thing for surface-level locks (GetSurfaceLevel(0)->LockRect) */
static __thread IDirect3DSurface9 *t_lockSurf;
static __thread D3DLOCKED_RECT t_slock;

static HRESULT STDMETHODCALLTYPE hk_SLockRect(IDirect3DSurface9 *s, D3DLOCKED_RECT *lr, const RECT *r, DWORD fl)
{
    HRESULT hr = o_SLockRect(s, lr, r, fl);
    if (SUCCEEDED(hr) && !r && !(fl & D3DLOCK_READONLY)) { t_lockSurf = s; t_slock = *lr; }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_SUnlockRect(IDirect3DSurface9 *s)
{
    if (s == t_lockSurf && g_nTexCb) {
        IDirect3DTexture9 *t = NULL;
        if (SUCCEEDED(IDirect3DSurface9_GetContainer(s, &IID_IDirect3DTexture9, (void **)&t)) && t) {
            IDirect3DSurface9 *l0 = NULL;
            if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(t, 0, &l0)) && l0) {
                if (l0 == s) {
                    D3DSURFACE_DESC d;
                    if (SUCCEEDED(IDirect3DSurface9_GetDesc(s, &d))) g_texCb(t, &d, t_slock.pBits, t_slock.Pitch);
                }
                IDirect3DSurface9_Release(l0);
            }
            IDirect3DTexture9_Release(t);
        }
        t_lockSurf = NULL;
    }
    return o_SUnlockRect(s);
}

static HRESULT STDMETHODCALLTYPE hk_UpdateTexture(IDirect3DDevice9 *dev, IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst)
{
    HRESULT hr = o_UpdateTexture(dev, src, dst);
    if (SUCCEEDED(hr)) for (int i = 0; i < g_nUpdCb; i++) g_updCb[i](src, dst);
    return hr;
}

HRESULT D3DUpdateTexture(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst)
{
    return o_UpdateTexture ? o_UpdateTexture(g_dev, src, dst) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE hk_CreateTexture(IDirect3DDevice9 *dev, UINT w, UINT h, UINT lv, DWORD usage,
                                                  D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *sh)
{
    HRESULT hr = o_CreateTexture(dev, w, h, lv, usage, fmt, pool, out, sh);
    if (SUCCEEDED(hr) && out && *out) GfxTextureCreated(*out, usage);
    if (SUCCEEDED(hr) && out && *out && !o_UnlockRect) {
        o_LockRect   = (LockRect_t)VtSwap(*out, 19, (void *)hk_LockRect);
        o_UnlockRect = (UnlockRect_t)VtSwap(*out, 20, (void *)hk_UnlockRect);
        IDirect3DSurface9 *sf = NULL;
        if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(*out, 0, &sf)) && sf) {
            o_SLockRect   = (SLockRect_t)VtSwap(sf, 13, (void *)hk_SLockRect);
            o_SUnlockRect = (SUnlockRect_t)VtSwap(sf, 14, (void *)hk_SUnlockRect);
            IDirect3DSurface9_Release(sf);
        }
        Log("d3d: texture/surface Lock/Unlock hooked");
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_Present(IDirect3DDevice9 *dev, const RECT *a, const RECT *b, HWND w, const RGNDATA *r)
{
    for (int i = 0; i < g_nFrameCb; i++) g_frameCb[i](dev);
#ifdef M2R_SHOT
    void ShotPre(IDirect3DDevice9 *d); ShotPre(dev);
#endif
    GfxPresent(dev);
#ifdef M2R_SHOT
    void ShotPresent(IDirect3DDevice9 *d); ShotPresent(dev);
#endif
    void FpsPresent(IDirect3DDevice9 *d); FpsPresent(dev);
    void StateCachePresent(void); StateCachePresent();
    return o_Present(dev, a, b, w, r);
}

static void (*g_resetCb[8])(void);
static int g_nResetCb;
void D3DAddResetCallback(void (*cb)(void)) { if (g_nResetCb < 8) g_resetCb[g_nResetCb++] = cb; }

/* ---- borderless window: the device is created windowed and the game window becomes a borderless popup
 * covering the adapter's monitor; Present scales the back buffer (ScrW x ScrH) to the window.
 * The game maps window mouse coordinates with ScrW / GetSystemMetrics(SM_CXSCREEN) in its fullscreen path,
 * so SM_CX/CYSCREEN report the size of the monitor the game runs on. */
static int g_borderless = -1;
static IDirect3D9 *g_d3d;
static UINT g_adapter;
static HWND g_wnd;
static int g_monW, g_monH;
typedef int (WINAPI *GetSystemMetrics_t)(int);
static GetSystemMetrics_t o_GetSystemMetrics;
static int WINAPI hk_GetSystemMetrics(int i)
{
    if (g_monW && (i == SM_CXSCREEN || i == SM_CYSCREEN)) return i == SM_CXSCREEN ? g_monW : g_monH;
    return o_GetSystemMetrics(i);
}
/* the game takes input only while GetForegroundWindow() == its window (0x117601C, checked in the window
 * procedure and the main loop). Any foreground window of this process counts as the game window here. */
typedef HWND (WINAPI *GetForegroundWindow_t)(void);
static GetForegroundWindow_t o_GetForegroundWindow;
static HWND WINAPI hk_GetForegroundWindow(void)
{
    HWND f = o_GetForegroundWindow();
    HWND game = *(HWND *)0x117601C;
    if (f && game && f != game) {
        DWORD pid = 0; GetWindowThreadProcessId(f, &pid);
        static int logged;
        if (!logged++) {
            char c1[64] = "", c2[64] = ""; GetClassNameA(f, c1, sizeof c1); GetClassNameA(game, c2, sizeof c2);
            Log("display: foreground %p (%s, %s) != game window %p (%s), device window %p", (void *)f, c1,
                pid == GetCurrentProcessId() ? "ours" : "other process", (void *)game, c2, (void *)g_wnd);
        }
        if (pid == GetCurrentProcessId()) return game;
    }
    return f;
}
static void Borderless(HWND w, D3DPRESENT_PARAMETERS *pp)
{
    if (g_borderless <= 0) return;
    if (pp->hDeviceWindow) w = pp->hDeviceWindow;
    if (w) g_wnd = w;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    if (!g_wnd) return;
    HMONITOR mon = g_d3d ? IDirect3D9_GetAdapterMonitor(g_d3d, g_adapter) : NULL;
    if (!mon) mon = MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(mon, &mi)) return;
    RECT r = mi.rcMonitor;
    g_monW = r.right - r.left; g_monH = r.bottom - r.top;
    SetWindowLongA(g_wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLongA(g_wnd, GWL_EXSTYLE, GetWindowLongA(g_wnd, GWL_EXSTYLE) & ~(WS_EX_TOPMOST | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME));
    SetWindowPos(g_wnd, HWND_TOP, r.left, r.top, g_monW, g_monH, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    SetForegroundWindow(g_wnd); SetActiveWindow(g_wnd); SetFocus(g_wnd);
    Log("d3d: borderless %ux%u back buffer on a %dx%d window at %ld,%ld (window %p, game window %p, foreground %p)",
        pp->BackBufferWidth, pp->BackBufferHeight, g_monW, g_monH, r.left, r.top, (void *)g_wnd, *(void **)0x117601C,
        (void *)(o_GetForegroundWindow ? o_GetForegroundWindow() : NULL));
}

static HRESULT STDMETHODCALLTYPE hk_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    for (int i = 0; i < g_nResetCb; i++) g_resetCb[i]();
    Borderless(NULL, pp);
    GfxReset();
    PostReset();
    { void StateCacheReset(void); StateCacheReset(); }
    { void FpsReset(void); FpsReset(); }
    GfxAdjustPP(NULL, 0, pp);
    HRESULT hr = o_Reset(dev, pp);
    { void StateCacheReset(void); StateCacheReset(); }
    Log("d3d: Reset %ux%u interval 0x%lx -> 0x%08lx", pp->BackBufferWidth, pp->BackBufferHeight, pp->PresentationInterval, hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateDevice(IDirect3D9 *d3d, UINT ad, D3DDEVTYPE type, HWND wnd, DWORD fl,
                                                 D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    g_d3d = d3d; g_adapter = ad;
    Borderless(wnd, pp);
    GfxAdjustPP(d3d, ad, pp);
    HRESULT hr = o_CreateDevice(d3d, ad, type, wnd, fl, pp, out);
    Log("d3d: CreateDevice %ux%u windowed=%d interval 0x%lx msaa %d flags 0x%lx -> 0x%08lx",
        pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed, pp->PresentationInterval, (int)pp->MultiSampleType, fl, hr);
    if (SUCCEEDED(hr) && out && *out) {
        g_dev = *out;
        void *p;
        if ((p = VtSwap(*out, 17, (void *)hk_Present)))       o_Present = (Present_t)p;
        if ((p = VtSwap(*out, 16, (void *)hk_Reset)))         o_Reset = (Reset_t)p;
        if ((p = VtSwap(*out, 23, (void *)hk_CreateTexture))) o_CreateTexture = (CreateTexture_t)p;
        if ((p = VtSwap(*out, 31, (void *)hk_UpdateTexture))) o_UpdateTexture = (UpdateTexture_t)p;
        { static int started; if (!started) { started = 1; CloseHandle(CreateThread(NULL, 0, VtWatch, NULL, 0, NULL)); }
          char m[64]; Log("d3d: device vtable %p, Present %p (%s)", (void *)*(void ***)*out, o_Present, ModOf((void *)o_Present, m, sizeof m)); }
        { void MsaaDevice(IDirect3DDevice9 *); MsaaDevice(*out); }   /* first: below every other device hook */
        GfxDevice(*out);
        DeformDevice(*out);
        PostDevice(*out);
        { void WaterDevice(IDirect3DDevice9 *); WaterDevice(*out); }
        { void ShaderWarmDevice(IDirect3DDevice9 *); ShaderWarmDevice(*out); }
        { void SoftDevice(IDirect3DDevice9 *); SoftDevice(*out); }   /* after ShaderWarm: sees the game's own shader code */
        { void BloomDevice(IDirect3DDevice9 *); BloomDevice(*out); }
        { void XbShaderDevice(IDirect3DDevice9 *); XbShaderDevice(*out); }
        { void TreeShadowDevice(IDirect3DDevice9 *); TreeShadowDevice(*out); }
        { void DepthAADevice(IDirect3DDevice9 *); DepthAADevice(*out); }
        { void StateCacheDevice(IDirect3DDevice9 *); StateCacheDevice(*out); }
    }
    return hr;
}

static IDirect3D9 *WINAPI hk_Direct3DCreate9(UINT sdk)
{
    IDirect3D9 *d = o_Direct3DCreate9(sdk);
    if (d) {
        void *p = VtSwap(d, 16, (void *)hk_CreateDevice);
        if (p) o_CreateDevice = (CreateDevice_t)p;
    }
    return d;
}

void D3DInit(void)
{
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");   /* statically imported by the game */
    if (!d3d9) { Log("d3d: d3d9.dll not loaded yet, D3D hooks skipped"); return; }
    o_Direct3DCreate9 = (Direct3DCreate9_t)GetProcAddress(d3d9, "Direct3DCreate9");
    /* not DPI aware: on a scaled desktop Windows stretches the game's output and virtualises cursor/window sizes */
    if (IniInt("Display", "DpiAware", 1)) {
        BOOL (WINAPI *dpi)(void) = (BOOL (WINAPI *)(void))GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDPIAware");
        if (dpi) Log("display: SetProcessDPIAware -> %d", dpi());
    }
    g_borderless = IniInt("Display", "Borderless", 1);
    GfxInit();
    if (g_borderless > 0) {
        o_GetSystemMetrics = (GetSystemMetrics_t)GetProcAddress(GetModuleHandleA("user32.dll"), "GetSystemMetrics");
        Log("hook %-24s %d slot(s)", "GetSystemMetrics", PatchIAT(GetModuleHandleA(NULL), (void *)o_GetSystemMetrics, (void *)hk_GetSystemMetrics));
        o_GetForegroundWindow = (GetForegroundWindow_t)GetProcAddress(GetModuleHandleA("user32.dll"), "GetForegroundWindow");
        Log("hook %-24s %d slot(s)", "GetForegroundWindow", PatchIAT(GetModuleHandleA(NULL), (void *)o_GetForegroundWindow, (void *)hk_GetForegroundWindow));
    }
    Log("hook %-24s %d slot(s)", "Direct3DCreate9",
        PatchIAT(GetModuleHandleA(NULL), (void *)o_Direct3DCreate9, (void *)hk_Direct3DCreate9));
}

void *VtSwapPublic(void *obj, int idx, void *hook) { return VtSwap(obj, idx, hook); }
