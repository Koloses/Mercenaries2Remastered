/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/*
 * Mercenaries2Remastered setup
 *
 * Installs the Mercenaries2Remastered mod (dinput8.dll + Mercenaries2Remastered.ini) into a Mercenaries 2: World in Flames
 * PC folder and, optionally, builds the Xbox 360 Title Update 4 + "Blow It Up Again Pack" content
 * from the user's own Xbox 360 files with the bundled converter (converter\python + converter\m2conv).
 *
 * Layout next to this exe:
 *   files\dinput8.dll, files\Mercenaries2Remastered.ini
 *   converter\python\python.exe, converter\m2conv\m2conv.py
 */
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <commdlg.h>
#include <stdio.h>
#include <wchar.h>

#define APP_TITLE L"Mercenaries2Remastered Beta 1 Setup"
#define EXE_MD5   "857b3387d54774a32c1328effb5de4d4"   /* retail 1.1 exe */
#define EXE4_MD5  "2e0d3a41df30c59714444c3a84c3f372"   /* the same exe, large address aware (header flag + PE checksum) */

enum {
    ID_GAME = 100, ID_GAME_BR, ID_OPT_FULL, ID_OPT_MOD, ID_ISO, ID_ISO_BR, ID_TU, ID_TU_BR, ID_DLC, ID_DLC_BR,
    ID_PROG, ID_STATUS, ID_LOG, ID_INSTALL, ID_UNINSTALL, ID_CLOSE, ID_LBL_ISO, ID_LBL_TU, ID_LBL_DLC, ID_GRP_X,
    ID_LBL_GAME, ID_GRP_OPT, ID_NOTE
};
#define WM_APP_LINE (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)

static HWND g_wnd, g_log, g_prog, g_status;
static HFONT g_font, g_bold;
static int g_dpi = 96;
static wchar_t g_home[MAX_PATH];          /* folder of this exe */
static wchar_t g_settings[MAX_PATH];      /* Mercenaries2RemasteredSetup.ini (remembered paths) */
static volatile LONG g_busy;
static HANDLE g_job, g_proc;
static int g_cancel;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

/* ---------------------------------------------------------------- helpers */
static void LogW(const wchar_t *s)
{
    int n = GetWindowTextLengthW(g_log);
    if (n > 200000) { SendMessageW(g_log, EM_SETSEL, 0, 100000); SendMessageW(g_log, EM_REPLACESEL, 0, (LPARAM)L""); n = GetWindowTextLengthW(g_log); }
    SendMessageW(g_log, EM_SETSEL, n, n);
    SendMessageW(g_log, EM_REPLACESEL, 0, (LPARAM)s);
    SendMessageW(g_log, EM_REPLACESEL, 0, (LPARAM)L"\r\n");
}
static void LogF(const wchar_t *fmt, ...)
{
    wchar_t b[2048]; va_list ap; va_start(ap, fmt); _vsnwprintf(b, 2047, fmt, ap); va_end(ap); b[2047] = 0; LogW(b);
}
static void Status(const wchar_t *s) { SetWindowTextW(g_status, s); }
static void GetText(int id, wchar_t *b, int n)
{
    GetDlgItemTextW(g_wnd, id, b, n);
    /* trim spaces and quotes */
    wchar_t *p = b; while (*p == L' ' || *p == L'"') p++;
    memmove(b, p, (wcslen(p) + 1) * sizeof(wchar_t));
    size_t l = wcslen(b); while (l && (b[l - 1] == L' ' || b[l - 1] == L'"' || b[l - 1] == L'\\')) b[--l] = 0;
}
static int Exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static int IsDir(const wchar_t *p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static void Join(wchar_t *out, const wchar_t *a, const wchar_t *b) { _snwprintf(out, MAX_PATH, L"%s\\%s", a, b); out[MAX_PATH - 1] = 0; }
static int Ask(const wchar_t *msg) { return MessageBoxW(g_wnd, msg, APP_TITLE, MB_YESNO | MB_ICONQUESTION) == IDYES; }
static void Err(const wchar_t *msg) { MessageBoxW(g_wnd, msg, APP_TITLE, MB_OK | MB_ICONERROR); }

static int FileMd5(const wchar_t *path, char out[33])
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    HCRYPTPROV pv = 0; HCRYPTHASH h = 0; int ok = 0;
    if (CryptAcquireContextW(&pv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) && CryptCreateHash(pv, CALG_MD5, 0, 0, &h)) {
        static BYTE buf[1 << 16]; DWORD r;
        while (ReadFile(f, buf, sizeof buf, &r, NULL) && r) CryptHashData(h, buf, r, 0);
        BYTE d[16]; DWORD dl = 16;
        if (CryptGetHashParam(h, HP_HASHVAL, d, &dl, 0)) { for (int i = 0; i < 16; i++) sprintf(out + 2 * i, "%02x", d[i]); ok = 1; }
    }
    if (h) CryptDestroyHash(h);
    if (pv) CryptReleaseContext(pv, 0);
    CloseHandle(f);
    return ok;
}

