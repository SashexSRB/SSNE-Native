// Builds SillyReEncounter-Game.exe on the player's PC from their own disc, so no game code is ever
// distributed. Steps: get the compiler (downloaded once from its official release), translate the installed
// main.dol with ssne-recomp, compile the generated C++ in parallel and link it with the prebuilt port/runtime
// libraries from the build kit. Everything is described by build-kit/kit.ini.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

struct BuildKit {
    std::string dir;                // build-kit folder
    std::string recomp, symbols;    // translator and function-name map (default: USA)
    std::vector<std::pair<std::string, std::string>> region_symbols;  // per game ID ("symbols.G3BP9G = ..." in kit.ini)
    std::string symbols_for(const std::string& game_id) const;
    std::string toolchain_dir;      // where the compiler lives / is unpacked to
    std::string toolchain_url, toolchain_sha256, toolchain_name;
    std::vector<std::string> toolchain_skip;  // zip path prefixes not unpacked (other CPU targets, docs)
    int toolchain_size_mb = 0;
    std::string compiler;           // absolute path
    std::string version;            // kit version: a game built with another version is rebuilt (engine updates)
    std::vector<std::string> compiler_flags, cxxflags, link;  // link: "{objs}" marks the generated objects

    bool compiler_present() const;
};

// Reads <kit_dir>/kit.ini. "kit:" inside values expands to the kit folder.
bool load_build_kit(const std::string& kit_dir, BuildKit& kit, std::string& error);
// Kit version the game executable was last built with ("" if unknown).
std::string built_kit_version(const BuildKit& kit);

class GameBuilder {
public:
    // report(step_text, fraction 0..1); returning false is not used, cancellation goes through `cancel`.
    using Report = std::function<void(const std::string&, double)>;

    bool build(const BuildKit& kit, const std::string& data_dir, const std::string& out_exe, const Report& report,
               std::atomic<bool>& cancel, std::string& error);

    std::string log_path;  // build log (set during build)

private:
    bool get_toolchain(const BuildKit& kit, const Report& report, std::atomic<bool>& cancel, std::string& error);
    bool run(const std::vector<std::string>& argv, const std::string& cwd, const std::string& log,
             std::atomic<bool>& cancel, std::string& error);
    void log(const std::string& line);
};
