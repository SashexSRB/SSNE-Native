// ssne-port: the PC executable. Boots the recompiled game on a guest thread and shows a live
// status window (game log, GPU/disc/audio activity) while the GX renderer is being built.
#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif
#include <SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <chrono>
#include <thread>
#include <vector>

#include "ssne/audio.h"
#include "ssne/exi.h"
#include "ssne/gx.h"
#include "ssne/gx_gpu.h"
#include "ssne/hw.h"
#include "ssne/input.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include "ssne/scheduler.h"
#include "ssne/thread_util.h"
#include "settings.h"
#include "settings_menu.h"
#include "png_write.h"

namespace {

std::mutex g_log_mutex;
std::deque<std::string> g_log;
FILE* g_log_file = nullptr;
std::string g_status = "Starting...";
std::atomic<bool> g_guest_running{false};
bool g_scroll_to_bottom = true;

void log_line(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    g_log.push_back(s);
    if (g_log.size() > 4000) g_log.pop_front();
    if (g_log_file) { fprintf(g_log_file, "%s\n", s.c_str()); fflush(g_log_file); }
    g_scroll_to_bottom = true;
}

void set_status(const std::string& s) {
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        g_status = s;
    }
    log_line("[port] " + s);
}

[[noreturn]] void throw_fatal(const char* msg) { throw std::runtime_error(msg); }

void guest_main(std::string data_dir) {
    g_guest_running = true;
    try {
        if (!g_ssne_has_game_code) {
            set_status("This build has no recompiled game code. Run ssne-recomp on your main.dol and rebuild "
                       "with -DSSNE_RECOMPILED_DIR (see docs/BUILDING.md).");
            g_guest_running = false;
            return;
        }
        static PPCContext ctx;
        std::string err;
        uint32_t entry = ssne::boot(ctx, data_dir, err);
        if (!entry) throw std::runtime_error(err);
        set_status("Running recompiled game code");
        ssne_call(ctx, entry);
        set_status("Game code returned from its entry point.");
    } catch (const std::exception& e) {
        set_status(std::string("Stopped: ") + e.what());
    }
    g_guest_running = false;
}

// ---------------------------------------------------------------- game view

GLuint g_tex = 0;
int g_tex_w = 0, g_tex_h = 0;
uint64_t g_frame_seq = 0;
std::vector<uint32_t> g_pixels;

bool update_game_texture() {
    int w, h;
    if (!ssne::gx_get_frame(g_pixels, w, h, g_frame_seq)) return false;
    if (!g_tex) {
        glGenTextures(1, &g_tex);
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, g_tex);
    if (w != g_tex_w || h != g_tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels.data());
        g_tex_w = w;
        g_tex_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels.data());
    }
    return true;
}

bool g_gpu = false;       // GPU renderer active
// Main-loop heartbeat for the watchdog (hang diagnostics in ssne.log).
std::atomic<uint64_t> g_heartbeat{0};
std::atomic<const char*> g_loop_phase{"start"};

void watchdog() {
    uint64_t last = 0;
    int stuck = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uint64_t hb = g_heartbeat.load();
        if (hb != last) {
            if (stuck >= 5) log_line("[watchdog] main loop resumed after " + std::to_string(stuck) + " s");
            last = hb; stuck = 0; continue;
        }
        stuck++;
        if (stuck == 5 || (stuck > 5 && stuck % 30 == 0)) {
            std::string msg = "[watchdog] main loop stuck for " + std::to_string(stuck) + " s in: " + g_loop_phase.load();
            if (g_gpu) msg += " | renderer: " + ssne::gpu_renderer_phase();
            log_line(msg);
        }
    }
}
// Settings (settings.ini next to the executable, F12 menu).
port::Settings g_set;
std::string g_settings_path;
// Widescreen modes (F8 cycles): factor applied to the game's horizontal field of view in gameplay.
struct AspectMode { const char* name; float factor; };
const AspectMode kAspects[] = {{"4:3", 1.0f}, {"16:9", 4.0f / 3.0f}, {"21:9", 7.0f / 4.0f}};
// Frame rate limit (F10 cycles these; the menu also offers 165 and 240): 60 = the game's own pacing.
const int kFpsModes[] = {60, 120, 144, 0};
std::string fps_mode_name(int v) { return v == 0 ? std::string("unlimited") : std::to_string(v); }
bool have_game_image() { return g_gpu ? ssne::gpu_renderer_output().texture != 0 : g_tex != 0; }

// ---------------------------------------------------------------- automatic internal resolution (scale = 0)
// Starts at the scale that fits the window (the GameCube image is ~448 lines), then follows the measured GPU time:
// one step down when frames take too long for the frame rate target, one step up (after a quiet period) when the
// next scale would still fit comfortably. This is what keeps weaker GPUs (integrated graphics) at full speed.
struct AutoScale {
    int cur = 0;                 // scale in use
    double gpu_ms = 0;           // last measured average GPU time per frame
    double acc_ms = 0;
    int acc_frames = 0;
    uint32_t window_start = 0, hold_up_until = 0;
    bool measurable = true;
} g_auto;

int fit_scale(SDL_Window* win) {
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(win, &w, &h);
    // Height of the game image in the window (4:3, or wider in widescreen gameplay), in GameCube lines (~448).
    float wide = g_gpu ? std::max(1.f, ssne::gpu_renderer_output().wide) : 1.f;
    double shown_h = std::min(double(h), double(w) / (4.0 / 3.0 * wide));
    int fit = std::clamp(int(shown_h / 448.0 + 0.75), 1, 6);
    if (g_set.antialiasing == "ssaa") fit = std::min(8, fit * 2);  // supersampling: twice the needed resolution
    return fit;
}

void update_auto_scale(SDL_Window* win) {
    if (!g_gpu) return;
    double ms = 0;
    int frames = 0;
    g_auto.measurable = ssne::gpu_renderer_take_gpu_time(ms, frames);
    g_auto.acc_ms += ms;
    g_auto.acc_frames += frames;
    uint32_t now = SDL_GetTicks();
    if (!g_auto.window_start) g_auto.window_start = now;
    int fit = fit_scale(win);
    if (g_set.scale != 0) g_auto.cur = g_set.scale;
    else if (g_auto.cur == 0 || g_auto.cur > fit) g_auto.cur = fit;  // start, or the window got smaller
    // Judge once a second, or after up to 3 s when the GPU is so slow that few frames finish (it must still be
    // able to step down from there).
    uint32_t elapsed = now - g_auto.window_start;
    if ((elapsed >= 1000 && g_auto.acc_frames >= 8) || (elapsed >= 3000 && g_auto.acc_frames >= 2) || elapsed >= 6000) {
        if (g_auto.acc_frames >= 2) g_auto.gpu_ms = g_auto.acc_ms / g_auto.acc_frames;
        bool judge = g_auto.acc_frames >= 2 && !ssne::paused();
        g_auto.acc_ms = 0;
        g_auto.acc_frames = 0;
        g_auto.window_start = now;
        if (g_set.scale == 0 && judge && g_auto.measurable) {
            int target_fps = ssne::frame_limit() == 0 ? 60 : std::max(60, ssne::frame_limit());
            double budget = 1000.0 / target_fps;
            double s = g_auto.cur;
            if (g_auto.gpu_ms > budget * 0.8 && g_auto.cur > 1) {
                g_auto.cur--;
                g_auto.hold_up_until = now + 30000;  // don't bounce straight back up
                char buf[128];
                snprintf(buf, sizeof(buf), "[gpu] automatic resolution: %dx (the GPU needed %.1f ms per frame)", g_auto.cur, g_auto.gpu_ms);
                log_line(buf);
            } else if (g_auto.cur < fit && now >= g_auto.hold_up_until &&
                       g_auto.gpu_ms * ((s + 1) * (s + 1)) / (s * s) < budget * 0.55) {
                g_auto.cur++;
                g_auto.hold_up_until = now + 5000;
                log_line("[gpu] automatic resolution: " + std::to_string(g_auto.cur) + "x");
            }
        }
    }
    if (ssne::gpu_renderer_scale() != g_auto.cur) ssne::gpu_renderer_set_scale(g_auto.cur);
}

