# Roadmap

The approach follows N64Recomp/Zelda64Recomp: recompile the CPU code statically, and replace the console's OS and
hardware with native code, mostly at the **SDK-function level** (HLE) instead of emulating registers.

## Milestone 0 — Foundation ✅ (this drop)
- [x] Launcher: pick/drag disc image → verify (magic, game ID, DOL integrity, SHA-1) → install `game_data/` → launch
- [x] Disc library + `gctool`
- [x] Recompiler: function discovery, jump tables, integer/FPU/paired-single/branch/CR translation
- [x] Runtime: guest RAM, big-endian access, locked cache, MMIO stubs, GQR quantization, hook table
- [x] Boots to game `main()`
- [x] Tests on synthetic discs, Windows cross-build

## Milestone 1 — Boot to the first frame ✅
- [x] **Name the SDK**: `ssne-symbols` (Dolphin signatures + decomp reference layouts + strings) grows
      `data/symbols/G3BE9G_r1.sym` from 4 to 608 names. `OSReport` is native now, so the game's boot log is visible.
- [x] **Interrupt controller**: pending sources delivered at safe points (function entries + loop back-edges) when
      MSR[EE] is set, through the SDK's own handler table, so `__ARHandler`, `__DVDInterruptHandler`, VI and PE handlers run unmodified
- [x] **ARAM**: 16 MB buffer + DSP/ARAM DMA registers (`ARInit`, `__ARChecksize`, ARQ all run natively)
- [x] **Threads**: native `OSCreateThread`/`Resume`/`Suspend`/`Sleep`/`Wakeup`/`Yield`/`Exit`/`Cancel`, one host thread per
      guest thread (64 MB stacks), single-baton priority scheduler like the Gekko's
- [x] **Alarms**: `OSSetAlarm`/`OSSetPeriodicAlarm`/`OSCancelAlarm` on a host timer
- [x] **DVD**: DI command emulation (read, read ID, inquiry, seek, error, audio status) served from `game_data/` via the FST
- [x] **VI**: 59.94 Hz retrace interrupts; **AI**: streaming sample counter (needed by `__AI_SRC_INIT`)
- [x] **DSP**: mailbox stand-in for the boot ROM + AX microcode protocol
- [x] **GX command processor**: parses the full FIFO stream (CP/XF/BP loads, primitives with VCD/VAT vertex sizes,
      display lists), PE draw-done/token interrupts, CP idle status. The game runs its main loop at ~30 fps.
- [x] **GX renderer (software)**: vertex decode (all formats, indexed), XF matrices/lighting/texgen, all texture
      formats + TLUT/preload, full TEV (16 stages, konst, swaps, compare modes), alpha test, z, blending/logic ops,
      scissor, EFB→texture copies (incl. render-to-texture post-processing) and display copies. Multithreaded
      (interleaved row stripes). Front end, Bink FMVs, and gameplay in the first level render correctly.
- [x] **SI controller** emulation; port maps keyboard/mouse/SDL gamepads to controller 1
- [x] **GPU renderer** (OpenGL 3.3, default): the guest thread records each frame (batched draws with a TEV/PE
      snapshot, clears, EFB copies); the render thread executes it into an N× EFB (1–8×, F6/F7). TEV configs compile
      to GLSL with the same integer math as the software TEV; blending incl. dual-source for constant alpha, logic
      ops, GX near/far clipping via clip distances; EFB→texture copies stay on the GPU at internal resolution
      (reinterpreted bit-exactly when sampled as another format); mipmaps + GX min filters. The software renderer
      remains the reference (`--renderer software`) and the automatic fallback.
- [x] Lines/points: expanded to quads (SU_LPSIZE widths in 1/6 px, texcoord offsets); used for pickup sparkles/particles
- [x] Fog (BP 0xEE-0xF2): all fsel curves, perspective + ortho, SW and GPU (range adjust not emulated; unused in level 1)
- [x] Indirect texturing: indirect lookups (IREF/IND_SS), IND_CMD bias/format/matrices (static, dynamic S/T),
      wrapping, add-previous, alpha bump; SW and GPU (same fixed-point math, after Dolphin)

## Milestone 2 — Playable
- [x] **PAD** → SDL_GameController + keyboard/mouse bindings (mouse-look drives the C-stick)
- [x] **AX/DSP audio** HLE: command lists, voice parameter blocks, DSP-ADPCM/PCM8/PCM16 via the sample
      accelerator (loops, streams), sample-rate conversion, volume envelope, low-pass filter, main/auxA/auxB buses
      with ramps, aux effects round-trip, compressor. **AI DMA** plays the mix in real time → SDL audio (port) or
      `--wav` (headless). Music (`StreamData.dat` streams), sound effects and Bink FMV audio all play.
