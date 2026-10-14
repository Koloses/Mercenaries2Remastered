/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Memory pools: the engine carves fixed heaps out of one VirtualAlloc using the
 * embedded [pcwin_Memory] config (0xBAF190). When the exe really runs large
 * address aware (PE flag set AND the process got >2 GB of user space) we enlarge
 * the pools in that text before the game parses it. */
#include "m2r.h"
#include <stdio.h>

static int ExeHasLaaFlag(void)
{
    BYTE *b = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    return (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
}

/* replace "<key> <oldval>" with a value padded to the same field width (the
 * shipped text has a trailing space after each value) */
static int PatchField(char *cfg, const char *key, const char *val)
{
    char pat[64]; sprintf(pat, "\n%s ", key);
    char *p = strstr(cfg, pat);
    if (!p) return 0;
    p += strlen(pat);
    char *e = p; while (*e && *e != '\r' && *e != '\n') e++;      /* field incl. padding */
    size_t w = e - p, n = strlen(val);
    if (n > w) return 0;
    char buf[64]; memset(buf, ' ', w); memcpy(buf, val, n);
    WriteMem((DWORD)p, buf, w);
    return 1;
}

static int CanReserve(DWORD mb)
{
    void *p = VirtualAlloc(NULL, (SIZE_T)mb << 20, MEM_RESERVE, PAGE_NOACCESS);
    if (p) VirtualFree(p, 0, MEM_RELEASE);
    return p != NULL;
}

void MemInit(void)
{
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
    int flag = ExeHasLaaFlag();
    DWORD userMB = (DWORD)(ms.ullTotalVirtual >> 20);
    int laa = flag && userMB > 2100;
    Log("mem: exe LAA flag %d, user address space %u MB -> %s", flag, userMB,
        laa ? "large address aware" : "2 GB (pools unchanged, run the setup to make Mercenaries2.exe 4 GB aware)");

    if (!IniInt("Performance", "TunePools", 1) || !laa) return;

    char *cfg = (char *)0xBAF190;
    if (!strstr(cfg, "\nMainHeap 608M")) { Log("mem: config text mismatch, skipped"); return; }

    int mainMB = IniInt("Performance", "MainHeapMB", 1024);
    int smallMB = IniInt("Performance", "SmallBlockMB", 192);
    if (mainMB < 608) mainMB = 608;
    if (mainMB > 1600) mainMB = 1600;
    if (smallMB < 128) smallMB = 128;
    if (smallMB > 512) smallMB = 512;

    /* the game aborts if the single block can't be reserved - back off until it fits */
    while (mainMB > 608 && !CanReserve(mainMB + smallMB + 64)) mainMB -= 64;

    char v[32]; int ok = 1;
    sprintf(v, "%dM", mainMB);                     ok &= PatchField(cfg, "MainHeap", v);
    sprintf(v, "%d", 28000 * mainMB / 608);        ok &= PatchField(cfg, "MainHeapBlocks", v);
    sprintf(v, "%dM", smallMB);                    ok &= PatchField(cfg, "SmallBlockTotal", v);
    Log("mem: pools MainHeap %d MB (was 608), blocks %d, SmallBlockTotal %d MB (was 128)%s",
        mainMB, 28000 * mainMB / 608, smallMB, ok ? "" : " - some fields failed");
}
