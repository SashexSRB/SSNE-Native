#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace platform {

// Native "Open file" dialog. Returns empty string if cancelled/unavailable. UTF-8 paths.
std::string open_file_dialog(const char* title);
// Directory containing the running executable (UTF-8, no trailing slash).
std::string exe_dir();
// Starts a detached process. Returns false with `error` set on failure.
bool launch(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd,
            std::string& error);
bool file_exists(const std::string& path);
std::string port_exe_name();          // SSNE-Native-Game(.exe)
std::vector<std::string> legacy_port_exe_names();  // game exe names from before the renames


// Child processes for the local game build. argv[0] is the program path. stdout and stderr are appended to
// `log_path`; no console window is shown.
struct Process;
Process* spawn(const std::vector<std::string>& argv, const std::string& cwd, const std::string& log_path,
               std::string& error);
bool poll(Process* p, int& exit_code);  // true once finished
void kill(Process* p);
void release(Process* p);

// HTTPS download to `path`. progress(done, total) (total 0 if unknown) may return false to cancel.
bool download(const std::string& url, const std::string& path,
              const std::function<bool(uint64_t, uint64_t)>& progress, std::string& error);

} // namespace platform