void draw_game_view() {
    GLuint tex = g_tex;
    if (g_gpu) tex = ssne::gpu_renderer_output().texture;
    if (!tex) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // The GameCube image is 4:3 regardless of the EFB height (640x448/480 -> 4:3 on a TV); widescreen gameplay
    // frames are rendered wider and shown at 4:3 x factor (menus and cutscenes stay 4:3, pillarboxed).
    float factor = g_gpu ? ssne::gpu_renderer_output().wide : ssne::widescreen_frame_factor();
    float W = vp->Size.x, H = vp->Size.y, aspect = 4.f / 3.f * std::max(1.f, factor);
    float w = W, h = W / aspect;
    if (h > H) { h = H; w = H * aspect; }
    ImVec2 p0(vp->Pos.x + (W - w) * 0.5f, vp->Pos.y + (H - h) * 0.5f);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    dl->AddCallback([](const ImDrawList*, const ImDrawCmd*) { glDisable(GL_BLEND); }, nullptr);  // the game image is opaque
    dl->AddImage((ImTextureID)(intptr_t)tex, p0, ImVec2(p0.x + w, p0.y + h));
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

// ---------------------------------------------------------------- input
// Keyboard/mouse and gamepad bindings come from g_set (defaults in settings.cpp; rebind in the F12 menu).
SDL_GameController* g_pad = nullptr;
float g_mouse_dx = 0, g_mouse_dy = 0;
bool g_mouse_captured = false;
Uint32 g_capture_click = 0;  // the mouse button whose click captured the mouse: only captures (update_input)

uint8_t axis_to_u8(float v) { return uint8_t(std::clamp(128.f + v * 127.f, 0.f, 255.f)); }

void release_mouse() { g_mouse_captured = false; SDL_SetRelativeMouseMode(SDL_FALSE); }

// Radial deadzone with the remaining range rescaled to 0..1.
void deadzone(float& x, float& y, float dz) {
    float m = std::sqrt(x * x + y * y);
    if (m <= dz || m <= 0) { x = y = 0; return; }
    float k = std::min(1.f, (m - dz) / std::max(1e-3f, 1.f - dz)) / m;
    x *= k; y *= k;
}

// Development: scripted controller input for automated runs of the real executable (same syntax as the headless
// runner's --press): SSNE_AUTOPRESS="SUP@8,A@9,A@12:0.5" holds each input at t seconds for dur (default 0.2 s).
struct AutoPress { std::string name; double at, dur; };
std::vector<AutoPress> g_autopress;
std::chrono::steady_clock::time_point g_autopress_t0;
void load_autopress() {
    const char* env = getenv("SSNE_AUTOPRESS");
    if (!env) return;
    g_autopress_t0 = std::chrono::steady_clock::now();
    std::string spec = env;
    for (size_t pos = 0; pos < spec.size();) {
        size_t comma = spec.find(',', pos);
        std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t at = item.find('@'), colon = item.find(':');
        if (at != std::string::npos)
            g_autopress.push_back({item.substr(0, at), atof(item.substr(at + 1).c_str()),
                                   colon == std::string::npos ? 0.2 : atof(item.substr(colon + 1).c_str())});
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
}
void apply_autopress(ssne::PadState& p) {
    if (g_autopress.empty()) return;
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_autopress_t0).count();
    static const struct { const char* n; uint16_t b; } kNames[] = {
        {"A", ssne::PAD_A}, {"B", ssne::PAD_B}, {"X", ssne::PAD_X}, {"Y", ssne::PAD_Y}, {"START", ssne::PAD_START},
        {"Z", ssne::PAD_Z}, {"L", ssne::PAD_L}, {"R", ssne::PAD_R}, {"UP", ssne::PAD_UP}, {"DOWN", ssne::PAD_DOWN},
        {"LEFT", ssne::PAD_LEFT}, {"RIGHT", ssne::PAD_RIGHT}};
    for (const AutoPress& a : g_autopress) {
        if (t < a.at || t >= a.at + a.dur) continue;
        for (auto& k : kNames) if (a.name == k.n) p.buttons |= k.b;
        if (a.name == "SUP") p.stick_y = 230;
        if (a.name == "SDOWN") p.stick_y = 26;
        if (a.name == "SLEFT") p.stick_x = 26;
        if (a.name == "SRIGHT") p.stick_x = 230;
        if (a.name == "CLEFT") p.cstick_x = 26;
        if (a.name == "CRIGHT") p.cstick_x = 230;
        if (a.name == "R") p.trigger_r = 255;
        if (a.name.rfind("RAW", 0) == 0) {  // RAW<x>_<y>: main stick at raw bytes (128 = center)
            int x = 128, y = 128;
            sscanf(a.name.c_str() + 3, "%d_%d", &x, &y);
            p.stick_x = uint8_t(x); p.stick_y = uint8_t(y);
        }
    }
}

// Rumble: the game switches the GameCube controller's motor on and off (bursts of 0.2-0.7 s while taking damage,
// explosions...). Mirror that on the gamepad; SDL's rumble call takes a duration, so an active motor is refreshed
// before it runs out and stopped explicitly when the game stops it, the menu opens or rumble is turned off.
void update_rumble() {
    static bool on = false;
    static uint32_t refreshed = 0, test_until = 0;
    uint32_t now = SDL_GetTicks();
    if (port::menu_take_rumble_test()) test_until = now + 400;
    bool want = (now < test_until) ||
                (g_set.rumble && !port::menu_open() && !ssne::paused() && ssne::pad_motor(0) == 1);
    if (!g_pad) { on = false; return; }
    if (want && (!on || now - refreshed > 500)) {
        float k = std::clamp(g_set.rumble_strength, 0.1f, 1.f);
        // Low-frequency (heavy) motor carries most of it, like the GameCube's single eccentric motor.
        SDL_GameControllerRumble(g_pad, Uint16(0xFFFF * k), Uint16(0x9000 * k), 1000);
        on = true;
        refreshed = now;
    } else if (!want && on) {
        SDL_GameControllerRumble(g_pad, 0, 0, 0);
        on = false;
    }
}

// Mouse wheel bindings: every notch becomes one press of the bound button, held long enough for the game to see it
// (it reads the pads once per frame) with a short release in between, so fast scrolling still counts each notch.
bool g_focused = true;  // the game window has keyboard focus (pause in the background)

// On-screen button prompts: Xbox / PlayStation glyphs for the pad buttons bound to the GameCube controls (runtime
// repaints the game's button glyphs). GameCube A has two meanings with the Xbox layout: in menus and cutscenes the
// jump button selects (see update_input), in gameplay A is "use".
uint8_t prompt_glyph(const port::Bind& b, bool ps) {
    if (b.type == port::Bind::PAD_AXIS)
        return b.code == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? (ps ? ssne::PS_L2 : ssne::XB_LT)
             : b.code == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ? (ps ? ssne::PS_R2 : ssne::XB_RT) : 0;
    if (b.type != port::Bind::PAD_BUTTON) return 0;
    switch (b.code) {
    case SDL_CONTROLLER_BUTTON_A: return ps ? ssne::PS_CROSS : ssne::XB_A;
    case SDL_CONTROLLER_BUTTON_B: return ps ? ssne::PS_CIRCLE : ssne::XB_B;
    case SDL_CONTROLLER_BUTTON_X: return ps ? ssne::PS_SQUARE : ssne::XB_X;
    case SDL_CONTROLLER_BUTTON_Y: return ps ? ssne::PS_TRIANGLE : ssne::XB_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return ps ? ssne::PS_L1 : ssne::XB_LB;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return ps ? ssne::PS_R1 : ssne::XB_RB;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return ps ? ssne::PS_L3 : ssne::XB_LS;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return ps ? ssne::PS_R3 : ssne::XB_RS;
    case SDL_CONTROLLER_BUTTON_BACK: return ps ? ssne::PS_SHARE : ssne::XB_VIEW;
    case SDL_CONTROLLER_BUTTON_START: return ps ? ssne::PS_OPTIONS : ssne::XB_MENU;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return ssne::GLYPH_DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return ssne::GLYPH_DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return ssne::GLYPH_DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return ssne::GLYPH_DPAD_RIGHT;
    }
    return 0;
}

// Keyboard / mouse glyph of a binding: the keycap of a key, or the mouse button / wheel (0 = none).
uint8_t kbm_glyph(const port::Bind& b) {
    switch (b.type) {
    case port::Bind::KEY: return ssne::prompt_key_glyph(SDL_GetScancodeName(SDL_Scancode(b.code)));
    case port::Bind::WHEEL: return b.code > 0 ? ssne::MS_WHEEL_UP : ssne::MS_WHEEL_DOWN;
    case port::Bind::MOUSE:
        switch (b.code) {
        case SDL_BUTTON_LEFT: return ssne::MS_LEFT;
        case SDL_BUTTON_RIGHT: return ssne::MS_RIGHT;
        case SDL_BUTTON_MIDDLE: return ssne::MS_MIDDLE;
        case SDL_BUTTON_X1: return ssne::MS_BACK;
        case SDL_BUTTON_X2: return ssne::MS_FORWARD;
        }
        return 0;
    default: return 0;
    }
}

bool g_input_pad = false;  // the last input came from the gamepad ("automatic" on-screen buttons follow it)

void update_prompts() {
    uint8_t menu[7] = {}, control[7] = {};
    static const int kSlots[7] = {port::ACT_A, port::ACT_B, port::ACT_X, port::ACT_Y, port::ACT_Z, port::ACT_L, port::ACT_R};
    std::string style = g_set.prompts;
    if (style == "auto") {
        style = "keyboard";
        if (g_pad && g_input_pad) {
            SDL_GameControllerType t = SDL_GameControllerGetType(g_pad);
            style = (t == SDL_CONTROLLER_TYPE_PS3 || t == SDL_CONTROLLER_TYPE_PS4 || t == SDL_CONTROLLER_TYPE_PS5) ? "playstation" : "xbox";
        }
    }
    if (style == "keyboard") {
        // Gameplay: each control's first binding with a glyph. Menus, the pause screen and cutscenes only take keys
        // (the mouse clicks, goes back and scrolls there, see update_input): A is a jump key (they select there) and
        // B is Esc when it pauses (it goes back there).
        auto first = [](const port::Bind* b, bool keys_only) {
            for (int k = 0; k < port::Settings::kKbSlots; k++)
                if (uint8_t g = (keys_only && b[k].type != port::Bind::KEY) ? 0 : kbm_glyph(b[k])) return g;
            return uint8_t(0);
        };
        for (int s = 0; s < 7; s++) {
            control[s] = first(g_set.kb[kSlots[s]], false);
            menu[s] = first(g_set.kb[kSlots[s]], true);
        }
        if (uint8_t g = first(g_set.kb[port::ACT_L], true)) menu[0] = g;
        for (const port::Bind& b : g_set.kb[port::ACT_START])
            if (b == port::Bind::key(SDL_SCANCODE_ESCAPE)) menu[1] = kbm_glyph(b);
    } else if (style != "gamecube") {
        bool ps = style == "playstation";
        for (int s = 0; s < 7; s++) menu[s] = control[s] = prompt_glyph(g_set.pad[kSlots[s]], ps);
        if (g_set.pad[port::ACT_L].type == port::Bind::PAD_BUTTON) menu[0] = prompt_glyph(g_set.pad[port::ACT_L], ps);
    }
    ssne::set_button_prompts(menu, control);
}

struct WheelPulse {
    int pending = 0;   // notches not yet turned into presses
    float t = 0;       // time left in the current phase
    int phase = 0;     // 0 idle, 1 pressed, 2 released gap
    bool step(float dt) {  // advances; true while the button is pressed
        if (phase != 0) t -= dt;
        if (phase == 1 && t <= 0) { phase = 2; t = 0.05f; }
        if (phase == 2 && t <= 0) phase = 0;
        if (phase == 0 && pending > 0) { pending--; phase = 1; t = 0.1f; }
        return phase == 1;
    }
};
WheelPulse g_wheel[2];  // [0] down, [1] up

void update_input(float dt) {
    ssne::PadState p;
    if (port::menu_open()) {  // the game gets a neutral controller while the menu is open
        g_mouse_dx = g_mouse_dy = 0;
        g_wheel[0] = g_wheel[1] = WheelPulse{};
        ssne::set_pad(0, p);
        return;
    }
    float lx = 0, ly = 0, cx = 0, cy = 0, lt = 0, rt = 0;
    float act[port::ACT_COUNT] = {};  // 0..1 per control
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    Uint32 mb = g_mouse_captured ? SDL_GetMouseState(nullptr, nullptr) : 0;
    bool kb_ok = !ImGui::GetIO().WantCaptureKeyboard;
    bool wheel_on[2] = {g_wheel[0].step(dt), g_wheel[1].step(dt)};
    // The game jumps with the (analog) L trigger and selects / skips with A. Outside player control (menus,
    // cutscenes, the pause screen) some inputs switch to menu duties:
    //  - keys and pad buttons bound to jump press A, so Space (or the Xbox layout's A button) jumps and selects,
    //  - Escape bound to Start (pause) presses B (back): it opens the pause menu in gameplay and closes it again,
    //  - the mouse works the menus instead of its bindings: left click selects (A), right click goes back (B) and
    //    the wheel moves the selection (stick up/down).
    // Those inputs, if still held when control changes, only count once released: firing or jumping as a cutscene
    // starts doesn't skip it, the Esc that paused doesn't close the pause menu, the click that starts a level
    // doesn't also fire.
    const bool control = ssne::player_control_active();
    static bool was_control = true;
    static Uint32 held_mouse = 0;
    static bool held_jump = false, held_esc = false, held_pad_jump = false;
    bool jump_key = false;
    for (const port::Bind& b : g_set.kb[port::ACT_L])
        if (b.type == port::Bind::KEY && kb_ok && b.code > 0 && b.code < SDL_NUM_SCANCODES && k[b.code]) jump_key = true;
    const bool esc = kb_ok && k[SDL_SCANCODE_ESCAPE];
    const port::Bind& pj = g_set.pad[port::ACT_L];
    const bool pad_jump = g_pad && pj.type == port::Bind::PAD_BUTTON &&
                          SDL_GameControllerGetButton(g_pad, SDL_GameControllerButton(pj.code));
    if (control != was_control) { held_mouse = mb; held_jump = jump_key; held_esc = esc; held_pad_jump = pad_jump; }
    was_control = control;
    held_mouse |= g_capture_click;
    g_capture_click = 0;
    held_mouse &= mb;
    held_jump &= jump_key;
    held_esc &= esc;
    held_pad_jump &= pad_jump;
    for (int a = 0; a < port::ACT_COUNT; a++)
        for (const port::Bind& b : g_set.kb[a]) {
            if (b.type == port::Bind::KEY && kb_ok && b.code > 0 && b.code < SDL_NUM_SCANCODES && k[b.code]) {
                if (a == port::ACT_START && b.code == SDL_SCANCODE_ESCAPE) { if (!held_esc) act[control ? a : port::ACT_B] = 1; }
                else if (a == port::ACT_L) { if (!held_jump) act[control ? a : port::ACT_A] = 1; }
                else act[a] = 1;
            }
            if (b.type == port::Bind::MOUSE && control && (mb & ~held_mouse & SDL_BUTTON(b.code))) act[a] = 1;
            if (b.type == port::Bind::WHEEL && control && wheel_on[b.code > 0 ? 1 : 0]) act[a] = 1;
        }
    if (!control) {
        Uint32 clicks = mb & ~held_mouse;
        if (clicks & SDL_BUTTON(SDL_BUTTON_LEFT)) act[port::ACT_A] = 1;
        if (clicks & SDL_BUTTON(SDL_BUTTON_RIGHT)) act[port::ACT_B] = 1;
        if (wheel_on[1]) ly += 1;
        if (wheel_on[0]) ly -= 1;
    }
    // Weapon wheel: holding X opens the game's weapon wheel after WeaponWheelDelay (0.25 s, base.cfg), which by
    // default pauses the game, and the wheel picks the weapon in the main stick's direction. A key / mouse hold of X
    // that starts in gameplay keeps X pressed until it is released (the pause would otherwise end player control and
    // hand the mouse buttons to the menus), and after the delay the mouse steers a virtual main stick instead of the
    // view: move towards a weapon, release the key / button to take it. Only on foot: in vehicles X does other things
    // and the main stick drives.
    static bool wheel_hold = false, x_was_down = false;
    static float wheel_t = 0, wheel_vx = 0, wheel_vy = 0;
    bool x_down = false, x_fresh = false;  // fresh: not a button that only captured the mouse / was held over
    for (const port::Bind& b : g_set.kb[port::ACT_X]) {
        if (b.type == port::Bind::KEY && kb_ok && b.code > 0 && b.code < SDL_NUM_SCANCODES && k[b.code]) x_down = x_fresh = true;
        if (b.type == port::Bind::MOUSE && (mb & SDL_BUTTON(b.code))) {
            x_down = true;
            if (!(held_mouse & SDL_BUTTON(b.code))) x_fresh = true;
        }
    }
    if (!x_down) wheel_hold = false;
    else if (!x_was_down && x_fresh && ssne::mouse_aim_active()) { wheel_hold = true; wheel_t = 0; wheel_vx = wheel_vy = 0; }
    x_was_down = x_down;
    if (wheel_hold) { act[port::ACT_X] = 1; wheel_t += dt; }
    const bool wheel_mouse = wheel_hold && wheel_t >= 0.25f && g_mouse_captured;
    if (wheel_mouse) {
        constexpr float kRadius = 100.f;  // mouse counts to full tilt; also how far back a change of mind takes
        wheel_vx += g_mouse_dx;
        wheel_vy -= g_mouse_dy;
        float len = std::hypot(wheel_vx, wheel_vy);
        if (len > kRadius) { wheel_vx *= kRadius / len; wheel_vy *= kRadius / len; len = kRadius; }
        if (len > kRadius * 0.35f) { lx += wheel_vx / len; ly += wheel_vy / len; }
        g_mouse_dx = g_mouse_dy = 0;
    }
    if (g_pad) {
        for (int a = port::kFirstButtonAction; a < port::ACT_COUNT; a++) {
            const port::Bind& b = g_set.pad[a];
            float v = 0;
            if (b.type == port::Bind::PAD_BUTTON) v = SDL_GameControllerGetButton(g_pad, SDL_GameControllerButton(b.code)) ? 1.f : 0.f;
            else if (b.type == port::Bind::PAD_AXIS) v = std::max(0.f, SDL_GameControllerGetAxis(g_pad, SDL_GameControllerAxis(b.code)) / 32767.f);
            int target = a;
            if (a == port::ACT_L && b.type == port::Bind::PAD_BUTTON) {
                if (held_pad_jump) continue;
                if (!control) target = port::ACT_A;
            }
            act[target] = std::max(act[target], v);
        }
        auto ax = [](SDL_GameControllerAxis a) { return SDL_GameControllerGetAxis(g_pad, a) / 32767.f; };
        float mx = ax(SDL_CONTROLLER_AXIS_LEFTX), my = -ax(SDL_CONTROLLER_AXIS_LEFTY);
        float rx = ax(SDL_CONTROLLER_AXIS_RIGHTX), ry = -ax(SDL_CONTROLLER_AXIS_RIGHTY);
        if (g_set.swap_sticks) { std::swap(mx, rx); std::swap(my, ry); }
        deadzone(mx, my, g_set.pad_deadzone);
        deadzone(rx, ry, g_set.pad_deadzone);
        lx += mx; ly += my;
        cx += rx * g_set.look_sens;
        cy += (g_set.pad_invert_y ? -ry : ry) * g_set.look_sens;
    }
    ly += act[port::ACT_MOVE_UP] - act[port::ACT_MOVE_DOWN];
    // Vehicles steer with the C-stick (the main stick's X does nothing there): the strafe keys steer while driving.
    (ssne::player_in_vehicle() ? cx : lx) += act[port::ACT_MOVE_RIGHT] - act[port::ACT_MOVE_LEFT];
    cy += act[port::ACT_LOOK_UP] - act[port::ACT_LOOK_DOWN];
    cx += act[port::ACT_LOOK_RIGHT] - act[port::ACT_LOOK_LEFT];
    struct { int a; uint16_t m; } map[] = {
        {port::ACT_A, ssne::PAD_A}, {port::ACT_B, ssne::PAD_B}, {port::ACT_X, ssne::PAD_X}, {port::ACT_Y, ssne::PAD_Y},
        {port::ACT_Z, ssne::PAD_Z}, {port::ACT_START, ssne::PAD_START},
        {port::ACT_DPAD_UP, ssne::PAD_UP}, {port::ACT_DPAD_DOWN, ssne::PAD_DOWN},
        {port::ACT_DPAD_LEFT, ssne::PAD_LEFT}, {port::ACT_DPAD_RIGHT, ssne::PAD_RIGHT}};
    for (auto& m : map) if (act[m.a] >= 0.5f) p.buttons |= m.m;
    // Weapon groups 1-9 (number keys by default), on foot only: the runtime picks and switches the weapon.
    static bool weapon_was[9] = {};
    for (int i = 0; i < 9; i++) {
        bool on = act[port::ACT_WEAPON1 + i] >= 0.5f;
        if (on && !weapon_was[i] && ssne::mouse_aim_active()) ssne::weapon_select_group(i);
        weapon_was[i] = on;
    }
    lt = act[port::ACT_L];
    rt = act[port::ACT_R];
    if (g_mouse_captured && g_set.mouse_look && g_set.mouse_aim && ssne::mouse_aim_active()) {
        // Direct aim: mouse counts -> radians, applied to the player's view by the runtime (no C-stick).
        const double k = 0.0012 * g_set.mouse_sens;
        ssne::mouse_aim_add(g_mouse_dx * k, (g_set.mouse_invert_y ? g_mouse_dy : -g_mouse_dy) * k);
    } else if (g_mouse_captured && g_set.mouse_look) {
        // Mouse motion drives the C-stick (menus, or direct aim off); scaled per frame so speed doesn't depend on frame rate.
        float sens = 0.02f * g_set.mouse_sens;
        cx += std::clamp(g_mouse_dx * sens / std::max(dt * 60.f, 0.25f), -1.f, 1.f);
        float dy = std::clamp(g_mouse_dy * sens / std::max(dt * 60.f, 0.25f), -1.f, 1.f);
        cy += g_set.mouse_invert_y ? dy : -dy;
    }
    g_mouse_dx = g_mouse_dy = 0;
    p.stick_x = axis_to_u8(std::clamp(lx, -1.f, 1.f));
    p.stick_y = axis_to_u8(std::clamp(ly, -1.f, 1.f));
    p.cstick_x = axis_to_u8(std::clamp(cx, -1.f, 1.f));
    p.cstick_y = axis_to_u8(std::clamp(cy, -1.f, 1.f));
    p.trigger_l = uint8_t(std::clamp(lt, 0.f, 1.f) * 255);
    p.trigger_r = uint8_t(std::clamp(rt, 0.f, 1.f) * 255);
    float click = std::min(g_set.trigger_threshold, 0.999f);
    if (lt >= click) p.buttons |= ssne::PAD_L;   // full press = digital click, as on the real controller
    if (rt >= click) p.buttons |= ssne::PAD_R;
    apply_autopress(p);
    ssne::set_pad(0, p);
}

// ---------------------------------------------------------------- audio
// The emulated AI DMA plays the game's mix in real time into a FIFO; the SDL callback drains it. The two clocks
// drift slightly, so the callback keeps the FIFO between ~40 and ~200 ms: it skips ahead when too much is queued
// and re-buffers a little after an underrun.
std::atomic<int> g_volume{100};
std::atomic<bool> g_muted{false};
SDL_AudioDeviceID g_audio_dev = 0;

void audio_callback(void*, Uint8* stream, int len) {
    static bool buffering = true;
    auto* out = reinterpret_cast<int16_t*>(stream);
    uint32_t frames = uint32_t(len / 4);
    uint32_t rate = ssne::audio_rate();
    uint32_t queued = ssne::audio_buffered();
    if (queued > rate * 200 / 1000) {  // too much latency: drop the oldest audio
        static std::vector<int16_t> sink;
        uint32_t drop = queued - rate * 80 / 1000;
        sink.resize(size_t(drop) * 2);
        ssne::audio_pull(sink.data(), drop);
        queued -= drop;
    }
    if (ssne::paused()) {  // game frozen by the settings menu: discard whatever the AI keeps producing
        static std::vector<int16_t> sink;
        sink.resize(size_t(queued) * 2);
        ssne::audio_pull(sink.data(), queued);
        memset(stream, 0, size_t(len));
        buffering = true;
        return;
    }
    if (buffering && queued < frames + rate * 40 / 1000) { memset(stream, 0, size_t(len)); return; }
    buffering = false;
    uint32_t got = ssne::audio_pull(out, frames);
    if (got < frames) buffering = true;
    int vol = g_muted ? 0 : g_volume.load();
    if (vol != 100)
        for (uint32_t i = 0; i < frames * 2; i++) out[i] = int16_t(std::clamp(int32_t(out[i]) * vol / 100, -32768, 32767));
}

void open_audio() {
    SDL_AudioSpec want{}, have{};
    want.freq = 32000;  // AX mixes at 32 kHz
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = audio_callback;
    g_audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);  // SDL converts to the device's format
    if (!g_audio_dev) { log_line(std::string("[audio] no audio device: ") + SDL_GetError()); return; }
    ssne::audio_set_sink_active(true);
    SDL_PauseAudioDevice(g_audio_dev, 0);
    log_line("[audio] output: " + std::string(SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?") +
             ", 32000 Hz stereo");
}

// ---------------------------------------------------------------- overlay (F1)
bool g_show_overlay = false;

// ---------------------------------------------------------------- window, screenshots

// Window position / size from settings.ini, if it is still on a connected display; else centered on the chosen
// (or first) display.
void initial_window_geometry(int& x, int& y, int& w, int& h) {
    w = g_set.win_w; h = g_set.win_h;
    int n = SDL_GetNumVideoDisplays();
    if (g_set.win_x != -1 || g_set.win_y != -1) {
        for (int d = 0; d < n; d++) {
            SDL_Rect b{};
            if (SDL_GetDisplayUsableBounds(d, &b) != 0) continue;
            // the title bar area must be on this display
            if (g_set.win_x + 40 >= b.x && g_set.win_x + 40 < b.x + b.w && g_set.win_y >= b.y - 8 && g_set.win_y < b.y + b.h - 40 &&
                (g_set.display < 0 || g_set.display == d)) {
                x = g_set.win_x; y = g_set.win_y;
                return;
            }
        }
    }
    int d = g_set.display >= 0 && g_set.display < n ? g_set.display : 0;
    x = int(SDL_WINDOWPOS_CENTERED_DISPLAY(d));
    y = int(SDL_WINDOWPOS_CENTERED_DISPLAY(d));
}

void remember_window(SDL_Window* win) {
    Uint32 f = SDL_GetWindowFlags(win);
    if (f & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED)) return;
    g_set.win_max = (f & SDL_WINDOW_MAXIMIZED) != 0;
    if (g_set.win_max) return;  // keep the restored size and position
    SDL_GetWindowPosition(win, &g_set.win_x, &g_set.win_y);
    SDL_GetWindowSize(win, &g_set.win_w, &g_set.win_h);
}

