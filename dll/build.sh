#!/bin/sh
# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
# out/dinput8.dll                         loader (picks the build for the game exe)
# out/Mercenaries2Remastered-1.0.dll      the mod for Mercenaries2.exe 1.0 (from src/)
# out/Mercenaries2Remastered-1.1.dll      the mod for Mercenaries2.exe 1.1 (src/ retargeted with v11.map)
set -e
cd "$(dirname "$0")"
mkdir -p out
SRCS="main.c util.c perf.c d3d.c pad.c cam.c online.c vehicle.c aim.c content.c prompts.c promptsgfx.c dxt.c gfx.c deform.c vspatch.c lod.c pickup.c post.c mem.c fps.c cull.c statecache.c securom.c forest.c shaderwarm.c cheats.c water.c stream.c drawpool.c msaa.c jitter.c movie.c assettable.c softpart.c paper.c bloom.c emitfix.c xbshader.c treeshadow.c depthaa.c"
# M2R_SHOT=1 ./build.sh: developer build with the screenshot key ([Display] ScreenshotKey, F11)
SHOT=""; [ "$M2R_SHOT" = 1 ] && SHOT="-DM2R_SHOT shot.c dump.c fireprobe.c msm.c"
CC="i686-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -shared -static-libgcc"
core() {   # core <srcdir> <out> [defines]
    ( cd "$1" && $CC -o "$2" $SRCS $SHOT $3 dinput8.def -Wl,--enable-stdcall-fixup -lwinmm -lm -s )
}
core src "$PWD/out/Mercenaries2Remastered-1.0.dll"
rm -rf out/src11
python3 tools/retarget.py v11.map src out/src11
core out/src11 "$PWD/out/Mercenaries2Remastered-1.1.dll" -DM2R_GAME11
i686-w64-mingw32-gcc -O2 -Wall -Wextra -shared -static-libgcc -o out/dinput8.dll loader/loader.c loader/dinput8.def \
  -Wl,--enable-stdcall-fixup -s
