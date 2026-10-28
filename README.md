# SSNE-Native

Play **Serious Sam: Next Encounter** (Nintendo GameCube, 2004) as a native program on **Linux** and **Windows**:
no emulator. The game's own code is translated to C++ and compiled on your PC from **your own disc image**, then
runs with a PC port that handles graphics (OpenGL), sound, keyboard, mouse and gamepads.

SSNE-Native is a fork of [Silly ReEncounter](https://github.com/takoshero96/Silly-ReEncounter) by takoshero96,
which made all of this possible. It adds a Linux version, fixes and PC-style controls (see
[What SSNE-Native adds](#what-ssne-native-adds)). The programs keep their original names (`SillyReEncounter`,
`SillyReEncounter-Game`).

**This repository contains no game code and no game data.** You need your own copy of the game.

## What you need

- **Your own disc image** of *Serious Sam: Next Encounter* for GameCube, dumped from a disc you own:
  USA (`G3BE9G`, Rev 1) or Europe/PAL (`G3BP9G`; English, French or German).
  Formats: `.iso`, `.gcm`, or Dolphin's `.rvz`, `.wia`, `.gcz`, `.ciso` (no conversion needed).
  Do not ask for, share or download disc images.
- **Linux** (64-bit x86, X11 or Wayland) or **Windows** 7 SP1 / 8.1 / 10 / 11 (64-bit).
- A GPU with **OpenGL 3.3** and an up-to-date driver, and about **3 GB** of free disk space.
- **Linux:** a C++ compiler, which builds the game on your PC. `clang` is recommended (faster), `g++` works too:
  `sudo pacman -S clang` (Arch), `sudo apt install clang` (Debian/Ubuntu), `sudo dnf install clang` (Fedora).
- **Windows:** an internet connection for the first install; the launcher downloads the free LLVM-MinGW compiler
  once (about 146 MB) from its official GitHub release.

## How to play

1. **Download the launcher package** for your system from
   [Releases](https://github.com/SashexSRB/SSNE-Native/releases) (`...-Linux-x86_64.tar.gz` or
   `...-Windows-x64.zip`) and unpack it into a folder you can write to: the game data, saves and settings are
   kept next to it. The package contains the launcher and the build kit, but no game code. To build it yourself,
   see [docs/BUILDING.md](docs/BUILDING.md).
2. **Start the launcher**: `SillyReEncounter` (Linux) or `SillyReEncounter.exe` (Windows).
3. **Select Disc**: pick your disc image, or drag it onto the launcher window. The launcher checks that it is the
   right game:
   - *Verified*: a known good dump.
   - *Recognized (unverified dump)*: the right game, but a different or modified dump (e.g. a scrubbed `.gcz` /
     `.ciso`). It still installs and plays.
   - *Wrong game / Not a valid disc*: rejected.
4. **Install Game**: copies the game's files out of the disc image into `game_data` (about 1.3 GB), then translates
   the game's program and compiles it on your PC. This takes about a minute with clang, a few minutes otherwise.
   It creates `SillyReEncounter-Game`.
5. **Play**. From then on you can also start the game directly from its folder:

   ```bash
   ./SillyReEncounter-Game --data game_data
   ```

   (Windows: `SillyReEncounter-Game.exe --data game_data`.)

After updating to a newer launcher package, choose **Update Game** in the launcher once: the game is rebuilt with
the new engine (your game data, saves and settings stay).

Everything made from your disc (`game_data`, `SillyReEncounter-Game`) is only for you: don't share it.

## Controls

**Keyboard and mouse** (change them in F12 › Keyboard & Mouse, up to three keys or buttons per control):

| Key | Action | Key | Action |
|---|---|---|---|
| W A S D / arrows | move (in vehicles: W / S drive, A / D steer) | Mouse | aim, steer vehicles (click the window to capture the mouse) |
| Left mouse / Ctrl | fire | Space | jump |
| E | use, enter / exit vehicles | R | reload, switch ammo type |
| Right mouse | sniper rifle zoom (hold) | Q | next weapon; **hold** for the weapon wheel |
| Mouse wheel | next / previous weapon | F | previous weapon |
| 1 – 9 | weapons (see below) | Tab | NETRICSA |
| Esc / Enter | pause | | |

- **Weapon wheel:** hold Q, move the mouse towards a weapon, release Q to take it.
- **Number keys:** 1 chainsaw, 2 Desert Hawk, 3 double shotgun, 4 Uzis / minigun, 5 rocket / grenade launcher,
  6 Sirian power gun / flamethrower, 7 sniper rifle, 8 cannon, 9 Serious Bomb. Press a key again for the other
  weapon of its pair; weapons you don't have yet are skipped.
- **Menus, pause screen, cutscenes:** the mouse wheel moves the selection, left click or Space selects (and skips
  cutscenes), right click or Esc goes back. Esc opens and closes the pause menu.
- The game's on-screen button hints show your keys and mouse buttons, or your controller's buttons when you play
  with a gamepad.

**Gamepad** (Xbox layout by default; PlayStation, Switch Pro and most others work): A jump (also selects in menus),
X use, B back, RT fire, LB / RB switch weapons, Y = the GameCube's Z, Start pause, Back = settings menu.
F12 › Gamepad has a "GameCube layout" button, rumble settings and the on-screen button style.

**Hotkeys** (also listed in F12 › Hotkeys):

| Key | Action |
|---|---|
| F12 | settings menu (graphics, audio, controls, save states, exit) |
| F5 / F9 | save state / load state (Shift+F5: next slot) |
| F11 or Alt+Enter | fullscreen |
| F10 | frame rate limit: 60 / 120 / 144 / unlimited |
| F8 | aspect ratio: 16:9 / 21:9 / 4:3 |
| F6 / F7 | internal resolution down / up |
| F1 | log and statistics window (Shift+F1: info bar at the top) |
| F2 | release the mouse |
| F3, F4 / Shift+F4 | mute, volume down / up |
| Print Screen or Shift+F12 | screenshot (PNG in the `Screenshots` folder) |

## Good to know

- **Where things are kept** (next to the game): `settings.ini`, memory card saves in `saves/`, save states in
  `states/`, screenshots in `Screenshots/`, and compiled shaders in `shadercache/` (safe to delete).
- **Frame rate:** gameplay on foot follows your frame rate limit (F10). Menus, cutscenes and vehicles always run
  at the original 60 fps, because their animations and the jeep's physics depend on it.
- **Slow on your PC?** F12 › Graphics › Presets › Low. Automatic resolution (on by default) already lowers the
  internal resolution when the GPU can't keep up.
- **Widescreen** (16:9 / 21:9, F8) applies in gameplay; menus and cutscenes stay 4:3 with bars on the sides,
  because the game places 3D models into 2D menu layouts.
- **Pause in the background** (on by default): switching to another window pauses and silences the game.
- **Graphics thread and GPU vertex processing:** the game's graphics commands are processed on their own CPU thread,
  in parallel with the game (like the console's GPU), and texture coordinates and lighting are computed on the GPU.
  If something looks wrong that didn't before, set `gx_thread = 0` or `gpu_vertex = 0` in `settings.ini` and restart,
  and please report it.
- **Linux:** Print Screen is usually taken by the desktop; use Shift+F12. If Select Disc doesn't open a file
  dialog, install `zenity` or `kdialog`, or drag the disc image onto the window.
- **Build problems:** the launcher shows the error, and the full logs are in `build-kit/work/logs/`.

## What SSNE-Native adds

On top of Silly ReEncounter 1.01 (details in [data/release/CHANGELOG.txt](data/release/CHANGELOG.txt)):

- **Linux version**: native launcher and game, built with the system's compiler.
- **In-engine cutscenes** show the cutscene instead of a frozen gameplay frame.
- **The jeep** keeps driving at high frame rate limits (vehicles run at 60 fps); Space no longer exits it.
- **Menu animations** no longer speed up above 60 fps.
- **Faster** (1.1): graphics processing on its own thread, texture coordinates and lighting on the GPU, a faster
  vertex path and a profile-optimized engine: about two and a half times the frame rate where the CPU is the limit.
- **PC-style controls**: classic shooter keys, WASD driving, mouse in menus, weapon wheel with the mouse, number keys for
  weapons, keyboard & mouse button hints, three bindings per control.
- Slightly lower CPU use on the game thread.

## Building

See [docs/BUILDING.md](docs/BUILDING.md).

## For developers

| Part | Path | What it does |
|---|---|---|
| Launcher (`SillyReEncounter`) | `launcher/` | Disc selection and verification, install, builds the game with the build kit, starts it |
| Disc library | `libgc/` | ISO/GCM and Dolphin formats, file system, DOL, SHA-1, verification |
| Recompiler (`ssne-recomp`) | `recomp/` | Translates the game's PowerPC `main.dol` to C++ (all 489,232 instructions, 7,919 functions) |
| Runtime | `runtime/` | CPU state, memory, hardware (GX graphics on OpenGL, AX audio, controllers, memory card), threads |
| PC port (`SillyReEncounter-Game`) | `port/` | SDL2 window, audio, input, F12 settings menu, F1 statistics |
| Tools | `tools/` | `gctool` (disc images), headless runner, GPU frame replay, release scripts |
| Tests | `tests/` | Synthetic discs and a recompiled-CPU test (no game data needed) |

Technical notes: [docs/FINDINGS.md](docs/FINDINGS.md) (how the game works and how things were found),
[docs/ROADMAP.md](docs/ROADMAP.md), [docs/SYMBOLS.md](docs/SYMBOLS.md).

## Legal

SSNE-Native, like Silly ReEncounter, is an unofficial, non-commercial fan project. It is not affiliated with,
endorsed by or sponsored by Croteam, Devolver Digital, Climax Group, Global Star Software, Take-Two Interactive or
Nintendo. "Serious Sam" and "Serious Sam: Next Encounter" are trademarks of their respective owners and are used
here only to identify the game this software works with. Nintendo GameCube is a trademark of Nintendo.

The project's own code is released into the public domain ([The Unlicense](LICENSE.txt)). Third-party components
keep their licenses (SDL2: zlib, Dear ImGui: MIT, Erica One / Outfit fonts: SIL OFL 1.1).

This project contains no copyrighted game code or assets. Files generated from your disc (`game_data/`,
`recompiled/`, the built game) are derived from the game: **do not commit or distribute them**. `.gitignore`
excludes them.