void toast(const std::string& s);

// Screenshot of the game image (without the menus / info bar), PNG in Screenshots next to the game. The PNG is
// encoded on a worker thread so the game doesn't hitch.
void take_screenshot(const std::string& base_dir) {
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    if (g_gpu) { if (!ssne::gpu_renderer_read_output(px, w, h)) px.clear(); }
    else if (!g_pixels.empty()) { px = g_pixels; w = g_tex_w; h = g_tex_h; }
    if (px.empty() || w <= 0 || h <= 0) { toast("No game image to capture yet"); return; }
    std::time_t t = std::time(nullptr);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char name[64];
    std::strftime(name, sizeof(name), "SillyReEncounter_%Y-%m-%d_%H-%M-%S", &lt);
    std::string path = base_dir + "Screenshots/" + name;
    static int same_second = 0;
    static std::string last_name;
    if (last_name == name) path += "_" + std::to_string(++same_second); else same_second = 0;
    last_name = name;
    path += ".png";
    toast("Screenshot: Screenshots/" + path.substr(path.rfind('/') + 1));
    std::thread([px = std::move(px), w, h, path] {
        if (!port::write_png(path, px.data(), w, h)) log_line("[port] could not write " + path);
    }).detach();
}

// ---------------------------------------------------------------- save states

