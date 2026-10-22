# Silly ReEncounter — a PC recompilation of Serious Sam: Next Encounter

A static recompilation of the GameCube version of *Serious Sam: Next Encounter* (Climax, 2004) to native PC code.
The PowerPC game executable is translated to C++ ahead of time, then compiled into a Windows executable. Game
assets are read at runtime from **your own disc image**. None of the game's code or data is included in this repository.

> **Status: early foundation.** The launcher, disc verification/installation, and the recompiler work.
> The recompiled game **runs from boot through the menus, FMVs and into gameplay**, rendered by a software GX
> renderer, with keyboard/mouse/gamepad input, **sound** (AX DSP audio done natively) and **memory card saves**
> (Dolphin-compatible card image), rendered on the **GPU at up to 8× resolution** (software renderer kept as
> reference/fallback). Next: widescreen. See [docs/ROADMAP.md](docs/ROADMAP.md).

## What's here

| Component | Path | Status |
|---|---|---|
| **Launcher** (`SillyReEncounter.exe`) | `launcher/` | Asks for your ISO, verifies it, installs the game data, launches the port |
| Disc library (ISO/GCM, FST, DOL, SHA-1, verify) | `libgc/` | Done, tested |
| **Recompiler** (`ssne-recomp.exe`) | `recomp/` | Translates 100% of the game's 489,232 instructions (7,919 functions, 23 switch tables) |
| Runtime (CPU, memory, MMIO, threads, GX renderer, AX audio, SI input) | `runtime/` | Runs the game from boot to gameplay |
| PC port (`SillyReEncounter-Game.exe`) | `port/` | SDL2 window, audio output, keyboard/mouse/gamepad, F12 settings menu (settings.ini), F1 stats overlay |
| `ssne-symbols.exe` | `recomp/` | Names functions (Dolphin signatures + decomp reference layouts): 608 named, see [docs/SYMBOLS.md](docs/SYMBOLS.md) |
| `gctool.exe` | `tools/` | Command-line `info / verify / extract / dol` (any supported image format) |
| Tests | `tests/` | Synthetic discs + recompiled-CPU test (no game data needed) |

## Using the launcher

1. Unpack the public release (launcher, `known_dumps.txt`, `build-kit/`) into a folder.
2. Run **SillyReEncounter.exe** → **Select Disc** (or drag your disc onto the window).
   * Accepted: plain **`.iso`/`.gcm`** dumps and Dolphin's compressed formats **`.rvz`, `.wia`, `.gcz`, `.ciso`**
     (all compression methods: zstd, LZMA, LZMA2, bzip2, zlib, purge, none). No conversion needed.
   * RVZ and WIA store the complete disc, so they hash the same as the original ISO. CISO/GCZ files are often
     *scrubbed* (padding removed); they install and play the same but show as "unverified".
3. The launcher checks the disc header (game ID **G3BE9G** for USA), validates `main.dol`, and computes the SHA-1.
   * **Verified**: the hash matches an entry in `known_dumps.txt`.
   * **Recognized (unverified dump)**: it's the right game, but the hash isn't listed (other revision, or a modified dump). It is still allowed.
   * **Wrong game / Not a valid disc**: rejected.
4. **Install Game** extracts the disc into `game_data/` next to the launcher (about 1.3 GB), then builds the game
   **on the player's PC**: it downloads LLVM-MinGW 20241119 once from its official GitHub release into
   `build-kit/toolchain/`, translates `game_data/sys/main.dol` with `ssne-recomp`, compiles the generated C++ in
   parallel and links it with the prebuilt port/runtime libraries in `build-kit/lib/` (described by
   `build-kit/kit.ini`). The result is `SillyReEncounter-Game.exe`. No game code is ever distributed.
5. **Play** starts `SillyReEncounter-Game.exe --data game_data`.

Making a release: `tools/make_public_release.sh <out_dir> <llvm-mingw dir>` builds the launcher, gctool, recompiler
and the port libraries (always without game code), assembles the build kit, licenses and this source tree, and
fails if any generated game function ends up in the shipped binaries.

## Building

See [docs/BUILDING.md](docs/BUILDING.md). Short version (Windows, Visual Studio 2022 or LLVM-MinGW):

```bat
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

To build the port **with game code**, run the recompiler on your own `main.dol` and point CMake at the output:

```bat
build\bin\ssne-recomp.exe game_data\sys\main.dol recompiled --map data\symbols\G3BE9G_r1.sym
cmake -B build -DSSNE_RECOMPILED_DIR=%CD%\recompiled
cmake --build build --config Release
```

## Legal

The project's own code is released into the public domain ([The Unlicense](LICENSE.txt)). Third-party
components keep their licenses (SDL2: zlib, Dear ImGui: MIT, Erica One / Outfit fonts: SIL OFL 1.1).

This project contains no copyrighted game code or assets. You need your own copy of the game, ideally dumped from
a disc you own. Generated `recompiled/` sources are derived from the game binary, so **do not commit or
distribute them**. `.gitignore` excludes them.
