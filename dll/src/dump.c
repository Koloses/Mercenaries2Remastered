/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Developer builds: snapshot of the running game for analysis (the copy protection resolves its detours while
 * the game runs, so a later snapshot has more of the original code). Ctrl+F12, and automatically after 3, 10
 * and 30 minutes; each one replaces Mercenaries2Remastered\dump\game.dmp.
 * Format: "M2RDUMP1", then per region: base, size, protect, type (4 x DWORD) and the bytes. Regions: everything
 * committed and readable in the exe image, and all executable memory outside it (code the protection made). */
#include "m2r.h"
#include <stdio.h>

static void DumpNow(const char *why)
{
    char dir[MAX_PATH], tmp[MAX_PATH], out[MAX_PATH];
    _snprintf(dir, sizeof dir, "%s\\Mercenaries2Remastered\\dump", g_gameDir); CreateDirectoryA(dir, NULL);
    _snprintf(tmp, sizeof tmp, "%s\\game.dmp.tmp", dir); _snprintf(out, sizeof out, "%s\\game.dmp", dir);
    HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { Log("dump: cannot write %s", tmp); return; }
    DWORD w, regions = 0, bytes = 0;
    WriteFile(f, "M2RDUMP1", 8, &w, NULL);
    MEMORY_BASIC_INFORMATION mi;
    for (BYTE *p = (BYTE *)0x10000; (DWORD)p < 0xFFFE0000u && VirtualQuery(p, &mi, sizeof mi); p = (BYTE *)mi.BaseAddress + mi.RegionSize) {
        if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
        DWORD b = (DWORD)mi.BaseAddress, s = (DWORD)mi.RegionSize;
        int inExe = b >= 0x400000 && b < 0x3800000;
        int exec = mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
        if (!inExe && !(exec && mi.Type == MEM_PRIVATE)) continue;
        DWORD h[4] = {b, s, mi.Protect, mi.Type};
        WriteFile(f, h, sizeof h, &w, NULL);
        WriteFile(f, mi.BaseAddress, s, &w, NULL);
        if (w != s) { DWORD z = 0; for (DWORD k = w; k < s; k += 4) WriteFile(f, &z, 4, &w, NULL); }
        regions++; bytes += s;
        if (b + s < b) break;
    }
    CloseHandle(f);
    MoveFileExA(tmp, out, MOVEFILE_REPLACE_EXISTING);
    Log("dump: %s - %lu regions, %lu MB -> %s", why, regions, bytes >> 20, out);
}
static DWORD WINAPI DumpThread(LPVOID u)
{
    (void)u;
    static const DWORD at[] = {3, 10, 30};
    DWORD t0 = GetTickCount(); int next = 0, down = 0;
    for (;;) {
        Sleep(100);
        int k = (GetAsyncKeyState(VK_F12) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
        if (k && !down) DumpNow("Ctrl+F12");
        down = k;
        if (next < 3 && GetTickCount() - t0 >= at[next] * 60000) { char m[32]; _snprintf(m, sizeof m, "%lu minutes", at[next]); DumpNow(m); next++; }
    }
    return 0;
}
void DumpInit(void)
{
    if (!IniInt("Debug", "Dump", 1)) return;
    CloseHandle(CreateThread(NULL, 0, DumpThread, NULL, 0, NULL));
    Log("dump: Ctrl+F12 (and after 3, 10, 30 minutes) -> Mercenaries2Remastered\\dump\\game.dmp");
}
