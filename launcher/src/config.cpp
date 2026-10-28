#include "config.h"

#include <fstream>

bool LauncherConfig::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (!v.empty() && v.back() == '\r') v.pop_back();
        if (k == "rom_path") rom_path = v;
        else if (k == "rom_sha1") rom_sha1 = v;
        else if (k == "game_id") game_id = v;
        else if (k == "installed") installed = v == "1";
        else if (k == "close_on_launch") close_on_launch = v == "1";
        else if (k == "fullscreen") fullscreen = v == "1";
        else if (k == "check_updates") check_updates = v == "1";
        else if (k == "skip_version") skip_version = v;
    }
    return true;
}

bool LauncherConfig::save(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << "rom_path=" << rom_path << "\n"
        << "rom_sha1=" << rom_sha1 << "\n"
        << "game_id=" << game_id << "\n"
        << "installed=" << (installed ? 1 : 0) << "\n"
        << "close_on_launch=" << (close_on_launch ? 1 : 0) << "\n"
        << "fullscreen=" << (fullscreen ? 1 : 0) << "\n"
        << "check_updates=" << (check_updates ? 1 : 0) << "\n"
        << "skip_version=" << skip_version << "\n";
    return bool(out);
}