std::string g_states_dir;
std::string g_toast;
uint32_t g_toast_until = 0;
int g_state_slot_busy = 0;  // slot of the request in flight (thumbnail after a save)

void toast(const std::string& s) {
    g_toast = s;
    g_toast_until = SDL_GetTicks() + 2500;
    log_line("[port] " + s);
}

std::string state_path(int slot) { return g_states_dir + "slot" + std::to_string(slot) + ".state"; }
std::string thumb_path(int slot) { return g_states_dir + "slot" + std::to_string(slot) + ".bmp"; }

std::string state_time(int slot, bool& exists) {
    uint64_t t = 0;
    exists = ssne::savestate_read_info(state_path(slot), t);
    if (!exists) return "";
    std::time_t tt = std::time_t(t);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &lt);
    return buf;
}

std::string state_slot_note(int slot) {
    bool exists;
    std::string when = state_time(slot, exists);
    return exists ? " (" + when + ")" : " (empty)";
}

void request_state(bool save, int slot) {
    if (!g_ssne_has_game_code || !have_game_image()) { toast("Save states are available once the game is running"); return; }
    if (ssne::savestate_pending()) return;
    if (!save) {
        bool exists;
        state_time(slot, exists);
        if (!exists) { toast("Slot " + std::to_string(slot) + " is empty"); return; }
    }
    g_state_slot_busy = slot;
    ssne::savestate_request(save, state_path(slot));
}

