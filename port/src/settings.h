// Port settings: graphics, audio and controls. Stored in settings.ini next to the game exe; edited in the F12
// settings menu (settings_menu.cpp). Command-line options override the file for that run.
#pragma once
#include <SDL.h>

#include <cstdint>
#include <string>

namespace port {

// A physical input bound to a GameCube control.
struct Bind {
    enum Type : uint8_t { NONE, KEY, MOUSE, PAD_BUTTON, PAD_AXIS, WHEEL };
    Type type = NONE;
    // SDL_Scancode / mouse button (SDL_BUTTON_*) / SDL_GameControllerButton / SDL_GameControllerAxis / wheel (+1 up, -1 down)
    int16_t code = 0;
    bool operator==(const Bind& o) const { return type == o.type && code == o.code; }
    static Bind key(SDL_Scancode s) { return {KEY, int16_t(s)}; }
    static Bind mouse(int b) { return {MOUSE, int16_t(b)}; }
    static Bind button(SDL_GameControllerButton b) { return {PAD_BUTTON, int16_t(b)}; }
    static Bind axis(SDL_GameControllerAxis a) { return {PAD_AXIS, int16_t(a)}; }
    static Bind wheel(int dir) { return {WHEEL, int16_t(dir > 0 ? 1 : -1)}; }
};

// GameCube controls. Stick/C-stick directions are digital on the keyboard (full tilt).
enum Action : int {
    ACT_MOVE_UP, ACT_MOVE_DOWN, ACT_MOVE_LEFT, ACT_MOVE_RIGHT,
    ACT_LOOK_UP, ACT_LOOK_DOWN, ACT_LOOK_LEFT, ACT_LOOK_RIGHT,
    ACT_A, ACT_B, ACT_X, ACT_Y, ACT_Z, ACT_START, ACT_L, ACT_R,
    ACT_DPAD_UP, ACT_DPAD_DOWN, ACT_DPAD_LEFT, ACT_DPAD_RIGHT,
    // Weapon groups 1-9 (not GameCube controls: the runtime picks the weapon, ssne::weapon_select_group)
    ACT_WEAPON1, ACT_WEAPON2, ACT_WEAPON3, ACT_WEAPON4, ACT_WEAPON5, ACT_WEAPON6, ACT_WEAPON7, ACT_WEAPON8, ACT_WEAPON9,
    ACT_COUNT
};
constexpr int kFirstButtonAction = ACT_A;  // gamepad bindings exist for ACT_A..ACT_DPAD_RIGHT

const char* action_name(int a);   // "Move forward", "A (jump / select)", ...
const char* action_id(int a);     // ini key suffix: "move_up", "a", ...

struct Settings {
    // Graphics
    std::string renderer = "gpu";  // "gpu" or "software" (applies on restart)
    int scale = 0;                 // GPU internal resolution 1..8, 0 = automatic (fits the window, lowered if the GPU is too slow)
    int aspect = 1;                // 0 4:3, 1 16:9, 2 21:9
    int anisotropy = 1;            // anisotropic filtering 1 (off), 2, 4, 8, 16
    std::string antialiasing = "off";  // off, fxaa, msaa2, msaa4, msaa8, ssaa (2x automatic resolution, filtered down)
    int fps_limit = 60;            // 60 original, 120, 144, 0 unlimited
    int fov = 63;                  // gameplay vertical field of view in degrees (63 = original)
    bool vsync = true;
    bool sync_display = true;      // 60 Hz display + vsync + 60 fps: tick the game with the display (no repeated frames)
    bool gx_thread = true;         // graphics commands processed on their own thread (read at start)
    bool fullscreen = false;
    int top_bar = 2;               // info bar at the top: 0 hidden, 1 frame rate only, 2 full (Shift+F1 cycles)
    int display = -1;              // monitor for fullscreen / the window (-1 = wherever the window is)
    int win_x = -1, win_y = -1, win_w = 1280, win_h = 720;  // window position and size (remembered)
    bool win_max = false;          // window maximized
    // Audio
    int volume = 100;              // 0..200 %
    int music_volume = 100;        // music and video sound, 0..100 % of the volume
    int sfx_volume = 100;          // sound effects, 0..100 % of the volume
    bool muted = false;
    bool audio_hq = true;          // band-limited resampling (off: the DSP's original linear interpolation)
    // Keyboard / mouse
    static constexpr int kKbSlots = 3;  // bindings per control
    Bind kb[ACT_COUNT][kKbSlots];
    bool mouse_look = true;        // captured mouse turns the view
    bool mouse_aim = true;         // direct 1:1 aim in gameplay (off: the mouse emulates the C-stick)
    float mouse_sens = 1.0f;
    bool mouse_invert_y = false;
    // Gamepad
    Bind pad[ACT_COUNT];           // digital/trigger controls (sticks are fixed: left = move, right = look)
    float pad_deadzone = 0.15f;
    float trigger_threshold = 0.8f;  // analog trigger travel that also clicks L/R
    float look_sens = 1.0f;
    bool pad_invert_y = false;
    bool swap_sticks = false;
    bool rumble = true;              // pass the game's controller rumble to the gamepad
    float rumble_strength = 1.0f;    // 0.1 .. 1
    std::string prompts = "auto";    // on-screen buttons: auto (keyboard & mouse, or the gamepad in use), keyboard, gamecube, xbox, playstation
    // General
    bool pause_in_menu = true;
    bool pause_unfocused = true;     // pause (and silence) the game while the window is in the background
    bool skip_intros = true;         // boot straight to the main menu (logos, videos and title screen skipped)
    int state_slot = 1;              // save state slot used by F5 / F9 (1..5)

    Settings();
    void reset_graphics();
    // Graphics presets: 0 low, 1 medium, 2 high, 3 ultra (resolution, anti-aliasing, texture filtering)
    void graphics_preset(int level);
    void reset_audio();
    void reset_keyboard();
    void reset_gamepad();                // Xbox layout + default stick/trigger/rumble settings
    void pad_preset_xbox();              // A jump (L), X use (A), B back, Y Z, LB/RB weapons (Y/X), RT fire (R)
    void pad_preset_gamecube();          // same buttons as the GameCube controller: A = A, LT jump (L), RT fire (R)
};

// Load/save settings.ini (missing file or keys keep defaults). Return false on I/O error.
bool load_settings(Settings& s, const std::string& path);
bool save_settings(const Settings& s, const std::string& path);

std::string bind_name(const Bind& b);   // "Space", "Mouse Left", "Pad A", "Pad Right Trigger", "-" for none
std::string bind_to_string(const Bind& b);  // ini form: "key:Space", "mouse:1", "button:a", "axis:righttrigger"
Bind bind_from_string(const std::string& s);

} // namespace port
