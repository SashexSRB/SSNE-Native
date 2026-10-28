#include "settings_menu.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace port {

namespace {

bool g_open = false;
bool g_rumble_test = false;
bool g_exit_requested = false;  // "Exit game" button
int g_state_req_slot = 0;       // Save states tab: requested slot (0 = none)
bool g_state_req_save = false;
int g_select_tab = -1;  // tab to select on the next frame
struct Capture { bool active = false; bool pad = false; int action = 0; int slot = 0; } g_cap;

const ImVec4 kDim(0.62f, 0.62f, 0.66f, 1), kAccent(1.0f, 0.76f, 0.22f, 1), kWarn(1.0f, 0.55f, 0.35f, 1);

void assign(Settings& s, const Bind& b) {
    if (!g_cap.pad) {
        // One key per control: drop the key from any other control it was bound to.
        for (int a = 0; a < ACT_COUNT; a++)
            for (int k = 0; k < Settings::kKbSlots; k++)
                if (s.kb[a][k] == b && !(a == g_cap.action && k == g_cap.slot)) s.kb[a][k] = Bind{};
        s.kb[g_cap.action][g_cap.slot] = b;
    } else {
        for (int a = kFirstButtonAction; a < ACT_COUNT; a++)
            if (s.pad[a] == b && a != g_cap.action) s.pad[a] = Bind{};
        s.pad[g_cap.action] = b;
    }
    g_cap.active = false;
}

void help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

bool slider_pct(const char* label, float& v, float lo, float hi) {
    int pct = int(v * 100.f + 0.5f);
    bool ch = ImGui::SliderInt(label, &pct, int(lo * 100), int(hi * 100), "%d%%");
    if (ch) v = float(pct) / 100.f;
    return ch;
}

bool bind_button(const Bind& b, bool pad, int action, int slot) {
    bool waiting = g_cap.active && g_cap.pad == pad && g_cap.action == action && g_cap.slot == slot;
    std::string label = waiting ? std::string(pad ? "press a pad button..." : "press a key, mouse button or wheel...") : bind_name(b);
    label += "##" + std::to_string(pad) + "_" + std::to_string(action) + "_" + std::to_string(slot);
    if (waiting) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.35f, 0.05f, 1));
    bool clicked = ImGui::Button(label.c_str(), ImVec2(-1, 0));
    if (waiting) ImGui::PopStyleColor();
    if (clicked) g_cap = {true, pad, action, slot};
    // Right click clears the binding.
    return ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
}

