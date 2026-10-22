#include "launcher_app.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "gc/disc.h"
#include "platform.h"
#include "theme.h"

namespace fs = std::filesystem;

namespace {
using theme::kGood; using theme::kWarn; using theme::kBad; using theme::kDim;

bool acceptable(gc::VerifyStatus s) {
    return s == gc::VerifyStatus::Verified || s == gc::VerifyStatus::UnknownDump;
}
} // namespace

LauncherApp::LauncherApp() {
    base_dir_ = platform::exe_dir();
    config_path_ = base_dir_ + "/launcher.ini";
    data_dir_ = base_dir_ + "/game_data";
    known_ = gc::load_known_dumps(base_dir_ + "/known_dumps.txt");
    {
        std::string err;
        kit_present_ = load_build_kit(base_dir_ + "/build-kit", kit_, err);
    }
    cfg_.load(config_path_);
    // Installed data must still be there.
    if (cfg_.installed && !platform::file_exists(data_dir_ + "/sys/main.dol")) cfg_.installed = false;
    if (!cfg_.rom_path.empty()) {
        strncpy(path_buf_, cfg_.rom_path.c_str(), sizeof(path_buf_) - 1);
        // Show the previous result without re-hashing 1.4 GB on every start.
        if (cfg_.installed && !cfg_.rom_sha1.empty()) {
            std::string err;
            if (auto d = gc::Disc::open(cfg_.rom_path, err)) {
                result_.header = d->header();
                result_.sha1 = cfg_.rom_sha1;
                result_.status = gc::VerifyStatus::UnknownDump;
                for (auto& k : known_)
                    if (k.sha1 == cfg_.rom_sha1) { result_.status = gc::VerifyStatus::Verified; result_.matched_label = k.label; }
                result_.message = gc::to_string(result_.status);
                have_result_ = true;
            }
        }
    }
}

LauncherApp::~LauncherApp() {
    cancel_ = true;
    join_worker();
}

void LauncherApp::join_worker() {
    if (worker_.joinable()) worker_.join();
}

void LauncherApp::on_file_dropped(const std::string& path) {
    if (phase_ == Phase::Idle) select_rom(path);
}

void LauncherApp::select_rom(const std::string& path) {
    if (path.empty()) return;
    strncpy(path_buf_, path.c_str(), sizeof(path_buf_) - 1);
    {
        std::lock_guard<std::mutex> lk(mu_);
        have_result_ = false;
        message_.clear();
    }
    start_verify();
}

void LauncherApp::start_verify() {
    join_worker();
    std::string path = path_buf_;
    cancel_ = false;
    prog_done_ = 0; prog_total_ = 1;
    phase_ = Phase::Verifying;
    worker_ = std::thread([this, path] {
        auto r = gc::verify_disc(path, known_, [this](uint64_t d, uint64_t t) {
            prog_done_ = d; prog_total_ = t ? t : 1;
            return !cancel_.load();
        });
        std::lock_guard<std::mutex> lk(mu_);
        result_ = r;
        have_result_ = true;
        message_ = r.message;
        message_is_error_ = !acceptable(r.status);
        if (acceptable(r.status)) {
            bool same_disc = cfg_.installed && cfg_.rom_sha1 == r.sha1;
            cfg_.rom_path = path;
            cfg_.game_id = r.header.game_id;
            if (!same_disc) { cfg_.installed = false; cfg_.rom_sha1 = r.sha1; }
            cfg_.save(config_path_);
        }
        phase_ = Phase::Idle;
    });
}

bool LauncherApp::game_built() const {
    if (platform::file_exists(base_dir_ + "/" + platform::port_exe_name())) return true;
    for (auto& old : platform::legacy_port_exe_names())
        if (platform::file_exists(base_dir_ + "/" + old)) return true;
    return false;
}

bool LauncherApp::game_outdated() const {
    return kit_present_ && !kit_.version.empty() && game_built() && built_kit_version(kit_) != kit_.version;
}

