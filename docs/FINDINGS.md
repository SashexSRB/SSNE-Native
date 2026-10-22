# Technical findings — Serious Sam: Next Encounter (GameCube, USA Rev 1)

Everything below comes from the disc's system files (boot.bin, bi2.bin, apploader.img, fst.bin, main.dol).

## Disc
| Field | Value |
|---|---|
| Game ID | **G3BE9G** (G = GameCube, 3B = title, E = USA, 9G = Global Star / Take-Two) |
| Disc revision | 1 |
| Boot title | `Serious Sam` |
| Game build string | `Build FINAL.107  (Jan 22 2004 @ 12:04:50)` |
| Dolphin SDK | OS release build `Sep  5 2002 05:32:39 (0x2301)`, apploader dated 2002/09/05 |
| main.dol offset | 0x0001EC00 |
| FST | 0x0028E500, 3,554 bytes, 135 entries |

## main.dol (2,554,080 bytes)
| Section | Address | Size |
|---|---|---|
| T0 (init) | 80003100 | 0x3A0 |
| T1 (.text) | 800034A0 | 0x1DD8A0 (≈1.87 MB, 489k instructions) |
| D0–D4 | 801E0D40 – 802DDB40 | ctors/dtors/rodata/data/sdata |
| BSS | 8026F6A0 | 0x6D124 |
| Entry | 80003100 (`__start`) | |

Source paths in the binary (`C:/SeriousSam/src/game/system/Engine/engine.cpp`, `xforms.cpp`) and class names
(`cWickerManAI`, `CcSeriousJeep`, `CcSeriousCombine`) show a C++ engine built with CodeWarrior. The game code
starts after the C runtime. Dolphin SDK libraries sit near the end of .text (≈ 801B0000–801E0000).

## File formats on disc (targets for native loaders)
| Ext | Contents | Notes |
|---|---|---|
| `.ssw` | Levels (`Levels/`, 60 files, 4.5–8.5 MB each) | Climax world format; loaded by the engine itself. No PC loader needed if the recompiled code reads it |
| `.ssg` | Packed resources (`Data.ssg`, `FrontEnd.ssg`, multiplayer skins) | Same |
| `.sst` | Textures (loading screens, title) | GX texture formats (CMPR/I8/RGB5A3…) → decoded by the GX layer |
| `.tdb` | Localized text (English USA/EUR, French, German) | |
| `.cfg` | Plain-text tuning files (`base.cfg`, `MinionStats.cfg`, …) | Moddable |
| `.spd/.spt` | Sound bank (`sfxbank1`), DSP-ADPCM | Needs DSP-ADPCM decoding (AX HLE) |
| `StreamData.dat` | 111 MB streamed audio | Streamed via ARAM/DVD |
| `.bik` | 35 Bink videos (FMVs, intro logos) | Bink 1, decodable with FFmpeg's `bink` decoder |
| `opening.bnr` | GameCube banner | Can serve as the launcher icon |

## Recompilation results
* Function discovery: entry + 3,978 `bl` targets + 257 code-address constants + 3,658 data pointers (vtables,
  callbacks) → **7,919 functions**. **23 of 23** `bctr` switch jump tables resolved.
* **100%** of executable instructions translated. The 23 "unimplemented" words are zero padding.
* The generated C++ (~25 MB) compiles cleanly with GCC 13 and Clang 19.

## Symbols
608 functions named automatically and by hand. See [SYMBOLS.md](SYMBOLS.md). SDK build confirmed from the game's own
log: every library reports `release build: Sep  5 2002 (0x2301)`. `OSMutex`/`OSMessage` aren't linked, and neither is `DVDOpen`.

## Boot log (headless runner, with the native OSReport)
```
<< Dolphin SDK - EXI ... SI ... >>
Dolphin OS $Revision: 58 $.  Kernel built : Sep  5 2002 05:32:39
Console Type : Retail 1   Memory 24 MB   Arena : 0x802edb80 - 0x81780000
<< Dolphin SDK - OS ... DVD ... >>
Startup Memory Free 21570688
<< Dolphin SDK - VI ... PAD ... >>
Microallocator handling 479k
<< Dolphin SDK - AR ... >>            <- stops in ARInit/__ARChecksize (ARAM size probe)
```