// Thumbnail of the current game image (what the player sees), ~192 px wide, next to the state file.
void save_thumbnail(int slot) {
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    if (g_gpu) { if (!ssne::gpu_renderer_read_output(px, w, h)) return; }
    else { px = g_pixels; w = g_tex_w; h = g_tex_h; }
    if (w <= 0 || h <= 0 || px.size() < size_t(w) * h) return;
    int tw = 192, th = std::max(1, h * tw / w);
    SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormat(0, tw, th, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!sf) return;
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            // box filter over the source pixels this thumbnail pixel covers
            int x0 = x * w / tw, x1 = std::max(x0 + 1, (x + 1) * w / tw), y0 = y * h / th, y1 = std::max(y0 + 1, (y + 1) * h / th);
            uint32_t acc[3] = {}, n = 0;
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++) {
                    uint32_t c = px[size_t(sy) * w + sx];
                    acc[0] += c & 255; acc[1] += (c >> 8) & 255; acc[2] += (c >> 16) & 255; n++;
                }
            uint32_t c = (acc[0] / n) | (acc[1] / n) << 8 | (acc[2] / n) << 16 | 0xFF000000u;
            memcpy((uint8_t*)sf->pixels + size_t(y) * sf->pitch + size_t(x) * 4, &c, 4);
        }
    SDL_SaveBMP(sf, thumb_path(slot).c_str());
    SDL_FreeSurface(sf);
}

void handle_state_result() {
    ssne::SavestateResult res;
    if (!ssne::savestate_take_result(res)) return;
    toast("Slot " + std::to_string(g_state_slot_busy) + ": " + res.message);
    if (res.save && res.ok) save_thumbnail(g_state_slot_busy);
}

// Menu: slot dates and thumbnails (textures reloaded when the file changes).
void fill_state_slots(port::MenuInfo& mi) {
    static GLuint tex[5] = {};
    static std::filesystem::file_time_type stamp[5];
    mi.states_available = g_ssne_has_game_code && have_game_image();
    mi.states_dir = g_states_dir;
    static uint32_t last = 0;
    static port::MenuInfo::Slot cache[5];
    uint32_t now = SDL_GetTicks();
    if (now - last > 500 || last == 0) {
        last = now;
        for (int i = 0; i < 5; i++) {
            port::MenuInfo::Slot& sl = cache[i];
            sl.when = state_time(i + 1, sl.exists);
            std::error_code ec;
            auto p = std::filesystem::u8path(thumb_path(i + 1));
            auto t = std::filesystem::last_write_time(p, ec);
            if (ec || !sl.exists) { sl.tex = 0; continue; }
            if (tex[i] && t == stamp[i]) { sl.tex = tex[i]; continue; }
            SDL_Surface* bmp = SDL_LoadBMP(thumb_path(i + 1).c_str());
            SDL_Surface* rgba = bmp ? SDL_ConvertSurfaceFormat(bmp, SDL_PIXELFORMAT_ABGR8888, 0) : nullptr;
            if (bmp) SDL_FreeSurface(bmp);
            if (!rgba) { sl.tex = 0; continue; }
            if (!tex[i]) glGenTextures(1, &tex[i]);
            glBindTexture(GL_TEXTURE_2D, tex[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, rgba->pitch / 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            sl.w = rgba->w; sl.h = rgba->h;
            SDL_FreeSurface(rgba);
            stamp[i] = t;
            sl.tex = tex[i];
        }
    }
    for (int i = 0; i < 5; i++) mi.slots[i] = cache[i];
}

void draw_overlay(const std::string& data_dir, float dt) {
    static uint64_t last_copies = 0;
    static float fps_timer = 0, fps = 0;
    auto rs = ssne::gx_render_stats();
    auto hw = ssne::hw_stats();
    auto st = ssne::scheduler_stats();
    fps_timer += dt;
    if (fps_timer >= 1.0f) { fps = float(rs.display_copies - last_copies) / fps_timer; last_copies = rs.display_copies; fps_timer = 0; }

    if (g_set.top_bar == 1) {  // frame rate only
        ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGui::Begin("##fpsonly", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("%.0f fps%s", fps, ssne::paused() ? "  [paused]" : "");
        ImGui::End();
    }
    if (g_set.top_bar == 2) {
        ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGui::Begin("##fps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("%.0f fps (limit %s)   F12: settings & hotkeys   F1: log/stats   Shift+F1: smaller bar%s", fps,
                    fps_mode_name(ssne::frame_limit()).c_str(), ssne::paused() ? "   [paused]" : "");
        if (!g_gpu)  // make the (much slower) CPU renderer obvious: it explains low frame rates in bug reports
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1), "Software renderer (CPU): %s",
                               g_set.renderer == "software" ? "selected in F12 > Graphics" : "the GPU renderer could not start, see F1");
        ImGui::End();
    }
    if (SDL_GetTicks() < g_toast_until) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + 56), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowBgAlpha(0.7f);
        ImGui::Begin("##toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextUnformatted(g_toast.c_str());
        ImGui::End();
    }
    if (!have_game_image() || g_show_overlay) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 20, vp->Pos.y + 50), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(760, 420), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("Silly ReEncounter - development build");
        {
            std::lock_guard<std::mutex> lk(g_log_mutex);
            ImGui::TextWrapped("%s", g_status.c_str());
        }
        ImGui::Text("Frames %llu  |  triangles %llu  |  textures %llu  |  disc reads %llu (%llu KB)  |  interrupts %llu, threads %d",
                    (unsigned long long)rs.display_copies, (unsigned long long)rs.triangles, (unsigned long long)rs.textures_decoded,
                    (unsigned long long)hw.disc_reads, (unsigned long long)(hw.disc_bytes / 1024), (unsigned long long)st.interrupts, st.threads);
        ImGui::Text("Data: %s   Recompiled functions: %zu   Controller: %s", data_dir.c_str(), g_ssne_func_count,
                    g_pad ? SDL_GameControllerName(g_pad) : "keyboard/mouse");
        {
            auto au = ssne::audio_stats();
            ImGui::Text("Audio: %s  volume %d%% (F3 mute, F4/F5 -/+)  |  %u voices  |  %u ms queued  |  %llu underruns",
                        g_audio_dev ? (g_muted ? "muted" : "on") : "no device", g_volume.load(), au.active_voices,
                        ssne::audio_buffered() * 1000 / std::max(1u, ssne::audio_rate()), (unsigned long long)au.underruns);
        }
        if (g_gpu) {
            auto gs = ssne::gpu_renderer_stats();
            auto o = ssne::gpu_renderer_output();
            ImGui::Text("Renderer: GPU %dx (%dx%d, F6/F7 to change) | %s | %.1f ms/frame, %llu draws, %llu shaders",
                        ssne::gpu_renderer_scale(), o.w, o.h, ssne::gpu_renderer_info().c_str(), gs.frame_ms,
                        (unsigned long long)gs.draws, (unsigned long long)gs.programs);
        } else {
            ImGui::Text("Renderer: software (CPU)");
        }
        {
            std::string mc = ssne::exi_memcard_path();
            ImGui::Text("Memory card A: %s", mc.empty() ? "none" : mc.c_str());
        }
        ImGui::Separator();
        ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard<std::mutex> lk(g_log_mutex);
            ImGuiListClipper clip;
            clip.Begin(int(g_log.size()));
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) ImGui::TextUnformatted(g_log[size_t(i)].c_str());
            if (g_scroll_to_bottom) { ImGui::SetScrollHereY(1.0f); g_scroll_to_bottom = false; }
        }
        ImGui::EndChild();
        ImGui::End();
    }
}