void LauncherApp::start_install() {
    join_worker();
    std::string path = cfg_.rom_path;
    // Only rebuild when the data is installed and the game hasn't been built yet; otherwise (re)install everything.
    bool extract = !(cfg_.installed && kit_present_ && !game_ready());
    cancel_ = false;
    prog_done_ = 0; prog_total_ = 1;
    {
        std::lock_guard<std::mutex> lk(mu_);
        message_.clear();
        build_step_.clear();
    }
    phase_ = extract ? Phase::Extracting : Phase::Building;
    worker_ = std::thread([this, path, extract] {
        std::string err;
        if (extract) {
            bool ok = false;
            if (auto d = gc::Disc::open(path, err)) {
                ok = d->extract(data_dir_, err, [this](uint64_t dn, uint64_t t) {
                    prog_done_ = dn; prog_total_ = t ? t : 1;
                    return !cancel_.load();
                });
            }
            std::lock_guard<std::mutex> lk(mu_);
            if (!ok) {
                message_ = "Install failed: " + err;
                message_is_error_ = true;
                phase_ = Phase::Idle;
                return;
            }
            cfg_.installed = true;
            cfg_.save(config_path_);
            message_ = "Game data installed.";
            message_is_error_ = false;
        }
        if (!kit_present_) { phase_ = Phase::Idle; return; }

        // Build the game on this PC from the installed files.
        phase_ = Phase::Building;
        prog_done_ = 0; prog_total_ = 1000;
        GameBuilder b;
        bool ok = b.build(kit_, data_dir_, base_dir_ + "/" + platform::port_exe_name(),
                          [this](const std::string& step, double f) {
                              std::lock_guard<std::mutex> lk(mu_);
                              build_step_ = step;
                              prog_done_ = uint64_t(std::clamp(f, 0.0, 1.0) * 1000);
                          },
                          cancel_, err);
        std::lock_guard<std::mutex> lk(mu_);
        message_ = ok ? "The game was built on this PC. Press Play!" : "Building the game failed: " + err;
        message_is_error_ = !ok;
        phase_ = Phase::Idle;
    });
}

void LauncherApp::launch_game() {
    std::string exe = base_dir_ + "/" + platform::port_exe_name();
    for (auto& old : platform::legacy_port_exe_names())  // builds from before the renames
        if (!platform::file_exists(exe)) exe = base_dir_ + "/" + old;
    if (!platform::file_exists(exe)) {
        std::lock_guard<std::mutex> lk(mu_);
        message_ = platform::port_exe_name() + " was not found next to the launcher. The game executable is not part "
                   "of the launcher download (it would contain code converted from the game); place it in this folder "
                   "to play.";
        message_is_error_ = true;
        return;
    }
    std::vector<std::string> args{"--data", data_dir_};
    if (cfg_.fullscreen) args.push_back("--fullscreen");
    std::string err;
    if (!platform::launch(exe, args, base_dir_, err)) {
        std::lock_guard<std::mutex> lk(mu_);
        message_ = "Launch failed: " + err;
        message_is_error_ = true;
        return;
    }
    cfg_.save(config_path_);
    if (cfg_.close_on_launch) quit_ = true;
}

// ------------------------------------------------------------------ UI
// Layout: title across the top, the game-style menu on the left, the disc / install panel on the right and the
// button-prompt bar along the bottom. Up/Down + Enter drive the menu as well as the mouse.

bool LauncherApp::item_enabled(int item) const {
    bool idle = phase_ == Phase::Idle;
    switch (item) {
    case ITEM_PLAY: return idle && cfg_.installed && (game_built() || !kit_present_);
    case ITEM_DISC: return idle;
    case ITEM_INSTALL: {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mu_));
        return idle && ((have_result_ && acceptable(result_.status)) || (cfg_.installed && kit_present_));
    }
    default: return true;
    }
}

void LauncherApp::browse() {
    std::string p = platform::open_file_dialog("Select your Serious Sam: Next Encounter disc image");
    if (!p.empty()) select_rom(p);
}

void LauncherApp::activate(int item) {
    if (!item_enabled(item)) return;
    switch (item) {
    case ITEM_PLAY: launch_game(); break;
    case ITEM_DISC: browse(); break;
    case ITEM_INSTALL: start_install(); break;
    case ITEM_FULLSCREEN: cfg_.fullscreen = !cfg_.fullscreen; cfg_.save(config_path_); break;
    case ITEM_CLOSE: cfg_.close_on_launch = !cfg_.close_on_launch; cfg_.save(config_path_); break;
    case ITEM_QUIT: quit_ = true; break;
    }
}