## Current state: playable-looking first level (software renderer)
With the software GX renderer and SI controller emulation, the recompiled game shows everything from boot to gameplay:
"Licensed by Nintendo" → memory-card check (no card: "Continue Without Save") → Global Star/Climax/Croteam logos and
Bink FMVs (decoded by the game's own recompiled Bink code) → main menu with 3D Sam → player/level/difficulty select →
intro cutscene with subtitles → **gameplay in "Incident at the Arena"** (Colosseum, HUD, weapon).

Rendering facts learned from the game:
* Each gameplay frame: scene → EFB-to-texture copy (RGB565) + clear → half-res GB8 copy → scene texture redrawn as a
  full-screen quad → more geometry → HUD → display copy. ~13k triangles, ~0.94 M pixels written per frame.
* That redraw uses **manual texcoord scaling** (`GXSetTexCoordScaleManually`): texcoords are in texels and the SU scale
  registers (BP 0x30-0x3F) are 1. Honoring those registers fixed a flat-color screen.
* Menus navigate with the **analog stick**, not the D-pad.
* Performance (see also "Performance work" below): game code alone runs at a locked 60 fps; the software rasterizer costs ~110 ms/frame single-threaded
  on the build sandbox (≈87 ns/pixel). **On the user's Windows PC the game build runs at a locked 60 fps** in gameplay.
* "Failed to find file textures/… / Models/…" log lines are normal: the game probes for loose files that aren't on
  the disc and falls back to the copies packed in its .ssg archives.

## Audio (AX HLE)
* AX microcode: CRC **0x07F88145** (HashEctor over the 0x1F20-byte ucode at 0x802693C0), the standard GameCube AX
  whose parameter block includes the low-pass filter. Every 5 ms the AX library sends `0xBABE0000|size` + a command
  list address; the DSP replies `DSP_YIELD` after writing 160 stereo frames (R,L big-endian s16) to RAM.
* AI DMA: 640-byte buffers (5 ms at 32 kHz), double-buffered; the next buffer is programmed from the AIDINT handler.
* Voice types seen: menu/gameplay **music** = 6 DSP-ADPCM stream voices (`is_stream` = 1, ~1 s ARAM ring buffers
  refilled from `StreamData.dat`, no SRC, mixed as 3 L/R pairs with envelope cross-fades); **Bink FMV audio** = 2 PCM16
  looping voices at ratio 0x1607B (44.1 → 32 kHz); **sound effects** = DSP-ADPCM one-shots from `sfxbank1.spd` in ARAM.
* The IPL SRAM isn't emulated, which made `OSGetSoundMode` report mono (every voice mixed to the center). It is
  replaced natively and reports stereo.

## GPU renderer
* Per gameplay frame: ~50 batches after merging identical states, 2 EFB→texture copies (RGB565 scene copy
  redrawn as a full-screen quad; half-res GB8 copy), ~17 distinct TEV programs over a whole session.
* The scene-copy quad is drawn **exactly on the near plane** (clip z = −w) with depth test LEQUAL and no update,
  against scene geometry that was itself clipped by the near plane. The software clipper keeps on-plane geometry
  (it only clips distances < 0) and the depths tie. On the GPU, clip distances need a small tolerance and fragment
  depth is snapped to the viewport's near/far plane when within a few 24-bit ulps, so ties resolve like hardware.
* Build 6 showed black intro/gameplay on NVIDIA (RTX 4060) but not on Mesa: the internal copy/reinterpretation
  passes ran with `GL_CLIP_DISTANCE0-2` enabled while their shaders don't write `gl_ClipDistance` (undefined
  behavior: NVIDIA culled the passes, so every EFB copy the game redraws its scene from came out empty).
* Black gameplay/cutscenes on NVIDIA (builds 6-9) turned out to be presentation, not rendering (found with the F9
  capture): the EFB alpha is 0 in those scenes and the GPU output kept it, so the port's alpha-blended image was
  invisible. The software path always forced alpha to 255. The display copy output is now opaque.
  **Confirmed fixed on the user's RTX 4060 (build 10).**
* Verification method: `ssne-headless --gpu` runs the GL path headlessly (surfaceless EGL / Mesa llvmpipe) and dumps
  frames; `SSNE_GPU_DUMP_FRAME=n` dumps the GPU EFB at every copy/clear of frame n with a draw log, to compare with
  the software renderer's `SSNE_GX_TRACE_FRAME` dumps.

## Widescreen
* HUD anchoring (fixed after build 15): the game draws 2D text one glyph per draw, so anchoring each draw to
  the nearest screen edge by position scattered letters of pause-menu and message text. 2D draws are now scaled
  around the center (original 4:3 layout) unless they touch a screen edge (|x| >= 0.97), which stay on that edge.
  Debug: `SSNE_GX_TRACE_AT=<seconds>` traces the next frame (software renderer), now with ORTHO/persp and x/y range.
* Every camera projection (menus and gameplay) comes from one camera function (`8014C090`) calling the SDK's
  `C_MTXPerspective` (801BA874) three times plus `C_MTXLightPerspective` (801BA5EC) with an aspect computed from the
  viewport (1.429 in game, fovY 42° in menus, 63° in gameplay). The first-person weapon uses a separate projection
  from `8014CDF0`, which only runs during gameplay; that is the gameplay detector.
* Widening the aspect in the SDK function is enough: the engine's culling follows the matrix (objects at the new
  edges render at 21:9).
* Front-end screens place 3D models inside 2D layouts, so they must stay 4:3.

## Player look (mouse aim)
* Found by RAM snapshots while turning (headless `--dump-ram`, `CLEFT/CRIGHT/CUP/CDOWN`): the player object
  (global pointer at 802D8948 -> e.g. 8133C6A0) holds view yaw at +0x290 (radians, fmod 2 pi, + = right) and pitch
  at +0x294 (radians, + = down; limits [+0x194]->+0x24 and [+0x198]->+0x24 in degrees, +/-75 in level 1). The camera
  object keeps a copy (yaw +8, pitch +0xC, then its rotation matrix).
* 800BFCA4 (once per gameplay frame, r3 = player, f1/f2 = stick, f3 = dt) integrates the C-stick: yaw rate grows with
  the square of the deflection (the "acceleration" feel), pitch has an invert flag ([+0x264]->+0x1C) and an
  optional auto-level ([+0x268]->+0x1C). Mouse aim runs it unchanged, then adds the mouse rotation and re-clamps.
  Headless `--aim T:YAW:PITCH` verified exact yaw deltas and pitch clamping.

