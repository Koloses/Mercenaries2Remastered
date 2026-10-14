/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
#pragma once
#include <d3d9.h>

extern IDirect3DDevice9 *g_dev;

/* called on UnlockRect(level 0) after a full-surface lock: bits are still valid */
typedef void (*D3DTexCallback)(IDirect3DTexture9 *t, const D3DSURFACE_DESC *d, void *bits, int pitch);
typedef void (*D3DFrameCallback)(IDirect3DDevice9 *dev);
typedef void (*D3DUpdateCallback)(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst);

void D3DInit(void);
void D3DSetTextureCallback(D3DTexCallback cb);
void D3DAddFrameCallback(D3DFrameCallback cb);
void D3DSetUpdateCallback(D3DUpdateCallback cb);
HRESULT D3DUpdateTexture(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst);
void D3DAddResetCallback(void (*cb)(void));   /* before IDirect3DDevice9::Reset */