- [x] Voice resampling: linear (src type 1) as on hardware, 4-tap polyphase (src type 0) with generated
      windowed-sinc tables for coef select 8K/12K/16K (the DSP ROM tables aren't available), and an optional
      high-quality 64-tap band-limited resampler (default; F12 > Audio > Resampling)
- [x] **Bink FMVs**: the game's own recompiled Bink decoder handles video and audio
- [x] **Memory card**: EXI bus emulation with a slot-A card device (flash command set: read array, sector
      erase, page program, status, IDs, interrupts) backed by a Dolphin-compatible raw image
      (`saves/MemoryCardA.USA.raw` next to the exe, 251 blocks, pre-formatted for this console). The SDK's CARD
      library runs unmodified. IPL device: SRAM (English, stereo, flash IDs) + RTC; the timebase starts at the
      RTC date so saves get real timestamps.
- [x] Audio: AI DMA waits for the game's buffer handoff (no stale-buffer replays when the game thread is busy)
- [x] Settings menu in the port (F12 / pad Back): graphics (resolution, aspect, frame limit, vsync, fullscreen,
      renderer), audio, keyboard/mouse rebinding (2 per control) + mouse options, gamepad rebinding + deadzone,
      look sensitivity, trigger click point, swap sticks, Nintendo layout. Pauses the game; saved to settings.ini

## Milestone 3 — Enhancements
- [x] High internal resolution (GPU renderer, up to 8×)
- [x] **Widescreen** (16:9 / 21:9, F8): native `C_MTXPerspective`/`C_MTXLightPerspective` widen the gameplay camera's
      horizontal FOV (gameplay detected by the first-person weapon projection; menus/cutscenes stay 4:3, pillarboxed);
      the GPU EFB is rendered anamorphically wider; 2D HUD draws keep 4:3 proportions and are re-anchored to the
      nearest screen edge. No edge culling seen in the first level.
- [x] 60 fps+: the game steps by measured real time (median of 5 frames), so no tick/render split is needed.
      Frame limit (F10 / `--fps`): 60 = original VI-synced pacing, 120/144 = host-paced cap, unlimited = vsync
- [x] Mouse aim: direct 1:1 view rotation (hook on the player look update 800BFCA4), settings toggle + sensitivity
- [x] FOV slider (45-110 deg vertical): scene-camera projections (8014C690/6B8/6D8 + light 8014C6FC) get their
      tangent scaled in gameplay; the weapon projection (8014CDF0) and menus are untouched
- [x] Performance pass: automatic internal resolution (GPU-timed), early-Z friendly depth snapping, GL state and
      uniform caching, on-disk shader cache, display-synced VI, no busy main loop, faster vertex decoder, GPU frame
      capture/replay tool for pixel-exact regression tests (see FINDINGS "Performance work")
- [x] Anti-aliasing: FXAA, MSAA 2x/4x/8x (resolved at every EFB read), supersampling with filtered downscale
- [x] Anisotropic filtering 2x-16x (3D surface textures with mipmaps; off is pixel-identical)
- [x] Graphics presets, music/SFX volume, monitor choice + window memory, PNG screenshots, compact info bar
- [x] Save states (5 slots, F5/F9, thumbnails)
- [x] Skip intros, pause in the background, Xbox gamepad layout + Xbox/PlayStation button prompts
- [x] Windows 7 SP1 / 8.1 support (MSVCRT toolchain everywhere, TLS 1.2 for downloads)
- [x] Jump on Space (game jumps with analog L), mouse wheel bindings, Exit button, safer exit
- [x] Fixed down-left stick flicker (game rescaled the stick in place; repeated passes at PC frame rates flipped it)
- [x] Controller rumble (game motor commands from the SI output word -> SDL gamepad rumble, strength setting)
- [ ] Texture replacement hooks
- [x] Renamed to **Silly ReEncounter** (`SillyReEncounter.exe` launcher, `SillyReEncounter-Game.exe` game; briefly
      "Serious Re Encounter" before that, whose game exe name the launcher still accepts);
      launcher restyled after the game's menus (animated blue backdrop, chunky gradient lettering, bouncing
      selection, prompt bar; Erica One + Outfit fonts, OFL, embedded), keyboard navigation
- [x] Launcher: native **RVZ / WIA / GCZ / CISO** support with built-in decoders (zstd, LZMA, LZMA2, bzip2, zlib,
      purge; RVZ junk-data regeneration), so users don't need to convert
- [x] Public release model: players build the game on their own PC. The launcher downloads LLVM-MinGW from its
      official release, runs ssne-recomp on their main.dol, compiles and links with the prebuilt port (build kit).
      Verified on Linux with the cross toolchain: the result matches the private build (same .text size).
      Project code released into the public domain (Unlicense)
- [x] PAL (G3BP9G, FINAL.113): symbol map derived by function matching, per-version hook addresses, launcher/kit
      support, per-region memory card (see FINDINGS "PAL version")
- [ ] USA Rev 0 support; known-good SHA-1s from Redump for both releases

## Notes and risks
* **Distribution model.** Generated code is derived from the game. Like other recomps, the release binary will
  either embed recompiled code (requiring legal review) or ship the recompiler and a compiler for local builds.
  Decide before public release.
* **Floating point.** Gekko `fmadds` rounding and `frsqrte`/`fres` estimates differ slightly from IEEE on x86.
  This is fine for rendering but may matter for physics determinism.
* **Self-modifying code / overlays.** None detected (no REL modules on disc), which makes static recompilation a good fit.
