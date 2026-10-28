#include "platform.h"

#include <filesystem>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <winhttp.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <climits>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace platform {

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
static std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}
#endif

std::string open_file_dialog(const char* title) {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"GameCube disc image (*.iso;*.gcm;*.rvz;*.wia;*.gcz;*.ciso)\0*.iso;*.gcm;*.rvz;*.wia;*.gcz;*.ciso\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf) / sizeof(buf[0]);
    std::wstring wtitle = widen(title);
    ofn.lpstrTitle = wtitle.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    return narrow(buf);
#else
    // Linux/macOS: try zenity / kdialog; drag-and-drop and the path box also work.
    const char* cmds[] = {
        "zenity --file-selection --title='Select Serious Sam disc image' --file-filter='*.iso *.gcm *.rvz *.wia *.gcz *.ciso' 2>/dev/null",
        "kdialog --getopenfilename . '*.iso *.gcm *.rvz *.wia *.gcz *.ciso' 2>/dev/null",
    };
    for (auto* c : cmds) {
        FILE* p = popen(c, "r");
        if (!p) continue;
        char buf[4096] = {};
        bool got = fgets(buf, sizeof(buf), p) != nullptr;
        pclose(p);
        if (got) {
            std::string s(buf);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (!s.empty()) return s;
        }
    }
    return {};
#endif
}

std::string exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, sizeof(buf) / sizeof(buf[0]));
    return narrow(fs::path(std::wstring(buf, n)).parent_path().wstring());
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    return fs::path(buf).parent_path().string();
#endif
}

bool file_exists(const std::string& path) {
    std::error_code ec;
#ifdef _WIN32
    return fs::is_regular_file(fs::path(widen(path)), ec);
#else
    return fs::is_regular_file(path, ec);
#endif
}

std::string port_exe_name() {
#ifdef _WIN32
    return "SSNE-Native-Game.exe";
#else
    return "SSNE-Native-Game";
#endif
}

std::vector<std::string> legacy_port_exe_names() {
#ifdef _WIN32
    return {"SillyReEncounter-Game.exe", "SeriousReEncounter-Game.exe", "ssne-port.exe"};
#else
    return {"SillyReEncounter-Game", "SeriousReEncounter-Game", "ssne-port"};
#endif
}

bool launch(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd,
            std::string& error) {
#ifdef _WIN32
    std::wstring cmd = L"\"" + widen(exe) + L"\"";
    for (auto& a : args) cmd += L" \"" + widen(a) + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring wcwd = widen(cwd);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, wcwd.c_str(), &si, &pi)) {
        error = "CreateProcess failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    std::string old = fs::current_path().string();
    if (chdir(cwd.c_str()) != 0) { error = "cannot enter " + cwd; return false; }
    pid_t pid;
    int rc = posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ);
    if (chdir(old.c_str()) != 0) {}
    if (rc != 0) { error = "posix_spawn failed (" + std::to_string(rc) + ")"; return false; }
    return true;
#endif
}

// ------------------------------------------------------------------ child processes
#ifdef _WIN32
struct Process { HANDLE h = nullptr; };

static std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { bs++; continue; }
        if (c == L'"') { q.append(bs * 2 + 1, L'\\'); q += c; bs = 0; continue; }
        q.append(bs, L'\\'); bs = 0; q += c;
    }
    q.append(bs * 2, L'\\');
    return q + L"\"";
}

