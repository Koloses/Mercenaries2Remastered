/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/*
 * Mercenaries2Remastered - dinput8.dll proxy for Mercenaries 2: World in Flames (PC)
 *
 * Fix: the install has data\vz.wad cut at 2 GB - 1 (0x7FFFFFFF) with the rest
 * in data\vz.wad_1. The game only opens vz.wad; as soon as the streamer reads
 * past 2 GB, ReadFile fails and the I/O thread falls into `jmp $`
 * (0x8271B4 / 0x8273E0) -> endless loading screen with a core at 100%.
 *
 * We hook the game's own kernel32 IAT entries (CreateFileA, ReadFile,
 * SetFilePointer, GetFileSize(Ex), FindFirstFileA, CloseHandle) and present
 * <file> + <file>_1 + <file>_2 ... as one continuous file. Game files on
 * disk are never modified.
 *
 * Also: loads *.asi plugins from the game folder, scripts\ and plugins\
 * (once, at CRT startup, outside the loader lock) for future mods.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "m2r.h"
#include "d3dhook.h"

#define MAX_PARTS   8
#define MAX_SPLITS  32

/* ------------------------------------------------------- original APIs */
typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL   (WINAPI *ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef DWORD  (WINAPI *SetFilePointer_t)(HANDLE, LONG, PLONG, DWORD);
typedef BOOL   (WINAPI *SetFilePointerEx_t)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
typedef BOOL   (WINAPI *GetFileSizeEx_t)(HANDLE, PLARGE_INTEGER);
typedef DWORD  (WINAPI *GetFileSize_t)(HANDLE, LPDWORD);
typedef HANDLE (WINAPI *FindFirstFileA_t)(LPCSTR, LPWIN32_FIND_DATAA);
typedef BOOL   (WINAPI *CloseHandle_t)(HANDLE);
typedef VOID   (WINAPI *GetSystemTimeAsFileTime_t)(LPFILETIME);

static CreateFileA_t      o_CreateFileA;
static ReadFile_t         o_ReadFile;
static SetFilePointer_t   o_SetFilePointer;
static SetFilePointerEx_t o_SetFilePointerEx;
static GetFileSizeEx_t    o_GetFileSizeEx;
static GetFileSize_t      o_GetFileSize;
static FindFirstFileA_t   o_FindFirstFileA;
static CloseHandle_t      o_CloseHandle;
static GetSystemTimeAsFileTime_t o_GetSystemTimeAsFileTime;

/* ------------------------------------------------- split file registry */
typedef struct {
    HANDLE           h;                 /* handle the game holds (real part 0) */
    int              nparts;
    HANDLE           part[MAX_PARTS];   /* our own buffered read handles */
    ULONGLONG        start[MAX_PARTS];  /* virtual offset where part begins */
    ULONGLONG        size[MAX_PARTS];
    ULONGLONG        total;
    ULONGLONG        pos;               /* virtual file pointer (sync reads) */
    ULONGLONG        pbase[MAX_PARTS];  /* offset inside the part file (overlay data) */
    int              npatch;            /* overlay byte patches (header edits) */
    ULONGLONG       *poff; DWORD *plen; BYTE **pdat;
    ULONGLONG        patchLo, patchHi;
    CRITICAL_SECTION lock;
} SplitFile;

/* ---- content overlays: Mercenaries2Remastered\content\<name>.ovl
 *  'M2OV' u32 ver(1) u32 npatch u32 0 | u64 baseTotal u64 dataOff u64 dataSize
 *  npatch x (u64 voff, u32 len, u32 0) followed by the patch bytes; data at dataOff
 *  data is mapped at virtual offset baseTotal (end of the original file). */
const char *ContentDir(void);
typedef struct { ULONGLONG base, dataOff, dataSize; int npatch; } OvlHdr;
static int OverlayPath(LPCSTR path, char *out, int n)
{
    const char *cd = ContentDir();
    if (!cd || !cd[0]) return 0;
    const char *b = strrchr(path, '\\'); const char *b2 = strrchr(path, '/');
    if (b2 > b) b = b2;
    b = b ? b + 1 : path;
    _snprintf(out, n, "%s\\%s.ovl", cd, b); out[n - 1] = 0;
    return GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
}
static int OverlayHeader(const char *op, OvlHdr *oh, HANDLE *ph)
{
    HANDLE f = o_CreateFileA(op, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    BYTE h[40]; DWORD r = 0;
    if (!o_ReadFile(f, h, 40, &r, NULL) || r != 40 || memcmp(h, "M2OV", 4) || *(DWORD *)(h + 4) != 1) { o_CloseHandle(f); return 0; }
    oh->npatch = *(int *)(h + 8);
    memcpy(&oh->base, h + 16, 8); memcpy(&oh->dataOff, h + 24, 8); memcpy(&oh->dataSize, h + 32, 8);
    if (ph) *ph = f; else o_CloseHandle(f);
    return 1;
}

static SplitFile       *g_split[MAX_SPLITS];
static volatile LONG    g_splitCount;   /* fast "any registered?" check */
static CRITICAL_SECTION g_splitLock;

static SplitFile *FindSplit(HANDLE h)
{
    SplitFile *r = NULL;
    if (!g_splitCount || h == NULL || h == INVALID_HANDLE_VALUE) return NULL;
    EnterCriticalSection(&g_splitLock);
    for (int i = 0; i < MAX_SPLITS; i++)
        if (g_split[i] && g_split[i]->h == h) { r = g_split[i]; break; }
    LeaveCriticalSection(&g_splitLock);
    return r;
}

static int FileSize64(LPCSTR path, ULONGLONG *out)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return 0;
    if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    *out = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    return 1;
}

/* Count <path>_1, <path>_2 ... ; returns number of extra parts and total size. */
static int ProbeParts(LPCSTR path, ULONGLONG sizes[MAX_PARTS], int *nparts)
{
    char p[MAX_PATH + 8];
    if (!FileSize64(path, &sizes[0])) return 0;
    int n = 1;
    for (; n < MAX_PARTS; n++) {
        if (_snprintf(p, sizeof p, "%s_%d", path, n) >= (int)sizeof p) break;
        if (!FileSize64(p, &sizes[n])) break;
    }
    *nparts = n;
    return n > 1;
}

static void RegisterSplit(HANDLE h, LPCSTR path)
{
    ULONGLONG sizes[MAX_PARTS];
    int n = 0;
    char op[MAX_PATH]; OvlHdr oh; HANDLE of = INVALID_HANDLE_VALUE;
    int multi = ProbeParts(path, sizes, &n);
    int ovl = OverlayPath(path, op, sizeof op) && OverlayHeader(op, &oh, &of);
    if (!multi && !ovl) return;
    if (!multi) { if (!FileSize64(path, &sizes[0])) { if (of != INVALID_HANDLE_VALUE) o_CloseHandle(of); return; } n = 1; }

    SplitFile *sf = (SplitFile *)calloc(1, sizeof *sf);
    if (!sf) return;
    sf->h = h;
    sf->nparts = n;
    char p[MAX_PATH + 8];
    for (int i = 0; i < n; i++) {
        if (i == 0) lstrcpynA(p, path, sizeof p);
        else _snprintf(p, sizeof p, "%s_%d", path, i);
        sf->part[i] = o_CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
        if (sf->part[i] == INVALID_HANDLE_VALUE) {
            Log("ERROR: cannot open part '%s' (err %lu), split disabled for %s", p, GetLastError(), path);
            for (int j = 0; j < i; j++) o_CloseHandle(sf->part[j]);
            free(sf);
            return;
        }
        sf->start[i] = sf->total;
        sf->size[i]  = sizes[i];
        sf->total   += sizes[i];
    }
    if (ovl) {
        if (oh.base != sf->total || n >= MAX_PARTS) {
            Log("Overlay %s ignored: built for %I64u bytes, file has %I64u", op, oh.base, sf->total);
            o_CloseHandle(of);
        } else {
            sf->npatch = oh.npatch;
            sf->poff = (ULONGLONG *)calloc(oh.npatch + 1, 8); sf->plen = (DWORD *)calloc(oh.npatch + 1, 4); sf->pdat = (BYTE **)calloc(oh.npatch + 1, sizeof(BYTE *));
            sf->patchLo = ~0ULL; sf->patchHi = 0;
            for (int k = 0; k < oh.npatch; k++) {
                BYTE e[16]; DWORD r = 0;
                o_ReadFile(of, e, 16, &r, NULL);
                memcpy(&sf->poff[k], e, 8); sf->plen[k] = *(DWORD *)(e + 8);
            }
            for (int k = 0; k < oh.npatch; k++) {
                DWORD r = 0; sf->pdat[k] = (BYTE *)malloc(sf->plen[k] + 1);
                o_ReadFile(of, sf->pdat[k], sf->plen[k], &r, NULL);
                if (sf->poff[k] < sf->patchLo) sf->patchLo = sf->poff[k];
                if (sf->poff[k] + sf->plen[k] > sf->patchHi) sf->patchHi = sf->poff[k] + sf->plen[k];
            }
            sf->part[n] = of; sf->pbase[n] = oh.dataOff;
            sf->start[n] = sf->total; sf->size[n] = oh.dataSize; sf->total += oh.dataSize; sf->nparts = ++n;
            Log("Overlay %s: %d header patch(es), %I64u bytes of data", op, oh.npatch, oh.dataSize);
        }
    }
    InitializeCriticalSection(&sf->lock);

    EnterCriticalSection(&g_splitLock);
    int slot = -1;
    for (int i = 0; i < MAX_SPLITS; i++) if (!g_split[i]) { slot = i; break; }
    if (slot >= 0) { g_split[slot] = sf; InterlockedIncrement(&g_splitCount); }
    LeaveCriticalSection(&g_splitLock);

    if (slot < 0) {
        Log("ERROR: too many split files open");
        for (int j = 0; j < n; j++) o_CloseHandle(sf->part[j]);
        DeleteCriticalSection(&sf->lock);
        free(sf);
        return;
    }
    Log("Joined %s: %d parts, %I64u bytes total (handle %p)", path, n, sf->total, h);
}

static void UnregisterSplit(HANDLE h)
{
    SplitFile *sf = NULL;
    if (!g_splitCount) return;
    EnterCriticalSection(&g_splitLock);
    for (int i = 0; i < MAX_SPLITS; i++)
        if (g_split[i] && g_split[i]->h == h) { sf = g_split[i]; g_split[i] = NULL; InterlockedDecrement(&g_splitCount); break; }
    LeaveCriticalSection(&g_splitLock);
    if (!sf) return;
    for (int i = 0; i < sf->nparts; i++) o_CloseHandle(sf->part[i]);
    for (int k = 0; k < sf->npatch; k++) free(sf->pdat[k]);
    free(sf->poff); free(sf->plen); free(sf->pdat);
    DeleteCriticalSection(&sf->lock);
    free(sf);
}

/* Read [off, off+n) of the virtual file. Returns bytes read; 0 + FALSE on I/O error. */
static BOOL SplitRead(SplitFile *sf, ULONGLONG off, BYTE *buf, DWORD n, DWORD *got)
{
    *got = 0;
    EnterCriticalSection(&sf->lock);
    while (n && off < sf->total) {
        int i = sf->nparts - 1;
        while (i > 0 && off < sf->start[i]) i--;
        ULONGLONG local = off - sf->start[i];
        ULONGLONG avail = sf->size[i] - local;
        DWORD chunk = (DWORD)((avail < n) ? avail : n);
        OVERLAPPED ov; memset(&ov, 0, sizeof ov);
        ULONGLONG fo = local + sf->pbase[i];
        ov.Offset = (DWORD)fo; ov.OffsetHigh = (DWORD)(fo >> 32);
        DWORD r = 0;
        if (!o_ReadFile(sf->part[i], buf, chunk, &r, &ov)) {
            DWORD e = GetLastError();
            if (e != ERROR_HANDLE_EOF) {
                LeaveCriticalSection(&sf->lock);
                Log("ERROR: read part %d @%I64u len %lu failed (err %lu)", i, local, chunk, e);
                SetLastError(e);
                return FALSE;
            }
        }
        if (r == 0) break;            /* unexpected EOF on part */
        *got += r; buf += r; off += r; n -= r;
    }
    LeaveCriticalSection(&sf->lock);
    return TRUE;
}

static void ApplyPatches(SplitFile *sf, ULONGLONG off, BYTE *buf, DWORD got)
{
    for (int k = 0; k < sf->npatch; k++) {
        ULONGLONG a = sf->poff[k], b = a + sf->plen[k];
        ULONGLONG lo = a > off ? a : off, hi = b < off + got ? b : off + got;
        if (lo < hi) memcpy(buf + (lo - off), sf->pdat[k] + (lo - a), (size_t)(hi - lo));
    }
}
/* ---------------------------------------------------------------- hooks */
static HANDLE WINAPI hk_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                    DWORD disp, DWORD flags, HANDLE tmpl)
{
    /* game data is opened unbuffered (FILE_FLAG_NO_BUFFERING): every load goes to the disk again. Let
     * Windows cache it ([Performance] CachedReads) - reloads and respawns come from memory */
    static int cached = -1;
    if (cached < 0) cached = IniInt("Performance", "CachedReads", 1);
    if (cached && (flags & FILE_FLAG_NO_BUFFERING) && !(acc & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA))) {
        flags &= ~FILE_FLAG_NO_BUFFERING;
    }
    HANDLE h = o_CreateFileA(name, acc, share, sa, disp, flags, tmpl);
    if (name && (strstr(name, "-patch") || strstr(name, "Mercenaries2Remastered\\content"))) {
        DWORD e = GetLastError();
        Log("open %s -> %p (flags %08lX, err %lu)", name, h, flags, h == INVALID_HANDLE_VALUE ? e : 0);
        SetLastError(e);
    }
    if (h == INVALID_HANDLE_VALUE && name) {
        /* DLC stream files: the DLC sound script builds "<mount>\\<audio dir>\\NAME.PWS"; look in content\Data\Audios */
        size_t ln = strlen(name);
        if (ln > 4 && !_stricmp(name + ln - 4, ".wad") && ContentDir()[0]) {
            /* level packages that only exist in our content folder (DLC01.wad) */
            const char *b = strrchr(name, '\\'); b = b ? b + 1 : name;
            char alt[MAX_PATH]; _snprintf(alt, sizeof alt, "%s\\%s", ContentDir(), b); alt[MAX_PATH - 1] = 0;
            if (_stricmp(alt, name)) {
                HANDLE h2 = o_CreateFileA(alt, acc, share, sa, disp, flags, tmpl);
                if (h2 != INVALID_HANDLE_VALUE) { Log("open %s -> redirected to %s", name, alt); RegisterSplit(h2, alt); return h2; }
            }
        }
        if (ln > 4 && !_stricmp(name + ln - 4, ".pws") && ContentDir()[0]) {
            const char *b = strrchr(name, '\\'); b = b ? b + 1 : name;
            char alt[MAX_PATH]; _snprintf(alt, sizeof alt, "%s\\Data\\Audios\\%s", ContentDir(), b); alt[MAX_PATH - 1] = 0;
            HANDLE h2 = o_CreateFileA(alt, acc, share, sa, disp, flags, tmpl);
            if (h2 != INVALID_HANDLE_VALUE) {
                Log("open %s -> redirected to %s", name, alt);
                if (disp == OPEN_EXISTING) RegisterSplit(h2, alt);
                return h2;
            }
        }
        static volatile LONG nfail;
        DWORD e = GetLastError();
        if (InterlockedIncrement(&nfail) <= 300) Log("open FAILED %s (err %lu)", name, e);
        SetLastError(e);
    }
    if (h != INVALID_HANDLE_VALUE && name && disp == OPEN_EXISTING &&
        !(acc & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA))) {
        DWORD e = GetLastError();
        RegisterSplit(h, name);
        SetLastError(e);
    }
    return h;
}

