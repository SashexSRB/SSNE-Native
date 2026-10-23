# Building

Requirements: CMake ≥ 3.20, a C++20 compiler, Python 3 (tests only), Git (CMake fetches SDL2 2.30.9 and Dear ImGui 1.91.5).

## Windows — Visual Studio 2022
```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
Binaries land in `build\bin\`.

## Windows binaries from Linux (llvm-mingw)
```sh
cmake -B build-win -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
      -DLLVM_MINGW=/opt/llvm-mingw \
      -DSSNE_BUILD_TESTS=OFF
ninja -C build-win
```
The output is statically linked and needs only standard Windows DLLs (msvcrt, OpenGL32). Releases use the
**MSVCRT** llvm-mingw variant (`llvm-mingw-20241119-msvcrt-*`), the same one `data/kit/kit.ini` downloads on the
player's PC, so everything runs on Windows 7 SP1 / 8.1 / 10 / 11 without the UCRT update; check a build with
`llvm-readobj --coff-imports` (no `api-ms-win-crt-*` DLLs).

### Windows release (launcher + build kit)
```sh
tools/make_public_release.sh <out_dir> /opt/llvm-mingw
```

## Linux
Everything builds natively (launcher, port, tools, tests). SDL2 is fetched and linked statically (2.32.10 on Linux,
since 2.30.9 no longer builds against current PipeWire headers); it loads X11/Wayland/PipeWire/PulseAudio at run time.
```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build && (cd build && ctest --output-on-failure)
```
Tools and tests only: add `-DSSNE_BUILD_LAUNCHER=OFF -DSSNE_BUILD_PORT=OFF`.

### Linux release (launcher + build kit)
```sh
tools/make_linux_release.sh <out_dir>
```
Same layout as the Windows release, but `build-kit/kit.ini` (from `data/kit/kit-linux.ini.in`) names the system's own
C++ compiler instead of a download: `compiler = clang++` (or `g++`) is looked up in `PATH`. The script uses clang++
if installed, else g++; set `CXX`/`CC` to choose. The launcher's Install Game then works exactly as on Windows.

## Recompiling the game
1. Install your disc with the launcher, or run `gctool extract game.iso game_data`.
2. Translate the executable:
   ```sh
   ssne-recomp game_data/sys/main.dol recompiled --map data/symbols/G3BE9G_r1.sym
   ```
   Expected output for USA Rev 1: `functions: 7919, jump tables: 23`, `translated 489232 instructions, 23 unimplemented`.
   The 23 "unimplemented" entries are zero words in padding and are never executed.
   Add `--no-comments` for smaller output (~25 MB instead of ~60 MB).
3. Reconfigure with `-DSSNE_RECOMPILED_DIR=<abs path>/recompiled` and build. This produces:
   * `SillyReEncounter-Game` (target `ssne-port`): the game window
   * `ssne-headless`: runs the game code without a window (`--data game_data --seconds 10`); `--press A@8,START@30` scripts
     controller input, `--dump-frames DIR` saves frames, `--wav out.wav` records the audio output,
     `--gpu --scale N` renders with the GPU renderer through surfaceless EGL (Linux/Mesa) for testing

Compiling the generated code takes a minute or two (32 translation units). Use `/bigobj` on MSVC; the CMake file already sets it.

## Tests
`tests/make_test_iso.py` builds synthetic discs, including a hand-assembled PowerPC program. CTest then checks that:
the verifier accepts/rejects the right images, extraction works, and the **recompiled** test program computes correct
results (bl/blr, bdnz, rlwinm incl. wrap masks, CR logic, floats, bctrl, switch jump tables, paired singles).