bool tab_graphics(Settings& s, const MenuInfo& info) {
    bool ch = false;
    ImGui::SeparatorText("Presets");
    {
        static const char* kNames[] = {"Low", "Medium", "High", "Ultra"};
        static const char* kHelp[] = {
            "Original GameCube resolution, no anti-aliasing: for integrated graphics and older PCs.",
            "Automatic resolution, FXAA, 4x texture filtering: a good start for most PCs.",
            "Automatic resolution, MSAA 4x, 16x texture filtering.",
            "Supersampling (twice the resolution, scaled down) and 16x texture filtering: for fast graphics cards."};
        ImGui::BeginDisabled(!info.gpu_active);
        for (int i = 0; i < 4; i++) {
            if (i) ImGui::SameLine();
            if (ImGui::Button(kNames[i], ImVec2(90, 0))) { s.graphics_preset(i); ch = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kHelp[i]);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(kDim, "  (resolution, anti-aliasing, filtering)");
    }
    ImGui::SeparatorText("Display");
    ch |= ImGui::Checkbox("Fullscreen  (F11 / Alt+Enter)", &s.fullscreen);
    if (info.displays.size() > 1) {
        std::string cur = s.display < 0 || s.display >= int(info.displays.size()) ? "Where the window is"
                        : std::to_string(s.display + 1) + ": " + info.displays[size_t(s.display)];
        ImGui::SetNextItemWidth(300);
        if (ImGui::BeginCombo("Monitor", cur.c_str())) {
            if (ImGui::Selectable("Where the window is", s.display < 0)) { s.display = -1; ch = true; }
            for (size_t i = 0; i < info.displays.size(); i++) {
                std::string name = std::to_string(i + 1) + ": " + info.displays[i];
                if (ImGui::Selectable(name.c_str(), s.display == int(i))) { s.display = int(i); ch = true; }
            }
            ImGui::EndCombo();
        }
        help("The screen used for fullscreen (and where the window opens).");
    }
    ch |= ImGui::Checkbox("Vertical sync", &s.vsync);
    help("Syncs to your monitor's refresh rate: no tearing. Turn it off only with a frame rate limit set.");
    {
        ImGui::SetNextItemWidth(300);
        ch |= ImGui::Combo("Info bar at the top  (Shift+F1)", &s.top_bar, "Hidden\0Frame rate only\0Full (frame rate, key hints)\0");
        help("The small bar in the top-left corner. Shift+F1 cycles through the three.");
    }

    ImGui::SeparatorText("Frame rate");
    static const int kFps[] = {60, 120, 144, 165, 240, 0};
    static const char* kFpsNames[] = {"60 (original)", "120", "144", "165", "240", "Unlimited (vsync)"};
    int cur = 0;
    for (int i = 0; i < 6; i++) if (kFps[i] == s.fps_limit) cur = i;
    ImGui::SetNextItemWidth(260);
    if (ImGui::Combo("Frame rate limit  (F10)", &cur, kFpsNames, 6)) { s.fps_limit = kFps[cur]; ch = true; }
    help("The game times gameplay by real elapsed time, so it plays at the same speed at any frame rate.\n"
         "Menus, the pause screen, cutscenes and vehicles always run at 60 (their animations and the jeep's\n"
         "physics depend on the original frame rate).\n"
         "Unlimited is still capped by your monitor when vertical sync is on.");
    ch |= ImGui::Checkbox("Sync game to display", &s.sync_display);
    help("With vertical sync on, the 60 fps limit and a 60 Hz display, the game advances exactly once per display "
         "refresh instead of on its own 59.94 Hz clock, which otherwise drifts against the display and shows a "
         "frame twice every few seconds (a small stutter).");
    if (s.sync_display) {
        ImGui::SameLine();
        ImGui::TextColored(kDim, info.display_synced ? "(active)" : "(inactive: needs vsync, 60 fps and a ~60 Hz display)");
    }

    ImGui::SeparatorText("Image");
    static const char* kAspect[] = {"4:3 (original)", "16:9", "21:9"};
    ImGui::SetNextItemWidth(260);
    ch |= ImGui::Combo("Aspect ratio  (F8)", &s.aspect, kAspect, 3);
    help("Widescreen widens the view during gameplay; menus (also pause, NETRICSA and briefings) and cutscenes stay 4:3.");
    if (s.aspect > 0) {
        ch |= ImGui::Checkbox("HUD at the screen edges", &s.hud_edges);
        help("Widescreen gameplay: health, ammo and the weapon bar sit at the edges of the screen, like in a widescreen\n"
             "game. Off: the HUD keeps the GameCube's 4:3 layout in the middle of the screen.");
    }
    ch |= ImGui::Checkbox("Fill the sides of 4:3 screens", &s.side_fill);
    help("Menus and cutscenes are 4:3: on a wider window, show a blurred, darkened copy of the picture at the sides\n"
         "instead of black bars (GPU renderer).");
    {
        static const float kWide[3] = {1.0f, 4.0f / 3.0f, 7.0f / 4.0f};
        float hfov = 2.f * std::atan(std::tan(0.5f * float(s.fov) * 0.017453292f) * 1.4286f * kWide[std::clamp(s.aspect, 0, 2)]) / 0.017453292f;
        char ffmt[64];
        snprintf(ffmt, sizeof(ffmt), s.fov == 63 ? "%%d deg (original)  - %.0f horizontal" : "%%d deg  - %.0f horizontal", hfov);
        ImGui::SetNextItemWidth(260);
        ch |= ImGui::SliderInt("Field of view", &s.fov, 45, 110, ffmt);
        help("Vertical field of view in gameplay (the game uses 63). The horizontal value depends on the aspect ratio.\n"
             "Your weapon, menus and cutscene framing are not affected. Right-click to reset.");
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { s.fov = 63; ch = true; }
    }
    ImGui::BeginDisabled(!info.gpu_active);
    {
        static const char* kAaIds[] = {"off", "fxaa", "msaa2", "msaa4", "msaa8", "ssaa"};
        static const char* kAaNames[] = {"Off", "FXAA (fast, any GPU)", "MSAA 2x", "MSAA 4x", "MSAA 8x",
                                         "Supersampling (2x resolution)"};
        int aa = 0;
        for (int i = 0; i < 6; i++) if (s.antialiasing == kAaIds[i]) aa = i;
        ImGui::SetNextItemWidth(260);
        if (ImGui::Combo("Anti-aliasing", &aa, kAaNames, 6)) { s.antialiasing = kAaIds[aa]; ch = true; }
        help("Smooths jagged edges.\n"
             "FXAA: a quick filter over the finished image; almost free, slightly softer.\n"
             "MSAA: smooths the edges of 3D geometry while it is drawn; sharp, costs more GPU time the higher it goes.\n"
             "Supersampling: renders at twice the resolution (automatic resolution) and scales it down; best quality, "
             "most GPU time (automatic resolution still lowers it if the GPU can't keep up).");
        if (aa >= 2 && aa <= 4 && info.gpu_active) {
            int want = 1 << (aa - 1);
            if (info.msaa_active < want)
                ImGui::TextColored(kWarn, "Your GPU allows MSAA %dx at most here.", info.msaa_active);
        }
        if (aa == 5 && s.scale > 0) ImGui::TextColored(kWarn, "Supersampling needs Automatic or Native resolution (below).");
    }
    {
        static const char* kAfNames[] = {"Off (original)", "2x", "4x", "8x", "16x"};
        int af = 0;
        while (af < 4 && (2 << af) <= s.anisotropy) af++;
        ImGui::SetNextItemWidth(260);
        if (ImGui::Combo("Anisotropic filtering", &af, kAfNames, 5)) { s.anisotropy = 1 << af; ch = true; }
        help("Keeps floor, wall and road textures sharp when seen at a steep angle or far away, instead of\n"
             "blurring them. Only 3D surface textures are affected; menus and text stay as they were.\n"
             "Costs very little on most GPUs; 16x is the sharpest.");
        if (af > 0 && info.gpu_active && info.aniso_active < s.anisotropy)
            ImGui::TextColored(kWarn, "Your GPU allows %dx at most.", info.aniso_active);
    }
    {
        static const char* kRes[] = {"Automatic", "Native (screen / window resolution)", "1x  (640x448)", "2x  (1280x896)",
                                      "3x  (1920x1344)", "4x  (2560x1792)", "5x  (3200x2240)", "6x  (3840x2688)",
                                      "7x  (4480x3136)", "8x  (5120x3584)"};
        int sel = s.scale == 0 ? 0 : s.scale < 0 ? 1 : s.scale + 1;
        ImGui::SetNextItemWidth(320);
        if (ImGui::Combo("Internal resolution  (F6/F7)", &sel, kRes, IM_ARRAYSIZE(kRes))) {
            s.scale = sel == 0 ? 0 : sel == 1 ? -1 : sel - 1;
            ch = true;
        }
        help("Automatic: fits your window and lowers the resolution while your GPU can't keep up.\n"
             "Native: renders at exactly the size the game is shown at (e.g. 1920x1080 in widescreen gameplay on a\n"
             "1080p screen) - the sharpest image without scaling; GPU cost between 2x and 3x on a 1080p screen.\n"
             "1x-8x: multiples of the GameCube's 640x448 (scaled to your window).");
        if (s.scale == 0 && info.current_scale > 0)
            ImGui::TextColored(kDim, "Now rendering at %dx (%dx%d)", info.current_scale, 640 * info.current_scale,
                               448 * info.current_scale);
        else if (s.scale < 0 && info.native_h > 0)
            ImGui::TextColored(kDim, "Now rendering at %dx%d", info.out_w, info.out_h);
        if (info.gpu_ms > 0) {
            if (s.scale == 0) { ImGui::SameLine(); ImGui::TextColored(kDim, " -  GPU %.1f ms per frame", info.gpu_ms); }
            else ImGui::TextColored(info.gpu_ms > 14.f ? kWarn : kDim, "GPU %.1f ms per frame%s", info.gpu_ms,
                                    info.gpu_ms > 14.f ? " - too slow for 60 fps, try a lower resolution" : "");
        }
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Renderer");
    int r = s.renderer == "software" ? 1 : 0;
    static const char* kRend[] = {"GPU (OpenGL 3.3)", "Software (CPU)"};
    ImGui::SetNextItemWidth(260);
    if (ImGui::Combo("Renderer", &r, kRend, 2)) { s.renderer = r ? "software" : "gpu"; ch = true; }
    bool running_sw = !info.gpu_active;
    if ((s.renderer == "software") != running_sw) ImGui::TextColored(kWarn, "Takes effect the next time you start the game.");
    if (info.gpu_active) ImGui::TextColored(kDim, "Running: %s", info.renderer_info.c_str());
    else ImGui::TextColored(kDim, "Running: software renderer");
    ImGui::Spacing();
    if (ImGui::Button("Reset graphics to defaults")) { s.reset_graphics(); ch = true; }
    return ch;
}

void tab_hotkeys() {
    ImGui::TextColored(kDim, "Keys that work any time in the game window (not rebindable).");
    ImGui::Spacing();
    static const char* keys[][2] = {
        {"F12  /  pad Back", "Settings menu (Esc closes it)"},
        {"F1", "Log and statistics window"},
        {"Shift+F1", "Info bar at the top: full / frame rate only / hidden"},
        {"F2", "Release the mouse (click the game to capture it again)"},
        {"F3", "Mute / unmute"},
        {"F5", "Save state (selected slot, see F12 > Save states)"},
        {"F9", "Load state (selected slot)"},
        {"Shift+F5", "Next save state slot (1-5)"},
        {"F4  /  Shift+F4", "Volume down / up (10% steps)"},
        {"F6  /  F7", "Internal resolution down / up (GPU renderer; turns off automatic resolution)"},
        {"F8", "Aspect ratio: 16:9 / 21:9 / 4:3"},
        {"Shift+F9", "Capture the next frame to the debug folder (GPU renderer, for bug reports)"},
        {"Print Screen  /  Shift+F12", "Screenshot (PNG in the Screenshots folder)"},
        {"F10", "Frame rate limit: 60 / 120 / 144 / unlimited"},
        {"F11  /  Alt+Enter", "Fullscreen"},
    };
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 170);
        ImGui::TableSetupColumn("Action");
        for (auto& k : keys) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(kAccent, "%s", k[0]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(k[1]);
        }
        ImGui::EndTable();
    }
}