static BOOL WINAPI hk_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD pRead, LPOVERLAPPED ov)
{
    SplitFile *sf = FindSplit(h);
    if (!sf) {
        BOOL ok = o_ReadFile(h, buf, n, pRead, ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING && GetLastError() != ERROR_HANDLE_EOF) {
            static volatile LONG nrf;
            DWORD e = GetLastError();
            if (InterlockedIncrement(&nrf) <= 50) Log("ReadFile(%p, %lu) failed, err %lu", h, n, e);
            SetLastError(e);
        }
        return ok;
    }

    ULONGLONG off = ov ? (((ULONGLONG)ov->OffsetHigh << 32) | ov->Offset) : sf->pos;

    /* Entirely inside the first part with an explicit offset: leave the
       game's original call untouched (keeps its unbuffered fast path). */
    if (ov && off + n <= sf->size[0] && (!sf->npatch || off >= sf->patchHi || off + n <= sf->patchLo))
        return o_ReadFile(h, buf, n, pRead, ov);

    DWORD got = 0;
    BOOL ok = SplitRead(sf, off, (BYTE *)buf, n, &got);
    DWORD err = ok ? 0 : GetLastError();
    if (ok && sf->npatch) ApplyPatches(sf, off, (BYTE *)buf, got);
    if (!ov) sf->pos = off + got;
    if (pRead) *pRead = got;

    if (ok && ov && got == 0 && n) { ok = FALSE; err = ERROR_HANDLE_EOF; }
    if (ov) {
        ov->Internal     = ok ? 0 /*STATUS_SUCCESS*/ : (err == ERROR_HANDLE_EOF ? 0xC0000011 : 0xC0000185);
        ov->InternalHigh = got;
        if (ov->hEvent) SetEvent(ov->hEvent);
    }
    if (!ok) { Log("Split read @%I64u len %lu -> err %lu", off, n, err); SetLastError(err); }
    return ok;
}

