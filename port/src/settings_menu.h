// In-game settings menu (F12, or Back/Select on a gamepad). Dear ImGui window with Graphics, Audio,
// Keyboard & Mouse and Gamepad tabs, including input rebinding.
#pragma once
#include <SDL.h>

#include <string>
#include <vector>

#include "settings.h"

namespace port {

struct MenuInfo {
    bool gpu_active = false;        // GPU renderer running (resolution slider available)
    std::string renderer_info;      // e.g. "NVIDIA GeForce RTX 4060 ..."
    int out_w = 0, out_h = 0;       // current GPU output size
    int current_scale = 0;          // internal resolution in use (with automatic resolution: what it picked)
    float gpu_ms = 0;               // measured GPU time per frame (0 = unknown)
    bool display_synced = false;    // the game is currently ticking with the display (sync_display active)
    int aniso_active = 1;           // anisotropic filtering level in use (capped by the GPU)
    int msaa_active = 1;            // MSAA samples in use (the driver may allow fewer than chosen)
    std::string pad_name;           // connected controller, empty if none
    std::vector<std::string> displays;  // monitor names (index = SDL display index)
    std::string settings_path;
    // Save states
    struct Slot { bool exists = false; std::string when; unsigned tex = 0; int w = 0, h = 0; };
    Slot slots[5];
    bool states_available = false;  // the game is running (states can be saved / loaded)
    std::string states_dir;
};

bool menu_open();
void menu_set_open(bool open);
void menu_select_tab(int tab);    // 0 graphics, 1 audio, 2 keyboard & mouse, 3 gamepad, 4 hotkeys, 5 save states
bool menu_capturing();              // waiting for a key/button to bind
bool menu_take_exit_request();     // the "Exit game" button was pressed (cleared by this call)
bool menu_take_rumble_test();       // the "Test rumble" button was pressed (cleared by this call)
bool menu_take_state_request(int& slot, bool& save);  // a Save / Load button was pressed (slot 1..5)

// Feed SDL events first; returns true if the event was consumed by a pending rebind.
// Sets `changed` when a binding was assigned.
bool menu_handle_event(const SDL_Event& e, Settings& s, bool& changed);

// Draws the menu when open. Returns true if any setting changed this frame.
bool draw_menu(Settings& s, const MenuInfo& info);

} // namespace port
