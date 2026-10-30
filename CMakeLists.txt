cmake_minimum_required(VERSION 3.20)
project(SSNERecomp LANGUAGES C CXX)
set(SSNE_VERSION "1.2.4")  # release version (the launcher's self-update compares it with the latest GitHub release)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SSNE_BUILD_LAUNCHER "Build the SDL2/ImGui launcher" ON)
option(SSNE_BUILD_PORT     "Build the PC port executable"   ON)
# Tests run the freshly built translator during the build, which can't work when cross-compiling (e.g. Windows
# binaries from Linux), so they default to off then.
if(CMAKE_CROSSCOMPILING)
  option(SSNE_BUILD_TESTS  "Build tests"                    OFF)
else()
  option(SSNE_BUILD_TESTS  "Build tests"                    ON)
endif()
# Directory produced by `ssne-recomp` (see docs/BUILDING.md). If empty, the port builds without
# game code and only shows a status screen.
set(SSNE_RECOMPILED_DIR "" CACHE PATH "Directory holding recompiled game code (funcs_*.cpp)")

if(MSVC)
  add_compile_options(/W3 /utf-8 /bigobj)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wno-unused-label -Wno-unused-variable)
endif()

# ---------------------------------------------------------------- core disc library
add_library(gc STATIC
  libgc/src/sha1.cpp libgc/src/disc.cpp libgc/src/dol.cpp libgc/src/verify.cpp
  libgc/src/blob.cpp libgc/src/decomp_inflate.cpp libgc/src/decomp_zstd.cpp libgc/src/decomp_lzma.cpp
  libgc/src/decomp_bzip2.cpp libgc/src/sha256.cpp libgc/src/zip.cpp)
target_include_directories(gc PUBLIC libgc/include)

add_executable(gctool tools/gctool.cpp)
target_link_libraries(gctool PRIVATE gc)

# ---------------------------------------------------------------- recompiler (offline tool)
add_executable(ssne-recomp
  recomp/src/main.cpp recomp/src/ppc_decode.cpp recomp/src/analysis.cpp recomp/src/codegen.cpp)
target_include_directories(ssne-recomp PRIVATE recomp/src)
target_link_libraries(ssne-recomp PRIVATE gc)

add_executable(ssne-symbols recomp/src/symbols_main.cpp recomp/src/symbols.cpp recomp/src/analysis.cpp
  recomp/src/ppc_decode.cpp)
