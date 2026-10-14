#!/bin/sh
# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
# Builds dist/Mercenaries2Remastered-Setup and dist/Mercenaries2Remastered-<version>-Setup.zip (Beta 1 -> Beta1)
# Needs: i686-w64-mingw32-gcc, x86_64-w64-mingw32-gcc (+ windres), python3, pip, curl, zip
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER=$(sed -n 's/#define M2R_VERSION "\(.*\)"/\1/p' "$ROOT/dll/src/m2r.h" | tr -d ' ')
CACHE="$ROOT/.cache"; DIST="$ROOT/dist"; D="$DIST/Mercenaries2Remastered-Setup"
PYVER=3.11.9
mkdir -p "$CACHE"
rm -rf "$D"; mkdir -p "$D/files" "$D/converter"

echo "== dinput8.dll"
"$ROOT/dll/build.sh"
cp "$ROOT/dll/out/dinput8.dll" "$D/files/"
python3 "$ROOT/tools/gen_default_ini.py" "$ROOT/dll/src/util.c" "$D/files/Mercenaries2Remastered.ini"

echo "== setup"
mkdir -p "$ROOT/setup/out"
(cd "$ROOT/setup" && x86_64-w64-mingw32-windres setup.rc -O coff -o out/setup_res.o && \
 x86_64-w64-mingw32-gcc -O2 -Wall -municode -mwindows -o out/Mercenaries2RemasteredSetup.exe setup.c out/setup_res.o \
   -lcomctl32 -lshell32 -lole32 -ladvapi32 -lcomdlg32 -s -static)
cp "$ROOT/setup/out/Mercenaries2RemasteredSetup.exe" "$D/"

echo "== converter"
cp -r "$ROOT/converter/m2conv" "$D/converter/m2conv"
find "$D/converter/m2conv" -name __pycache__ -prune -exec rm -rf {} +
mkdir -p "$D/converter/m2conv/bin"
x86_64-w64-mingw32-gcc -O2 -shared -static-libgcc -o "$D/converter/m2conv/bin/imaenc.dll" "$ROOT/converter/native/imaenc.c"

echo "== Python $PYVER (embeddable) + numpy, Pillow, capstone, ffmpeg"
[ -f "$CACHE/python-$PYVER-embed-amd64.zip" ] || curl -sSL -o "$CACHE/python-$PYVER-embed-amd64.zip" "https://www.python.org/ftp/python/$PYVER/python-$PYVER-embed-amd64.zip"
pip download -q --only-binary=:all: --platform win_amd64 --python-version 3.11 --implementation cp --no-deps \
    numpy==2.2.6 pillow==12.3.0 capstone==5.0.7 imageio-ffmpeg==0.6.0 -d "$CACHE"
mkdir -p "$D/converter/python/Lib/site-packages"
(cd "$D/converter/python" && unzip -q "$CACHE/python-$PYVER-embed-amd64.zip")
for w in numpy-2.2.6-cp311-cp311-win_amd64 pillow-12.3.0-cp311-cp311-win_amd64 capstone-5.0.7-py3-none-win_amd64; do
    (cd "$D/converter/python/Lib/site-packages" && unzip -q -o "$CACHE/$w.whl")
done
printf 'python311.zip\r\n.\r\nLib\\site-packages\r\n..\\m2conv\r\n' > "$D/converter/python/python311._pth"
(cd "$D/converter/python/Lib/site-packages" && find . -type d -name tests -prune -exec rm -rf {} + && find . -name '*.pyi' -delete && rm -rf numpy/f2py numpy/_pyinstaller)
unzip -q -o -j "$CACHE/imageio_ffmpeg-0.6.0-py3-none-win_amd64.whl" 'imageio_ffmpeg/binaries/ffmpeg-win-x86_64-*.exe' -d "$CACHE/ff"
cp "$CACHE"/ff/ffmpeg-win-x86_64-*.exe "$D/converter/m2conv/bin/ffmpeg.exe"

cp "$ROOT/tools/README-release.txt" "$D/README.txt"
cp "$ROOT/LICENSE" "$ROOT/LICENSE-EXCEPTION" "$ROOT/AUTHORS" "$D/"
cd "$DIST"; rm -f "Mercenaries2Remastered-$VER-Setup.zip"; zip -qr -9 "Mercenaries2Remastered-$VER-Setup.zip" Mercenaries2Remastered-Setup
echo "== $DIST/Mercenaries2Remastered-$VER-Setup.zip"