## Frame pacing
* The main loop (80155638) gets its time step from 8015544C: OSGetTick delta in seconds, replaced by 1/59.94 when
  < 0 or > 1.5 s, then the median of the last 5 values. Game logic is therefore frame-rate independent.
* CcEngine::Flip (801183F0) calls VIWaitForRetrace (8011855C) only if no retrace happened since the previous flip
  (flag set by the VI post-retrace callback 8011731C, which also reads the pads): a 59.94 fps cap.
* Frame limit: the Flip wait is skipped when the limit is not 60, and frames are paced on the host clock at each
  display copy. Headless test (`--norender 97 --fps N`, walking forward 4 s): 60, 141 and ~1350 fps all end at the
  same spot, so movement scales with real time.
* Debug: `SSNE_TRACE_CALLS=addr,...` counts calls per second and prints guest backtraces (pass-through hooks).

## Indirect texturing
* Every gameplay frame the scene copy is redrawn as a full-screen quad with one indirect stage (IREF map 1 /
  coord 1, IND_CMD 0x000270: 8-bit, bias S/T/U, matrix 0 = swap s/t at 0.25). The indirect map is a 320x224 IA8
  EFB copy that is flat 127 (no distortion) until effects draw into it: a screen-space distortion buffer.
* Heat haze/refraction sprites (e.g. minigun muzzle while firing) use a 128x128 CMPR offset texture with matrix
  scale 12 (offsets of a few texels).
* Headless runner: `--press NAME@t:dur` holds an input for `dur` seconds; `CLEFT/CRIGHT` drive the C-stick and R/L
  also set the analog triggers (needed to fire). `SSNE_TRACE_IND=1` logs indirect state per frame, `SSNE_NO_IND=1`
  disables indirect texturing for A/B comparisons.

## Audio resampling
* Voices seen in play (`SSNE_TRACE_AX=1` prints src type and coef select): sound effects are DSP-ADPCM at 32 kHz with
  src type 2 (no conversion); music streams are PCM16 at 44.1 kHz (ratio 0x1607B) with src type 1 (linear). Src type
  0 (polyphase) is rare (a couple of ratio-1.0 voices). So on hardware the music is linearly interpolated from
  44.1 to 32 kHz: roughly -2 dB at 13-15 kHz and aliasing near 16 kHz.
* High-quality mode (default, `SSNE_AUDIO_ORIGINAL=1` / headless `--audio-original` turns it off): 64-tap Kaiser
  sinc (beta 7), 256 phases with linear phase interpolation, cutoff `0.5*min(1,1/ratio) - 0.034` cycles/input
  sample: flat to ~13.5 kHz, -45 dB at 16 kHz for the 44.1 kHz music. The host keeps 128 samples of history per
  parameter block and a 32-sample delay (1 ms) for lookahead; PB_LAST still holds the newest four samples and a
  mismatch (voice restarted) resets the history. Measured on the title music: +2 dB at 13-15 kHz, 15-16 kHz
  aliasing -15 dB, same levels otherwise; ~0 CPU cost.

## Compressed disc images (launcher)
* `libgc` reads CISO, GCZ, WIA and RVZ through a `BlobReader` that presents the plain image, so verification and
  extraction are unchanged. The decoders are built in (no zlib/zstd/liblzma/bzip2 dependency): inflate, zstd
  (RFC 8878), LZMA/LZMA2 and bzip2, each checked byte-exact against the reference encoders (234 vectors) and fuzzed
  under ASan/UBSan.
* RVZ "packed" chunks replace disc padding with 68-byte seeds for Nintendo's lagged-Fibonacci junk generator
  (K=521, J=32); the reader regenerates it, so an RVZ hashes like the original ISO. WIA/RVZ images of GameCube
  discs have a single raw-data region starting at 0x80 (the first 0x80 bytes live in the header), grouped in
  32 KiB-aligned chunks.
* Test containers built by `tests/make_container_images.py` (all methods, chunk sizes 32 KiB-2 MiB) decode to the
  same SHA-1 as the source image; speed here: zstd ~250 MB/s, bzip2 ~35 MB/s, LZMA ~15-40 MB/s.
* CISO has no image-size field (Dolphin reports the full block-map range), so the reader uses the standard
  1,459,978,240-byte disc size when the map covers it.

## Points, lines and fog
* The game draws particle sprites (e.g. the sparkle ring around weapon pickups) as GX_POINTS with SU_LPSIZE point
  sizes; these were silently dropped before. They are expanded to screen-aligned quads (size in 1/6 pixel, texcoord
  offsets from SU_SSIZE bit 19/18 via the {0,1/16,1/8,1/4,1/2,1,1,1} table), with culling off, in both renderers.
* Fog follows the hardware formula: perspective ze = a*2^24/(bmag - (z>>bshift)), ortho ze = a*z/2^24, f = clamp(ze-c),
  then the fsel curve (lin/exp/exp2/bexp/bexp2); colour blended by round(f*256)>>8. The first level never enables it.
  Range adjustment (BP 0xE8-0xED) is not emulated yet (a warning is logged if the game turns it on).

