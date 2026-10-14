/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Xbox prompt data types. The data itself (icon cells, Xbox glyphs, font_16 atlas) comes from the
 * player's own game files: tools/prompts.py (run by the installer) writes <game>\Mercenaries2Remastered\prompts.bin. */
#pragma once
#include <stddef.h>
enum { ACT_NONE, ACT_FORWARD, ACT_BACKWARD, ACT_MOVELEFT, ACT_MOVERIGHT, ACT_PRIMARYATTACK, ACT_PRIMARYSWITCH, ACT_RELOAD, ACT_SECONDARYATTACK, ACT_SECONDARYSWITCH, ACT_MELEEATTACK, ACT_USE, ACT_JUMP, ACT_CROUCH, ACT_BINOCULARS, ACT_PDA, ACT_LOOKUP, ACT_LOOKDOWN, ACT_LOOKLEFT, ACT_LOOKRIGHT, ACT_UP, ACT_DOWN, ACT_RIGHT, ACT_WALK, ACT_SPRINT, ACT_START, ACT_MOVE, ACT_LOOK, ACT_COUNT };
static const char *const g_actName[] = { "NONE", "Forward", "Backward", "MoveLeft", "MoveRight", "PrimaryAttack", "PrimarySwitch", "Reload", "SecondaryAttack", "SecondarySwitch", "MeleeAttack", "Use", "Jump", "Crouch", "Binoculars", "PDA", "LookUp", "LookDown", "LookLeft", "LookRight", "Up", "Down", "Right", "Walk", "Sprint", "Start", "MOVE", "LOOK" };
typedef struct { unsigned char x0,y0,x1,y1,act; } PCell;
typedef struct { char name[32]; unsigned crc; int w,h,n; const PCell *cells; } PAtlas;
enum { G_LB, G_RB, G_START, G_BACK, G_DPAD, G_DPAD_UP, G_DPAD_DOWN, G_DPAD_RIGHT, G_DPAD_LEFT, G_DPAD_UD, G_DPAD_LR, G_LS, G_RS, G_LS_LEFT, G_LS_RIGHT, G_RS_LEFT, G_RS_RIGHT, G_LS_CLICK, G_RS_CLICK, G_A, G_B, G_X, G_Y, G_LT, G_RT, G_LS_UP, G_LS_DOWN, G_RS_UP, G_RS_DOWN, G_COUNT };
typedef struct { unsigned off; unsigned char w,h; } XGlyph;
#define MAX_ATLAS 4
extern PAtlas g_atlases[MAX_ATLAS];
extern int g_natlases;
extern XGlyph g_xg[G_COUNT];
extern const unsigned char *g_xgPix;
extern unsigned g_f16crc, g_f16len;
extern const unsigned char *g_f16xbox;
/* parse prompts.bin (buffer must stay alive); 1 = ok */
int PromptsParse(const unsigned char *b, size_t n);