// Applies g_set to the running game/renderer/window. `win` may be null before the window exists.
void apply_settings(SDL_Window* win) {
    ssne::set_widescreen(kAspects[std::clamp(g_set.aspect, 0, 2)].factor);
    {
        const std::string& aa = g_set.antialiasing;
        int msaa = aa == "msaa2" ? 2 : aa == "msaa4" ? 4 : aa == "msaa8" ? 8 : 1;
        ssne::gpu_renderer_set_antialiasing(msaa, aa == "fxaa", aa != "off");
    }
    ssne::gpu_renderer_set_anisotropy(g_set.anisotropy);
    ssne::set_frame_limit(g_set.fps_limit);
    ssne::set_fov(float(g_set.fov));
    ssne::set_skip_intros(g_set.skip_intros);
    g_volume = g_set.volume;
    g_muted = g_set.muted;
    ssne::audio_set_hq(g_set.audio_hq);
    ssne::audio_set_mix(g_set.music_volume, g_set.sfx_volume);
    if (win) {
        SDL_GL_SetSwapInterval(g_set.vsync ? 1 : 0);
        bool fs = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP;
        // Monitor choice: fullscreen goes to the display the window is on, so move the window there first.
        static int applied_display = -2;
        int want = g_set.display < SDL_GetNumVideoDisplays() ? g_set.display : -1;
        bool display_changed = want != applied_display;
        applied_display = want;
        if (want >= 0 && SDL_GetWindowDisplayIndex(win) != want && (display_changed || g_set.fullscreen != fs)) {
            if (fs) { SDL_SetWindowFullscreen(win, 0); fs = false; }
            SDL_SetWindowPosition(win, int(SDL_WINDOWPOS_CENTERED_DISPLAY(want)), int(SDL_WINDOWPOS_CENTERED_DISPLAY(want)));
        }
        if (fs != g_set.fullscreen) SDL_SetWindowFullscreen(win, g_set.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        if (g_gpu && g_set.scale != 0 && ssne::gpu_renderer_scale() != g_set.scale) ssne::gpu_renderer_set_scale(g_set.scale);
    }
}

void save_now() {
    if (!port::save_settings(g_set, g_settings_path)) log_line("[port] could not save " + g_settings_path);
}

} // namespace

int main(int argc, char** argv) {
    std::string data_dir = "game_data";
    bool headless = false;
    std::string memcard;  // default: saves/MemoryCardA.<USA|EUR>.raw next to the executable (by the disc's region)
    std::string base_dir;
    {
        char* base = SDL_GetBasePath();
        base_dir = base ? base : "";
        SDL_free(base);
    }
    g_settings_path = base_dir + "settings.ini";
    bool have_file = port::load_settings(g_set, g_settings_path);
    // Command-line options override settings.ini for this run (and are saved if the menu changes anything).
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--data" && i + 1 < argc) data_dir = argv[++i];
        else if (a == "--headless") headless = true;
        else if (a == "--fullscreen") g_set.fullscreen = true;
        else if (a == "--volume" && i + 1 < argc) g_set.volume = std::clamp(atoi(argv[++i]), 0, 200);
        else if (a == "--mute") g_set.muted = true;
        else if (a == "--memcard" && i + 1 < argc) memcard = argv[++i];
        else if (a == "--no-memcard") memcard = "-";
        else if (a == "--renderer" && i + 1 < argc) g_set.renderer = argv[++i];
        else if (a == "--aspect" && i + 1 < argc) {
            std::string v = argv[++i];
            for (int k = 0; k < 3; k++) if (v == kAspects[k].name) g_set.aspect = k;
        }
        else if (a == "--scale" && i + 1 < argc) g_set.scale = std::clamp(atoi(argv[++i]), 0, 8);  // 0 = automatic
        else if (a == "--fov" && i + 1 < argc) g_set.fov = std::clamp(atoi(argv[++i]), 45, 110);
        else if (a == "--fps" && i + 1 < argc) {
            std::string v = argv[++i];
            g_set.fps_limit = v == "unlimited" ? 0 : std::max(0, atoi(v.c_str()));
        }
    }
    ssne::set_fatal_handler(throw_fatal);
    if (memcard.empty()) {
        // One card per region, like Dolphin (a PAL save isn't a USA save): the disc ID's 4th letter is the region.
        std::string region = "USA";
        if (FILE* bf = fopen((data_dir + "/sys/boot.bin").c_str(), "rb")) {
            char id[4] = {};
            if (fread(id, 1, 4, bf) == 4 && (id[3] == 'P' || id[3] == 'D' || id[3] == 'F')) region = "EUR";
            else if (id[3] == 'J') region = "JAP";
            fclose(bf);
        }
        memcard = base_dir + "saves/MemoryCardA." + region + ".raw";
    }
    ssne::exi_configure_memcard(memcard == "-" ? std::string() : memcard);
    {   // save states: states/<disc ID>/slotN.state (+ .bmp thumbnail)
        std::string id = "unknown";
        if (FILE* bf = fopen((data_dir + "/sys/boot.bin").c_str(), "rb")) {
            char b[7] = {};
            if (fread(b, 1, 6, bf) == 6) id = b;
            fclose(bf);
        }
        g_states_dir = base_dir + "states/" + id + "/";
    }
    apply_settings(nullptr);
#ifdef _WIN32
    timeBeginPeriod(1);  // 1 ms sleeps for the audio/VI/alarm timers (default is ~15.6 ms)
