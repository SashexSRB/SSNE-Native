#include "game_builder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "gc/sha256.h"
#include "gc/zip.h"
#include "platform.h"

namespace fs = std::filesystem;

namespace {
fs::path P(const std::string& s) { return fs::u8path(s); }
std::string S(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}
std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string t;
    while (in >> t) out.push_back(t);
    return out;
}
std::string expand(std::string v, const std::string& kit) {
    for (size_t p; (p = v.find("kit:")) != std::string::npos;) v.replace(p, 4, kit + "/");
    return v;
}
bool exists(const std::string& p) {
    std::error_code ec;
    return fs::exists(P(p), ec);
}
// A bare program name ("clang++") is looked up in PATH, as a shell would. Returns the name unchanged if not found.
std::string find_in_path(const std::string& name) {
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    const char* env = std::getenv("PATH");
    std::string path = env ? env : "";
    for (size_t a = 0; a <= path.size();) {
        size_t b = path.find(sep, a);
        if (b == std::string::npos) b = path.size();
        if (b > a) {
            fs::path c = P(path.substr(a, b - a)) / P(name);
            std::error_code ec;
            if (fs::is_regular_file(c, ec)) return S(c);
        }
        a = b + 1;
    }
    return name;
}
} // namespace

// The unpacked compiler is marked with the archive it came from, so a kit that switches to another compiler
// release (e.g. kit 10: UCRT -> MSVCRT for Windows 7/8.1) downloads it again instead of using the old one.
static std::string toolchain_stamp_path(const BuildKit& kit) { return S(P(kit.toolchain_dir) / "toolchain-url.txt"); }
bool BuildKit::compiler_present() const {
    if (compiler.empty() || !exists(compiler)) return false;
    if (toolchain_url.empty()) return true;  // a compiler installed by hand
    std::ifstream f(P(toolchain_stamp_path(*this)));
    std::string url;
    std::getline(f, url);
    return url == toolchain_url;
}

std::string BuildKit::symbols_for(const std::string& game_id) const {
    for (auto& [id, file] : region_symbols)
        if (id == game_id) return file;
    return symbols;
}

std::string built_kit_version(const BuildKit& kit) {
    std::ifstream in(P(kit.dir) / "built-with.txt");
    std::string v;
    if (in) std::getline(in, v);
    return trim(v);
}

bool load_build_kit(const std::string& kit_dir, BuildKit& kit, std::string& error) {
    std::ifstream in(P(kit_dir) / "kit.ini");
    if (!in) { error = "build kit not found"; return false; }
    kit = BuildKit{};
    kit.dir = kit_dir;
    std::string line, compiler_rel;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "recomp") kit.recomp = expand(v, kit_dir);
        else if (k == "version") kit.version = v;
        else if (k.rfind("symbols.", 0) == 0) kit.region_symbols.push_back({k.substr(8), expand(v, kit_dir)});
        else if (k == "symbols") kit.symbols = expand(v, kit_dir);
        else if (k == "toolchain_dir") kit.toolchain_dir = expand(v, kit_dir);
        else if (k == "toolchain_url") kit.toolchain_url = v;
        else if (k == "toolchain_sha256") kit.toolchain_sha256 = v;
        else if (k == "toolchain_name") kit.toolchain_name = v;
        else if (k == "toolchain_skip") kit.toolchain_skip = split(v);
        else if (k == "toolchain_size_mb") kit.toolchain_size_mb = atoi(v.c_str());
        else if (k == "compiler") compiler_rel = v;
        else if (k == "compiler_flags") kit.compiler_flags = split(v);
        else if (k == "cxxflags") { kit.cxxflags.clear(); for (auto& t : split(v)) kit.cxxflags.push_back(expand(t, kit_dir)); }
        else if (k == "link") { kit.link.clear(); for (auto& t : split(v)) kit.link.push_back(expand(t, kit_dir)); }
    }
    // toolchain_dir is only needed for a downloaded compiler; a kit may instead name the system's own compiler.
    if (kit.recomp.empty() || compiler_rel.empty() || kit.link.empty() ||
        (kit.toolchain_dir.empty() && !kit.toolchain_url.empty())) {
        error = "build kit settings are incomplete (kit.ini)";
        return false;
    }
    if (P(compiler_rel).is_absolute()) kit.compiler = compiler_rel;
    else if (compiler_rel.find_first_of("/\\") == std::string::npos && kit.toolchain_url.empty())
        kit.compiler = find_in_path(compiler_rel);  // e.g. "compiler = clang++": the one installed on this system
    else kit.compiler = S(P(kit.toolchain_dir) / P(compiler_rel));
    return true;
}