## Memory card
* Without a card: "No Memory Card found". With an **erased** card image the game says "The Memory Card in Slot A is
  damaged" and offers no format option: `CARDMount` rejects a card whose header serial doesn't match the console
  SRAM's flash ID (`__CARDVerify`), so new cards are formatted by the runtime exactly like `CARDFormat` does, and the
  SRAM flash ID is derived from whatever card is inserted (as Dolphin does), so cards formatted elsewhere mount too.
* The game creates a 3-block save file `SeriousSam` (G3BE9G) automatically during the new-game flow and reads it
  back (blocks 5-7) on the next boot.
* Audio crackle root causes: (1) with a coarse host timer (Windows ~15.6 ms sleeps) the AI DMA engine caught up by
  several buffers at once and replayed a buffer the game hadn't replaced (40% of output); (2) the game thread
  answers the AI interrupt late while it flushes a frame to the rasterizer, same effect. Fixed by stopping at each
  buffer end until the game's `AIInitDMA` handoff (up to 100 ms), plus 1 ms timer resolution on Windows.

## Earlier state: the game loop runs
With the interrupt controller, native threads/alarms, ARAM, DI, VI, AI, DSP mailbox and GX command processor, the
recompiled game boots all the way into its engine loop. In 10 s headless (USA Rev 1):
* ~300 frames (EFB copies), ~46k primitives / 186k vertices, 6.5 MB of GX commands (stream stays in sync)
* 300 disc reads (9 MB): `Global.ssg`, `Data.ssg`, `BackGround.sst`, loading screens... all through the SDK DVD driver
* 275 ARAM DMAs, ~2,200 interrupts, AX audio microcode "booted" (DSP_INIT handshake)
Game log excerpt:
```
------ >> IMPORT_Read_Old(/Data.ssg)
IMPORT: Detected new style export header "CLIMAX-". (64)
------ << IMPORT_Read_Old(/Data.ssg) success
------ >> IMPORT_Read_Old(textures/BackGround.sst)   ... success
```
The game's own GPU hang detector (`SYSTEM-HANG … SyncToken … GP status`) was what pointed at the missing command processor.

More bugs found on the real binary:
* Function-pointer discovery lost `lis` bases kept in callee-saved registers across calls (missed ARQ callback 80156E20).
* 64-bit stores (`stfd`) to the GX FIFO bypassed MMIO.
* Missing game files (my first test set had only `sys/`) made a recursive hierarchy walk (800F601C) overflow into SDK data.

## Boot progress history (headless runner)
1. `__start` → `OSInit` (801B42C0): interrupt masking, BAT/MMU setup through real-mode `rfi` (Config24MB), EXI init. ✅
2. `__OSInitAudioSystem` (801B51C8): DSP handshake, replaced by a native no-op hook. ✅
3. Game `main()` at **8010A664** → 80161CC8 → 8010E374 → **`ARInit` (801C31B4) → `__ARChecksize` (801C32F8)**: polls the
   DSP/ARAM DMA registers (CC005020/22, CC00500A). ⛔ **Current stop point.** Next step: ARAM + AR DMA HLE (16 MB ARAM buffer).

Bugs found and fixed on the real binary:
* `rlwinm` with MB > ME (wrap-around mask) was inverted, which made `__OSMaskInterrupts` loop forever.
* Switch-case blocks were being picked up as "functions" through their jump-table entries. Jump tables are now detected first.
* Real-mode (physical) branch targets (0x001B7108) are now mapped to their cached virtual address.

## Performance work
Measured on the build sandbox (2 cores, Mesa llvmpipe as a stand-in for a slow GPU) with the headless runner and
the replay tool; where the time goes decides what helps on weaker PCs.
* **GPU renderer, GPU side** dominates on weak hardware. At 1x on llvmpipe a gameplay frame costs ~38 ms, half of it
  in the TEV fragment shaders (a trivial shader halves the frame time). The previous default of 3x internal
  resolution is 9x the fragments of 1x: fine on an RTX 4060, too much for integrated graphics.
  -> **Automatic resolution** (scale = 0, the new default): starts at the scale that fits the window (image height
  / 448 lines, rounded), measures GPU time per frame with `GL_TIME_ELAPSED` queries (read back a few frames later,
  never stalling), steps down when a frame needs more than 80% of the frame budget, and back up after 30 s if the
  next scale is estimated (pixel ratio) to need less than 55%. Old settings.ini files without `settings_version`
  that still had the old default (3) move to automatic.
