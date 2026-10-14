/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define M2R_VERSION "Beta 1"
/* The sources address Mercenaries2.exe 1.0 (Aug 13 2008). The 1.1 build (Sep 12 2008) is compiled from a copy
 * made by tools/retarget.py with v11.map (every game address and changed byte pattern), with M2R_GAME11 set. */
#ifdef M2R_GAME11
#define M2R_GAME "1.1"
#else
#define M2R_GAME "1.0"
#endif

extern char g_gameDir[MAX_PATH];

void  UtilInit(void);
void  Log(const char *fmt, ...);
void  LogFlush(void);
void  LogFinal(void);

/* Mercenaries2Remastered.ini */
int   IniInt(const char *sec, const char *key, int def);
float IniFloat(const char *sec, const char *key, float def);
void  IniStr(const char *sec, const char *key, const char *def, char *out, int n);

/* memory / code patching in the game image */
int   PatchIAT(HMODULE mod, void *orig, void *hook);
int   Expect(DWORD addr, const char *hex);              /* bytes at addr == hex string? */
void  WriteMem(DWORD addr, const void *src, size_t n);
void  WriteJmp(DWORD at, void *to, size_t padTo);       /* E9 rel32 (+NOP padding) */
void  WriteCall(DWORD at, void *to, size_t padTo);      /* E8 rel32 (+NOP padding) */
void  WritePtr(DWORD addr, const void *p);              /* overwrite a 32-bit operand */
int   GameVersionOK(void);

/* modules */
void  PerfInit(void);
void  PadInit(void);
void  PromptsInit(void);
void  CamInit(void);
void  TurretInit(void);
void  OnlineInit(void);
void  VehicleInit(void);
void  DeformInit(void);
void  LodInit(void);
void  PostInit(void);
void  PickupInit(void);
void  MemInit(void);
void  AimInit(void);
void  ContentInit(void);
void  ContentSetDlcMount(const char *p);