void LauncherApp::frame() {
    ImGuiIO& io = ImGui::GetIO();
    time_ += io.DeltaTime;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 p0 = vp->WorkPos, p1 = vp->WorkPos + vp->WorkSize;
    float w = p1.x - p0.x, h = p1.y - p0.y;
    theme::draw_background(ImGui::GetBackgroundDrawList(), p0, p1, time_);

    // Keyboard navigation (not while typing in the path box).
    if (!io.WantTextInput) {
        auto step = [&](int dir) {
            for (int n = 0; n < ITEM_COUNT; n++) {
                selected_ = (selected_ + dir + ITEM_COUNT) % ITEM_COUNT;
                if (item_enabled(selected_)) break;
            }
        };
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) step(1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) step(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Space, false))
            activate(selected_);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && phase_ != Phase::Idle) cancel_ = true;
    // Don't leave the cursor on an item that just became unavailable.
    if (!item_enabled(selected_)) {
        for (int n = 0; n < ITEM_COUNT; n++)
            if (item_enabled(n)) { selected_ = n; break; }
    }
    // Once installed, start on Play.
    static bool was_installed = false;
    if (cfg_.installed && !was_installed) selected_ = ITEM_PLAY;
    was_installed = cfg_.installed;

    ImGui::SetNextWindowPos(p0);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoScrollWithMouse);
    float bar_h = std::clamp(h * 0.075f, 36.0f, 52.0f);
    float title_h = std::clamp(h * 0.25f, 120.0f, 210.0f);
    draw_title(p0, ImVec2(p1.x, p0.y + title_h));
    float top = p0.y + title_h, bottom = p1.y - bar_h - h * 0.035f;
    float split = p0.x + w * 0.45f;
    draw_menu(ImVec2(p0.x + w * 0.04f, top), ImVec2(split, bottom));
    draw_panel(ImVec2(split + w * 0.01f, top), ImVec2(p1.x - w * 0.04f, bottom));
    draw_prompt_bar(ImVec2(p0.x, p1.y - bar_h), p1);
    ImGui::End();
}

void LauncherApp::draw_title(ImVec2 p0, ImVec2 p1) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = p1.x - p0.x, h = p1.y - p0.y;
    float size = std::min(h * 0.56f, w * 0.082f);
    const char* title = "Silly ReEncounter";
    ImVec2 ts = theme::chunky_text(nullptr, ImVec2(0, 0), size, title, theme::Look::Normal, time_);
    ImVec2 tp(p0.x + (w - ts.x) * 0.5f, p0.y + h * 0.14f);
    theme::chunky_text(dl, tp, size, title, theme::Look::Normal, time_);

    const char* sub = "PC RECOMPILATION";
    float ss = std::max(13.0f, size * 0.26f), sp = ss * 0.55f;
    float sw = theme::spaced_text(nullptr, theme::fonts.bold, ss, ImVec2(0, 0), 0, sub, sp);
    ImVec2 sub_pos(p0.x + (w - sw) * 0.5f, tp.y + ts.y + h * 0.06f);
    theme::spaced_text(dl, theme::fonts.bold, ss, sub_pos + ImVec2(1, 2), IM_COL32(0, 0, 10, 200), sub, sp);
    theme::spaced_text(dl, theme::fonts.bold, ss, sub_pos, IM_COL32(235, 238, 255, 255), sub, sp);

    const char* legal = "Unofficial fan project. Not affiliated with or endorsed by Croteam, Devolver Digital, Climax, "
                        "Global Star Software, Take-Two or Nintendo. Requires your own copy of the game.";
    float ls = 13;
    ImVec2 lt = theme::fonts.small->CalcTextSizeA(ls, FLT_MAX, 0, legal);
    dl->AddText(theme::fonts.small, ls, ImVec2(p0.x + (w - lt.x) * 0.5f, sub_pos.y + ss + h * 0.07f),
                IM_COL32(150, 158, 210, 200), legal);
}