/* does the file contain an ASCII marker (our dinput8.dll has "Mercenaries2Remastered") */
static int FileHas(const wchar_t *path, const char *mark)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD sz = GetFileSize(f, NULL), r = 0; int hit = 0;
    if (sz && sz < (64u << 20)) {
        char *b = (char *)HeapAlloc(GetProcessHeap(), 0, sz);
        if (b && ReadFile(f, b, sz, &r, NULL)) {
            size_t ml = strlen(mark);
            for (DWORD i = 0; i + ml <= r && !hit; i++) if (!memcmp(b + i, mark, ml)) hit = 1;
        }
        if (b) HeapFree(GetProcessHeap(), 0, b);
    }
    CloseHandle(f);
    return hit;
}

static void DeleteTree(const wchar_t *dir)
{
    wchar_t from[MAX_PATH + 2] = {0};
    wcsncpy(from, dir, MAX_PATH); from[wcslen(from) + 1] = 0;
    SHFILEOPSTRUCTW op = {0};
    op.wFunc = FO_DELETE; op.pFrom = from; op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    if (IsDir(dir)) SHFileOperationW(&op);
}

/* ------------------------------------------------------- game folder */
static int GameOk(const wchar_t *g)
{
    wchar_t p[MAX_PATH];
    Join(p, g, L"Mercenaries2.exe"); if (!Exists(p)) return 0;
    Join(p, g, L"data\\vz.wad"); if (!Exists(p)) return 0;
    Join(p, g, L"data\\shell.wad"); if (!Exists(p)) return 0;
    return 1;
}

static void DetectGame(wchar_t *out)
{
    out[0] = 0;
    wchar_t p[MAX_PATH];
    /* setup unpacked inside (or next to) the game folder */
    wcscpy(p, g_home);
    for (int up = 0; up < 3; up++) {
        if (GameOk(p)) { wcscpy(out, p); return; }
        wchar_t *s = wcsrchr(p, L'\\'); if (!s) break; *s = 0;
    }
    static const wchar_t *keys[] = {
        L"SOFTWARE\\WOW6432Node\\EA Games\\Mercenaries 2 World in Flames", L"SOFTWARE\\EA Games\\Mercenaries 2 World in Flames",
        L"SOFTWARE\\WOW6432Node\\Electronic Arts\\Mercenaries 2 World in Flames", L"SOFTWARE\\Electronic Arts\\Mercenaries 2 World in Flames",
    };
    static const wchar_t *vals[] = {L"Install Dir", L"InstallDir", L"Install Path", L"Path"};
    for (int k = 0; k < 4; k++) for (int v = 0; v < 4; v++) {
        DWORD sz = sizeof p;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, keys[k], vals[v], RRF_RT_REG_SZ, NULL, p, &sz) == ERROR_SUCCESS) {
            size_t l = wcslen(p); while (l && p[l - 1] == L'\\') p[--l] = 0;
            if (GameOk(p)) { wcscpy(out, p); return; }
        }
    }
    static const wchar_t *guess[] = {
        L"C:\\Program Files (x86)\\EA Games\\Mercenaries 2 World in Flames", L"C:\\Program Files\\EA Games\\Mercenaries 2 World in Flames",
        L"C:\\Program Files (x86)\\Electronic Arts\\Mercenaries 2 World in Flames", L"C:\\Games\\Mercenaries 2 World in Flames",
    };
    for (int i = 0; i < 4; i++) if (GameOk(guess[i])) { wcscpy(out, guess[i]); return; }
}

/* ------------------------------------------------------------ browse */
static void BrowseFolder(int id)
{
    BROWSEINFOW bi = {0};
    bi.hwndOwner = g_wnd; bi.lpszTitle = L"Select the Mercenaries 2: World in Flames folder (with Mercenaries2.exe)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pl = SHBrowseForFolderW(&bi);
    if (pl) {
        wchar_t p[MAX_PATH];
        if (SHGetPathFromIDListW(pl, p)) SetDlgItemTextW(g_wnd, id, p);
        CoTaskMemFree(pl);
    }
}
static void BrowseFile(int id, const wchar_t *title, const wchar_t *filter)
{
    wchar_t p[MAX_PATH]; GetText(id, p, MAX_PATH);
    OPENFILENAMEW of = {0};
    of.lStructSize = sizeof of; of.hwndOwner = g_wnd; of.lpstrFile = p; of.nMaxFile = MAX_PATH;
    of.lpstrFilter = filter; of.lpstrTitle = title; of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&of)) SetDlgItemTextW(g_wnd, id, p);
}

