// launcher.ini — remembers the chosen disc and what has been verified/installed.
#pragma once
#include <string>

struct LauncherConfig {
    std::string rom_path;
    std::string rom_sha1;       // hash of the disc that game_data was extracted from
    std::string game_id;
    bool installed = false;     // game_data/ extracted from rom_sha1
    bool close_on_launch = true;
    bool fullscreen = false;
    bool check_updates = true;  // ask GitHub for a newer release at start
    std::string skip_version;   // a release the player chose to skip

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};