static DWORD WINAPI hk_SetFilePointer(HANDLE h, LONG lo, PLONG hi, DWORD method)
{
    SplitFile *sf = FindSplit(h);
    if (!sf) return o_SetFilePointer(h, lo, hi, method);
    LONGLONG d = hi ? (((LONGLONG)*hi << 32) | (DWORD)lo) : (LONGLONG)lo;
    LONGLONG base = method == FILE_BEGIN ? 0 : method == FILE_CURRENT ? (LONGLONG)sf->pos : (LONGLONG)sf->total;
    LONGLONG np = base + d;
    if (np < 0) { SetLastError(ERROR_NEGATIVE_SEEK); return INVALID_SET_FILE_POINTER; }
    sf->pos = (ULONGLONG)np;
    if (hi) *hi = (LONG)(np >> 32);
    SetLastError(NO_ERROR);
    return (DWORD)np;
}

static BOOL WINAPI hk_SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, PLARGE_INTEGER newPos, DWORD method)
{
    SplitFile *sf = FindSplit(h);
    if (!sf) return o_SetFilePointerEx(h, dist, newPos, method);
    LONGLONG base = method == FILE_BEGIN ? 0 : method == FILE_CURRENT ? (LONGLONG)sf->pos : (LONGLONG)sf->total;
    LONGLONG np = base + dist.QuadPart;
    if (np < 0) { SetLastError(ERROR_NEGATIVE_SEEK); return FALSE; }
    sf->pos = (ULONGLONG)np;
    if (newPos) newPos->QuadPart = np;
    return TRUE;
}