/* ------------------------------------------------------- settings */
static void SaveSettings(void)
{
    static const int ids[] = {ID_GAME, ID_ISO, ID_TU, ID_DLC};
    static const wchar_t *keys[] = {L"Game", L"Iso", L"TU4", L"DLC"};
    for (int i = 0; i < 4; i++) {
        wchar_t b[MAX_PATH]; GetText(ids[i], b, MAX_PATH);
        WritePrivateProfileStringW(L"Setup", keys[i], b, g_settings);
    }
    WritePrivateProfileStringW(L"Setup", L"Content", IsDlgButtonChecked(g_wnd, ID_OPT_FULL) ? L"1" : L"0", g_settings);
}
static void LoadSettings(void)
{
    static const int ids[] = {ID_GAME, ID_ISO, ID_TU, ID_DLC};
    static const wchar_t *keys[] = {L"Game", L"Iso", L"TU4", L"DLC"};
    for (int i = 0; i < 4; i++) {
        wchar_t b[MAX_PATH];
        GetPrivateProfileStringW(L"Setup", keys[i], L"", b, MAX_PATH, g_settings);
        if (b[0]) SetDlgItemTextW(g_wnd, ids[i], b);
    }
    int full = GetPrivateProfileIntW(L"Setup", L"Content", 1, g_settings);
    CheckRadioButton(g_wnd, ID_OPT_FULL, ID_OPT_MOD, full ? ID_OPT_FULL : ID_OPT_MOD);
}

/* ------------------------------------------------------------ UI state */
static void Enable(void)
{
    int busy = g_busy != 0, full = IsDlgButtonChecked(g_wnd, ID_OPT_FULL) == BST_CHECKED;
    static const int xs[] = {ID_ISO, ID_ISO_BR, ID_TU, ID_TU_BR, ID_DLC, ID_DLC_BR, ID_LBL_ISO, ID_LBL_TU, ID_LBL_DLC};
    for (int i = 0; i < 9; i++) EnableWindow(GetDlgItem(g_wnd, xs[i]), full && !busy);
    static const int gs[] = {ID_GAME, ID_GAME_BR, ID_OPT_FULL, ID_OPT_MOD, ID_INSTALL, ID_UNINSTALL};
    for (int i = 0; i < 6; i++) EnableWindow(GetDlgItem(g_wnd, gs[i]), !busy);
    SetDlgItemTextW(g_wnd, ID_CLOSE, busy ? L"Cancel" : L"Close");
}

/* --------------------------------------------------------- converter */
typedef struct { wchar_t cmd[4096]; wchar_t cwd[MAX_PATH]; } Job;
static const int g_weight[8] = {5, 8, 2, 3, 60, 2, 18, 2};   /* share of each converter stage in the bar */

static void PostLine(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    wchar_t *w = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    PostMessageW(g_wnd, WM_APP_LINE, 0, (LPARAM)w);
}

static DWORD WINAPI ConvThread(LPVOID arg)
{
    Job *j = (Job *)arg;
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {0}; PROCESS_INFORMATION pi = {0};
    si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    /* environment: UTF-8 output, no user site packages / PYTHONPATH leaking in */
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");
    SetEnvironmentVariableW(L"PYTHONPATH", NULL);
    SetEnvironmentVariableW(L"PYTHONHOME", NULL);
    DWORD rc = 1;
    if (!CreateProcessW(NULL, j->cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, j->cwd, &si, &pi)) {
        CloseHandle(wr); CloseHandle(rd);
        PostLine("ERROR: could not start the converter (converter\\python\\python.exe missing?)");
        PostMessageW(g_wnd, WM_APP_DONE, 1, 0);
        HeapFree(GetProcessHeap(), 0, j);
        return 0;
    }
    AssignProcessToJobObject(g_job, pi.hProcess);
    g_proc = pi.hProcess;
    ResumeThread(pi.hThread); CloseHandle(pi.hThread);
    CloseHandle(wr);
    char buf[4096], line[8192]; int ln = 0; DWORD r;
    while (ReadFile(rd, buf, sizeof buf, &r, NULL) && r) {
        for (DWORD i = 0; i < r; i++) {
            char c = buf[i];
            if (c == '\n' || ln >= (int)sizeof line - 1) {
                line[ln] = 0; if (ln && line[ln - 1] == '\r') line[ln - 1] = 0;
                PostLine(line); ln = 0;
            } else line[ln++] = c;
        }
    }
    if (ln) { line[ln] = 0; PostLine(line); }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &rc);
    g_proc = NULL;
    CloseHandle(pi.hProcess);
    PostMessageW(g_wnd, WM_APP_DONE, rc, 0);
    HeapFree(GetProcessHeap(), 0, j);
    return 0;
}

