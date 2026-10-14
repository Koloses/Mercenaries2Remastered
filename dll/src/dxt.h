/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
#pragma once
#include <stdint.h>
#include <stdlib.h>
void DxtDecode5(const uint8_t *src, int w, int h, uint8_t *rgba);
void DxtEncode5(const uint8_t *rgba, int w, int h, uint8_t *dst);