static BOOL WINAPI hk_GetFileSizeEx(HANDLE h, PLARGE_INTEGER sz)
{
    SplitFile *sf = FindSplit(h);
    if (!sf) return o_GetFileSizeEx(h, sz);
    sz->QuadPart = (LONGLONG)sf->total;
    return TRUE;
}

static DWORD WINAPI hk_GetFileSize(HANDLE h, LPDWORD hi)
{
    SplitFile *sf = FindSplit(h);
    if (!sf) return o_GetFileSize(h, hi);
    if (hi) *hi = (DWORD)(sf->total >> 32);
    SetLastError(NO_ERROR);
    return (DWORD)sf->total;
}

static HANDLE WINAPI hk_FindFirstFileA(LPCSTR pat, LPWIN32_FIND_DATAA fd)
{
    HANDLE h = o_FindFirstFileA(pat, fd);
    if (h == INVALID_HANDLE_VALUE && pat && !strpbrk(pat, "*?")) {
        size_t ln = strlen(pat);
        if (ln > 4 && !_stricmp(pat + ln - 4, ".wad") && ContentDir()[0]) {
            const char *b = strrchr(pat, '\\'); b = b ? b + 1 : pat;
            char alt[MAX_PATH]; _snprintf(alt, sizeof alt, "%s\\%s", ContentDir(), b); alt[MAX_PATH - 1] = 0;
            if (_stricmp(alt, pat)) {
                HANDLE h2 = o_FindFirstFileA(alt, fd);
                if (h2 != INVALID_HANDLE_VALUE) { Log("find %s -> %s", pat, alt); return h2; }
            }
        }
        DWORD e = GetLastError(); static volatile LONG nf;
        if (InterlockedIncrement(&nf) <= 100) Log("find FAILED %s", pat);
        SetLastError(e);
    }
    if (h != INVALID_HANDLE_VALUE && pat && !strpbrk(pat, "*?") &&
        !(fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        ULONGLONG sizes[MAX_PARTS]; int n = 0;
        DWORD e = GetLastError();
        char op[MAX_PATH]; OvlHdr oh;
        int ovl = OverlayPath(pat, op, sizeof op) && OverlayHeader(op, &oh, NULL);
        int multi = ProbeParts(pat, sizes, &n);
        if (!multi) { n = 1; sizes[0] = ((ULONGLONG)fd->nFileSizeHigh << 32) | fd->nFileSizeLow; }
        if (multi || ovl) {
            ULONGLONG t = 0;
            for (int i = 0; i < n; i++) t += sizes[i];
            if (ovl && oh.base == t) t += oh.dataSize;
            fd->nFileSizeHigh = (DWORD)(t >> 32);
            fd->nFileSizeLow  = (DWORD)t;
        }
        SetLastError(e);
    }
    return h;
}

static BOOL WINAPI hk_CloseHandle(HANDLE h)
{
    UnregisterSplit(h);
    return o_CloseHandle(h);
}

/* ------------------------------------------------------- plugin loader */
static void LoadPluginsIn(const char *sub)
{
    char dir[MAX_PATH], pat[MAX_PATH], full[MAX_PATH];
    if (sub) _snprintf(dir, sizeof dir, "%s\\%s", g_gameDir, sub);
    else     lstrcpynA(dir, g_gameDir, sizeof dir);
    _snprintf(pat, sizeof pat, "%s\\*.asi", dir);
    WIN32_FIND_DATAA fd;
    HANDLE f = o_FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        _snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName);
        HMODULE m = LoadLibraryA(full);
        if (m) Log("Loaded plugin %s", full);
        else   Log("ERROR: failed to load plugin %s (err %lu)", full, GetLastError());
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}

