#!/bin/sh
# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
set -e
cd "$(dirname "$0")"
mkdir -p out
# M2R_SHOT=1 ./build.sh: developer build with the screenshot key ([Display] ScreenshotKey, F11)
EXTRA=""; [ "$M2R_SHOT" = 1 ] && EXTRA="-DM2R_SHOT src/shot.c"
i686-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -shared -static-libgcc \
  -o out/dinput8.dll src/main.c src/util.c src/perf.c src/d3d.c src/pad.c src/cam.c src/online.c src/vehicle.c src/aim.c src/content.c src/prompts.c src/promptsgfx.c src/dxt.c src/gfx.c src/deform.c src/vspatch.c src/lod.c src/pickup.c src/post.c src/mem.c src/fps.c src/cull.c src/statecache.c src/securom.c src/forest.c src/shaderwarm.c src/cheats.c src/water.c src/stream.c src/drawpool.c src/msaa.c src/jitter.c src/movie.c src/assettable.c $EXTRA \
  src/dinput8.def -Wl,--enable-stdcall-fixup -lwinmm -lm -s