#endif
    g_log_file = fopen("ssne.log", "w");
    ssne::set_log_sink([](const char* line) { log_line(line); });
    log_line(have_file ? "[port] settings: " + g_settings_path : "[port] settings: defaults (" + g_settings_path + " will be created)");
    if (!have_file) save_now();

    if (headless) {
        guest_main(data_dir);
        return 0;
    }

    // Rumble on PlayStation controllers goes through SDL's HIDAPI drivers, which keep it off unless asked.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (getenv("SSNE_VIRTUAL_PAD")) {  // development: a virtual gamepad that logs rumble (testing without hardware)
        SDL_VirtualJoystickDesc d{};
        d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        d.naxes = SDL_CONTROLLER_AXIS_MAX;
        d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
        d.name = "Virtual test pad";
        d.Rumble = [](void*, Uint16 lo, Uint16 hi) -> int {
            static Uint16 last_lo = 1, last_hi = 1;
            if (lo != last_lo || hi != last_hi) log_line("[dev] virtual pad rumble " + std::to_string(lo) + " " + std::to_string(hi));
            last_lo = lo; last_hi = hi;
            return 0;
        };
        if (SDL_JoystickAttachVirtualEx(&d) < 0) log_line(std::string("[dev] virtual pad failed: ") + SDL_GetError());
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)  // no audio device: play silently
        log_line(std::string("[audio] audio unavailable: ") + SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    int wx, wy, ww, wh;
    initial_window_geometry(wx, wy, ww, wh);
    SDL_Window* win = SDL_CreateWindow("Silly ReEncounter", wx, wy, ww, wh,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                           (g_set.win_max ? SDL_WINDOW_MAXIMIZED : 0));
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    bool core = gl != nullptr;
    if (!gl) {  // no GL 3.3 core: fall back to a basic context and the software renderer
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        gl = SDL_GL_CreateContext(win);
        std::string have = "none";
        if (gl) {
            const char* v = reinterpret_cast<const char*>(glGetString(GL_VERSION));
            const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            have = std::string(v ? v : "?") + " on " + (r ? r : "?");
        }
        log_line("[gpu] OpenGL 3.3 core not available (driver offers " + have + "); using the software renderer. "
                 "Updating the graphics driver usually fixes this.");
    }
    if (!gl) { fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError()); return 1; }
    if (core && g_set.renderer != "software") {
        std::string err;
        {
            std::string dir = base_dir + "shadercache";
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
            if (!ec) ssne::gpu_renderer_set_shader_cache(dir);
        }
        int start_scale = g_set.scale ? g_set.scale : fit_scale(win);
        if (ssne::gpu_renderer_init(SDL_GL_GetProcAddress, start_scale, err)) {
            g_gpu = true;
            g_auto.cur = start_scale;
            ssne::gx_use_gpu(true);
            log_line("[gpu] " + ssne::gpu_renderer_info() + ", internal resolution " + std::to_string(start_scale) + "x" +
                     (g_set.scale ? "" : " (automatic)"));
        } else {
            log_line("[gpu] GPU renderer unavailable (" + err + "); using the software renderer");
        }
    }
    ssne::gx_set_threaded(g_set.gx_thread);
    ssne::gx_set_gpu_vertex(g_set.gpu_vertex);
    log_line(g_set.gx_thread && !getenv("SSNE_GX_SYNC") ? "[gx] graphics commands on their own thread"
                                                        : "[gx] graphics commands on the game's thread (gx_thread = 0)");
    apply_settings(win);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // the settings menu works with a controller
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(win, gl);
    ImGui_ImplOpenGL3_Init(core ? "#version 330 core" : "#version 130");

    open_audio();
    std::thread(watchdog).detach();
    ssne::spawn_guest_thread([data_dir] { guest_main(data_dir); });  // never returns (game loop)

    load_autopress();
    if (const char* t = getenv("SSNE_OPEN_MENU")) {  // development: open the menu on a tab at startup (screenshots)
        port::menu_set_open(true);
        port::menu_select_tab(atoi(t));
    }
    auto toggle_menu = [&] {
        port::menu_set_open(!port::menu_open());
        if (port::menu_open()) release_mouse();
        else save_now();
    };
    bool quit = false;
    uint32_t last = SDL_GetTicks();
    double swap_ms = 100;  // how long the last SwapWindow blocked (vsync); ~0 when it doesn't wait
    while (!quit) {
        SDL_Event e;
        bool changed = false;  // settings changed by hotkeys/menu this frame
        while (SDL_PollEvent(&e)) {
            if (port::menu_handle_event(e, g_set, changed)) continue;  // rebinding in progress
            if (e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEWHEEL) g_input_pad = false;
            else if (e.type == SDL_CONTROLLERBUTTONDOWN || (e.type == SDL_CONTROLLERAXISMOTION && std::abs(e.caxis.value) > 16000))
                g_input_pad = true;
            ImGui_ImplSDL2_ProcessEvent(&e);
            switch (e.type) {
            case SDL_QUIT: quit = true; break;
            case SDL_KEYUP:
                // Print Screen: Windows only delivers its key-up.
                if (e.key.keysym.scancode == SDL_SCANCODE_PRINTSCREEN) take_screenshot(base_dir);
                break;
            case SDL_KEYDOWN: {
                SDL_Scancode sc = e.key.keysym.scancode;
                if (sc == SDL_SCANCODE_F12 && (e.key.keysym.mod & KMOD_SHIFT)) { if (!e.key.repeat) take_screenshot(base_dir); break; }
                if (sc == SDL_SCANCODE_F12 && !e.key.repeat) { toggle_menu(); break; }
                if (sc == SDL_SCANCODE_ESCAPE && port::menu_open()) { toggle_menu(); break; }
                if (sc == SDL_SCANCODE_F1 && !e.key.repeat) {
                    if (e.key.keysym.mod & KMOD_SHIFT) { g_set.top_bar = (g_set.top_bar + 2) % 3; changed = true; }
                    else g_show_overlay = !g_show_overlay;
                }
                if (sc == SDL_SCANCODE_F3) { g_set.muted = !g_set.muted; changed = true; }
                if (sc == SDL_SCANCODE_F10) {
                    int k = -1;
                    for (int j = 0; j < 4; j++) if (kFpsModes[j] == g_set.fps_limit) k = j;
                    g_set.fps_limit = kFpsModes[(k + 1) % 4];
                    changed = true;
                    log_line("[port] frame rate limit: " + fps_mode_name(g_set.fps_limit));
                }
                if (sc == SDL_SCANCODE_F8) {
                    g_set.aspect = (g_set.aspect + 1) % 3;
                    changed = true;
                    log_line(std::string("[port] aspect ratio: ") + kAspects[g_set.aspect].name);
                }
                if (sc == SDL_SCANCODE_F5 && !e.key.repeat) {
                    if (e.key.keysym.mod & KMOD_SHIFT) {
                        g_set.state_slot = g_set.state_slot % 5 + 1;
                        changed = true;
                        toast("Save state slot " + std::to_string(g_set.state_slot) + state_slot_note(g_set.state_slot));
                    } else request_state(true, g_set.state_slot);
                }
                if (sc == SDL_SCANCODE_F9 && !e.key.repeat && !(e.key.keysym.mod & KMOD_SHIFT)) request_state(false, g_set.state_slot);
                if (g_gpu && sc == SDL_SCANCODE_F9 && (e.key.keysym.mod & KMOD_SHIFT)) {
                    std::string dir = base_dir + "debug";
                    std::error_code ec;
                    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
                    ssne::gpu_renderer_request_dump(dir);
                    log_line("[gpu] F9: capturing the next frame to " + dir);
                }
                // F6/F7 step from the resolution in use (leaving automatic resolution if it was on).
                if (g_gpu && sc == SDL_SCANCODE_F6) { g_set.scale = std::max(1, ssne::gpu_renderer_scale() - 1); changed = true; }
                if (g_gpu && sc == SDL_SCANCODE_F7) { g_set.scale = std::min(8, ssne::gpu_renderer_scale() + 1); changed = true; }
                if (sc == SDL_SCANCODE_F4) {
                    if (e.key.keysym.mod & KMOD_SHIFT) g_set.volume = std::min(200, g_set.volume + 10);
                    else g_set.volume = std::max(0, g_set.volume - 10);
                    changed = true;
                    toast("Volume " + std::to_string(g_set.volume) + "%");
                }
                if (sc == SDL_SCANCODE_F2) release_mouse();
                if (sc == SDL_SCANCODE_F11 || (sc == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                    g_set.fullscreen = !g_set.fullscreen;
                    changed = true;
                }
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
                if (!g_mouse_captured && !port::menu_open() && g_set.mouse_look && !ImGui::GetIO().WantCaptureMouse &&
                    have_game_image()) {
                    g_mouse_captured = true;
                    g_capture_click = SDL_BUTTON(e.button.button);
                    SDL_SetRelativeMouseMode(SDL_TRUE);
                }
                break;
            case SDL_MOUSEMOTION:
                if (g_mouse_captured) { g_mouse_dx += float(e.motion.xrel); g_mouse_dy += float(e.motion.yrel); }
                break;
            case SDL_MOUSEWHEEL:
                if (!port::menu_open() && e.wheel.y != 0) {
                    int dy = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
                    WheelPulse& w = g_wheel[dy > 0 ? 1 : 0];
                    w.pending = std::min(w.pending + std::abs(dy), 4);  // don't queue a long burst
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) toggle_menu();
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (!g_pad) { g_pad = SDL_GameControllerOpen(e.cdevice.which); g_input_pad = g_pad != nullptr; }
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (g_pad && SDL_GameControllerGetAttached(g_pad) == SDL_FALSE) { SDL_GameControllerClose(g_pad); g_pad = nullptr; }
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) { release_mouse(); g_focused = false; }
                if (e.window.event == SDL_WINDOWEVENT_MOVED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    e.window.event == SDL_WINDOWEVENT_MAXIMIZED || e.window.event == SDL_WINDOWEVENT_RESTORED)
                    remember_window(win);
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) g_focused = true;
                if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) g_focused = false;
                if (e.window.event == SDL_WINDOWEVENT_CLOSE) quit = true;
                break;
            default: break;
            }
        }
        uint32_t now = SDL_GetTicks();
        float dt = float(now - last) / 1000.0f;
        last = now;
        update_input(dt);
        update_rumble();
        bool background = !g_focused && g_set.pause_unfocused;
        // A save / load requested from the menu needs the game to reach its next frame: don't hold it meanwhile.
        ssne::set_paused((port::menu_open() && g_set.pause_in_menu && !ssne::savestate_pending()) || background);
        handle_state_result();
        if (background) SDL_Delay(15);  // nothing to show: keep the CPU/GPU mostly idle
        update_prompts();
        g_heartbeat++;
        bool new_frame = false;
        if (g_gpu) {
            g_loop_phase = "running GPU frames";
            new_frame = ssne::gpu_renderer_run(2);
            if (!new_frame && swap_ms < 2.0) {
                // Vsync off (or not honored by the driver): instead of spinning a CPU core on UI redraws the game
                // thread could use, sleep until the game queues its next frame (the UI still redraws >= 200 Hz).
                g_loop_phase = "waiting for a frame";
                if (ssne::gpu_renderer_wait(5)) new_frame = ssne::gpu_renderer_run(2);
            }
            update_auto_scale(win);
            static uint32_t next_stats = SDL_GetTicks() + 10000;
            if (SDL_GetTicks() >= next_stats) {  // periodic renderer stats in ssne.log (diagnostics)
                next_stats += 10000;
                auto gs = ssne::gpu_renderer_stats();
                auto o = ssne::gpu_renderer_output();
                char buf[256];
                snprintf(buf, sizeof(buf), "[gpu] frames %llu (dropped %llu), draws %llu, copies %llu, uploads %llu, shaders %llu (%llu from cache), render targets %llu, last %.1f ms, worst %.1f ms, GPU %.1f ms/frame, output %dx%d (%dx%s)",
                         (unsigned long long)gs.frames, (unsigned long long)ssne::gpu_dropped_frames(), (unsigned long long)gs.draws,
                         (unsigned long long)gs.tex_copies, (unsigned long long)gs.uploads, (unsigned long long)gs.programs,
                         (unsigned long long)gs.cached_programs,
                         (unsigned long long)gs.render_targets, gs.frame_ms, ssne::gpu_renderer_take_max_frame_ms(), g_auto.gpu_ms,
                         o.w, o.h, ssne::gpu_renderer_scale(), g_set.scale ? "" : ", automatic");
                log_line(buf);
            }
        }
        else {
            new_frame = update_game_texture();
            if (!new_frame && swap_ms < 2.0) { SDL_Delay(1); new_frame = update_game_texture(); }
        }
        g_loop_phase = "UI";
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        draw_game_view();
        draw_overlay(data_dir, dt);
        {
            port::MenuInfo mi;
            mi.gpu_active = g_gpu;
            if (g_gpu) {
                mi.renderer_info = ssne::gpu_renderer_info();
                auto o = ssne::gpu_renderer_output();
                mi.out_w = o.w; mi.out_h = o.h;
                mi.current_scale = ssne::gpu_renderer_scale();
                mi.msaa_active = ssne::gpu_renderer_msaa();
                mi.aniso_active = ssne::gpu_renderer_anisotropy();
                mi.gpu_ms = float(g_auto.gpu_ms);
            }
            mi.display_synced = ssne::vi_host_sync();
            if (g_pad) mi.pad_name = SDL_GameControllerName(g_pad) ? SDL_GameControllerName(g_pad) : "controller";
            mi.settings_path = g_settings_path;
            for (int d = 0; d < SDL_GetNumVideoDisplays(); d++) {
                const char* n = SDL_GetDisplayName(d);
                SDL_Rect b{};
                SDL_GetDisplayBounds(d, &b);
                mi.displays.push_back(std::string(n ? n : "Display") + "  (" + std::to_string(b.w) + "x" + std::to_string(b.h) + ")");
            }
            fill_state_slots(mi);
            bool was_open = port::menu_open();
            changed |= port::draw_menu(g_set, mi);
            if (port::menu_take_exit_request()) quit = true;
            { int slot; bool save; if (port::menu_take_state_request(slot, save)) request_state(save, slot); }
            if (was_open && !port::menu_open()) save_now();  // closed with the window's X button
        }
        if (changed) { apply_settings(win); save_now(); }
        ImGui::Render();
        int w, h;
        SDL_GL_GetDrawableSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (const char* shot = getenv("SSNE_SCREENSHOT")) {  // development: save the window after ~3 s and exit
            static uint32_t at = SDL_GetTicks() + 3000;
            if (SDL_GetTicks() >= at) {
                std::vector<uint8_t> px(size_t(w) * h * 4);
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
                for (int y = 0; y < h; y++) memcpy((uint8_t*)sf->pixels + size_t(y) * sf->pitch, &px[size_t(h - 1 - y) * w * 4], size_t(w) * 4);
                SDL_SaveBMP(sf, shot);
                SDL_FreeSurface(sf);
                quit = true;
            }
        }
        g_loop_phase = "present (SwapWindow)";
        auto swap_t0 = std::chrono::steady_clock::now();
        SDL_GL_SwapWindow(win);
        auto swap_t1 = std::chrono::steady_clock::now();
        swap_ms = std::chrono::duration<double, std::milli>(swap_t1 - swap_t0).count();
        {   // Display sync: the swap returned at a vertical blank; tick the game's VI with it when the display runs at
            // ~60 Hz and every swap waits for vsync (checked continuously; any irregularity falls back to the timer).
            static auto last_swap = swap_t1;
            static int good = 0, bad = 0;
            double period = std::chrono::duration<double, std::milli>(swap_t1 - last_swap).count();
            last_swap = swap_t1;
            bool eligible = g_set.sync_display && g_set.vsync && g_set.fps_limit == 60;
            bool regular = period > 15.4 && period < 17.9 && swap_ms > 0.3;
            if (!ssne::paused()) {  // the settings menu pauses the game: keep the current state meanwhile
                if (eligible && regular) { good++; bad = 0; } else { bad++; good = 0; }
            }
            bool on = ssne::vi_host_sync();
            if (!on && eligible && good >= 60) { ssne::set_vi_host_sync(true); log_line("[port] display sync on: the game ticks with the display"); }
            else if (on && (!eligible || bad >= 3)) {
                ssne::set_vi_host_sync(false);
                log_line(eligible ? "[port] display sync paused (irregular display timing), using the game clock" : "[port] display sync off");
            }
            if (ssne::vi_host_sync()) ssne::vi_host_vblank();
        }
        g_loop_phase = "events";
    }
    // Exit. Give the display back first (leave fullscreen, destroy the GL context and window) so nothing of the
    // game stays on screen even if a later step stalls, save, and end the process from a watchdog if shutting the
    // game threads down takes too long.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::_Exit(0);
    }).detach();
    if (g_pad) SDL_GameControllerRumble(g_pad, 0, 0, 0);  // don't leave the controller vibrating
    save_now();
    SDL_SetRelativeMouseMode(SDL_FALSE);
    SDL_SetWindowFullscreen(win, 0);
    SDL_HideWindow(win);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    SDL_GL_MakeCurrent(win, nullptr);
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    ssne::set_paused(false);
    ssne::scheduler_shutdown();
    log_line("[port] exit");
    if (g_log_file) fclose(g_log_file);
    std::_Exit(0);  // guest threads may still be inside game code
}