* **Early depth test**: every TEV shader wrote `gl_FragDepth` (near/far plane snapping, see "GPU renderer"), which
  disables early-Z/hierarchical Z. Snapping only matters for fragments within 4e-6 of a plane, which can only occur
  in draws with a vertex on or beyond the near/far plane (window depth is linear in screen space). The recorder now
  flags those draws (and doesn't merge them with unflagged ones); only flagged draws get a shader that writes depth.
  In gameplay ~45% of draws (~14% of triangles) are flagged. Output is bit-identical (llvmpipe replays).
  `SSNE_GPU_ALWAYS_FRAGDEPTH=1` restores depth writes in every shader, for A/B tests should a driver's fixed-function
  depth ever differ from `gl_FragCoord.z` on coplanar geometry (z-fighting that appears only with the new build).
* **Render thread CPU**: a GL state cache skips redundant enables/blend/depth/cull/scissor/texture/sampler/program
  calls, and per-program uniform shadows skip unchanged uniform uploads (most batches share them). Program lookup
  reuses its key buffer (no allocation per draw). Recorded frames are recycled instead of reallocating ~2 MB/frame.
* **EFB copies without a second pass**: every frame the game copies the scene (RGB565) and reads it back as RGB565,
  which took a copy pass plus a full-size reinterpretation pass (encode as the copy format, decode as the texture
  format). Full-size copies now write both the raw copy and the copy converted to its natural texture format in one
  pass (two render targets); the converted image is used directly when the game samples the copy in that format
  (always, in the captures: RGB565->RGB565, RGB5A3->RGB5A3). Half-size (box-filtered) copies keep the separate pass,
  because their stored value is rounded by the framebuffer first. ~10% less GPU time per gameplay frame on llvmpipe
  at 2x; bit-identical output. Mesa rejects a two-target draw while dual-source blend factors are still set (even
  with blending off), so the copy resets the blend factors first.
* `SSNE_GPU_PROFILE=1` (replay tool / debugging) attributes GPU time to draws, depth-writing draws, clears, copies,
  reinterpretations and display copies (glFinish around every command); `=2` also lists each command. In 2x gameplay
  on llvmpipe the full-screen passes dominate (the indirect scene redraw and a 4-texture floor near the camera), and
  the port's own CPU work is small: ~3 ms/frame on the game thread (vertex decode first) and <1 ms on the render
  thread outside the GL driver.
* **Shader cache**: linked programs are stored as driver binaries (ARB_get_program_binary) in `shadercache/`, keyed by
  a hash of the GLSL source + GL vendor/renderer/version; later runs load them instead of compiling (first-use
  hitches go away after the first session). Rejected binaries (driver update) are recompiled.
* **Main loop**: without vsync (or a driver that ignores it) the loop spun at thousands of iterations per second,
  stealing a core from the game thread on 2-4 core CPUs. It now sleeps until the game queues a frame (<= 5 ms) when
  the swap didn't block.
* **Display sync**: the VI ran on a free 59.94 Hz timer while a vsynced 60 Hz display presents frames, so the two
  drift and a frame is shown twice every ~16 s (plus jitter around the crossing). With vsync, the 60 fps limit and a
  measured ~60 Hz swap cadence, VI retraces are now triggered right after each swap (the timer is the fallback when
  swaps stop or become irregular). The game steps by measured time, so a 60.00 vs 59.94 Hz VI changes nothing else.
* **Game thread**: with rendering disabled the guest thread spent ~35% in `decode_vertices` (register decoding per
  vertex, byte-by-byte indexed reads through the MMIO-checking accessors). The fast decoder decodes the vertex
  layout once per primitive batch and reads array data through host pointers; `SSNE_CHECK_VERTICES=1` compares it
  with the reference decoder (0 mismatches over ~80 M vertices of intro + gameplay). The recompiled game code itself
  is a small share of the frame.
* **Software renderer**: ~180 ns per written pixel, split between the rasterizer/TEV loop and bilinear sampling
  (memory bound: branch-free sampling and wrap fast paths measured no gain). It stays the fallback; the top bar now
  says when it is running and why (the log records which OpenGL version the driver offered).
* **Regression testing**: `SSNE_GPU_CAPTURE=dir:first:count` saves recorded GX frames (commands, states, vertices,
  decoded textures); `gpu-replay <dir> <out> [--scale N] [--time] [--repeat R] [--shader-cache dir]` (Linux, EGL)
  executes them and writes the output images, so two builds of the renderer can be compared pixel for pixel and
  timed on identical input. All the changes above were checked this way on intro, menu and (widescreen) gameplay
  captures at 1x and 2x: 0 differing pixels.

## PAL version (Europe, G3BP9G)
* Disc: G3BP9G, disc revision 0, build `FINAL.113 (Feb 19 2004 @ 14:30:08)`, same Dolphin SDK build. Same file set
  as the USA disc (the USA, EUR, French and German text and FMVs are on both); `vssver.scc` is an extra leftover.
  The game asks for the language (English / Deutsch / Français) after the memory-card check.
* main.dol: .text is 0x60 bytes smaller (0x1DD840); data and BSS move by up to +0x700. Both versions have exactly
  7,930 functions (unnamed discovery) in the same order. Matching each USA function to the PAL one at the same index,
  with relocations masked (b/bl targets, addi/addis/ori/oris immediates, r2/r13 displacements), gives 5,680
  identical functions; the rest differ only in data addresses, and 10 changed size (800E267C..800E27FC,
  80104CFC..801062A8, 80162CF8, 80163EBC, 8017BF84: FINAL.113 fixes). The address offset is piecewise constant:
  0 up to 800E270C, then +0x1C/+0x24/+0x3C/+0x44, -0x74, -0x64, -0x48 (80106 to 80162), -0x3C, -0x58, -0x60 after
  8017BF90.
* `data/symbols/G3BP9G_r0.sym` is the USA map translated through that matching (all 611 names; the two map-only
  entries Config48MB and GXGetTexBufferSize at -0x60). With it the PAL dol recompiles to 7,932 functions, 23 jump
  tables, like the USA one.
* Hooked game addresses (runtime/src/hle.cpp `kVersions`, chosen from the disc ID at 0x80000000): PAL
  C_MTXPerspective 801BA814, C_MTXLightPerspective 801BA58C, VIWaitForRetrace 801BF904 (Flip's bl at 80118514),
  tanf 80181530, look update 800BFCA4 (unchanged), weapon projection return 8014CDA8, scene camera returns
  8014C648/670/690/6B4. The r13-relative constants the projection replacements read are at the same offsets.
* Region: the runtime reports an NTSC TV mode (0x800000CC = 0), and the PAL game runs at 60 Hz with the same
  640x448 render mode, so nothing else changes. Memory cards are per region (`MemoryCardA.EUR.raw`).
* Verified headless: boot -> language select -> logos/FMVs -> menus -> gameplay in "Incident at the Arena";
  widescreen 16:9, FOV slider, mouse aim (exact turn) and the >60 fps frame limiter all work on PAL.
* The launcher accepts G3BP9G and the build kit picks the function map by disc ID (`symbols.G3BP9G` in kit.ini).

## Anti-aliasing
* Setting `antialiasing` (F12 > Graphics): off, fxaa, msaa2/4/8, ssaa. Off renders exactly as before (replay check:
  0 differing pixels).
* MSAA: the EFB color and depth are multisample textures (GL 3.2 `TexImage2DMultisample`, optional; capped at
  `GL_MAX_SAMPLES`, falls back to off if the framebuffer is incomplete). Every read of the EFB goes through a resolve
  blit into a single-sample texture: texture copies resolve their source rectangle first (the copy and MRT
  conversion passes then read the resolved texels, so the game's render-to-texture effects - including the scene
  copy it redraws every frame - keep the smoothed edges), and the display copy's blit is itself the resolve.
  Alpha-tested edges are not smoothed (no alpha-to-coverage). Gameplay at 1x with 4 samples changes ~32k pixels a
  frame, all on geometry edges.
* FXAA: one post pass on the display image (classic FXAA "PC console" variant: luma edge detection, early out below
  a contrast threshold, directional blend), so ~4% of the pixels of a gameplay frame change by more than 8 levels.
* Supersampling: automatic resolution targets twice the window-fit scale (max 8x) and the display image gets mipmaps
  each frame (sampled trilinear), so the downscale is filtered instead of skipping pixels. Any anti-aliasing mode
  also turns that smooth downscaling on.
* Fix found while testing: automatic resolution judged the GPU only in one-second windows with at least 20 frames,
  so below ~20 fps (e.g. a weak GPU at a high start resolution) it never stepped down. It now judges with as few as
  8 frames per second, or 2 frames over 3 s.

## Rumble
* The game drives the controller motor through the SDK's motor call (801C1CE8, named PADControlMotor in the map; its
  argument is actually an array of four commands, i.e. PADControlAllMotors), which writes each channel's SI output
  word `0x0040030M` (M = 0 stop, 1 rumble, 2 hard stop). The SI emulation records M per channel when the low half of
  SICnOUTBUF is written (`ssne::pad_motor`), so it works for every game version without addresses.
* The game sends 2 (hard stop) once at boot and rumbles in bursts: in level 1 while walking into combat and firing,
  on/off phases of 0.15-0.7 s with short 20 ms gaps (no PWM), so the port mirrors the motor directly.
* Port: `SDL_GameControllerRumble` with the low-frequency motor at full and the high-frequency one at ~56% (the
  GameCube has a single eccentric motor), refreshed every 0.5 s while on (SDL rumble takes a duration), stopped when
  the game stops it, the menu opens, rumble is turned off or the game exits. PS4/PS5 rumble needs SDL's HIDAPI
  rumble hints. Settings: `pad_rumble`, `pad_rumble_strength` (F12 > Gamepad, with a Test button).
* Testing without hardware: `SSNE_VIRTUAL_PAD=1` attaches an SDL virtual gamepad that logs the rumble it receives;
  with scripted input (`SSNE_AUTOPRESS`) the full path game -> SI -> SDL was verified (22 on/off changes in 2 min).

## Anisotropic filtering
* Setting `anisotropy` (F12 > Graphics): 1 (off), 2, 4, 8, 16. Uses `GL_TEXTURE_MAX_ANISOTROPY` (GL 4.6 core /
  EXT/ARB_texture_filter_anisotropic; loaded optionally, capped at `GL_MAX_TEXTURE_MAX_ANISOTROPY`, off if missing).
* Applied only to game textures that have mipmaps and use a mipmapped linear min filter (GX min filter 5/6 with
  `linear` on): EFB copies, unmipmapped UI/font textures and point-sampled textures keep the original sampler, so
  menus and text are unchanged. The level is part of the sampler-object cache key.
* Off renders exactly as before (replay check: 0 differing pixels). 16x visibly sharpens far floors and walls seen
  at grazing angles; the GPU cost on test hardware was within timer noise.

## Down-left stick flicker (fixed)
* Report: holding the stick (either stick) fully down-left, or A+S on the keyboard, makes the player flicker and stop;
  a partial tilt mostly works; no other diagonal does this.
* Cause (game code, exposed by the PC timing): the VI retrace callback (8011731C) reads the pads with PADRead +
  PADClamp into the PADStatus array at 8028E824, and the game's controller processing (8014F83C, once per game frame)
  undoes the octagon clamp *in place*: x *= 72 / ((1 - |y|/40) * 32 + 40) when y <= 40 and
  x *= 72 / (40 + (y - 40) * 1.25) above (C-stick: 59/31/28). On the GameCube there is exactly one game frame per
  retrace, so each status is processed once. On the PC a status is processed twice or more whenever game frames
  come faster than retraces (frame limit above 60; ~2.5 times per second even at 60 from frame-time jitter;
  measured 2-5 passes per read at 144). Repeated passes rescale the already-rescaled values; for x, y < 0 both axes
  use the |.| formula, whose denominator turns negative: (-40,-40) -> (-72,-72) -> (-128,-128) -> (127,127), so on
  the third pass the stick points up-right and the movement cancels/jitters. The other diagonals use the formula
  that stays positive. Smaller tilts need more passes before flipping ("partial tilt kinda works"), and a second
  pass also over-amplified any tilt.
* Fix (runtime/src/hle.cpp, per version: PADClamp 801C0BAC / 801C0B4C, processing 8014F83C / 8014F7F4): a hook
  on PADClamp keeps the stick bytes it produced and a hook on the processing writes them back before every pass, so
  each pass sees the raw input as on hardware. `SSNE_NO_PAD_FIX=1` disables it (A/B), `SSNE_TRACE_PAD_FIX=1` logs
  reads processed more or less than once.
* Verified headless (USA and PAL, 60 and 144 fps): every direction moves at full speed; with the fix off the trace
  shows the (-128,-128) -> (127,127) flip at 144 fps. Headless `--press RAW<x>_<y>[_<cx>_<cy>]@t:dur` sets raw
  stick bytes (also `SSNE_AUTOPRESS` in the port).

## Controls: jump is the L trigger
* The game's controller processing (8014F83C) maps inputs through a per-scheme table (802444F0 + scheme * 0x200,
  pairs of input codes, 22 = none). Input codes: 0/1 stick Y, 2/3 stick X, 4/5 C-stick Y, 6/7 C-stick X, 8-11 D-pad
  up/down/left/right, 12 L analog, 13 L digital, 14 R analog, 15 R digital, 16 Z, 17 A, 18 B, 19 X, 20 Y, 21 Start.
  base.cfg: "THE JUMP IS CURRENTLY ANALOGUE". Verified headless: L jumps (any tap at 60 fps; camera height at
  80291B6C rises ~2.5 units), R fires, X/Y switch weapons, A does nothing visible in gameplay (select / skip).