Process* spawn(const std::vector<std::string>& argv, const std::string& cwd, const std::string& log_path,
               std::string& error) {
    std::wstring cmd;
    for (auto& a : argv) { if (!cmd.empty()) cmd += L' '; cmd += quote_arg(widen(a)); }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = CreateFileW(widen(log_path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) { error = "cannot create " + log_path; return nullptr; }
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = log;
    si.hStdError = log;
    PROCESS_INFORMATION pi{};
    std::wstring wcwd = widen(cwd);
    BOOL ok = CreateProcessW(widen(argv[0]).c_str(), cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, wcwd.c_str(), &si, &pi);
    DWORD e = GetLastError();
    CloseHandle(log);
    if (!ok) { error = "cannot start " + argv[0] + " (error " + std::to_string(e) + ")"; return nullptr; }
    CloseHandle(pi.hThread);
    return new Process{pi.hProcess};
}

bool poll(Process* p, int& exit_code) {
    if (WaitForSingleObject(p->h, 0) != WAIT_OBJECT_0) return false;
    DWORD c = 1;
    GetExitCodeProcess(p->h, &c);
    exit_code = int(c);
    return true;
}
void kill(Process* p) { TerminateProcess(p->h, 1); WaitForSingleObject(p->h, 5000); }
void release(Process* p) { if (p) { CloseHandle(p->h); delete p; } }

bool download(const std::string& url, const std::string& path,
              const std::function<bool(uint64_t, uint64_t)>& progress, std::string& error) {
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    wchar_t host[256], urlpath[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = urlpath; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { error = "bad URL"; return false; }
    HINTERNET ses = WinHttpOpen(L"SSNE-Native-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(L"SSNE-Native-Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { error = "WinHttpOpen failed"; return false; }
    {   // Windows 7 / 8.0: WinHTTP offers only TLS 1.0 by default (GitHub needs 1.2; KB3140245 adds it). Asking for
        // 1.2 explicitly works there once that update is installed, and is harmless on newer Windows.
        DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 |
                       WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos));
    }
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", urlpath, nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    auto cleanup = [&] { if (req) WinHttpCloseHandle(req); if (con) WinHttpCloseHandle(con); WinHttpCloseHandle(ses); };
    if (!req || !WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        error = "could not connect to " + narrow(host) + " (error " + std::to_string(GetLastError()) + ")";
        cleanup();
        return false;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) { error = "server answered HTTP " + std::to_string(status); cleanup(); return false; }
    wchar_t clen[32] = L"";
    DWORD cl = sizeof(clen);
    uint64_t total = 0;
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clen, &cl,
                            WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(clen, nullptr, 10);
    FILE* f = _wfopen(widen(path).c_str(), L"wb");
    if (!f) { error = "cannot write " + path; cleanup(); return false; }
    std::vector<char> buf(1 << 16);
    uint64_t done = 0;
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), DWORD(buf.size()), &got)) { error = "download interrupted"; ok = false; break; }
        if (got == 0) break;
        if (fwrite(buf.data(), 1, got, f) != got) { error = "cannot write " + path; ok = false; break; }
        done += got;
        if (progress && !progress(done, total)) { error = "cancelled"; ok = false; break; }
    }
    fclose(f);
    cleanup();
    if (ok && total && done != total) { error = "download incomplete"; ok = false; }
    return ok;
}
#else
struct Process { pid_t pid = -1; };

Process* spawn(const std::vector<std::string>& argv, const std::string& cwd, const std::string& log_path,
               std::string& error) {
    pid_t pid = fork();
    if (pid < 0) { error = "fork failed"; return nullptr; }
    if (pid == 0) {
        if (chdir(cwd.c_str()) != 0) _exit(127);
        int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        std::vector<char*> a;
        for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        execv(a[0], a.data());
        _exit(127);
    }
    return new Process{pid};
}

bool poll(Process* p, int& exit_code) {
    int st = 0;
    pid_t r = waitpid(p->pid, &st, WNOHANG);
    if (r == 0) return false;
    exit_code = r < 0 ? 1 : WIFEXITED(st) ? WEXITSTATUS(st) : 128;
    return true;
}
void kill(Process* p) { ::kill(p->pid, SIGKILL); int st; waitpid(p->pid, &st, 0); }
void release(Process* p) { delete p; }

bool download(const std::string& url, const std::string& path,
              const std::function<bool(uint64_t, uint64_t)>& progress, std::string& error) {
    // Uses curl if available (the Windows launcher downloads natively).
    Process* p = spawn({"/usr/bin/curl", "-fsSL", "-o", path, url}, ".", path + ".log", error);
    if (!p) return false;
    int code = 1;
    while (!poll(p, code)) {
        std::error_code ec;
        uint64_t got = fs::exists(path, ec) ? fs::file_size(path, ec) : 0;
        if (progress && !progress(got, 0)) { kill(p); release(p); error = "cancelled"; return false; }
        usleep(100000);
    }
    release(p);
    if (code != 0) { error = "download failed (curl exit " + std::to_string(code) + ")"; return false; }
    return true;
}
#endif

} // namespace platform
