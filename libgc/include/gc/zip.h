// Minimal .zip reader (stored and deflate entries, Zip64) used to unpack the compiler download.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gc::zip {

struct Entry {
    std::string name;          // as stored, '/' separators
    uint64_t compressed = 0, size = 0, local_offset = 0;
    uint16_t method = 0;       // 0 stored, 8 deflate
    uint32_t crc = 0;
    bool is_dir() const { return !name.empty() && name.back() == '/'; }
};

bool list(const std::string& zip_path, std::vector<Entry>& out, std::string& error);

// Extracts every file entry for which `map(name)` returns a non-empty relative output path (UTF-8, '/' ok).
// `progress(done_bytes, total_bytes)` may return false to cancel. CRC-32 of every file is checked.
bool extract(const std::string& zip_path, const std::string& out_dir,
             const std::function<std::string(const std::string&)>& map,
             const std::function<bool(uint64_t, uint64_t)>& progress, std::string& error);

uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len);

} // namespace gc::zip
