#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include <imgui.h>

#include "config.h"
#include "game_builder.h"
#include "gc/verify.h"

class LauncherApp {
public:
    LauncherApp();
    ~LauncherApp();

    void on_file_dropped(const std::string& path);
    void frame();                     // draws the whole UI; call between ImGui::NewFrame/Render
    bool wants_quit() const { return quit_; }

private:
    enum class Phase { Idle, Verifying, Extracting, Building };

    void select_rom(const std::string& path);
    void start_verify();
    void start_install();          // extract the disc (if needed) and build the game (if a build kit is present)
    bool game_built() const;       // a game executable exists next to the launcher
    bool game_outdated() const;    // built with an older build kit (engine update available): rebuild
    bool game_ready() const { return game_built() && !game_outdated(); }
    void launch_game();
    void join_worker();

    enum Item { ITEM_PLAY, ITEM_DISC, ITEM_INSTALL, ITEM_FULLSCREEN, ITEM_CLOSE, ITEM_QUIT, ITEM_COUNT };
    bool item_enabled(int item) const;
    void activate(int item);
    void browse();

    void draw_title(ImVec2 p0, ImVec2 p1);
    void draw_menu(ImVec2 p0, ImVec2 p1);
    void draw_panel(ImVec2 p0, ImVec2 p1);
    void draw_prompt_bar(ImVec2 p0, ImVec2 p1);
    void draw_progress();

    std::string base_dir_, config_path_, data_dir_;
    bool kit_present_ = false;
    BuildKit kit_;
    std::string build_step_;          // guarded by mu_
    LauncherConfig cfg_;
    std::vector<gc::KnownDump> known_;

    std::thread worker_;
    std::atomic<Phase> phase_{Phase::Idle};
    std::atomic<uint64_t> prog_done_{0}, prog_total_{1};
    std::atomic<bool> cancel_{false};

    std::mutex mu_;
    bool have_result_ = false;
    gc::VerifyResult result_;
    std::string message_;             // last error / info
    bool message_is_error_ = false;
    char path_buf_[1024] = {};
    bool quit_ = false;
    int selected_ = ITEM_DISC;
    float time_ = 0;
};