* The port's defaults had Space on A (labelled "jump"), so Space never jumped. Settings version 3: Space and the
  right mouse button are L (jump), F is A, and outside player control (menus, cutscenes, pause: the per-frame
  look update hasn't run for 200 ms, `ssne::player_control_active()`) keyboard keys bound to L also press A, so
  Space still selects and skips. Untouched old defaults migrate.
* Mouse wheel bindings (`wheel:up` / `wheel:down`): each notch is a 100 ms press with a 50 ms release (at most 4
  queued). Defaults: X = E + wheel down, Y = Q + wheel up.

## Exit
* F12 has an "Exit game" button. On exit the port leaves fullscreen, destroys the GL context and window first,
  then saves and stops; a watchdog ends the process after 3 s if shutting the game threads down stalls (report:
  the desktop stayed dimmed after Alt+F4 until a reboot, which points at the process or its window staying alive).

## Windows 7 / 8.1 compatibility
* All shipped binaries and the player-built game import only APIs present on Windows 7 SP1 (checked with
  llvm-readobj against the import lists; WinHTTP's automatic-proxy flag is 8.1+ but the launcher already falls back
  to the default proxy). The one hard dependency was the C runtime: the UCRT llvm-mingw variant imports
  api-ms-win-crt-*.dll, which Windows 7/8.1 only have with the KB2999226 update or a VC++ 2015+ redistributable.
* Kit 10 switches the whole chain (launcher, gctool, translator, prebuilt port libraries and the compiler the
  launcher downloads) to the MSVCRT variant of the same llvm-mingw release: msvcrt.dll ships with every Windows.
  The Windows-hosted MSVCRT toolchain (clang, lld, libLLVM, libc++) also imports only Windows 7 APIs. C++ code
  uses mingw's own printf (`__USE_MINGW_ANSI_STDIO` = 1), so %zu / %llu formatting is unaffected.
* The unpacked compiler now carries `toolchain-url.txt`; a kit that names another archive downloads it again.
* WinHTTP on Windows 7 / 8.0 offers only TLS 1.0 unless asked; the launcher requests TLS 1.0-1.2 (works with
  KB3140245). If the download still fails, the error message names the URL to fetch with a browser into
  build-kit\downloads.

## Skip intros
* Boot (USA, headless timeline): Licensed by Nintendo, memory card check (PAL: language choice), Legal.sst at ~7 s,
  Publisher.sst, Climax.bik, croteam.bik, SamLogo.bik, the title screen (TitleScreen.sst + 0_1_FMV.bik probe),
  FrontEnd.ssg (main menu) at ~49 s. A skips one screen at a time; Start on the legal screen jumps straight to the
  main menu (FrontEnd.ssg read 1.1 s later).
* The disc reader reports every file read (`intro_skip_file_read`); once Legal.sst has been read the SI report
  ORs in Start for 200 ms after 0.8 s, retried every 1.5 s up to 4 times until FrontEnd.ssg is read. Nothing is
  pressed before Legal.sst, so memory card prompts and the PAL language choice are left to the player. Main menu
  at ~9 s (USA and PAL verified). Setting `skip_intros` (default on); headless `--skip-intros`.

## Button prompts
* The game draws controller buttons as glyphs: two 512x256 font atlases (menu/HUD font and the large font) and
  the 256x128 front-end sprite sheet (menu button bar). Glyph rectangles (A, B, X, Y, Z, L, R) are fixed per
  atlas (gx_prompts.cpp); an atlas is recognized when decoded by its green A and red B glyphs (a third of each
  rectangle). The port repaints them with Xbox / PlayStation icons (tools/make_prompt_icons.py, Outfit Bold for
  the letters, box-filtered into each rectangle keeping the aspect). Same layouts on PAL.
* Two maps (menus/cutscenes vs player control) because with the Xbox layout GameCube A is "select" on the jump
  button in menus but "use" on X in gameplay; the texture cache key carries a variant so the atlas is decoded
  again when the map in effect changes.
* Setting `pad_prompts`: auto (Xbox / PlayStation glyphs while a controller is connected, by SDL controller type;
  GameCube otherwise), gamecube, xbox, playstation. Glyphs follow the actual pad bindings. Headless
  `--xbox-prompts` / `--ps-prompts`; `gpu_replay --dump-textures dir` writes a capture's decoded textures.
* Gamepad default is now the Xbox layout (settings version 4 migrates untouched GameCube defaults): A jump (L),
  X use (A), B, Y = Z, RB/LB = X/Y (weapons), RT fire. Outside player control the jump button presses A instead.

## Pause in the background
* Setting `pause_unfocused` (default on): window focus lost or minimized -> the game is paused (audio silent, as
  with the menu pause) and the main loop sleeps 15 ms per iteration.

## Save states
* The guest runs as native code on host threads, so a state can only be taken where the host stack is known:
  the start of CcEngine::Flip (USA 801183F0, PAL 801183A8), called from the main loop (return 80155BE8, guest call
  chain 80155BE8 / 8010A718 / 80003250 in menus, gameplay and pause alike; only the boot screens differ). The game
  then runs a single guest thread. The recompiled functions keep all guest state in the PPCContext and guest
  memory, so the host frames of that chain carry nothing: swapping memory, registers and devices is enough.
* Contents: CPU context, RAM (32 MiB backing, zero pages elided), locked cache, scheduler (pending interrupts,
  alarms; requires 1 live thread and no queued interrupt work, else retried for ~3 s), MMIO registers + ARAM +
  DSP mailbox + AI DMA, SI, EXI (channels, IPL/RTC, memory card incl. contents), GX (CP/XF/BP, TMEM, FIFO
  remainder, PE, TEV registers). The texture cache is cleared and the AX resampler history dropped on load; the
  guest timebase is shifted so the game's clock continues from the saved time (alarms stay valid).
* File: header (magic, version, disc ID, function count, call chain, timebase, wall time, CRC32 of the payload);
  ~26-28 MB. Loading checks ID, build (function count), CRC and the call chain before applying anything.
* Verified headless (USA and PAL): save, move, load -> position restored and the game keeps running; load from
  the main menu of a fresh session into gameplay; 13 saves + 14 loads in 35 s of gameplay (moving, firing,
  jumping) without a failure; memory card restored (.before-state-load.bak written).
* Port: F5 save / F9 load (selected slot), Shift+F5 next slot, F12 > Save states (5 slots, date, 192 px
  thumbnail BMP). Volume moved to F4 / Shift+F4, frame capture to Shift+F9. Headless: --save-state T:path,
  --load-state T:path.

## Settings and display QoL
* Graphics presets (Settings::graphics_preset): Low = 1x, AA off, AF off; Medium = automatic resolution, FXAA,
  AF 4x; High = automatic, MSAA 4x, AF 16x; Ultra = supersampling (automatic x2), AF 16x.
* Music / sound-effect volumes (audio_set_mix): applied per AX voice before the game's mixer. Music & video =
  streamed voices (PB is_stream = 1) and PCM16 voices (Bink audio); the rest are effects. Headless WAV check
  (--mix): music 0 % -> menu RMS 0 and gameplay idle 163 (from 5793); effects 0 % -> firing drops from 9310 to
  5454 while the music stays.
* Window: position / size / maximized saved in settings.ini (restored only if the title bar lands on a connected
  display); `display` picks the monitor (the window is moved there before going fullscreen-desktop).
* Screenshots: game image (renderer output, without ImGui) to Screenshots/*.png, PNG encoded on a worker thread
  by port/src/png_write.cpp (RGB, adaptive row filters, LZ77 + fixed-Huffman deflate; ~0.6 s and ~2.3 MB for a
  1080p frame, lossless round trip checked with Pillow). Print Screen is handled on key-up (Windows doesn't send
  its key-down); Shift+F12 also works. png_write.obj ships in the build kit (kit.ini link line).
* Info bar: top_bar 0/1/2 (hidden / frame rate only / full), migrated from show_top_bar (settings version 5).