target_include_directories(ssne-symbols PRIVATE recomp/src)
target_link_libraries(ssne-symbols PRIVATE gc)
set_target_properties(ssne-symbols PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------- runtime (linked into the port)
add_library(ssne-runtime STATIC runtime/src/memory.cpp runtime/src/runtime.cpp runtime/src/hle.cpp
  runtime/src/hw.cpp runtime/src/audio.cpp runtime/src/exi.cpp runtime/src/scheduler.cpp runtime/src/gx/gx_fifo.cpp runtime/src/gx/gx_vertex.cpp
  runtime/src/gx/gx_texture.cpp runtime/src/gx/gx_raster.cpp runtime/src/gx/gx_gpu_record.cpp runtime/src/gx/gx_gpu.cpp
  runtime/src/gx/gx_gpu_capture.cpp runtime/src/gx/gx_prompts.cpp runtime/src/savestate.cpp
  runtime/src/thread_util.cpp runtime/src/si.cpp)
find_package(Threads REQUIRED)
target_link_libraries(ssne-runtime PUBLIC Threads::Threads)
target_include_directories(ssne-runtime PUBLIC runtime/include)
target_link_libraries(ssne-runtime PUBLIC gc)

# Profile-guided optimization of the runtime (clang). Only the runtime is instrumented / optimized: it is shipped
# prebuilt, while the game code is compiled on the player's PC. tools/make_pgo_profile.sh trains
# data/pgo/runtime.proftext; the release scripts convert it for their compiler (tools/pgo_build_profile.sh).
#   SSNE_PGO_GENERATE=ON       instrumented runtime; running the game writes .profraw files (LLVM_PROFILE_FILE)
#   SSNE_PGO_PROFILE=<file>    runtime optimized with a .profdata matching this compiler
option(SSNE_PGO_GENERATE "Instrument the runtime for profile-guided optimization (clang)" OFF)
set(SSNE_PGO_PROFILE "" CACHE FILEPATH "Merged .profdata to optimize the runtime with (clang)")
if((SSNE_PGO_GENERATE OR SSNE_PGO_PROFILE) AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(WARNING "SSNE_PGO_GENERATE / SSNE_PGO_PROFILE need clang; building without profile-guided optimization")
elseif(SSNE_PGO_GENERATE)
  target_compile_options(ssne-runtime PRIVATE -fprofile-instr-generate)
  target_link_options(ssne-runtime INTERFACE -fprofile-instr-generate)
elseif(SSNE_PGO_PROFILE)
  target_compile_options(ssne-runtime PRIVATE "-fprofile-instr-use=${SSNE_PGO_PROFILE}"
                         -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin)
endif()

# GPU renderer regression/benchmark tool: replays frames captured with SSNE_GPU_CAPTURE (headless EGL, Linux).
if(UNIX AND NOT APPLE)
  add_executable(gpu-replay tools/gpu_replay.cpp port/src/no_game_code.cpp)
  target_link_libraries(gpu-replay PRIVATE ssne-runtime ${CMAKE_DL_LIBS})
  set_target_properties(gpu-replay PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# ---------------------------------------------------------------- third-party (fetched)
if(SSNE_BUILD_LAUNCHER OR SSNE_BUILD_PORT)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  # Linux: 2.30.9 no longer builds against current PipeWire headers; 2.32.10 does. Windows keeps the pinned release.
  if(WIN32)
    set(SSNE_SDL2_TAG release-2.30.9)
  else()
    set(SSNE_SDL2_TAG release-2.32.10)
  endif()
  FetchContent_Declare(SDL2 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
                            GIT_TAG ${SSNE_SDL2_TAG} GIT_SHALLOW TRUE)
  FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git
                             GIT_TAG v1.91.5 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(SDL2 imgui)
  find_package(OpenGL REQUIRED)

  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui PUBLIC SDL2::SDL2-static OpenGL::GL)
endif()

if(SSNE_BUILD_LAUNCHER)
  add_executable(ssne-launcher WIN32
    launcher/src/main.cpp launcher/src/launcher_app.cpp launcher/src/platform.cpp launcher/src/config.cpp
    launcher/src/theme.cpp launcher/src/game_builder.cpp launcher/src/updater.cpp)
  target_link_libraries(ssne-launcher PRIVATE gc imgui SDL2::SDL2main)
  # Fonts (SIL OFL, see launcher/fonts/*-OFL.txt) are compiled into the launcher.
  set(EMBED_DIR ${CMAKE_BINARY_DIR}/embed)
  foreach(font EricaOne-Regular Outfit-Regular Outfit-Bold)
    string(REPLACE "-" "_" sym "font_${font}")
    add_custom_command(OUTPUT ${EMBED_DIR}/${sym}.h
      COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/launcher/fonts/${font}.ttf -DOUT=${EMBED_DIR}/${sym}.h
              -DNAME=${sym} -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
      DEPENDS ${CMAKE_SOURCE_DIR}/launcher/fonts/${font}.ttf ${CMAKE_SOURCE_DIR}/cmake/embed.cmake)
    target_sources(ssne-launcher PRIVATE ${EMBED_DIR}/${sym}.h)
  endforeach()
  target_include_directories(ssne-launcher PRIVATE ${EMBED_DIR})
  target_compile_definitions(ssne-launcher PRIVATE IMGUI_DEFINE_MATH_OPERATORS SSNE_VERSION="${SSNE_VERSION}")
  set_target_properties(ssne-launcher PROPERTIES OUTPUT_NAME "SSNE-Native")
  if(WIN32)
    target_link_libraries(ssne-launcher PRIVATE comdlg32 shell32 winhttp)
  endif()
endif()

if(SSNE_BUILD_PORT)
  set(PORT_SOURCES port/src/main.cpp port/src/settings.cpp port/src/settings_menu.cpp port/src/png_write.cpp)
  if(SSNE_RECOMPILED_DIR)
    file(GLOB RECOMPILED_SOURCES CONFIGURE_DEPENDS ${SSNE_RECOMPILED_DIR}/*.cpp)
    list(APPEND PORT_SOURCES ${RECOMPILED_SOURCES})
    if(NOT MSVC)
      # Generated functions are huge; -O2 keeps build times sane with nearly the same speed as -O3.
      set_source_files_properties(${RECOMPILED_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-w")
    endif()
  else()
    list(APPEND PORT_SOURCES port/src/no_game_code.cpp)
  endif()
  add_executable(ssne-port WIN32 ${PORT_SOURCES})
  if(SSNE_RECOMPILED_DIR)
    target_include_directories(ssne-port PRIVATE ${SSNE_RECOMPILED_DIR})
    # Headless runner for bring-up (shares the recompiled sources).
    add_executable(ssne-headless tools/headless.cpp ${RECOMPILED_SOURCES})
    target_include_directories(ssne-headless PRIVATE ${SSNE_RECOMPILED_DIR})
    target_link_libraries(ssne-headless PRIVATE ssne-runtime)
    set_target_properties(ssne-headless PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  endif()
  target_link_libraries(ssne-port PRIVATE ssne-runtime imgui SDL2::SDL2main)
  set_target_properties(ssne-port PROPERTIES OUTPUT_NAME "SSNE-Native-Game")
  if(WIN32)
    target_link_libraries(ssne-port PRIVATE winmm)
  endif()
endif()

# Launcher expects the port and data files next to it.
foreach(t ssne-launcher ssne-port)
  if(TARGET ${t})
    set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  endif()
endforeach()
set_target_properties(gctool ssne-recomp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
configure_file(data/known_dumps.txt ${CMAKE_BINARY_DIR}/bin/known_dumps.txt COPYONLY)

if(SSNE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