void GameBuilder::log(const std::string& line) {
#ifdef _WIN32
    FILE* f = _wfopen(P(log_path).c_str(), L"a");
#else
    FILE* f = fopen(log_path.c_str(), "a");
#endif
    if (f) {
        std::time_t t = std::time(nullptr);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&t));
        fprintf(f, "[%s] %s\n", ts, line.c_str());
        fclose(f);
    }
}

bool GameBuilder::run(const std::vector<std::string>& argv, const std::string& cwd, const std::string& logfile,
                      std::atomic<bool>& cancel, std::string& error) {
    std::string cmd;
    for (auto& a : argv) cmd += (cmd.empty() ? "" : " ") + a;
    log("run: " + cmd);
    platform::Process* p = platform::spawn(argv, cwd, logfile, error);
    if (!p) return false;
    int code = 0;
    while (!platform::poll(p, code)) {
        if (cancel) { platform::kill(p); platform::release(p); error = "cancelled"; return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    platform::release(p);
    if (code != 0) { error = "exit code " + std::to_string(code); return false; }
    return true;
}

bool GameBuilder::get_toolchain(const BuildKit& kit, const Report& report, std::atomic<bool>& cancel,
                                std::string& error) {
    if (kit.compiler_present()) { log("compiler: " + kit.compiler); return true; }
    if (kit.toolchain_url.empty()) {
        error = "compiler not found: " + kit.compiler;
#ifndef _WIN32
        error += ". Install " + (kit.toolchain_name.empty() ? std::string("a C++ compiler") : kit.toolchain_name) +
                 " with your package manager, or change \"compiler\" in " + S(P(kit.dir) / "kit.ini");
#endif
        return false;
    }
    std::error_code ec;
    fs::path dl_dir = P(kit.dir) / "downloads";
    fs::create_directories(dl_dir, ec);
    std::string name = kit.toolchain_url.substr(kit.toolchain_url.rfind('/') + 1);
    fs::path zip = dl_dir / P(name), part = dl_dir / P(name + ".part");
    if (!fs::exists(zip, ec)) {
        log("downloading " + kit.toolchain_url);
        std::string label = "Downloading the compiler (" + kit.toolchain_name + ")";
        bool ok = platform::download(kit.toolchain_url, S(part), [&](uint64_t d, uint64_t t) {
            double total = t ? double(t) : kit.toolchain_size_mb * 1048576.0;
            char buf[64];
            snprintf(buf, sizeof(buf), "  %.0f / %.0f MB", d / 1048576.0, total / 1048576.0);
            report(label + buf, total > 0 ? std::min(1.0, d / total) : 0.0);
            return !cancel.load();
        }, error);
        if (!ok) {
            fs::remove(part, ec);
            error = "download failed: " + error + ". If it keeps failing (e.g. Windows 7 without TLS 1.2), download " +
                    kit.toolchain_url + " in a web browser and put the .zip file into " + S(dl_dir) +
                    ", then build again.";
            return false;
        }
        fs::rename(part, zip, ec);
        if (ec) { error = "cannot save " + S(zip); return false; }
    }
    report("Checking the download", 0);
    std::string sha = gc::sha256_file(S(zip));
    log("toolchain sha256: " + sha);
    if (!kit.toolchain_sha256.empty() && sha != kit.toolchain_sha256) {
        fs::remove(zip, ec);
        error = "the downloaded compiler does not match the expected checksum; try again";
        return false;
    }
    // Unpack, dropping the archive's top-level folder and the parts we don't need.
    std::vector<gc::zip::Entry> entries;
    if (!gc::zip::list(S(zip), entries, error)) return false;
    std::string root;
    if (!entries.empty()) {
        root = entries[0].name.substr(0, entries[0].name.find('/') + 1);
        for (auto& e : entries)
            if (e.name.compare(0, root.size(), root) != 0) { root.clear(); break; }
    }
    fs::path tmp = P(kit.toolchain_dir + ".tmp");
    fs::remove_all(tmp, ec);
    log("unpacking to " + kit.toolchain_dir);
    bool ok = gc::zip::extract(S(zip), S(tmp), [&](const std::string& n) -> std::string {
        std::string rel = n.substr(root.size());
        for (auto& s : kit.toolchain_skip)
            if (rel.compare(0, s.size(), s) == 0) return {};
        return rel;
    }, [&](uint64_t d, uint64_t t) {
        report("Unpacking the compiler", t ? double(d) / double(t) : 0.0);
        return !cancel.load();
    }, error);
    if (!ok) { fs::remove_all(tmp, ec); error = "unpacking the compiler failed: " + error; return false; }
    fs::remove_all(P(kit.toolchain_dir), ec);
    fs::rename(tmp, P(kit.toolchain_dir), ec);
    if (ec) { error = "cannot move the compiler into place: " + ec.message(); return false; }
    { std::ofstream st(P(toolchain_stamp_path(kit))); st << kit.toolchain_url << "\n"; }
    if (!kit.compiler_present()) { error = "the compiler download does not contain " + kit.compiler; return false; }
    fs::remove(zip, ec);  // 150+ MB no longer needed
    return true;
}

bool GameBuilder::build(const BuildKit& kit, const std::string& data_dir, const std::string& out_exe,
                        const Report& report, std::atomic<bool>& cancel, std::string& error) {
    std::error_code ec;
    fs::path work = P(kit.dir) / "work";
    fs::path logs = work / "logs";
    fs::path src = work / "recompiled", obj = work / "obj";
    fs::remove_all(src, ec);
    fs::remove_all(obj, ec);
    fs::remove_all(logs, ec);
    fs::create_directories(logs, ec);
    fs::create_directories(obj, ec);
    log_path = S(logs / "build.log");
    log("SSNE-Native game build");

    // 1. Compiler
    report("Preparing the compiler", 0);
    if (!get_toolchain(kit, report, cancel, error)) { log("error: " + error); return false; }

    // 2. Translate the game's executable from the installed disc files.
    report("Translating the game code", 0);
    std::string dol = S(P(data_dir) / "sys" / "main.dol");
    if (!exists(dol)) { error = "game data is not installed (sys/main.dol missing)"; return false; }
    std::vector<std::string> rargv = {kit.recomp, dol, S(src), "--no-comments"};
    std::string game_id;  // the installed disc's ID picks the function-name map (USA / Europe)
    {
        std::ifstream boot(P(data_dir) / "sys" / "boot.bin", std::ios::binary);
        char id[6] = {};
        if (boot.read(id, 6)) game_id.assign(id, 6);
    }
    std::string symbols = kit.symbols_for(game_id);
    log("game " + (game_id.empty() ? std::string("?") : game_id) + ", symbols " + symbols);
    if (!symbols.empty()) { rargv.push_back("--map"); rargv.push_back(symbols); }
    if (!run(rargv, kit.dir, S(logs / "recomp.log"), cancel, error)) {
        error = "translating the game code failed (" + error + "); see " + S(logs / "recomp.log");
        log("error: " + error);
        return false;
    }
    if (cancel) { error = "cancelled"; return false; }

    // 3. Compile the generated sources in parallel.
    std::vector<fs::path> sources;
    for (auto& e : fs::directory_iterator(src, ec))
        if (e.path().extension() == ".cpp") sources.push_back(e.path());
    std::sort(sources.begin(), sources.end());
    if (sources.empty()) { error = "the translator produced no code; see " + S(logs / "recomp.log"); return false; }
    unsigned jobs = std::max(1u, std::min(std::thread::hardware_concurrency(), 16u));
    log("compiling " + std::to_string(sources.size()) + " files with " + std::to_string(jobs) + " jobs");
    struct Job { platform::Process* p; size_t idx; };
    std::vector<Job> running;
    std::vector<std::string> objects(sources.size());
    size_t next = 0, done = 0;
    bool failed = false;
    std::string fail_log;
    auto status = [&] {
        report("Compiling the game (" + std::to_string(done) + " of " + std::to_string(sources.size()) + " files)",
               double(done) / double(sources.size()));
    };
    status();
    while ((next < sources.size() && !failed) || !running.empty()) {
        while (!failed && !cancel && next < sources.size() && running.size() < jobs) {
            std::string stem = S(sources[next].stem());
            objects[next] = S(obj / P(stem + ".o"));
            std::vector<std::string> argv = {kit.compiler};
            argv.insert(argv.end(), kit.compiler_flags.begin(), kit.compiler_flags.end());
            argv.insert(argv.end(), kit.cxxflags.begin(), kit.cxxflags.end());
            argv.push_back("-I" + S(src));
            argv.insert(argv.end(), {"-c", S(sources[next]), "-o", objects[next]});
            std::string lg = S(logs / P(stem + ".log")), err;
            platform::Process* p = platform::spawn(argv, S(src), lg, err);
            if (!p) { failed = true; error = err; break; }
            running.push_back({p, next});
            next++;
        }
        for (size_t i = 0; i < running.size();) {
            int code = 0;
            if (cancel) { platform::kill(running[i].p); platform::release(running[i].p); running.erase(running.begin() + i); continue; }
            if (platform::poll(running[i].p, code)) {
                platform::release(running[i].p);
                if (code != 0 && !failed) {
                    failed = true;
                    fail_log = S(logs / P(S(sources[running[i].idx].stem()) + ".log"));
                    error = "compiling " + S(sources[running[i].idx].filename()) + " failed; see " + fail_log;
                }
                running.erase(running.begin() + i);
                done++;
                status();
            } else {
                i++;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (cancel) { error = "cancelled"; return false; }
    if (failed) { log("error: " + error); return false; }

    // 4. Link with the prebuilt port, runtime and library code.
    report("Linking", 0.5);
    fs::path tmp_exe = P(out_exe + ".new");
    fs::remove(tmp_exe, ec);
    std::vector<std::string> argv = {kit.compiler};
    argv.insert(argv.end(), kit.compiler_flags.begin(), kit.compiler_flags.end());
    for (auto& t : kit.link) {
        if (t == "{objs}") argv.insert(argv.end(), objects.begin(), objects.end());
        else argv.push_back(t);
    }
    argv.insert(argv.end(), {"-o", S(tmp_exe)});
    if (!run(argv, S(work), S(logs / "link.log"), cancel, error)) {
        error = "linking failed (" + error + "); see " + S(logs / "link.log");
        log("error: " + error);
        return false;
    }
    fs::remove(P(out_exe), ec);
    if (ec && exists(out_exe)) { error = "cannot replace " + out_exe + " (is the game still running?)"; return false; }
    fs::rename(tmp_exe, P(out_exe), ec);
    if (ec) { error = "cannot create " + out_exe + ": " + ec.message(); return false; }

    // Remember which kit version built the game: a newer kit (engine update) makes the launcher rebuild it.
    {
        std::ofstream stamp(P(kit.dir) / "built-with.txt", std::ios::trunc);
        stamp << kit.version << "\n";
    }
    // The generated code and objects came from the player's own disc and are only needed for the build.
    fs::remove_all(src, ec);
    fs::remove_all(obj, ec);
    log("done: " + out_exe);
    report("Done", 1);
    return true;
}
