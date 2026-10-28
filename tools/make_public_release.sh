#!/bin/sh
# Builds the public SSNE-Native release: launcher, gctool, build kit (translator + prebuilt PC port, no
# game code), licenses and the source tree. Game code is never included: the port is configured without
# SSNE_RECOMPILED_DIR and players build the game on their own PC from their own disc.
#
#   tools/make_public_release.sh <out_dir> <llvm-mingw dir> [SDL2 source dir] [imgui source dir]
#
# The llvm-mingw release must match toolchain_url in data/kit/kit.ini (the prebuilt libraries are linked with the
# compiler the launcher downloads).
set -e
OUT=$1; LLVM=$2; SDLSRC=$3; IMGUISRC=$4
[ -n "$OUT" ] && [ -n "$LLVM" ] || { echo "usage: $0 <out_dir> <llvm-mingw dir> [sdl2 src] [imgui src]"; exit 1; }
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$ROOT/build-release-win
EXTRA=""
[ -n "$SDLSRC" ] && EXTRA="$EXTRA -DFETCHCONTENT_SOURCE_DIR_SDL2=$SDLSRC"
[ -n "$IMGUISRC" ] && EXTRA="$EXTRA -DFETCHCONTENT_SOURCE_DIR_IMGUI=$IMGUISRC"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64-x86_64.cmake" \
      -DLLVM_MINGW="$LLVM" -DSSNE_RECOMPILED_DIR= -DSSNE_BUILD_TESTS=OFF -DSSNE_PGO_PROFILE= $EXTRA >/dev/null
# Profile-guided optimization of the runtime: the trained profile converted for this llvm-mingw (or none).
PGO=$("$ROOT/tools/pgo_build_profile.sh" "$BUILD" "$LLVM/bin/llvm-profdata" "$BUILD/runtime.profdata") || PGO=
cmake -S "$ROOT" -B "$BUILD" -DSSNE_PGO_PROFILE="$PGO" >/dev/null
cmake --build "$BUILD" --target ssne-launcher gctool ssne-recomp ssne-port

STRIP=$LLVM/bin/llvm-strip
rm -rf "$OUT"; mkdir -p "$OUT/build-kit/lib/port" "$OUT/build-kit/include" "$OUT/licenses"
cp "$BUILD/bin/SSNE-Native.exe" "$BUILD/bin/gctool.exe" "$ROOT/data/known_dumps.txt" "$OUT/"
cp "$BUILD/bin/ssne-recomp.exe" "$ROOT/data/symbols/G3BE9G_r1.sym" "$ROOT/data/symbols/G3BP9G_r0.sym" "$ROOT/data/kit/kit.ini" "$OUT/build-kit/"
"$STRIP" "$OUT/SSNE-Native.exe" "$OUT/gctool.exe" "$OUT/build-kit/ssne-recomp.exe"
cp -r "$ROOT/runtime/include/ssne" "$OUT/build-kit/include/"
for f in main settings settings_menu png_write; do cp "$BUILD/CMakeFiles/ssne-port.dir/port/src/$f.cpp.obj" "$OUT/build-kit/lib/port/$f.obj"; done
cp "$BUILD/libssne-runtime.a" "$BUILD/libimgui.a" "$BUILD/libgc.a" "$OUT/build-kit/lib/"
cp "$BUILD/_deps/sdl2-build/libSDL2main.a" "$BUILD/_deps/sdl2-build/libSDL2.a" "$OUT/build-kit/lib/"
"$STRIP" -g "$OUT"/build-kit/lib/*.a "$OUT"/build-kit/lib/port/*.obj

cp "$ROOT/data/release/README.txt" "$ROOT/data/release/CHANGELOG.txt" "$ROOT/data/release/THIRD-PARTY-NOTICES.txt" "$ROOT/LICENSE.txt" "$OUT/"
SDLDIR=${SDLSRC:-$BUILD/_deps/sdl2-src}; IMDIR=${IMGUISRC:-$BUILD/_deps/imgui-src}
cp "$SDLDIR/LICENSE.txt" "$OUT/licenses/SDL2-LICENSE.txt"
cp "$IMDIR/LICENSE.txt" "$OUT/licenses/DearImGui-LICENSE.txt"
cp "$ROOT/launcher/fonts/EricaOne-OFL.txt" "$ROOT/launcher/fonts/Outfit-OFL.txt" "$OUT/licenses/"
cp "$LLVM/LICENSE.TXT" "$OUT/licenses/LLVM-LICENSE.txt"
M=$(ls -d "$LLVM"/*-w64-mingw32/share/mingw32 | head -1)
cp "$M/COPYING.MinGW-w64-runtime.txt" "$OUT/licenses/MinGW-w64-runtime-COPYING.txt"
cp "$M/COPYING.winpthreads.txt" "$OUT/licenses/winpthreads-COPYING.txt"

# Source: everything committed to the repository.
mkdir -p "$OUT/source"
git -C "$ROOT" archive HEAD | tar -x -C "$OUT/source"
# Sanity check: no generated game code anywhere in the release.
# (generated functions are named f_<address>; none may appear in the shipped binaries)
for f in "$OUT"/*.exe "$OUT"/build-kit/*.exe "$OUT"/build-kit/lib/*.a "$OUT"/build-kit/lib/port/*.obj; do
  if grep -aqE "f_80[0-9A-F]{6}" "$f"; then echo "ERROR: generated game code found in $f"; exit 1; fi
done
echo "release written to $OUT"
