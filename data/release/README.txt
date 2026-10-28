SILLY REENCOUNTER
Unofficial fan project: play Serious Sam: Next Encounter (Nintendo GameCube, 2004) as a native PC program,
built on your own PC from your own copy of the game.

HOW IT WORKS
  This download contains NO game code and NO game data. You supply your own disc image, and the launcher turns it
  into a PC program on your computer:
  1. Select Disc: pick your disc image (.iso, .gcm, or Dolphin .rvz / .wia / .gcz / .ciso), or drop it on the window.
     The launcher checks that it is Serious Sam: Next Encounter (USA or Europe/PAL) and whether it matches a known
     good dump.
  2. Install Game:
       - copies the game's files out of your disc image into the game_data folder (about 1.3 GB),
       - downloads the free LLVM-MinGW compiler (about 170 MB, only the first time) from its official GitHub
         release into build-kit\toolchain,
       - translates the game's program (main.dol) from your disc into C++ and compiles it, then links it with the
         PC port in build-kit\lib. This makes SillyReEncounter-Game.exe. It takes a few minutes.
  3. Play.
  Everything made from your disc stays on your PC. Do not share game_data or SillyReEncounter-Game.exe: they are
  made from the game and are only for you.

REQUIREMENTS
  64-bit Windows 7 SP1, 8.1, 10 or 11; a GPU with OpenGL 3.3 (with an up-to-date graphics driver); about 3 GB of
  free disk space; an internet connection the first time you install (for the compiler download only).
  Windows 7: if the compiler download fails, install the TLS 1.2 update for Windows (KB3140245) or download the
  file named in the error message with a web browser and put it in build-kit\downloads, then build again.
  Your own legally obtained Serious Sam: Next Encounter for GameCube, dumped from a disc you own:
    USA (G3BE9G, Rev 1) or Europe/PAL (G3BP9G; English, French or German, chosen in the game at startup).
  Do not ask for, share or download disc images.

FILES
  SillyReEncounter.exe   the launcher
  gctool.exe               command-line disc tool: gctool info|verify|extract <image> [...]
  known_dumps.txt          SHA-1 checksums of known good dumps (checksums only)
  build-kit\               what the launcher needs to build the game on your PC:
      ssne-recomp.exe        translator from the GameCube program to C++
      G3BE9G_r1.sym          function names for the translator
      include\, lib\         the PC port and its runtime, prebuilt (no game code)
      kit.ini                build settings (compiler download address, compiler flags)
      work\logs\             build logs, if something goes wrong
  CHANGELOG.txt            what changed since the first public release
  LICENSE.txt              public-domain dedication (The Unlicense) for this project's code
  THIRD-PARTY-NOTICES.txt  and the licenses folder: licenses of the libraries and fonts used
  source\                  complete source code (see source\README.md and source\docs\BUILDING.md)

  The launcher saves launcher.ini next to itself. It connects to the internet only to download the compiler from
  github.com, and only when you choose Install Game.

IN-GAME
  Settings are saved to settings.ini and memory card saves to saves\MemoryCardA.USA.raw (Europe/PAL:
  MemoryCardA.EUR.raw), next to the game program. The PAL version runs at 60 Hz like the USA one.
  Compiled graphics shaders are kept in the shadercache folder so they don't have to be rebuilt (a short hitch)
  the next time; deleting it is harmless.