void LauncherApp::draw_menu(ImVec2 p0, ImVec2 p1) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = p1.x - p0.x, h = p1.y - p0.y;
    float big = std::clamp(h * 0.105f, 26.0f, 50.0f), small = big * 0.72f;
    bool installed = cfg_.installed;
    struct Row { int item; const char* label; float size; const char* value; };
    Row rows[ITEM_COUNT] = {
        {ITEM_PLAY, "Play", big * 1.12f, nullptr},
        {ITEM_DISC, "Select Disc", big, nullptr},
        {ITEM_INSTALL, !installed ? "Install Game" : game_outdated() ? "Update Game" : (kit_present_ && !game_ready()) ? "Build Game" : "Reinstall", big, nullptr},
        {ITEM_FULLSCREEN, "Fullscreen", small, cfg_.fullscreen ? "ON" : "OFF"},
        {ITEM_CLOSE, "Close On Launch", small, cfg_.close_on_launch ? "ON" : "OFF"},
        {ITEM_QUIT, "Quit", big, nullptr},
    };
    float gap = big * 0.30f, total = 0;
    for (auto& r : rows) total += r.size * 1.05f + gap;
    float y = p0.y + std::max(0.0f, (h - total) * 0.5f);
    ImVec2 mouse = ImGui::GetIO().MousePos;
    for (auto& r : rows) {
        bool en = item_enabled(r.item);
        theme::Look look = !en ? theme::Look::Disabled : selected_ == r.item ? theme::Look::Selected : theme::Look::Normal;
        ImVec2 ts = theme::chunky_text(nullptr, ImVec2(0, 0), r.size, r.label, look, time_);
        float vw = 0, vs = r.size * 0.62f;
        if (r.value) vw = theme::fonts.bold->CalcTextSizeA(vs, FLT_MAX, 0, r.value).x + r.size * 0.5f;
        float x = p0.x + (w - ts.x - vw) * 0.5f;
        ImVec2 a(x - 12, y - 4), b(x + ts.x + vw + 12, y + r.size * 1.05f + 4);
        bool hover = ImGui::IsWindowHovered() && mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y;
        if (hover && en) {
            if (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0) selected_ = r.item;
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { selected_ = r.item; activate(r.item); }
        }
        if (look == theme::Look::Selected) {
            // Soft highlight behind the selected entry.
            float cy = (a.y + b.y) * 0.5f;
            dl->AddRectFilledMultiColor(ImVec2(p0.x, a.y), ImVec2((p0.x + p1.x) * 0.5f, b.y), IM_COL32(255, 150, 0, 0),
                                        IM_COL32(255, 150, 0, 38), IM_COL32(255, 150, 0, 38), IM_COL32(255, 150, 0, 0));
            dl->AddRectFilledMultiColor(ImVec2((p0.x + p1.x) * 0.5f, a.y), ImVec2(p1.x, b.y), IM_COL32(255, 150, 0, 38),
                                        IM_COL32(255, 150, 0, 0), IM_COL32(255, 150, 0, 0), IM_COL32(255, 150, 0, 38));
            (void)cy;
        }
        theme::chunky_text(dl, ImVec2(x, y), r.size, r.label, look, time_);
        if (r.value) {
            ImVec2 vp(x + ts.x + r.size * 0.5f, y + r.size * 0.62f - vs * 0.5f);
            ImU32 vc = strcmp(r.value, "ON") == 0 ? IM_COL32(140, 255, 110, 255) : IM_COL32(170, 180, 230, 255);
            dl->AddText(theme::fonts.bold, vs, vp + ImVec2(1, 2), IM_COL32(0, 0, 10, 200), r.value);
            dl->AddText(theme::fonts.bold, vs, vp, vc, r.value);
        }
        y += r.size * 1.05f + gap;
    }
}

void LauncherApp::draw_progress() {
    float f = float(double(prog_done_.load()) / double(prog_total_.load()));
    std::string what = phase_ == Phase::Verifying ? "Checking disc" : "Copying the game files";
    if (phase_ == Phase::Building) {
        std::lock_guard<std::mutex> lk(mu_);
        what = build_step_.empty() ? "Building the game" : build_step_;
    }
    char label[256];
    snprintf(label, sizeof(label), "%s  %d%%", what.c_str(), int(f * 100));
    theme::progress_bar(f, ImVec2(-110, 30), label);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(-1, 30))) cancel_ = true;
}

