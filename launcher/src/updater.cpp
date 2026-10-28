#include "updater.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>
#include <vector>

#include "gc/zip.h"
#include "platform.h"

namespace fs = std::filesystem;

namespace updater {
namespace {

fs::path P(const std::string& s) { return fs::u8path(s); }
std::string S(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

const char* kLatestUrl = "https://api.github.com/repos/SashexSRB/SSNE-Native/releases/latest";
#ifdef _WIN32
const char* kAssetSuffix = "-Windows-x64.zip";
const char* kLauncherName = "SSNE-Native.exe";
#else
const char* kAssetSuffix = "-Linux-x86_64.tar.gz";
const char* kLauncherName = "SSNE-Native";
#endif
const char* kLeftovers = "update-leftovers.txt";

// The string value of the next "key": "..." at or after `from` (GitHub's JSON: no escapes in the values we read).
std::string json_string(const std::string& js, const std::string& key, size_t from, size_t* at = nullptr) {
    size_t k = js.find("\"" + key + "\"", from);
    if (k == std::string::npos) return {};
    size_t q = js.find('"', js.find(':', k) + 1);
    if (q == std::string::npos) return {};
    size_t e = js.find('"', q + 1);
    if (e == std::string::npos) return {};
    if (at) *at = e;
    return js.substr(q + 1, e - q - 1);
}

std::vector<int> parse_version(const std::string& v) {
    std::vector<int> out;
    std::stringstream ss(v[0] == 'v' ? v.substr(1) : v);
    std::string part;
    while (std::getline(ss, part, '.')) out.push_back(atoi(part.c_str()));
    return out;
}
bool newer(const std::string& a, const std::string& b) {  // a > b
    std::vector<int> x = parse_version(a), y = parse_version(b);
    x.resize(std::max(x.size(), y.size())); y.resize(x.size());
    return x > y;
}

// Moves `from` to `to`, replacing it. A file in use (the running launcher on Windows) can't be replaced but can be
// renamed: it's moved aside first, and deleted now or, failing that, by cleanup() after the restart.
bool replace_file(const fs::path& from, const fs::path& to, std::vector<std::string>& leftovers, std::string& error) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (fs::exists(to, ec)) {
        fs::path old = to;
        old += ".old";
        fs::remove(old, ec);
        fs::rename(to, old, ec);
        if (ec) { error = "cannot replace " + S(to) + ": " + ec.message(); return false; }
        fs::remove(old, ec);
        if (ec) leftovers.push_back(S(old));
    }
    fs::rename(from, to, ec);
    if (ec) { error = "cannot move " + S(to) + " into place: " + ec.message(); return false; }
    return true;
}

} // namespace

const char* current_version() { return SSNE_VERSION; }

bool check(Release& r, std::string& error) {
    const char* url = getenv("SSNE_UPDATE_URL");  // testing: another release feed
    fs::path tmp = fs::temp_directory_path() / "ssne-native-latest.json";
    if (!platform::download(url ? url : kLatestUrl, S(tmp), nullptr, error)) return false;
    std::ifstream in(tmp, std::ios::binary);
    std::string js((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::error_code ec;
    fs::remove(tmp, ec);
    fs::remove(S(tmp) + ".log", ec);
    std::string tag = json_string(js, "tag_name", 0);
    if (tag.empty()) { error = "no release information"; return false; }
    r.version = tag[0] == 'v' ? tag.substr(1) : tag;
    if (!newer(r.version, current_version())) return false;
    for (size_t at = 0;;) {
        std::string u = json_string(js, "browser_download_url", at, &at);
        if (u.empty()) break;
        if (u.size() > strlen(kAssetSuffix) && u.compare(u.size() - strlen(kAssetSuffix), std::string::npos, kAssetSuffix) == 0) {
            r.asset_url = u;
            r.asset_name = u.substr(u.rfind('/') + 1);
            return true;
        }
    }
    return false;  // no package for this OS
}

bool install(const Release& r, const std::string& base_dir,
             const std::function<bool(const std::string&, double)>& progress, std::string& launcher, std::string& error) {
    std::error_code ec;
    fs::path base = P(base_dir), dir = base / "update", files = dir / "files";
    fs::remove_all(dir, ec);
    fs::create_directories(files, ec);
    if (ec) { error = "cannot create " + S(dir) + ": " + ec.message(); return false; }
    fs::path pkg = dir / P(r.asset_name), part = pkg;
    part += ".part";
    std::string label = "Downloading SSNE-Native " + r.version;
    if (!platform::download(r.asset_url, S(part), [&](uint64_t d, uint64_t t) {
            char buf[64];
            snprintf(buf, sizeof(buf), "  %.1f MB", d / 1048576.0);
            return progress(label + buf, t ? double(d) / double(t) : 0.0);
        }, error)) {
        error = "download failed: " + error;
        return false;
    }
    fs::rename(part, pkg, ec);
    if (!progress("Unpacking", 0)) { error = "cancelled"; return false; }
#ifdef _WIN32
    if (!gc::zip::extract(S(pkg), S(files), [](const std::string& n) { return n; },
                          [&](uint64_t d, uint64_t t) { return progress("Unpacking", t ? double(d) / double(t) : 0.0); }, error))
        return false;
#else
    {
        std::string tar = fs::exists("/usr/bin/tar") ? "/usr/bin/tar" : "/bin/tar";
        platform::Process* p = platform::spawn({tar, "-xzf", S(pkg), "-C", S(files)}, S(dir), S(dir / "tar.log"), error);
        if (!p) return false;
        int code = 1;
        while (!platform::poll(p, code)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        platform::release(p);
        if (code != 0) { error = "unpacking failed (tar exit " + std::to_string(code) + ")"; return false; }
    }
#endif
    // The package has one top folder (SSNE-Native-<version>-<os>/).
    fs::path root = files;
    {
        int n = 0;
        fs::path only;
        for (auto& e : fs::directory_iterator(files, ec)) { n++; only = e.path(); }
        if (n == 1 && fs::is_directory(only, ec)) root = only;
    }
    if (!fs::exists(root / kLauncherName, ec)) { error = "the package has no " + std::string(kLauncherName); return false; }
    progress("Installing", 0);
    std::vector<std::string> leftovers;
    std::vector<fs::path> moves;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) moves.push_back(it->path());
    for (const fs::path& f : moves)
        if (!replace_file(f, base / f.lexically_relative(root), leftovers, error)) return false;
    if (!leftovers.empty()) {
        std::ofstream out(base / kLeftovers, std::ios::app);
        for (auto& l : leftovers) out << l << "\n";
    }
    fs::remove_all(dir, ec);
    launcher = S(base / kLauncherName);
    return true;
}

void cleanup(const std::string& base_dir) {
    std::error_code ec;
    fs::path base = P(base_dir), list = base / kLeftovers;
    fs::remove_all(base / "update", ec);
    std::ifstream in(list);
    if (!in) return;
    std::string line;
    while (std::getline(in, line))
        if (!line.empty()) fs::remove(P(line), ec);
    in.close();
    fs::remove(list, ec);
}

} // namespace updater