bool tab_audio(Settings& s) {
    bool ch = false;
    ImGui::SetNextItemWidth(300);
    ch |= ImGui::SliderInt("Volume  (F4 / Shift+F4)", &s.volume, 0, 200, "%d%%");
    ImGui::SetNextItemWidth(300);
    ch |= ImGui::SliderInt("Music and videos", &s.music_volume, 0, 100, "%d%%");
    help("The soundtrack and the sound of the cutscene videos (on top of the volume above).");
    ImGui::SetNextItemWidth(300);
    ch |= ImGui::SliderInt("Sound effects", &s.sfx_volume, 0, 100, "%d%%");
    help("Weapons, enemies, pickups, menu sounds (on top of the volume above).");
    ch |= ImGui::Checkbox("Mute  (F3)", &s.muted);
    ImGui::SeparatorText("Quality");
    int q = s.audio_hq ? 1 : 0;
    ImGui::SetNextItemWidth(300);
    if (ImGui::Combo("Resampling", &q, "Original (linear, as on GameCube)\0High quality (band-limited)\0")) {
        s.audio_hq = q == 1;
        ch = true;
    }
    ImGui::TextColored(kDim, "The music is 44.1 kHz and the GameCube converts it to 32 kHz with linear interpolation,\n"
                             "which dulls the treble and adds aliasing. High quality uses a 64-tap filter instead.");
    ImGui::Spacing();
    if (ImGui::Button("Reset audio to defaults")) { s.reset_audio(); ch = true; }
    return ch;
}