void LauncherApp::draw_panel(ImVec2 p0, ImVec2 p1) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    theme::panel(dl, p0, p1);
    const float pad = 20;
    ImGui::SetCursorScreenPos(p0 + ImVec2(pad, pad * 0.8f));
    ImGui::BeginChild("##panel", ImVec2(p1.x - p0.x - pad * 2, p1.y - p0.y - pad * 1.6f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    auto heading = [&](const char* text) {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImVec2 sz = theme::chunky_text(cdl, pos + ImVec2(3, 0), 34, text, theme::Look::Normal, time_);
        ImGui::Dummy(ImVec2(sz.x, sz.y + 2));
    };
    bool busy = phase_ != Phase::Idle;

    heading("Disc Image");
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (phase_ == Phase::Verifying) {
            ImGui::TextColored(kDim, "Reading the whole disc to check it...");
        } else if (!have_result_) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped("Choose your Serious Sam: Next Encounter disc image: an .iso / .gcm dump or a Dolphin "
                               "image (.rvz, .wia, .gcz, .ciso). You can also drop it on this window.");
            ImGui::PopStyleColor();
        } else {
            const auto& r = result_;
            ImVec4 col = r.status == gc::VerifyStatus::Verified ? kGood
                       : r.status == gc::VerifyStatus::UnknownDump ? kWarn : kBad;
            ImGui::PushFont(theme::fonts.bold);
            ImGui::TextColored(col, "%s", gc::to_string(r.status));
            ImGui::PopFont();
            if (!r.header.game_id.empty())
                ImGui::Text("%s   |   %s   |   Rev %d   |   Disc %d", r.header.title.c_str(), r.header.game_id.c_str(),
                            r.header.version, r.header.disc_number + 1);
            ImGui::PushFont(theme::fonts.small);
            if (!r.format.empty()) ImGui::TextColored(kDim, "Image: %s", r.format.c_str());
            if (!r.sha1.empty()) ImGui::TextColored(kDim, "SHA-1: %s", r.sha1.c_str());
            if (!r.matched_label.empty()) ImGui::TextColored(kDim, "Matches: %s", r.matched_label.c_str());
            ImGui::PopFont();
        }
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(-190);
    bool enter = ImGui::InputTextWithHint("##rom", "Path to your disc image", path_buf_, sizeof(path_buf_),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Browse", ImVec2(85, 0))) browse();
    ImGui::SameLine();
    if (ImGui::Button("Verify", ImVec2(-1, 0)) || enter) select_rom(path_buf_);
    ImGui::EndDisabled();
    if (phase_ == Phase::Verifying) draw_progress();

    ImGui::Dummy(ImVec2(0, 10));
    heading("Game");
    bool ok;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ok = have_result_ && acceptable(result_.status);
    }
    bool ready = game_ready();
    if (phase_ == Phase::Extracting) {
        ImGui::TextColored(kDim, "Step 1: copying the game's files out of your disc image...");
        draw_progress();
    } else if (phase_ == Phase::Building) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("Step 2: building the game on this PC from your disc. This takes a few minutes.");
        ImGui::PopStyleColor();
        draw_progress();
    } else if (cfg_.installed && (ready || !kit_present_)) {
        ImGui::PushFont(theme::fonts.bold);
        ImGui::TextColored(kGood, "Installed");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextColored(kDim, ready ? "and built on this PC. Ready to play." : "in game_data next to the launcher.");
    } else if (cfg_.installed && game_outdated()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
        ImGui::TextWrapped("This launcher has an updated game engine (performance improvements). Choose Update Game to "
                           "rebuild the game on this PC (a few minutes, nothing is downloaded). Play still starts the "
                           "old build.");
        ImGui::PopStyleColor();
    } else if (cfg_.installed) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
        ImGui::TextWrapped("Game files installed, but the game hasn't been built yet. Choose Build Game.");
        ImGui::PopStyleColor();
    } else if (ok) {
        if (kit_present_ && kit_.toolchain_url.empty()) {
            ImGui::TextWrapped("Install Game copies the game's files out of your disc (about 1.3 GB), then builds the game "
                               "on this PC with the %s compiler installed on this system. Nothing is downloaded.",
                               kit_.toolchain_name.empty() ? "C++" : kit_.toolchain_name.c_str());
        } else if (kit_present_) {
            ImGui::TextWrapped("Install Game copies the game's files out of your disc (about 1.3 GB), then builds the game "
                               "on this PC. The first time it downloads the free %s compiler (about %d MB) from its "
                               "official release. Nothing from the game is ever downloaded.",
                               kit_.toolchain_name.c_str(), kit_.toolchain_size_mb);
        } else {
            ImGui::TextWrapped("Install Game copies the game's files (about 1.3 GB) out of the disc image into game_data "
                               "next to the launcher.");
        }
    } else {
        ImGui::TextColored(kDim, "Waiting for a verified disc.");
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!message_.empty() && phase_ == Phase::Idle) {
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::PushStyleColor(ImGuiCol_Text, message_is_error_ ? kBad : kDim);
            ImGui::TextWrapped("%s", message_.c_str());
            ImGui::PopStyleColor();
        }
    }

    // Progress strip along the bottom: Disc -> Data -> Ready.
    const char* steps[3] = {"1  Disc verified", "2  Game installed", "3  Ready to play"};
    bool built = cfg_.installed && (ready || !kit_present_);
    bool done[3] = {ok || cfg_.installed, built, built};
    float avail = ImGui::GetContentRegionAvail().x, ph = 34, gap = 10;
    float pw = (avail - gap * 2) / 3;
    ImVec2 base(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - ph - 2);
    for (int i = 0; i < 3; i++) {
        ImVec2 a(base.x + i * (pw + gap), base.y), b(a.x + pw, a.y + ph);
        bool next = !done[i] && (i == 0 || done[i - 1]);
        ImU32 fill = done[i] ? IM_COL32(40, 120, 40, 200) : next ? IM_COL32(150, 80, 0, 200) : IM_COL32(20, 24, 70, 200);
        ImU32 edge = done[i] ? IM_COL32(140, 255, 110, 200) : next ? IM_COL32(255, 170, 40, 230) : IM_COL32(90, 100, 170, 160);
        cdl->AddRectFilled(a, b, fill, ph * 0.5f);
        cdl->AddRect(a, b, edge, ph * 0.5f, 0, 1.5f);
        ImVec2 ts = theme::fonts.bold->CalcTextSizeA(16, FLT_MAX, 0, steps[i]);
        cdl->AddText(theme::fonts.bold, 16, ImVec2(a.x + (pw - ts.x) * 0.5f, a.y + (ph - ts.y) * 0.5f),
                     done[i] || next ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 160, 210, 255), steps[i]);
    }
    ImGui::EndChild();
}