static volatile LONG g_pluginsLoaded;

/* First call comes from the CRT's __security_init_cookie at game entry:
   loader lock is released, so it is safe to load plugins here. */
static VOID WINAPI hk_GetSystemTimeAsFileTime(LPFILETIME ft)
{
    if (InterlockedExchange(&g_pluginsLoaded, 1) == 0) {
        LoadPluginsIn(NULL);
        LoadPluginsIn("scripts");
        LoadPluginsIn("plugins");
    }
    o_GetSystemTimeAsFileTime(ft);
}

#define HOOK(name) do { \
        o_##name = (name##_t)GetProcAddress(k32, #name); \
        if (o_##name) Log("hook %-24s %d slot(s)", #name, PatchIAT(exe, (void *)o_##name, (void *)hk_##name)); \
    } while (0)

static void InstallHooks(void)
{
    HMODULE exe = GetModuleHandleA(NULL);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    HOOK(CreateFileA);
    HOOK(ReadFile);
    HOOK(SetFilePointer);
    HOOK(SetFilePointerEx);
    HOOK(GetFileSizeEx);
    HOOK(GetFileSize);
    HOOK(FindFirstFileA);
    HOOK(CloseHandle);
    HOOK(GetSystemTimeAsFileTime);
    /* our own code always needs these even if the game doesn't import them */
    if (!o_CreateFileA)    o_CreateFileA    = (CreateFileA_t)GetProcAddress(k32, "CreateFileA");
    if (!o_ReadFile)       o_ReadFile       = (ReadFile_t)GetProcAddress(k32, "ReadFile");
    if (!o_CloseHandle)    o_CloseHandle    = (CloseHandle_t)GetProcAddress(k32, "CloseHandle");
    if (!o_FindFirstFileA) o_FindFirstFileA = (FindFirstFileA_t)GetProcAddress(k32, "FindFirstFileA");
}

