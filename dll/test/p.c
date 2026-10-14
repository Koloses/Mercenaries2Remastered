/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID x){ if(r==DLL_PROCESS_ATTACH) SetEnvironmentVariableA("M2R_TEST_PLUGIN","loaded"); return TRUE; }