static void OnLine(wchar_t *w)
{
    if (w[0] == L'@') {
        int k = 0, n = 0; double f = 0; int used = 0;
        if (swscanf(w + 1, L"%d/%d %lf %n", &k, &n, &f, &used) >= 3 && k >= 1 && k <= 8) {
            int base = 0, tot = 0;
            for (int i = 0; i < 8; i++) { if (i < k - 1) base += g_weight[i]; tot += g_weight[i]; }
            double pos = (base + g_weight[k - 1] * (f < 0 ? 0 : f > 1 ? 1 : f)) / tot;
            SendMessageW(g_prog, PBM_SETPOS, (WPARAM)(pos * 1000), 0);
            Status(w + 1 + used);
        }
        return;
    }
    LogW(w);
}

/* ------------------------------------------------------------- install */
static wchar_t g_game[MAX_PATH];
static int g_full;

/* Xbox button prompt glyphs from the player's own game files -> <game>\\Mercenaries2Remastered\\prompts.bin */
static void MakePrompts(void)
{
    wchar_t py[MAX_PATH], scr[MAX_PATH], cmd[2048], cwd[MAX_PATH];
    Join(py, g_home, L"converter\\python\\python.exe");
    Join(scr, g_home, L"converter\\m2conv\\prompts.py");
    Join(cwd, g_home, L"converter");
    if (!Exists(py) || !Exists(scr)) { LogW(L"Button prompt tool missing - Xbox prompts stay off."); return; }
    _snwprintf(cmd, 2047, L"\"%s\" -u -X utf8 \"%s\" --game \"%s\"", py, scr, g_game); cmd[2047] = 0;
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr; CreatePipe(&rd, &wr, &sa, 0); SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {0}; PROCESS_INFORMATION pi = {0};
    si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr;
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr); LogW(L"Could not run the button prompt tool."); return;
    }
    CloseHandle(wr);
    char buf[4096]; DWORD r, tot = 0; static char out[16384];
    while (ReadFile(rd, buf, sizeof buf, &r, NULL) && r) { if (tot + r < sizeof out - 1) { memcpy(out + tot, buf, r); tot += r; } }
    out[tot] = 0; CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 60000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    char *line = strtok(out, "\r\n");
    while (line) {
        wchar_t w[1024]; MultiByteToWideChar(CP_UTF8, 0, line, -1, w, 1024); w[1023] = 0; LogW(w);
        line = strtok(NULL, "\r\n");
    }
}


/* Mercenaries2.exe -> large address aware: the retail exe is kept as Mercenaries2.bak and replaced by a copy with
 * IMAGE_FILE_LARGE_ADDRESS_AWARE set and the PE checksum updated (the result is EXE4_MD5). */
static int PatchExeLaa(void)
{
    wchar_t exe[MAX_PATH], bak[MAX_PATH], tmp[MAX_PATH]; char md5[33] = "";
    Join(exe, g_game, L"Mercenaries2.exe"); Join(bak, g_game, L"Mercenaries2.bak"); Join(tmp, g_game, L"Mercenaries2.exe.mercenaries2remastered-tmp");
    if (!FileMd5(exe, md5)) { LogW(L"ERROR: cannot read Mercenaries2.exe"); return 0; }
    if (!strcmp(md5, EXE4_MD5)) { LogW(L"Mercenaries2.exe is already large address aware (4 GB)"); return 1; }
    if (strcmp(md5, EXE_MD5)) { LogW(L"Mercenaries2.exe is not the retail 1.1 exe - left unchanged (no 4 GB patch)"); return 1; }
    HANDLE f = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { LogW(L"ERROR: cannot read Mercenaries2.exe"); return 0; }
    DWORD n = GetFileSize(f, NULL), r = 0;
    BYTE *d = (BYTE *)HeapAlloc(GetProcessHeap(), 0, n);
    int ok = d && ReadFile(f, d, n, &r, NULL) && r == n;
    CloseHandle(f);
    if (!ok) { if (d) HeapFree(GetProcessHeap(), 0, d); LogW(L"ERROR: cannot read Mercenaries2.exe"); return 0; }
    DWORD pe = *(DWORD *)(d + 0x3C), co = pe + 24 + 64;
    d[pe + 22] |= 0x20;                                     /* FileHeader.Characteristics: LARGE_ADDRESS_AWARE */
    memset(d + co, 0, 4);
    DWORD sum = 0;
    for (DWORD i = 0; i + 1 < n; i += 2) { sum += d[i] | (d[i + 1] << 8); sum = (sum & 0xFFFF) + (sum >> 16); }
    if (n & 1) { sum += d[n - 1]; sum = (sum & 0xFFFF) + (sum >> 16); }
    sum = ((sum & 0xFFFF) + (sum >> 16)) + n;
    memcpy(d + co, &sum, 4);
    if (!Exists(bak) && !CopyFileW(exe, bak, TRUE)) {
        HeapFree(GetProcessHeap(), 0, d);
        LogF(L"ERROR: cannot back up Mercenaries2.exe to Mercenaries2.bak (error %lu)", GetLastError()); return 0;
    }
    f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD w = 0;
    ok = f != INVALID_HANDLE_VALUE && WriteFile(f, d, n, &w, NULL) && w == n;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    HeapFree(GetProcessHeap(), 0, d);
    if (!ok || !FileMd5(tmp, md5) || strcmp(md5, EXE4_MD5) || !MoveFileExW(tmp, exe, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp);
        LogF(L"ERROR: cannot write the 4 GB Mercenaries2.exe (error %lu). Is the game running?", GetLastError()); return 0;
    }
    LogW(L"Mercenaries2.exe is now large address aware (4 GB); the original is kept as Mercenaries2.bak");
    return 1;
}

