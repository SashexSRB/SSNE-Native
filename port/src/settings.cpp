#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <type_traits>

namespace port {

namespace {

struct ActionInfo { const char* id; const char* name; };
const ActionInfo kActions[ACT_COUNT] = {
    {"move_up", "Move forward"},        {"move_down", "Move back"},
    {"move_left", "Strafe left"},       {"move_right", "Strafe right"},
    {"look_up", "Look up"},             {"look_down", "Look down"},
    {"look_left", "Turn left"},         {"look_right", "Turn right"},
    {"a", "A  (use / select / skip)"},  {"b", "B  (reload / ammo type / zoom / back)"},
    {"x", "X  (next weapon; hold: weapon wheel)"}, {"y", "Y  (previous weapon)"},
    {"z", "Z  (NETRICSA)"},             {"start", "Start  (pause)"},
    {"l", "L trigger  (jump)"},         {"r", "R trigger  (fire)"},
    {"dpad_up", "D-pad up"},            {"dpad_down", "D-pad down"},
    {"dpad_left", "D-pad left"},        {"dpad_right", "D-pad right"},
    {"weapon1", "Weapon 1  (chainsaw)"},  {"weapon2", "Weapon 2  (Desert Hawk)"},
    {"weapon3", "Weapon 3  (double shotgun)"}, {"weapon4", "Weapon 4  (Uzis / minigun)"},
    {"weapon5", "Weapon 5  (rocket / grenade launcher)"}, {"weapon6", "Weapon 6  (Sirian power gun / flamethrower)"},
    {"weapon7", "Weapon 7  (sniper rifle)"}, {"weapon8", "Weapon 8  (cannon)"},
    {"weapon9", "Weapon 9  (Serious Bomb)"},
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

const char* mouse_name(int b) {
    switch (b) {
    case SDL_BUTTON_LEFT: return "Mouse Left";
    case SDL_BUTTON_RIGHT: return "Mouse Right";
    case SDL_BUTTON_MIDDLE: return "Mouse Middle";
    case SDL_BUTTON_X1: return "Mouse Back";
    case SDL_BUTTON_X2: return "Mouse Forward";
    }
    return "Mouse ?";
}

const char* pad_button_name(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return "Pad A / Cross";
    case SDL_CONTROLLER_BUTTON_B: return "Pad B / Circle";
    case SDL_CONTROLLER_BUTTON_X: return "Pad X / Square";
    case SDL_CONTROLLER_BUTTON_Y: return "Pad Y / Triangle";
    case SDL_CONTROLLER_BUTTON_BACK: return "Pad Back / Select";
    case SDL_CONTROLLER_BUTTON_GUIDE: return "Pad Guide";
    case SDL_CONTROLLER_BUTTON_START: return "Pad Start";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "Pad Left Stick Click";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "Pad Right Stick Click";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "Pad Left Bumper";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "Pad Right Bumper";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "Pad D-pad Up";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "Pad D-pad Down";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "Pad D-pad Left";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "Pad D-pad Right";
    }
    return "Pad Button ?";
}

const char* pad_axis_name(int a) {
    switch (a) {
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return "Pad Left Trigger";
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "Pad Right Trigger";
    }
    return "Pad Axis ?";
}

} // namespace

const char* action_name(int a) { return a >= 0 && a < ACT_COUNT ? kActions[a].name : "?"; }
const char* action_id(int a) { return a >= 0 && a < ACT_COUNT ? kActions[a].id : "?"; }

Settings::Settings() {
    reset_graphics();
    reset_audio();
    reset_keyboard();
    reset_gamepad();
    pause_in_menu = true;
}

void Settings::reset_graphics() {
    renderer = "gpu"; scale = 0; aspect = 1; antialiasing = "off"; anisotropy = 1; fps_limit = 60; fov = 63; vsync = true; sync_display = true; gx_thread = true; fullscreen = false; top_bar = 2;
}

void Settings::reset_audio() { volume = 100; music_volume = 100; sfx_volume = 100; muted = false; audio_hq = true; }

void Settings::graphics_preset(int level) {
    switch (level) {
    case 0: scale = 1; antialiasing = "off"; anisotropy = 1; break;      // native GameCube resolution
    case 1: scale = 0; antialiasing = "fxaa"; anisotropy = 4; break;     // automatic resolution
    case 2: scale = 0; antialiasing = "msaa4"; anisotropy = 16; break;
    default: scale = 0; antialiasing = "ssaa"; anisotropy = 16; break;   // supersampling
    }
}

void Settings::reset_keyboard() {
    for (auto& b : kb) for (Bind& x : b) x = Bind{};
    auto k = [](SDL_Scancode s) { return Bind::key(s); };
    kb[ACT_MOVE_UP][0] = k(SDL_SCANCODE_W);        kb[ACT_MOVE_UP][1] = k(SDL_SCANCODE_UP);
    kb[ACT_MOVE_DOWN][0] = k(SDL_SCANCODE_S);      kb[ACT_MOVE_DOWN][1] = k(SDL_SCANCODE_DOWN);
    kb[ACT_MOVE_LEFT][0] = k(SDL_SCANCODE_A);      kb[ACT_MOVE_LEFT][1] = k(SDL_SCANCODE_LEFT);
    kb[ACT_MOVE_RIGHT][0] = k(SDL_SCANCODE_D);     kb[ACT_MOVE_RIGHT][1] = k(SDL_SCANCODE_RIGHT);
    kb[ACT_LOOK_UP][0] = k(SDL_SCANCODE_I);
    kb[ACT_LOOK_DOWN][0] = k(SDL_SCANCODE_K);
    kb[ACT_LOOK_LEFT][0] = k(SDL_SCANCODE_J);
    kb[ACT_LOOK_RIGHT][0] = k(SDL_SCANCODE_L);
    // Classic PC shooter layout. In menus, the pause screen and cutscenes the jump keys also select (A), Esc goes back
    // (B) and the mouse clicks / scrolls through the menus (main.cpp).
    kb[ACT_A][0] = k(SDL_SCANCODE_E);              // use, enter / exit vehicles
    // B: reload / ammo type, and the sniper rifle's zoom (held) on the right mouse button.
    kb[ACT_B][0] = k(SDL_SCANCODE_R);              kb[ACT_B][1] = Bind::mouse(SDL_BUTTON_RIGHT);
    kb[ACT_B][2] = k(SDL_SCANCODE_BACKSPACE);
    kb[ACT_X][0] = k(SDL_SCANCODE_Q);              kb[ACT_X][1] = Bind::wheel(-1);  // next weapon; hold Q: weapon wheel
    kb[ACT_Y][0] = Bind::wheel(1);                 kb[ACT_Y][1] = k(SDL_SCANCODE_F);  // previous weapon
    // (in menus the mouse buttons and wheel select, go back and scroll: Q / F reach X / Y there)
    kb[ACT_Z][0] = k(SDL_SCANCODE_TAB);            // NETRICSA
    kb[ACT_START][0] = k(SDL_SCANCODE_ESCAPE);     kb[ACT_START][1] = k(SDL_SCANCODE_RETURN);
    kb[ACT_L][0] = k(SDL_SCANCODE_SPACE);          // jump (analog L)
    kb[ACT_R][0] = Bind::mouse(SDL_BUTTON_LEFT);   kb[ACT_R][1] = k(SDL_SCANCODE_LCTRL);
    kb[ACT_DPAD_UP][0] = k(SDL_SCANCODE_KP_8);
    kb[ACT_DPAD_DOWN][0] = k(SDL_SCANCODE_KP_2);
    kb[ACT_DPAD_LEFT][0] = k(SDL_SCANCODE_KP_4);
    kb[ACT_DPAD_RIGHT][0] = k(SDL_SCANCODE_KP_6);
    for (int i = 0; i < 9; i++) kb[ACT_WEAPON1 + i][0] = k(SDL_Scancode(SDL_SCANCODE_1 + i));
    mouse_look = true; mouse_aim = true; mouse_sens = 1.0f; mouse_invert_y = false;
}

void Settings::reset_gamepad() {
    pad_preset_xbox();
    pad_deadzone = 0.15f; trigger_threshold = 0.8f; look_sens = 1.0f; pad_invert_y = false; swap_sticks = false;
    rumble = true; rumble_strength = 1.0f; prompts = "auto";
}

void Settings::pad_preset_xbox() {
    for (auto& b : pad) b = Bind{};
    pad[ACT_L] = Bind::button(SDL_CONTROLLER_BUTTON_A);             // jump (also selects in menus, main.cpp)
    pad[ACT_A] = Bind::button(SDL_CONTROLLER_BUTTON_X);             // use
    pad[ACT_B] = Bind::button(SDL_CONTROLLER_BUTTON_B);
    pad[ACT_Z] = Bind::button(SDL_CONTROLLER_BUTTON_Y);
    pad[ACT_X] = Bind::button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER); // weapons
    pad[ACT_Y] = Bind::button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    pad[ACT_R] = Bind::axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);      // fire
    pad[ACT_START] = Bind::button(SDL_CONTROLLER_BUTTON_START);
    pad[ACT_DPAD_UP] = Bind::button(SDL_CONTROLLER_BUTTON_DPAD_UP);
    pad[ACT_DPAD_DOWN] = Bind::button(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    pad[ACT_DPAD_LEFT] = Bind::button(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    pad[ACT_DPAD_RIGHT] = Bind::button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
}

void Settings::pad_preset_gamecube() {
    for (auto& b : pad) b = Bind{};
    pad[ACT_A] = Bind::button(SDL_CONTROLLER_BUTTON_A);
    pad[ACT_B] = Bind::button(SDL_CONTROLLER_BUTTON_B);
    pad[ACT_X] = Bind::button(SDL_CONTROLLER_BUTTON_X);
    pad[ACT_Y] = Bind::button(SDL_CONTROLLER_BUTTON_Y);
    pad[ACT_Z] = Bind::button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    pad[ACT_START] = Bind::button(SDL_CONTROLLER_BUTTON_START);
    pad[ACT_L] = Bind::axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    pad[ACT_R] = Bind::axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    pad[ACT_DPAD_UP] = Bind::button(SDL_CONTROLLER_BUTTON_DPAD_UP);
    pad[ACT_DPAD_DOWN] = Bind::button(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    pad[ACT_DPAD_LEFT] = Bind::button(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    pad[ACT_DPAD_RIGHT] = Bind::button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
}

std::string bind_name(const Bind& b) {
    switch (b.type) {
    case Bind::KEY: {
        const char* n = SDL_GetScancodeName(SDL_Scancode(b.code));
        return n && *n ? n : "Key " + std::to_string(b.code);
    }
    case Bind::MOUSE: return mouse_name(b.code);
    case Bind::WHEEL: return b.code > 0 ? "Wheel Up" : "Wheel Down";
    case Bind::PAD_BUTTON: return pad_button_name(b.code);
    case Bind::PAD_AXIS: return pad_axis_name(b.code);
    default: return "-";
    }
}

std::string bind_to_string(const Bind& b) {
    switch (b.type) {
    case Bind::KEY: {
        const char* n = SDL_GetScancodeName(SDL_Scancode(b.code));
        return n && *n ? std::string("key:") + n : "scancode:" + std::to_string(b.code);
    }
    case Bind::MOUSE: return "mouse:" + std::to_string(b.code);
    case Bind::WHEEL: return b.code > 0 ? "wheel:up" : "wheel:down";
    case Bind::PAD_BUTTON: {
        const char* n = SDL_GameControllerGetStringForButton(SDL_GameControllerButton(b.code));
        return std::string("button:") + (n ? n : "?");
    }
    case Bind::PAD_AXIS: {
        const char* n = SDL_GameControllerGetStringForAxis(SDL_GameControllerAxis(b.code));
        return std::string("axis:") + (n ? n : "?");
    }
    default: return "none";
    }
}

Bind bind_from_string(const std::string& s0) {
    std::string s = trim(s0);
    size_t c = s.find(':');
    if (c == std::string::npos) return {};
    std::string t = s.substr(0, c), v = s.substr(c + 1);
    if (t == "key") {
        SDL_Scancode sc = SDL_GetScancodeFromName(v.c_str());
        return sc == SDL_SCANCODE_UNKNOWN ? Bind{} : Bind::key(sc);
    }
    if (t == "scancode") return Bind::key(SDL_Scancode(atoi(v.c_str())));
    if (t == "mouse") { int b = atoi(v.c_str()); return b >= 1 && b <= 5 ? Bind::mouse(b) : Bind{}; }
    if (t == "wheel") return v == "up" ? Bind::wheel(1) : v == "down" ? Bind::wheel(-1) : Bind{};
    if (t == "button") {
        auto b = SDL_GameControllerGetButtonFromString(v.c_str());
        return b == SDL_CONTROLLER_BUTTON_INVALID ? Bind{} : Bind::button(b);
    }
    if (t == "axis") {
        auto a = SDL_GameControllerGetAxisFromString(v.c_str());
        return a == SDL_CONTROLLER_AXIS_INVALID ? Bind{} : Bind::axis(a);
    }
    return {};
}

bool load_settings(Settings& s, const std::string& path) {
    std::ifstream in(std::filesystem::u8path(path));
    if (!in) return false;
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string val = line.substr(eq + 1);
        size_t hash = val.find(" #");  // trailing comment
        if (hash != std::string::npos) val = val.substr(0, hash);
        kv[trim(line.substr(0, eq))] = trim(val);
    }
    auto get = [&](const char* k, auto& v) {
        auto it = kv.find(k);
        if (it == kv.end()) return;
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) v = it->second == "1" || it->second == "true" || it->second == "on";
        else if constexpr (std::is_same_v<T, int>) v = atoi(it->second.c_str());
        else if constexpr (std::is_same_v<T, float>) v = float(atof(it->second.c_str()));
        else v = it->second;
    };
    int version = 1;
    get("settings_version", version);
    get("renderer", s.renderer);
    get("scale", s.scale);
    // Before version 2 every new install was written with scale = 3 (the old fixed default), which is too much for
    // many integrated GPUs: move untouched defaults to the automatic resolution.
    if (version < 2 && s.scale == 3) s.scale = 0;
    get("aspect", s.aspect);
    get("antialiasing", s.antialiasing);
    get("anisotropy", s.anisotropy);
    get("fps_limit", s.fps_limit);
    get("fov", s.fov);
    get("vsync", s.vsync);
    get("sync_to_display", s.sync_display);
    get("gx_thread", s.gx_thread);
    {
        bool old = true;  // before version 5: show_top_bar = 0/1
        auto it = kv.find("show_top_bar");
        if (it != kv.end()) { get("show_top_bar", old); s.top_bar = old ? 2 : 0; }
    }
    get("top_bar", s.top_bar);
    get("display", s.display);
    get("window_x", s.win_x); get("window_y", s.win_y); get("window_w", s.win_w); get("window_h", s.win_h);
    get("window_maximized", s.win_max);
    get("music_volume", s.music_volume);
    get("sfx_volume", s.sfx_volume);
    get("fullscreen", s.fullscreen);
    get("volume", s.volume);
    get("muted", s.muted);
    get("audio_hq", s.audio_hq);
    get("mouse_look", s.mouse_look);
    get("mouse_aim", s.mouse_aim);
    get("mouse_sensitivity", s.mouse_sens);
    get("mouse_invert_y", s.mouse_invert_y);
    get("pad_deadzone", s.pad_deadzone);
    get("pad_trigger_threshold", s.trigger_threshold);
    get("pad_look_sensitivity", s.look_sens);
    get("pad_invert_y", s.pad_invert_y);
    get("pad_swap_sticks", s.swap_sticks);
    get("pad_rumble", s.rumble);
    get("pad_rumble_strength", s.rumble_strength);
    get("pause_in_menu", s.pause_in_menu);
    get("pause_unfocused", s.pause_unfocused);
    get("skip_intros", s.skip_intros);
    get("state_slot", s.state_slot);
    get("pad_prompts", s.prompts);
    for (int a = 0; a < ACT_COUNT; a++) {
        auto it = kv.find(std::string("kb.") + action_id(a));
        if (it != kv.end()) {
            std::string v = it->second;
            for (int k = 0; k < Settings::kKbSlots; k++) {  // "a, b, c" (older files have two)
                size_t comma = v.find(',');
                s.kb[a][k] = bind_from_string(v.substr(0, comma));
                v = comma == std::string::npos ? std::string() : v.substr(comma + 1);
            }
        }
        if (a >= kFirstButtonAction) {
            auto p = kv.find(std::string("pad.") + action_id(a));
            if (p != kv.end()) s.pad[a] = bind_from_string(p->second);
        }
    }
    // Before version 3 the defaults had jump wrong: Space was bound to A (select / use), while the game jumps with
    // the analog L trigger (then on the right mouse button / Left Shift). Untouched old defaults move to the new
    // ones (Space jumps and still selects in menus), and the weapon buttons get the mouse wheel.
    if (version < 3) {
        auto same = [](const Bind* b, Bind x, Bind y) { return b[0] == x && b[1] == y; };
        if (same(s.kb[ACT_A], Bind::key(SDL_SCANCODE_SPACE), Bind{}) &&
            same(s.kb[ACT_L], Bind::mouse(SDL_BUTTON_RIGHT), Bind::key(SDL_SCANCODE_LSHIFT))) {
            s.kb[ACT_A][0] = Bind::key(SDL_SCANCODE_F); s.kb[ACT_A][1] = Bind{};
            s.kb[ACT_L][0] = Bind::key(SDL_SCANCODE_SPACE); s.kb[ACT_L][1] = Bind::mouse(SDL_BUTTON_RIGHT);
        }
        if (same(s.kb[ACT_X], Bind::key(SDL_SCANCODE_E), Bind{})) s.kb[ACT_X][1] = Bind::wheel(-1);
        if (same(s.kb[ACT_Y], Bind::key(SDL_SCANCODE_Q), Bind{})) s.kb[ACT_Y][1] = Bind::wheel(1);
    }
    // Version 6: classic PC shooter keys (E use, R reload, Esc pause, right mouse / wheel for weapons). An untouched
    // older default keyboard layout (after the version 3 update above) moves to it; rebound keys stay as they are.
    if (version < 6) {
        auto k = [](SDL_Scancode c) { return Bind::key(c); };
        const Bind old[][2] = {
            {k(SDL_SCANCODE_F), {}}, {k(SDL_SCANCODE_BACKSPACE), k(SDL_SCANCODE_ESCAPE)},
            {k(SDL_SCANCODE_E), Bind::wheel(-1)}, {k(SDL_SCANCODE_Q), Bind::wheel(1)}, {k(SDL_SCANCODE_TAB), {}},
            {k(SDL_SCANCODE_RETURN), {}}, {k(SDL_SCANCODE_SPACE), Bind::mouse(SDL_BUTTON_RIGHT)},
            {Bind::mouse(SDL_BUTTON_LEFT), k(SDL_SCANCODE_LCTRL)}};
        const int acts[] = {ACT_A, ACT_B, ACT_X, ACT_Y, ACT_Z, ACT_START, ACT_L, ACT_R};
        bool untouched = true;
        for (int i = 0; i < 8; i++) untouched &= s.kb[acts[i]][0] == old[i][0] && s.kb[acts[i]][1] == old[i][1];
        if (untouched) {
            Settings fresh;
            for (int a : acts) for (int k = 0; k < Settings::kKbSlots; k++) s.kb[a][k] = fresh.kb[a][k];
        }
    }
    // Version 7: a third binding per control, and keys for X / Y (F / Q), which menus need (Add / Delete Player):
    // there the mouse buttons and wheel select, go back and scroll. Added to untouched version 6 X / Y bindings when
    // F / Q aren't bound to anything else (whatever version wrote the file: an older build keeps the bindings).
    if (version < 7) {
        auto bound = [&](Bind b) {
            for (auto& slots : s.kb) for (const Bind& x : slots) if (x == b) return true;
            return false;
        };
        if (s.kb[ACT_X][0] == Bind::mouse(SDL_BUTTON_RIGHT) && s.kb[ACT_X][1] == Bind::wheel(-1) &&
            s.kb[ACT_X][2] == Bind{} && !bound(Bind::key(SDL_SCANCODE_F)))
            s.kb[ACT_X][2] = Bind::key(SDL_SCANCODE_F);
        if (s.kb[ACT_Y][0] == Bind::wheel(1) && s.kb[ACT_Y][1] == Bind{} && !bound(Bind::key(SDL_SCANCODE_Q)))
            s.kb[ACT_Y][1] = Bind::key(SDL_SCANCODE_Q);
    }
    // Version 8: the right mouse button zooms the sniper rifle (B, held), so the weapon wheel moves to holding Q (X)
    // and previous weapon gets F. An untouched version 7 B / X / Y layout moves to it.
    if (version < 8) {
        auto k = [](SDL_Scancode c) { return Bind::key(c); };
        auto is = [&](int a, Bind x, Bind y, Bind z) { return s.kb[a][0] == x && s.kb[a][1] == y && s.kb[a][2] == z; };
        if (is(ACT_B, k(SDL_SCANCODE_R), k(SDL_SCANCODE_BACKSPACE), Bind{}) &&
            is(ACT_X, Bind::mouse(SDL_BUTTON_RIGHT), Bind::wheel(-1), k(SDL_SCANCODE_F)) &&
            is(ACT_Y, Bind::wheel(1), k(SDL_SCANCODE_Q), Bind{})) {
            Settings fresh;
            for (int a : {ACT_B, ACT_X, ACT_Y})
                for (int i = 0; i < Settings::kKbSlots; i++) s.kb[a][i] = fresh.kb[a][i];
        }
    }
    // Version 4: the gamepad default became the Xbox layout (A jumps). Untouched old defaults (the GameCube layout)
    // move to it; "GameCube layout" in F12 > Gamepad brings the old one back.
    if (version < 4) {
        Settings gc;
        gc.pad_preset_gamecube();
        bool same = true;
        for (int a = kFirstButtonAction; a < ACT_COUNT; a++) same &= s.pad[a] == gc.pad[a];
        if (same) s.pad_preset_xbox();
    }
    s.state_slot = std::clamp(s.state_slot, 1, 5);
    s.top_bar = std::clamp(s.top_bar, 0, 2);
    s.music_volume = std::clamp(s.music_volume, 0, 100);
    s.sfx_volume = std::clamp(s.sfx_volume, 0, 100);
    if (s.win_w < 320 || s.win_h < 240) { s.win_w = 1280; s.win_h = 720; }
    if (s.prompts != "gamecube" && s.prompts != "xbox" && s.prompts != "playstation" && s.prompts != "keyboard")
        s.prompts = "auto";
    // Sanitize.
    if (s.renderer != "software") s.renderer = "gpu";
    s.scale = std::clamp(s.scale, 0, 8);
    s.aspect = std::clamp(s.aspect, 0, 2);
    {
        static const char* kAa[] = {"off", "fxaa", "msaa2", "msaa4", "msaa8", "ssaa"};
        bool ok = false;
        for (auto* a : kAa) ok |= s.antialiasing == a;
        if (!ok) s.antialiasing = "off";
    }
    {
        int a = 1;
        while (a * 2 <= std::clamp(s.anisotropy, 1, 16)) a *= 2;
        s.anisotropy = a;
    }
    s.fov = std::clamp(s.fov, 45, 110);
    if (s.fps_limit != 0) s.fps_limit = std::clamp(s.fps_limit, 30, 1000);
    s.volume = std::clamp(s.volume, 0, 200);
    s.mouse_sens = std::clamp(s.mouse_sens, 0.1f, 10.f);
    s.pad_deadzone = std::clamp(s.pad_deadzone, 0.f, 0.6f);
    s.trigger_threshold = std::clamp(s.trigger_threshold, 0.1f, 1.f);
    s.look_sens = std::clamp(s.look_sens, 0.1f, 4.f);
    s.rumble_strength = std::clamp(s.rumble_strength, 0.1f, 1.f);
    return true;
}

bool save_settings(const Settings& s, const std::string& path) {
    std::ostringstream o;
    o << "# Silly ReEncounter settings (edited in-game with F12)\n";
    o << "settings_version = 8\n";
    o << "[graphics]\n";
    o << "renderer = " << s.renderer << "\n";
    o << "scale = " << s.scale << "    # internal resolution 1-8, 0 = automatic\n";
    o << "aspect = " << s.aspect << "    # 0 = 4:3, 1 = 16:9, 2 = 21:9\n";
    o << "antialiasing = " << s.antialiasing << "    # off, fxaa, msaa2, msaa4, msaa8, ssaa\n";
    o << "anisotropy = " << s.anisotropy << "    # anisotropic filtering: 1 (off), 2, 4, 8, 16\n";
    o << "fps_limit = " << s.fps_limit << "    # 60 = original, 0 = unlimited (vsync)\n";
    o << "fov = " << s.fov << "    # gameplay vertical field of view in degrees, 63 = original\n";
    o << "vsync = " << (s.vsync ? 1 : 0) << "\n";
    o << "sync_to_display = " << (s.sync_display ? 1 : 0) << "    # 60 fps on a 60 Hz display: game ticks with the display\n";
    o << "gx_thread = " << (s.gx_thread ? 1 : 0) << "    # graphics commands on their own CPU thread (faster); 0 = on the game's thread (takes effect at start)\n";
    o << "top_bar = " << s.top_bar << "    # info bar at the top: 0 hidden, 1 frame rate only, 2 full (Shift+F1)\n";
    o << "display = " << s.display << "    # monitor for fullscreen (0 = first, -1 = where the window is)\n";
    o << "window_x = " << s.win_x << "\nwindow_y = " << s.win_y << "\nwindow_w = " << s.win_w << "\nwindow_h = " << s.win_h << "\n";
    o << "window_maximized = " << (s.win_max ? 1 : 0) << "\n";
    o << "fullscreen = " << (s.fullscreen ? 1 : 0) << "\n";
    o << "[audio]\n";
    o << "volume = " << s.volume << "\n";
    o << "music_volume = " << s.music_volume << "    # music and video sound, % of the volume\n";
    o << "sfx_volume = " << s.sfx_volume << "    # sound effects, % of the volume\n";
    o << "muted = " << (s.muted ? 1 : 0) << "\n";
    o << "audio_hq = " << (s.audio_hq ? 1 : 0) << "  # 1 high-quality resampling, 0 original (linear, as on the GameCube)\n";
    o << "[keyboard_mouse]\n";
    o << "mouse_look = " << (s.mouse_look ? 1 : 0) << "\n";
    o << "mouse_aim = " << (s.mouse_aim ? 1 : 0) << "    # 1 = direct aim, 0 = mouse emulates the C-stick\n";
    o << "mouse_sensitivity = " << s.mouse_sens << "\n";
    o << "mouse_invert_y = " << (s.mouse_invert_y ? 1 : 0) << "\n";
    for (int a = 0; a < ACT_COUNT; a++)
        o << "kb." << action_id(a) << " = " << bind_to_string(s.kb[a][0]) << ", " << bind_to_string(s.kb[a][1]) << ", "
          << bind_to_string(s.kb[a][2]) << "\n";
    o << "[gamepad]\n";
    o << "pad_deadzone = " << s.pad_deadzone << "\n";
    o << "pad_trigger_threshold = " << s.trigger_threshold << "\n";
    o << "pad_look_sensitivity = " << s.look_sens << "\n";
    o << "pad_invert_y = " << (s.pad_invert_y ? 1 : 0) << "\n";
    o << "pad_swap_sticks = " << (s.swap_sticks ? 1 : 0) << "\n";
    o << "pad_rumble = " << (s.rumble ? 1 : 0) << "\n";
    o << "pad_rumble_strength = " << s.rumble_strength << "\n";
    o << "pad_prompts = " << s.prompts << "    # on-screen buttons: auto, keyboard, gamecube, xbox, playstation\n";
    for (int a = kFirstButtonAction; a < ACT_COUNT; a++) o << "pad." << action_id(a) << " = " << bind_to_string(s.pad[a]) << "\n";
    o << "[general]\n";
    o << "pause_in_menu = " << (s.pause_in_menu ? 1 : 0) << "\n";
    o << "pause_unfocused = " << (s.pause_unfocused ? 1 : 0) << "    # pause while the window is in the background\n";
    o << "skip_intros = " << (s.skip_intros ? 1 : 0) << "    # start at the main menu\n";
    o << "state_slot = " << s.state_slot << "    # save state slot for F5 (save) / F9 (load), 1-5\n";
    namespace fs = std::filesystem;
    fs::path dst = fs::u8path(path), tmp = fs::u8path(path + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << o.str();
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, dst, ec);  // replaces an existing file
    return !ec;
}

} // namespace port
