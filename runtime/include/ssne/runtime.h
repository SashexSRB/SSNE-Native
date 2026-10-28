#pragma once
#include <cstdint>
#include <string>

#include "ssne/ppc_context.h"

namespace ssne {

using FatalHandler = void (*)(const char* message);
void set_fatal_handler(FatalHandler h);
[[noreturn]] void fatal(const char* message);

void reset_cpu(PPCContext& ctx);
void boot_globals(uint32_t mem_size = 0x01800000);

// Load <data_dir>/sys/main.dol into guest RAM and set up what the IPL/apploader would leave behind
// (disc ID, boot info, FST, BI2). Also mounts the disc for the DI emulation. Returns entry point or 0.
uint32_t load_game(const std::string& data_dir, std::string& error);

// Registers native replacements for Dolphin SDK functions found by name in the function table.
void install_sdk_hooks();

// Everything a runner needs: load, scheduler, all native replacements. Returns entry or 0.
uint32_t boot(PPCContext& ctx, const std::string& data_dir, std::string& error);

// Log sink for guest OSReport output (the port shows it in its window).
using LogSink = void (*)(const char* line);
void set_log_sink(LogSink sink);
void log_message(const std::string& line);
// Widescreen: horizontal field-of-view factor applied to the game's perspective projections
// (1 = 4:3, 4/3 = 16:9, 7/4 = 21:9). Takes effect on the next projection the game computes.
// Frame rate limit: 60 = the game's own VI-synced pacing (default), N > 60 = host-paced cap, 0 = unlimited.
// While the player is in a vehicle the game runs at 60 fps whatever the limit (its physics need that step).
void set_frame_limit(int fps);
int frame_limit();
void frame_limiter_wait();  // called by GX at each display copy
// Mouse aim: rotation (radians; +yaw = turn right, +pitch = look up) applied directly to the player's view on
// the next gameplay frame. active() is true while the game is running its first-person look update.
void mouse_aim_add(double yaw, double pitch);
bool mouse_aim_available();
bool mouse_aim_active();
// True while the player controls Sam (the game ran its per-frame look update in the last 200 ms) or drives a
// vehicle: false in menus, cutscenes, loading and the pause screen.
bool player_control_active();
// The player is driving a vehicle (jeep, combine harvester).
bool player_in_vehicle();
// Number-key weapon groups 0-8 (keys 1-9): picks the group's first available weapon (the next one if a weapon of the
// group is already in hand) and switches to it the game's own way (hle.cpp). Ignored unless on foot.
void weapon_select_group(int group);
// Pause: the game thread stops at its next display copy until unpaused (port settings menu).
void set_paused(bool paused);
bool paused();
// Gameplay vertical field of view in degrees (the game uses 63); 0 = original.
void set_fov(float degrees);
// Skip intros: when the game shows its first logo screen (Legal.sst), press Start for the player, which takes the
// game straight to the main menu (the publisher/developer logos and videos and the title screen are skipped).
void set_skip_intros(bool on);
// Save states (savestate.cpp): saved / loaded at the start of the game's next frame flip (normally within a frame;
// up to ~3 s while the game is loading). The result (message for the player) is picked up with take_result.
struct SavestateResult { bool save = false, ok = false; std::string path, message; };
void savestate_request(bool save, const std::string& path);
bool savestate_pending();
bool savestate_take_result(SavestateResult& out);
bool savestate_read_info(const std::string& path, uint64_t& unix_time);  // false: no (valid) state there
void savestate_safe_point(PPCContext& ctx);  // internal: called at CcEngine::Flip
// Button prompts: icons drawn over the game's GameCube button glyphs (A, B, X, Y, Z, L, R in that order) when the
// player uses another controller. Icon ids match tools/make_prompt_icons.py (0 = keep the GameCube glyph).
// `menu` applies in menus and cutscenes, `control` while the player controls Sam.
enum PromptGlyph : uint8_t {
    GLYPH_GC = 0, XB_A, XB_B, XB_X, XB_Y, XB_LB, XB_RB, XB_LT, XB_RT, XB_LS, XB_RS, XB_VIEW, XB_MENU,
    GLYPH_DPAD_UP, GLYPH_DPAD_DOWN, GLYPH_DPAD_LEFT, GLYPH_DPAD_RIGHT,
    PS_CROSS, PS_CIRCLE, PS_SQUARE, PS_TRIANGLE, PS_L1, PS_R1, PS_L2, PS_R2, PS_L3, PS_R3, PS_SHARE, PS_OPTIONS,
    MS_LEFT, MS_RIGHT, MS_MIDDLE, MS_WHEEL_UP, MS_WHEEL_DOWN, MS_BACK, MS_FORWARD,  // then the keyboard keycaps
};
void set_button_prompts(const uint8_t menu[7], const uint8_t control[7]);
// Glyph id of the keycap for a key by its SDL scancode name ("E", "Space", "Escape", ...), 0 if there is none.
uint8_t prompt_key_glyph(const char* sdl_scancode_name);
float fov();
void set_widescreen(float factor);
void set_hud_edges(bool on);  // widescreen: HUD pieces left / right of the middle at the screen's edges (default on)
float widescreen();
float widescreen_frame_factor();  // factor the game's current frame was rendered with (1 outside gameplay)

} // namespace ssne