bool tab_keyboard(Settings& s) {
    bool ch = false;
    ImGui::TextColored(kDim, "Click a slot, then press a key, mouse button or wheel. Right-click clears. Esc cancels.");
    ImGui::SeparatorText("Mouse");
    ch |= ImGui::Checkbox("Mouse look  (click the game to capture the mouse, F2 releases it)", &s.mouse_look);
    ImGui::BeginDisabled(!s.mouse_look);
    ch |= ImGui::Checkbox("Direct mouse aim", &s.mouse_aim);
    help("On: the mouse turns the view directly, 1:1 like a PC shooter (no acceleration, same at any frame rate).\n"
         "Off: the mouse moves the GameCube C-stick, like the original controls.");
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(300);
    ch |= slider_pct("Mouse sensitivity", s.mouse_sens, 0.1f, 5.0f);
    ch |= ImGui::Checkbox("Invert mouse Y", &s.mouse_invert_y);
    ImGui::SeparatorText("Bindings");
    if (ImGui::BeginTable("##kb", 1 + Settings::kKbSlots, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("GameCube control", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Primary", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Secondary", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Third", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (int a = 0; a < ACT_COUNT; a++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(action_name(a));
            for (int k = 0; k < Settings::kKbSlots; k++) {
                ImGui::TableSetColumnIndex(1 + k);
                if (bind_button(s.kb[a][k], false, a, k)) { s.kb[a][k] = Bind{}; ch = true; }
            }
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (ImGui::Button("Reset keyboard & mouse to defaults")) { s.reset_keyboard(); ch = true; }
    return ch;
}

bool tab_gamepad(Settings& s, const MenuInfo& info) {
    bool ch = false;
    if (info.pad_name.empty()) ImGui::TextColored(kWarn, "No controller connected (plug one in at any time).");
    else ImGui::TextColored(kDim, "Controller: %s", info.pad_name.c_str());
    ImGui::TextColored(kDim, "Left stick = move, right stick = look. Back/Select opens this menu.");
    ImGui::SeparatorText("Sticks and triggers");
    ImGui::SetNextItemWidth(300);
    ch |= slider_pct("Stick deadzone", s.pad_deadzone, 0.0f, 0.5f);
    help("Stick movement smaller than this is ignored (fixes drift). The rest of the range is rescaled.");
    ImGui::SetNextItemWidth(300);
    ch |= slider_pct("Look sensitivity", s.look_sens, 0.25f, 3.0f);
    ImGui::SetNextItemWidth(300);
    ch |= slider_pct("Trigger click point", s.trigger_threshold, 0.1f, 1.0f);
    help("How far an analog trigger must be pulled to also press the GameCube's digital L/R click.");
    ch |= ImGui::Checkbox("Invert look Y", &s.pad_invert_y);
    ch |= ImGui::Checkbox("Swap sticks (left = look, right = move)", &s.swap_sticks);
    ImGui::SeparatorText("Rumble");
    ch |= ImGui::Checkbox("Rumble", &s.rumble);
    help("The GameCube controller's vibration (taking damage, explosions, heavy weapons), on your gamepad.\n"
         "Works with Xbox, PlayStation, Switch Pro and most other controllers.");
    ImGui::BeginDisabled(!s.rumble);
    ImGui::SetNextItemWidth(300);
    ch |= slider_pct("Rumble strength", s.rumble_strength, 0.1f, 1.0f);
    ImGui::SameLine();
    if (ImGui::Button("Test")) g_rumble_test = true;
    ImGui::EndDisabled();
    ImGui::SeparatorText("Buttons");
    if (ImGui::Button("Xbox layout (A jumps)")) { s.pad_preset_xbox(); ch = true; }
    help("A jump (also selects in menus and skips cutscenes), X use, B back, Y the GameCube's Z,\n"
         "LB / RB switch weapons, RT fire, Start pause. The default.");
    ImGui::SameLine();
    if (ImGui::Button("GameCube layout")) { s.pad_preset_gamecube(); ch = true; }
    help("The buttons in the GameCube controller's positions: A use / select, B, X and Y as on the GameCube,\n"
         "LT jump (analog, like the GameCube's L), RT fire, RB the Z button.");
    ImGui::SameLine();
    if (ImGui::Button("Swap A/B and X/Y")) {
        std::swap(s.pad[ACT_A], s.pad[ACT_B]);
        std::swap(s.pad[ACT_X], s.pad[ACT_Y]);
        ch = true;
    }
    help("For a Switch Pro / Nintendo-layout controller with the GameCube layout.");
    {
        static const char* kIds[] = {"auto", "keyboard", "gamecube", "xbox", "playstation"};
        static const char* kNames[] = {"Automatic (what I use)", "Keyboard & mouse", "GameCube", "Xbox", "PlayStation"};
        int cur = 0;
        for (int i = 0; i < 5; i++) if (s.prompts == kIds[i]) cur = i;
        ImGui::SetNextItemWidth(300);
        if (ImGui::Combo("On-screen buttons", &cur, kNames, 5)) { s.prompts = kIds[cur]; ch = true; }
        help("The buttons the game shows in menus and hints (\"A Select\", \"Press A to skip\").\n"
             "Automatic shows your keys and mouse buttons, or Xbox / PlayStation buttons when you play with a\n"
             "controller, matching your bindings.");
    }
    ImGui::TextColored(kDim, "Click a slot, then press a controller button or pull a trigger. Right-click clears. Esc cancels.");
    if (ImGui::BeginTable("##pad", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("GameCube control", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();
        for (int a = kFirstButtonAction; a < ACT_COUNT; a++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(action_name(a));
            ImGui::TableSetColumnIndex(1);
            if (bind_button(s.pad[a], true, a, 0)) { s.pad[a] = Bind{}; ch = true; }
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (ImGui::Button("Reset gamepad to defaults")) { s.reset_gamepad(); ch = true; }
    return ch;
}

bool tab_states(Settings& s, const MenuInfo& info) {
    bool ch = false;
    ImGui::TextWrapped("Save the game exactly where you are and come back to that moment later. F5 saves to the "
                       "selected slot and F9 loads it (Shift+F5 picks the next slot).");
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("Loading a state also puts the memory card back as it was when the state was saved "
                       "(the card as it was before loading is kept as a .bak file next to it).");
    ImGui::PopStyleColor();
    if (!info.states_available) ImGui::TextColored(kWarn, "Save states are available once the game is running.");
    ImGui::Spacing();
    ImGui::BeginDisabled(!info.states_available);
    for (int i = 0; i < 5; i++) {
        const MenuInfo::Slot& sl = info.slots[i];
        ImGui::PushID(i);
        bool selected = s.state_slot == i + 1;
        if (ImGui::RadioButton("##sel", selected)) { s.state_slot = i + 1; ch = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Use this slot for F5 / F9");
        ImGui::SameLine();
        ImVec2 thumb(128, 90);
        if (sl.tex) ImGui::Image((ImTextureID)(intptr_t)sl.tex, thumb);
        else ImGui::Dummy(thumb);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("Slot %d", i + 1);
        if (sl.exists) ImGui::TextColored(kDim, "%s", sl.when.c_str());
        else ImGui::TextColored(kDim, "empty");
        if (ImGui::Button("Save")) { g_state_req_slot = i + 1; g_state_req_save = true; }
        ImGui::SameLine();
        ImGui::BeginDisabled(!sl.exists);
        if (ImGui::Button("Load")) { g_state_req_slot = i + 1; g_state_req_save = false; }
        ImGui::EndDisabled();
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (!info.states_dir.empty()) ImGui::TextColored(kDim, "Folder: %s", info.states_dir.c_str());
    return ch;
}

} // namespace

bool menu_take_rumble_test() {
    bool t = g_rumble_test;
    g_rumble_test = false;
    return t;
}

bool menu_open() { return g_open; }
bool menu_take_state_request(int& slot, bool& save) {
    if (!g_state_req_slot) return false;
    slot = g_state_req_slot;
    save = g_state_req_save;
    g_state_req_slot = 0;
    return true;
}
bool menu_take_exit_request() { bool r = g_exit_requested; g_exit_requested = false; return r; }
void menu_set_open(bool open) { g_open = open; if (!open) g_cap.active = false; }
void menu_select_tab(int tab) { g_select_tab = tab; }
bool menu_capturing() { return g_open && g_cap.active; }

bool menu_handle_event(const SDL_Event& e, Settings& s, bool& changed) {
    if (!menu_capturing()) return false;
    if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) { g_cap.active = false; return true; }
    if (!g_cap.pad) {
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            SDL_Scancode sc = e.key.keysym.scancode;
            if (sc == SDL_SCANCODE_F12) return true;  // reserved for the menu
            assign(s, Bind::key(sc));
            changed = true;
            return true;
        }
        if (e.type == SDL_MOUSEBUTTONDOWN) {
            if (ImGui::GetIO().WantCaptureMouse && e.button.button == SDL_BUTTON_LEFT && !ImGui::IsAnyItemHovered())
                return false;  // clicking empty menu space: not a binding
            assign(s, Bind::mouse(e.button.button));
            changed = true;
            return true;
        }
        if (e.type == SDL_MOUSEWHEEL && e.wheel.y != 0) {
            int dy = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
            assign(s, Bind::wheel(dy));
            changed = true;
            return true;
        }
        if (e.type == SDL_KEYUP || e.type == SDL_TEXTINPUT) return true;
    } else {
        if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) { g_cap.active = false; return true; }  // reserved
            assign(s, Bind::button(SDL_GameControllerButton(e.cbutton.button)));
            changed = true;
            return true;
        }
        if (e.type == SDL_CONTROLLERAXISMOTION && e.caxis.value > 24000 &&
            (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
            assign(s, Bind::axis(SDL_GameControllerAxis(e.caxis.axis)));
            changed = true;
            return true;
        }
    }
    return false;
}

bool draw_menu(Settings& s, const MenuInfo& info) {
    if (!g_open) return false;
    bool ch = false;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 size(std::min(760.f, vp->Size.x - 40), std::min(640.f, vp->Size.y - 40));
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);
    bool open = true;
    ImGui::Begin("Settings", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(kAccent, "SERIOUS SAM: NEXT ENCOUNTER NATIVE - Settings");
    ImGui::SameLine();
    ImGui::TextColored(kDim, "   F12 / Esc to close. Changes apply and save immediately.");
    ch |= ImGui::Checkbox("Pause in this menu", &s.pause_in_menu);
    ImGui::SameLine();
    ch |= ImGui::Checkbox("Pause in the background", &s.pause_unfocused);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pause and silence the game while its window isn't focused (Alt+Tab).");
    ImGui::SameLine();
    ch |= ImGui::Checkbox("Skip intros", &s.skip_intros);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Start at the main menu: the logos, intro videos and title screen are skipped\n"
                          "(the memory card check stays). Applies from the next start.");
    ImGui::SameLine(ImGui::GetWindowWidth() - 120);
    if (ImGui::Button("Exit game", ImVec2(100, 0))) g_exit_requested = true;
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Graphics", nullptr, g_select_tab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::BeginChild("##g"); ch |= tab_graphics(s, info); ImGui::EndChild(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Audio", nullptr, g_select_tab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::BeginChild("##a"); ch |= tab_audio(s); ImGui::EndChild(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Keyboard & Mouse", nullptr, g_select_tab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::BeginChild("##k"); ch |= tab_keyboard(s); ImGui::EndChild(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Gamepad", nullptr, g_select_tab == 3 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::BeginChild("##p"); ch |= tab_gamepad(s, info); ImGui::EndChild(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Save states", nullptr, g_select_tab == 5 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::BeginChild("##s"); ch |= tab_states(s, info); ImGui::EndChild(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Hotkeys", nullptr, g_select_tab == 4 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::BeginChild("##h"); tab_hotkeys(); ImGui::EndChild(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
        g_select_tab = -1;
    }
    ImGui::End();
    if (!open) menu_set_open(false);
    return ch;
}

} // namespace port