void LauncherApp::draw_prompt_bar(ImVec2 p0, ImVec2 p1) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = p1.y - p0.y;
    dl->AddRectFilledMultiColor(p0, p1, IM_COL32(70, 72, 90, 150), IM_COL32(70, 72, 90, 150),
                                IM_COL32(30, 32, 44, 200), IM_COL32(30, 32, 44, 200));
    dl->AddLine(p0, ImVec2(p1.x, p0.y), IM_COL32(255, 255, 255, 50), 1.0f);
    float kh = h * 0.56f, x = p0.x + 18, y = p0.y + (h - kh) * 0.5f;
    x += theme::key_prompt(dl, ImVec2(x, y), kh, "Enter", "Select", IM_COL32(40, 170, 60, 255)) + 26;
    x += theme::key_prompt(dl, ImVec2(x, y), kh, "Up/Down", "Move", IM_COL32(60, 90, 200, 255)) + 26;
    if (phase_ != Phase::Idle) theme::key_prompt(dl, ImVec2(x, y), kh, "Esc", "Cancel", IM_COL32(200, 40, 40, 255));
    const char* hint = "Drop a disc image anywhere on this window";
    ImVec2 ts = theme::fonts.small->CalcTextSizeA(kh * 0.6f, FLT_MAX, 0, hint);
    dl->AddText(theme::fonts.small, kh * 0.6f, ImVec2(p1.x - ts.x - 20, p0.y + (h - ts.y) * 0.5f),
                IM_COL32(190, 195, 230, 220), hint);
}