PERFORMANCE
  - Automatic resolution (F12 > Graphics, on by default): the game picks the internal resolution that fits your
    window and lowers it while your GPU can't keep up (and raises it again when there is headroom). Laptops and
    integrated graphics stay at full speed. Turn it off (or press F6/F7) to choose the resolution yourself.
  - Anti-aliasing (F12 > Graphics, off by default) smooths jagged edges: FXAA (almost free, a little softer),
    MSAA 2x/4x/8x (sharp, costs GPU time) or Supersampling (twice the resolution, scaled down; best quality,
    heaviest - automatic resolution still lowers it if your GPU can't keep up).
  - Rumble (F12 > Gamepad, on by default): the game's controller vibration on Xbox, PlayStation, Switch Pro and
    most other gamepads, with a strength slider and a Test button.
  - Sync game to display (on by default): with vertical sync, the 60 fps limit and a 60 Hz screen, the game ticks
    once per screen refresh, so frames are never shown twice (no periodic micro-stutter).
  - "Software renderer (CPU)" at the top of the screen means the GPU renderer could not start (OpenGL 3.3 missing):
    update your graphics driver. The CPU renderer is much slower.
  - Updating: when a new launcher comes with engine improvements, choose Update Game in the launcher once to
    rebuild the game on your PC (a few minutes, nothing is downloaded).

DEFAULT CONTROLS (keyboard & mouse; change them in F12 > Keyboard & Mouse)
  W A S D / arrows ... move            Mouse ............ aim (click the game window to capture the mouse)
  Left mouse / Ctrl .. fire (R)        Space ............ jump (the GameCube's L trigger)
  E .................. use, enter / exit vehicles (A)    R .. reload / switch ammo type (B)
  Mouse wheel ........ next / previous weapon (X / Y)    Right mouse .. next weapon; hold: weapon wheel (X)
  Tab ................ NETRICSA (Z)    Esc / Enter ...... pause (Start)
  Menus, the pause screen and cutscenes: the mouse wheel moves the selection, left click / Space selects and skips
  cutscenes, right click / Esc goes back (Esc also closes the pause menu).
  Gamepad (Xbox layout, the default): A jump (also selects in menus and skips cutscenes), X use, B back,
  RT fire, LB / RB switch weapons, Y = the GameCube's Z, Start pause, Back = settings menu.
  F12 > Gamepad has a "GameCube layout" button (A use/select, LT jump, RT fire) and the on-screen button style:
  the game's button hints show Xbox or PlayStation buttons to match your controller.

SAVE STATES
  Save the game anywhere (F5) and come back to that exact moment later (F9), on top of the game's own memory card
  saves. States are kept in states\<disc ID>\ next to the game. Loading a state also puts the memory card back
  as it was when the state was saved (the card from before loading is kept as a .bak file). A state is taken at
  the next frame; while a level is loading it waits a moment. States work from the main menu on, not on the boot
  screens, and only with the same game release (an engine update can make older states unusable).

GRAPHICS PRESETS AND AUDIO
  F12 > Graphics > Presets: Low (original resolution, for integrated graphics), Medium, High, Ultra. They set the
  resolution, anti-aliasing and texture filtering in one click; everything stays adjustable afterwards.
  F12 > Audio: separate volume for music & videos and for sound effects (on top of the main volume).
  The window remembers its size, position and maximized state.

GENERAL OPTIONS (top of the F12 menu)
  Skip intros (on): starts at the main menu; the memory card check (and language choice on PAL) still shows.
  Pause in the background (on): Alt+Tab or clicking another window pauses and silences the game.
  Exit: F12 > "Exit game" (or close the window / Alt+F4).

HOTKEYS (work any time in the game window; also listed in F12 > Hotkeys)
  F12 / pad Back ..... settings menu (Esc closes it)
  F1 ................. log and statistics window
  Shift+F1 ........... info bar at the top: full / frame rate only / hidden (also in F12 > Graphics)
  F2 ................. release the mouse (click the game window to capture it again)
  F3 ................. mute / unmute
  F5 ................. save state (selected slot; F12 > Save states has 5 slots with pictures)
  F9 ................. load state (selected slot)
  Shift+F5 ........... next save state slot
  F4 / Shift+F4 ...... volume down / up (10% steps)
  F6 / F7 ............ internal resolution down / up (GPU renderer; turns off automatic resolution)
  F8 ................. aspect ratio: 16:9 / 21:9 / 4:3
  Shift+F9 ........... capture the next frame to the debug folder (GPU renderer, for bug reports)
  F10 ................ frame rate limit: 60 (original) / 120 / 144 / unlimited
  F11 or Alt+Enter ... fullscreen (F12 > Graphics > Monitor picks the screen when you have several)
  Print Screen or Shift+F12  screenshot of the game image (PNG in the Screenshots folder next to the game)

LEGAL
  Silly ReEncounter is an unofficial, non-commercial fan project. It is not affiliated with, endorsed by or
  sponsored by Croteam, Devolver Digital, Climax Group, Global Star Software, Take-Two Interactive or Nintendo.
  "Serious Sam" and "Serious Sam: Next Encounter" are trademarks of their respective owners and are used here only
  to identify the game this software works with. Nintendo GameCube is a trademark of Nintendo.
  The project's own code is dedicated to the public domain (LICENSE.txt). It is provided "as is", without warranty
  of any kind.