/* ------------------------------------------------------ dinput8 proxy */
static HMODULE g_realDI8;
static FARPROC RealDI8(const char *fn)
{
    if (!g_realDI8) {
        char p[MAX_PATH];
        GetSystemDirectoryA(p, MAX_PATH);
        lstrcatA(p, "\\dinput8.dll");
        g_realDI8 = LoadLibraryA(p);
        if (!g_realDI8) { Log("ERROR: cannot load %s", p); return NULL; }
    }
    return GetProcAddress(g_realDI8, fn);
}

HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID *out, LPVOID outer)
{
    typedef HRESULT (WINAPI *fn_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPVOID);
    fn_t f = (fn_t)RealDI8("DirectInput8Create");
    return f ? f(hinst, ver, riid, out, outer) : E_FAIL;
}
HRESULT WINAPI DllCanUnloadNow(void)
{ typedef HRESULT (WINAPI *fn_t)(void); fn_t f = (fn_t)RealDI8("DllCanUnloadNow"); return f ? f() : S_FALSE; }
HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID i, LPVOID *o)
{ typedef HRESULT (WINAPI *fn_t)(REFCLSID, REFIID, LPVOID *); fn_t f = (fn_t)RealDI8("DllGetClassObject"); return f ? f(c, i, o) : E_FAIL; }
HRESULT WINAPI DllRegisterServer(void)
{ typedef HRESULT (WINAPI *fn_t)(void); fn_t f = (fn_t)RealDI8("DllRegisterServer"); return f ? f() : E_FAIL; }
HRESULT WINAPI DllUnregisterServer(void)
{ typedef HRESULT (WINAPI *fn_t)(void); fn_t f = (fn_t)RealDI8("DllUnregisterServer"); return f ? f() : E_FAIL; }

