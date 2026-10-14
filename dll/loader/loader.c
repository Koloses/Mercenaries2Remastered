/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* dinput8.dll: picks the mod build that matches the game executable and loads it from Mercenaries2Remastered\.
 *   Mercenaries2.exe 1.0 (Aug 13 2008) -> Mercenaries2Remastered\Mercenaries2Remastered-1.0.dll
 *   Mercenaries2.exe 1.1 (Sep 12 2008) -> Mercenaries2Remastered\Mercenaries2Remastered-1.1.dll
 * Other executables get the 1.0 build, which then keeps only its file fixes. DirectInput calls go to the loaded
 * build, which forwards them to the system dinput8.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static HMODULE g_core, g_sys;

static DWORD ExeStamp(void)
{
    BYTE *b = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    return nt->FileHeader.TimeDateStamp;
}

static FARPROC Fn(const char *name)
{
    FARPROC f = g_core ? GetProcAddress(g_core, name) : NULL;
    if (f) return f;
    if (!g_sys) {
        char p[MAX_PATH];
        GetSystemDirectoryA(p, MAX_PATH);
        lstrcatA(p, "\\dinput8.dll");
        g_sys = LoadLibraryA(p);
    }
    return g_sys ? GetProcAddress(g_sys, name) : NULL;
}

HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID *out, LPVOID outer)
{ typedef HRESULT (WINAPI *fn_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPVOID); fn_t f = (fn_t)Fn("DirectInput8Create"); return f ? f(hinst, ver, riid, out, outer) : E_FAIL; }
HRESULT WINAPI DllCanUnloadNow(void)
{ typedef HRESULT (WINAPI *fn_t)(void); fn_t f = (fn_t)Fn("DllCanUnloadNow"); return f ? f() : S_FALSE; }
HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID i, LPVOID *o)
{ typedef HRESULT (WINAPI *fn_t)(REFCLSID, REFIID, LPVOID *); fn_t f = (fn_t)Fn("DllGetClassObject"); return f ? f(c, i, o) : E_FAIL; }
HRESULT WINAPI DllRegisterServer(void)
{ typedef HRESULT (WINAPI *fn_t)(void); fn_t f = (fn_t)Fn("DllRegisterServer"); return f ? f() : E_FAIL; }
HRESULT WINAPI DllUnregisterServer(void)
{ typedef HRESULT (WINAPI *fn_t)(void); fn_t f = (fn_t)Fn("DllUnregisterServer"); return f ? f() : E_FAIL; }

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(hinst);
    const char *build = ExeStamp() == 0x48CAE190u ? "1.1" : "1.0";
    char dir[MAX_PATH], p[MAX_PATH];
    GetModuleFileNameA(hinst, dir, MAX_PATH);
    char *s = strrchr(dir, '\\'); if (s) *s = 0;
    _snprintf(p, sizeof p, "%s\\Mercenaries2Remastered\\Mercenaries2Remastered-%s.dll", dir, build);
    p[sizeof p - 1] = 0;
    g_core = LoadLibraryA(p);
    if (!g_core) {
        char m[MAX_PATH + 128];
        _snprintf(m, sizeof m, "Mercenaries2Remastered: cannot load\n%s\n(error %lu). The game starts without the mod.", p, GetLastError());
        m[sizeof m - 1] = 0;
        MessageBoxA(NULL, m, "Mercenaries2Remastered", MB_OK | MB_ICONWARNING);
    }
    return TRUE;
}
