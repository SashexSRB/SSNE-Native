// Launcher self-update from the project's GitHub releases: the launcher asks for the latest release when it starts,
// and on the player's go downloads its package for this OS, unpacks it over the launcher's folder and restarts.
// The package holds only the launcher, its tools and the build kit: game_data, saves, states, settings.ini,
// launcher.ini and the built game are not in it and stay as they are.
#pragma once
#include <functional>
#include <string>

namespace updater {

struct Release {
    std::string version;     // "1.2.1" (tag without the "v")
    std::string asset_url;   // package for this OS
    std::string asset_name;
};

const char* current_version();  // this launcher's version (SSNE_VERSION)

// Fetches the latest release. True with `r` filled if it is newer than this launcher and has a package for this
// OS; false otherwise (error set only if the check itself failed).
bool check(Release& r, std::string& error);

// Downloads and unpacks `r` over `base_dir` (the launcher's folder). progress(step, fraction) may return false to
// cancel. On success `launcher` is the new launcher's path, to be started in place of this one.
bool install(const Release& r, const std::string& base_dir,
             const std::function<bool(const std::string&, double)>& progress, std::string& launcher, std::string& error);

// Removes what an earlier update left behind (replaced files Windows couldn't delete while they were in use).
void cleanup(const std::string& base_dir);

} // namespace updater