static int IsOurDll(const wchar_t *p) { return FileHas(p, "Mercenaries2Remastered"); }

/* settings file: the default one; an existing file is rebuilt from it, keeping the values of the settings
   that are still there (settings that were dropped or renamed go away) */
static void WriteIni(void)
{
    wchar_t ini[MAX_PATH], defini[MAX_PATH], tmp[MAX_PATH];
    Join(ini, g_game, L"Mercenaries2Remastered.ini");
    Join(defini, g_home, L"files\\Mercenaries2Remastered.ini");
    if (!Exists(defini)) return;
    if (!Exists(ini)) { if (CopyFileW(defini, ini, TRUE)) LogF(L"Created %s", ini); return; }
    Join(tmp, g_game, L"Mercenaries2Remastered.ini.new");
    if (!CopyFileW(defini, tmp, FALSE)) return;
    static wchar_t secs[4096], keys[16384], v[512];
    GetPrivateProfileSectionNamesW(secs, 4096, tmp);
    for (wchar_t *sec = secs; *sec; sec += wcslen(sec) + 1) {
        GetPrivateProfileStringW(sec, NULL, L"", keys, 16384, tmp);
        for (wchar_t *k = keys; *k; k += wcslen(k) + 1)
            if (GetPrivateProfileStringW(sec, k, L"\x01", v, 512, ini) && v[0] != 1) WritePrivateProfileStringW(sec, k, v, tmp);
    }
    if (MoveFileExW(tmp, ini, MOVEFILE_REPLACE_EXISTING)) LogF(L"Updated %s (your values kept)", ini);
    else DeleteFileW(tmp);
}

static int InstallMod(void)
{
    wchar_t src[MAX_PATH], dst[MAX_PATH], bak[MAX_PATH];
    Join(src, g_home, L"files\\dinput8.dll");
    if (!Exists(src)) { Err(L"files\\dinput8.dll is missing next to the setup program."); return 0; }
    Join(dst, g_game, L"dinput8.dll");
    if (Exists(dst) && !IsOurDll(dst)) {
        Join(bak, g_game, L"dinput8.dll.mercenaries2remastered-backup");
        if (!Exists(bak) && MoveFileW(dst, bak)) LogW(L"Existing dinput8.dll (another mod) kept as dinput8.dll.mercenaries2remastered-backup");
    }
    if (!CopyFileW(src, dst, FALSE)) {
        LogF(L"ERROR: cannot write %s (error %lu). Is the game running?", dst, GetLastError());
        return 0;
    }
    LogF(L"Installed %s", dst);
    if (!PatchExeLaa()) return 0;
    WriteIni();
    MakePrompts();
    return 1;
}

static void RemoveContent(void)
{
    wchar_t p[MAX_PATH];
    Join(p, g_game, L"Mercenaries2Remastered\\content"); DeleteTree(p);
    Join(p, g_game, L"Mercenaries2Remastered\\build"); DeleteTree(p);
    Join(p, g_game, L"Mercenaries2Remastered"); RemoveDirectoryW(p);
}

static void StartConverter(void)
{
    wchar_t iso[MAX_PATH], tu[MAX_PATH], dlc[MAX_PATH], py[MAX_PATH], scr[MAX_PATH];
    GetText(ID_ISO, iso, MAX_PATH); GetText(ID_TU, tu, MAX_PATH); GetText(ID_DLC, dlc, MAX_PATH);
    Join(py, g_home, L"converter\\python\\python.exe");
    Join(scr, g_home, L"converter\\m2conv\\m2conv.py");
    Job *j = (Job *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Job));
    _snwprintf(j->cmd, 4095, L"\"%s\" -u -X utf8 \"%s\" --game \"%s\" --iso \"%s\" --tu \"%s\" --dlc \"%s\"", py, scr, g_game, iso, tu, dlc);
    Join(j->cwd, g_home, L"converter");
    InterlockedExchange(&g_busy, 1); g_cancel = 0; Enable();
    SendMessageW(g_prog, PBM_SETPOS, 0, 0);
    Status(L"Starting the converter...");
    LogW(L"Converting the Xbox 360 content (this takes a few minutes; it can be resumed if interrupted)...");
    CloseHandle(CreateThread(NULL, 0, ConvThread, j, 0, NULL));
}

