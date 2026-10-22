#!/bin/sh
# Builds the Linux Silly ReEncounter release: launcher, gctool, build kit (translator + prebuilt PC port, no game
# code), licenses and the source tree. Like tools/make_public_release.sh for Windows, game code is never included:
# the port is configured without SSNE_RECOMPILED_DIR and players build the game on their own PC from their own disc,
# here with the system's C++ compiler instead of a downloaded one.
#
#   tools/make_linux_release.sh <out_dir> [SDL2 source dir] [imgui source dir]
#
# CXX / CC pick the compiler (default: clang++/clang if installed, else g++/gcc). The kit names the same compiler.
set -e
OUT=$1; SDLSRC=$2; IMGUISRC=$3
[ -n "$OUT" ] || { echo "usage: $0 <out_dir> [sdl2 src] [imgui src]"; exit 1; }
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$ROOT/build-release-linux

if [ -z "$CXX" ]; then
  if command -v clang++ >/dev/null 2>&1; then CXX=clang++; CC=${CC:-clang}; else CXX=g++; CC=${CC:-gcc}; fi
fi
CC=${CC:-cc}
CXX_PATH=$(command -v "$CXX") || { echo "C++ compiler '$CXX' not found"; exit 1; }
CXX_BASE=$(basename "$CXX_PATH")
CXX_DESC=$("$CXX_PATH" --version | head -1)
case "$CXX_BASE" in *clang*) CXX_NAME="Clang (clang++)";; *) CXX_NAME="GCC (g++)";; esac

EXTRA=""
[ -n "$SDLSRC" ] && EXTRA="$EXTRA -DFETCHCONTENT_SOURCE_DIR_SDL2=$SDLSRC"
[ -n "$IMGUISRC" ] && EXTRA="$EXTRA -DFETCHCONTENT_SOURCE_DIR_IMGUI=$IMGUISRC"
# PIE everywhere, so the kit links the same way whatever the distribution's compiler defaults are.
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX_PATH" \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DSSNE_RECOMPILED_DIR= -DSSNE_BUILD_TESTS=OFF $EXTRA >/dev/null
cmake --build "$BUILD" --target ssne-launcher gctool ssne-recomp ssne-port

rm -rf "$OUT"; mkdir -p "$OUT/build-kit/lib/port" "$OUT/build-kit/include" "$OUT/licenses"
cp "$BUILD/bin/SillyReEncounter" "$BUILD/bin/gctool" "$ROOT/data/known_dumps.txt" "$OUT/"
cp "$BUILD/bin/ssne-recomp" "$ROOT/data/symbols/G3BE9G_r1.sym" "$ROOT/data/symbols/G3BP9G_r0.sym" "$OUT/build-kit/"
strip "$OUT/SillyReEncounter" "$OUT/gctool" "$OUT/build-kit/ssne-recomp"
VERSION=$(sed -n 's/^version *= *//p' "$ROOT/data/kit/kit.ini")
sed -e "s|@VERSION@|$VERSION|" -e "s|@CXX@|$CXX_BASE|" -e "s|@CXX_NAME@|$CXX_NAME|" -e "s|@CXX_DESC@|$CXX_DESC|" \
    "$ROOT/data/kit/kit-linux.ini.in" > "$OUT/build-kit/kit.ini"
cp -r "$ROOT/runtime/include/ssne" "$OUT/build-kit/include/"
for f in main settings settings_menu png_write; do cp "$BUILD/CMakeFiles/ssne-port.dir/port/src/$f.cpp.o" "$OUT/build-kit/lib/port/$f.o"; done
cp "$BUILD/libssne-runtime.a" "$BUILD/libimgui.a" "$BUILD/libgc.a" "$OUT/build-kit/lib/"
cp "$BUILD/_deps/sdl2-build/libSDL2main.a" "$BUILD/_deps/sdl2-build/libSDL2.a" "$OUT/build-kit/lib/"
strip -g "$OUT"/build-kit/lib/*.a "$OUT"/build-kit/lib/port/*.o

cp "$ROOT/data/release/README.txt" "$ROOT/data/release/README-Linux.txt" "$ROOT/data/release/CHANGELOG.txt" "$ROOT/LICENSE.txt" "$OUT/"
cp "$ROOT/data/release/THIRD-PARTY-NOTICES-Linux.txt" "$OUT/THIRD-PARTY-NOTICES.txt"
SDLDIR=${SDLSRC:-$BUILD/_deps/sdl2-src}; IMDIR=${IMGUISRC:-$BUILD/_deps/imgui-src}
cp "$SDLDIR/LICENSE.txt" "$OUT/licenses/SDL2-LICENSE.txt"
cp "$IMDIR/LICENSE.txt" "$OUT/licenses/DearImGui-LICENSE.txt"
cp "$ROOT/launcher/fonts/EricaOne-OFL.txt" "$ROOT/launcher/fonts/Outfit-OFL.txt" "$OUT/licenses/"

# Source: everything committed to the repository (or the tree as-is outside git).
mkdir -p "$OUT/source"
if git -C "$ROOT" rev-parse HEAD >/dev/null 2>&1; then
  git -C "$ROOT" archive HEAD | tar -x -C "$OUT/source"
else
  (cd "$ROOT" && tar -c --exclude='./build*' --exclude=./recompiled --exclude=./game_data .) | tar -x -C "$OUT/source"
fi
# Sanity check: no generated game code anywhere in the release.
# (generated functions are named f_<address>; none may appear in the shipped binaries)
for f in "$OUT/SillyReEncounter" "$OUT/gctool" "$OUT/build-kit/ssne-recomp" "$OUT"/build-kit/lib/*.a "$OUT"/build-kit/lib/port/*.o; do
  if grep -aqE "f_80[0-9A-F]{6}" "$f"; then echo "ERROR: generated game code found in $f"; exit 1; fi
done
echo "release written to $OUT (game compiler: $CXX_BASE, $CXX_DESC)"
