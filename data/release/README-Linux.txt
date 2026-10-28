SERIOUS SAM: NEXT ENCOUNTER NATIVE (SSNE-NATIVE) FOR LINUX
Unofficial fan project: play Serious Sam: Next Encounter (Nintendo GameCube, 2004) as a native PC program,
built on your own PC from your own copy of the game. This is the Linux build of SSNE-Native (based on Silly
ReEncounter 1.01); the game itself works exactly as described in README.txt (hotkeys, F12 menu, save states, settings).

WHAT IS DIFFERENT FROM WINDOWS
  - The launcher does not download a compiler. It builds the game with the C++ compiler installed on your system
    (named in build-kit/kit.ini, "compiler = ..."). clang++ is recommended: it compiles the generated code faster
    and with less memory than g++. Both work.
  - The game program is called SSNE-Native-Game (no .exe).
  - Select Disc uses zenity or kdialog if one is installed. Without them, drag the disc image onto the window.
  - Print Screen is usually taken by the desktop for its own screenshot tool; Shift+F12 always works in the game.

REQUIREMENTS
  64-bit x86 Linux with X11 or Wayland; a GPU with OpenGL 3.3; about 3 GB of free disk space.
  A C++20 compiler:            Arch: sudo pacman -S clang        Debian/Ubuntu: sudo apt install clang
                               Fedora: sudo dnf install clang     (or g++ / gcc-c++)
  Your own legally obtained Serious Sam: Next Encounter for GameCube (USA G3BE9G, or Europe G3BP9G), dumped from a
  disc you own. Do not ask for, share or download disc images.

HOW IT WORKS
  1. Run ./SSNE-Native from this folder (the folder must be writable: game_data, saves and settings are kept
     next to the programs).
  2. Select Disc: pick your disc image (.iso, .gcm, .rvz, .wia, .gcz, .ciso), or drop it on the window.
  3. Install Game: extracts the game's files into game_data (about 1.3 GB), translates the game's main.dol to C++
     with build-kit/ssne-recomp, compiles it with your compiler and links it with the prebuilt port in
     build-kit/lib. This makes SSNE-Native-Game. Build logs are in build-kit/work/logs.
  4. Play.
  Do not share game_data or SSNE-Native-Game: they are made from the game and are only for you.

WITHOUT THE LAUNCHER
  ./gctool extract game.iso game_data
  build-kit/ssne-recomp game_data/sys/main.dol recompiled --map build-kit/G3BE9G_r1.sym --no-comments
  then build from source/ with: cmake -B build -DSSNE_RECOMPILED_DIR=$PWD/recompiled && cmake --build build
  ./SSNE-Native-Game --data game_data