static void OnInstall(void)
{
    GetText(ID_GAME, g_game, MAX_PATH);
    g_full = IsDlgButtonChecked(g_wnd, ID_OPT_FULL) == BST_CHECKED;
    if (!GameOk(g_game)) { Err(L"Please select the Mercenaries 2: World in Flames folder (the one with Mercenaries2.exe and data\\vz.wad)."); return; }
    wchar_t exe[MAX_PATH]; char md5[33] = "";
    Join(exe, g_game, L"Mercenaries2.exe");
    if (FileMd5(exe, md5) && strcmp(md5, EXE_MD5) && strcmp(md5, EXE4_MD5)) {
        if (!Ask(L"Mercenaries2.exe is not the version Mercenaries2Remastered was made for (retail exe, Sep 4 2008).\n"
                 L"The mod's game fixes stay off with other versions; only the file fixes work.\n\nContinue anyway?")) return;
    }
    if (g_full) {
        static const int ids[] = {ID_ISO, ID_TU, ID_DLC};
        static const wchar_t *what[] = {L"Xbox 360 game image (.iso)", L"Title Update 4 package (or its .zip)", L"Blow It Up Again Pack package (or its .zip)"};
        for (int i = 0; i < 3; i++) {
            wchar_t p[MAX_PATH]; GetText(ids[i], p, MAX_PATH);
            if (!p[0] || !Exists(p) || IsDir(p)) {
                wchar_t m[512]; _snwprintf(m, 511, L"Please select the %s.", what[i]); m[511] = 0; Err(m); return;
            }
        }
    }
    SaveSettings();
    SetWindowTextW(g_log, L"");
    LogF(L"Game folder: %s", g_game);
    if (!InstallMod()) { Status(L"Installation failed."); return; }
    if (g_full) { StartConverter(); return; }
    wchar_t c[MAX_PATH]; Join(c, g_game, L"Mercenaries2Remastered\\content");
    if (IsDir(c) && Ask(L"Remove the Xbox 360 content installed earlier (Title Update 4 and DLC)?")) { RemoveContent(); LogW(L"Removed the TU4 / DLC content."); }
    SendMessageW(g_prog, PBM_SETPOS, 1000, 0);
    Status(L"Done - Mercenaries2Remastered is installed.");
    LogW(L"Done. Settings: Mercenaries2Remastered.ini in the game folder.");
    MessageBoxW(g_wnd, L"Mercenaries2Remastered is installed.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
}

static void OnDone(DWORD rc)
{
    InterlockedExchange(&g_busy, 0); Enable();
    if (rc == 0) {
        SendMessageW(g_prog, PBM_SETPOS, 1000, 0);
        Status(L"Done - Mercenaries2Remastered with Title Update 4 and the DLC is installed.");
        LogW(L"Done. The DLC is in Extras > Downloadable Content; the DLC outfits are in the PMC wardrobe.");
        MessageBoxW(g_wnd, L"Mercenaries2Remastered with Title Update 4 and the Blow It Up Again Pack is installed.\n\n"
                           L"The DLC is started from Extras > Downloadable Content.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
    } else if (g_cancel) {
        Status(L"Cancelled - run Install again to continue where it stopped.");
        LogW(L"Cancelled. The mod itself is installed; the content conversion continues where it stopped when you run Install again.");
    } else {
        Status(L"The content conversion failed - see the log.");
        Err(L"The TU4 / DLC conversion failed. The log above says why.\n\nThe mod itself is installed and works without that content.");
    }
}

/* ------------------------------------------------------------ uninstall */
static void OnUninstall(void)
{
    GetText(ID_GAME, g_game, MAX_PATH);
    wchar_t dll[MAX_PATH], bak[MAX_PATH], p[MAX_PATH];
    Join(dll, g_game, L"dinput8.dll");
    if (!IsDir(g_game) || (!Exists(dll) && !IsDir((Join(p, g_game, L"Mercenaries2Remastered"), p)))) { Err(L"Mercenaries2Remastered is not installed in the selected folder."); return; }
    if (!Ask(L"Remove Mercenaries2Remastered, its settings and the converted TU4 / DLC content from this game folder?")) return;
    SetWindowTextW(g_log, L"");
    if (Exists(dll)) {
        if (IsOurDll(dll)) { if (DeleteFileW(dll)) LogW(L"Removed dinput8.dll"); else LogF(L"ERROR: cannot remove %s (game running?)", dll); }
        else LogW(L"dinput8.dll is not Mercenaries2Remastered - left alone");
    }
    Join(bak, g_game, L"dinput8.dll.mercenaries2remastered-backup");
    if (Exists(bak) && !Exists(dll) && MoveFileW(bak, dll)) LogW(L"Restored the previous dinput8.dll");
    {   /* the original exe back from Mercenaries2.bak */
        wchar_t exe[MAX_PATH]; char md5[33] = "";
        Join(exe, g_game, L"Mercenaries2.exe"); Join(p, g_game, L"Mercenaries2.bak");
        if (Exists(p) && FileMd5(p, md5) && !strcmp(md5, EXE_MD5)) {
            if (MoveFileExW(p, exe, MOVEFILE_REPLACE_EXISTING)) LogW(L"Restored the original Mercenaries2.exe from Mercenaries2.bak");
            else LogF(L"ERROR: cannot restore Mercenaries2.exe from Mercenaries2.bak (error %lu)", GetLastError());
        }
    }
    Join(p, g_game, L"Mercenaries2Remastered\\prompts.bin"); DeleteFileW(p);
    Join(p, g_game, L"Mercenaries2Remastered\\shaders.bin"); DeleteFileW(p);
    RemoveContent();
    static const wchar_t *files[] = {L"Mercenaries2Remastered.ini", L"Mercenaries2Remastered.log", L"Mercenaries2Remastered.1.log",
                                     L"Mercenaries2Remastered.2.log", L"Mercenaries2Remastered.3.log", L"Mercenaries2Remastered.4.log",
                                     L"Mercenaries2Remastered.5.log"};
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) { Join(p, g_game, files[i]); if (Exists(p)) DeleteFileW(p); }
    LogW(L"Mercenaries2Remastered removed. Mercenaries2.exe is the original again; no other game file was changed.");
    Status(L"Uninstalled.");
}

/* ------------------------------------------------------------- window */
static HWND Ctl(const wchar_t *cls, const wchar_t *txt, DWORD st, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(!wcscmp(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, txt, WS_CHILD | WS_VISIBLE | st,
                             S(x), S(y), S(w), S(h), g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void Build(void)
{
    int W = 620, x = 16, y = 14;
    HWND t = Ctl(L"STATIC", L"Mercenaries2Remastered for Mercenaries 2: World in Flames (PC)", SS_LEFT, x, y, W - 32, 22, 0);
    SendMessageW(t, WM_SETFONT, (WPARAM)g_bold, TRUE);
    y += 30;
    Ctl(L"STATIC", L"Game folder:", SS_LEFT, x, y + 3, 90, 18, ID_LBL_GAME);
    Ctl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, x + 92, y, W - 32 - 92 - 88, 23, ID_GAME);
    Ctl(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, W - 16 - 80, y, 80, 24, ID_GAME_BR);
    y += 36;
    Ctl(L"BUTTON", L"What to install", BS_GROUPBOX, x, y, W - 32, 74, ID_GRP_OPT);
    Ctl(L"BUTTON", L"Mod + Xbox 360 Title Update 4 and the \"Blow It Up Again Pack\" DLC (converted from your Xbox 360 files)",
        BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, x + 12, y + 20, W - 56, 20, ID_OPT_FULL);
    Ctl(L"BUTTON", L"Mod only (fixes and features, no Xbox 360 content)", BS_AUTORADIOBUTTON, x + 12, y + 44, W - 56, 20, ID_OPT_MOD);
    y += 86;
    Ctl(L"BUTTON", L"Your Xbox 360 files", BS_GROUPBOX, x, y, W - 32, 122, ID_GRP_X);
    static const wchar_t *lbl[] = {L"Game disc image (.iso):", L"Title Update 4 (package or .zip):", L"DLC package (or .zip):"};
    static const int ed[] = {ID_ISO, ID_TU, ID_DLC}, br[] = {ID_ISO_BR, ID_TU_BR, ID_DLC_BR}, lb[] = {ID_LBL_ISO, ID_LBL_TU, ID_LBL_DLC};
    for (int i = 0; i < 3; i++) {
        int yy = y + 22 + i * 32;
        Ctl(L"STATIC", lbl[i], SS_LEFT, x + 12, yy + 3, 180, 18, lb[i]);
        Ctl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, x + 194, yy, W - 32 - 194 - 100, 23, ed[i]);
        Ctl(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, W - 16 - 92, yy, 80, 24, br[i]);
    }
    y += 132;
    g_prog = Ctl(PROGRESS_CLASSW, L"", 0, x, y, W - 32, 18, ID_PROG);
    SendMessageW(g_prog, PBM_SETRANGE32, 0, 1000);
    y += 24;
    g_status = Ctl(L"STATIC", L"Ready.", SS_LEFT | SS_ENDELLIPSIS, x, y, W - 32, 18, ID_STATUS);
    y += 24;
    g_log = Ctl(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, x, y, W - 32, 170, ID_LOG);
    SendMessageW(g_log, EM_SETLIMITTEXT, 0, 0);
    y += 182;
    Ctl(L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, W - 16 - 3 * 96 + 8, y, 88, 28, ID_INSTALL);
    Ctl(L"BUTTON", L"Uninstall", BS_PUSHBUTTON | WS_TABSTOP, W - 16 - 2 * 96 + 8, y, 88, 28, ID_UNINSTALL);
    Ctl(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, W - 16 - 96 + 8, y, 88, 28, ID_CLOSE);
    Ctl(L"STATIC", L"Game files are never modified.", SS_LEFT, x, y + 6, 260, 18, ID_NOTE);
    y += 44;
    RECT r = {0, 0, S(W), S(y)};
    AdjustWindowRectEx(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    SetWindowPos(g_wnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE: {
        g_wnd = h;
        HDC dc = GetDC(h); g_dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(h, dc);
        NONCLIENTMETRICSW nm = {sizeof nm};
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0);
        g_font = CreateFontIndirectW(&nm.lfMessageFont);
        nm.lfMessageFont.lfWeight = FW_BOLD; nm.lfMessageFont.lfHeight = nm.lfMessageFont.lfHeight * 5 / 4;
        g_bold = CreateFontIndirectW(&nm.lfMessageFont);
        Build();
        CheckRadioButton(h, ID_OPT_FULL, ID_OPT_MOD, ID_OPT_FULL);
        LoadSettings();
        wchar_t g[MAX_PATH]; GetText(ID_GAME, g, MAX_PATH);
        if (!GameOk(g)) { DetectGame(g); if (g[0]) SetDlgItemTextW(h, ID_GAME, g); }
        Enable();
        LogW(L"Mercenaries2Remastered installs into the game folder as dinput8.dll + Mercenaries2Remastered.ini.");
        LogW(L"For the Title Update 4 / DLC content select your own Xbox 360 files:");
        LogW(L"  - the Mercenaries 2 game disc image (.iso),");
        LogW(L"  - Title Update 4 (the TU_... package, or a .zip holding it),");
        LogW(L"  - the Blow It Up Again Pack package (or a .zip holding it).");
        LogW(L"The content is converted on your PC (a few minutes, about 1.5 GB of free space needed).");
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_GAME_BR: BrowseFolder(ID_GAME); break;
        case ID_ISO_BR: BrowseFile(ID_ISO, L"Mercenaries 2 Xbox 360 game image", L"Disc images (*.iso)\0*.iso\0All files\0*.*\0"); break;
        case ID_TU_BR: BrowseFile(ID_TU, L"Mercenaries 2 Title Update 4", L"All files\0*.*\0Zip archives (*.zip)\0*.zip\0"); break;
        case ID_DLC_BR: BrowseFile(ID_DLC, L"Blow It Up Again Pack", L"All files\0*.*\0Zip archives (*.zip)\0*.zip\0"); break;
        case ID_OPT_FULL: case ID_OPT_MOD: Enable(); break;
        case ID_INSTALL: if (!g_busy) OnInstall(); break;
        case ID_UNINSTALL: if (!g_busy) OnUninstall(); break;
        case ID_CLOSE:
            if (g_busy) {
                if (Ask(L"Stop the content conversion? It continues where it stopped the next time you press Install.")) {
                    g_cancel = 1; if (g_job) TerminateJobObject(g_job, 2);
                }
            } else DestroyWindow(h);
            break;
        }
        return 0;
    case WM_APP_LINE: { wchar_t *w = (wchar_t *)lp; OnLine(w); HeapFree(GetProcessHeap(), 0, w); return 0; }
    case WM_APP_DONE: OnDone((DWORD)wp); return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_log) { SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW)); return (LRESULT)GetSysColorBrush(COLOR_WINDOW); }
        break;
    case WM_CLOSE:
        if (g_busy) { SendMessageW(h, WM_COMMAND, ID_CLOSE, 0); return 0; }
        DestroyWindow(h); return 0;
    case WM_DESTROY:
        if (g_job) TerminateJobObject(g_job, 2);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, LPWSTR cmd, int show)
{
    (void)prev; (void)cmd;
    INITCOMMONCONTROLSEX ic = {sizeof ic, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    GetModuleFileNameW(NULL, g_home, MAX_PATH);
    wchar_t *s = wcsrchr(g_home, L'\\'); if (s) *s = 0;
    Join(g_settings, g_home, L"Mercenaries2RemasteredSetup.ini");
    /* the converter and everything it starts die with the setup window */
    g_job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.lpszClassName = L"Mercenaries2RemasteredSetup";
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, APP_TITLE, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 600, 600, NULL, NULL, hi, NULL);
    ShowWindow(w, show); UpdateWindow(w);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(w, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return 0;
}