/* ---------------------------------------------------------------- main */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_DETACH) { LogFinal(); return TRUE; }
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        InitializeCriticalSection(&g_splitLock);
        UtilInit();
        Log("Mercenaries2Remastered %s for Mercenaries2.exe %s loaded, game dir: %s", M2R_VERSION, M2R_GAME, g_gameDir);

        InstallHooks();
        D3DInit();
        { void FpsInit(void); FpsInit(); }
#ifdef M2R_SHOT
        { void ShotInit(void); ShotInit(); }
        { void DumpInit(void); DumpInit(); }
#endif
        { void ShaderWarmInit(void); ShaderWarmInit(); }
        { void ForestInit(void); ForestInit(); }
        { void CullInit(void); CullInit(); }
        { void SecuromInit(void); SecuromInit(); }
        { void StreamInit(void); StreamInit(); }
        if (!GameVersionOK()) {
            Log("WARNING: Mercenaries2.exe is not build " M2R_GAME " - only the file fix is active");
        } else {
            MemInit();
            PerfInit();
            PadInit();
            PromptsInit();
            CamInit();
            TurretInit();
            OnlineInit();
            VehicleInit();
            DeformInit();
            { void DrawPoolInit(void); DrawPoolInit(); }
            LodInit();
            PostInit();
            PickupInit();
            AimInit();
            ContentInit();
            { void AssetTableInit(void); AssetTableInit(); }
            { void CheatsInit(void); CheatsInit(); }
            { void JitterInit(void); JitterInit(); }
            { void MovieInit(void); MovieInit(); }
            { void PaperInit(void); PaperInit(); }
            { void EmitFixInit(void); EmitFixInit(); }
            { void TreeShadowInit(void); TreeShadowInit(); }
            { void BloomInit(void); BloomInit(); }
#ifdef M2R_SHOT
            { void FireProbeInit(void); FireProbeInit(); }
#endif
        }
    }
    return TRUE;
}
