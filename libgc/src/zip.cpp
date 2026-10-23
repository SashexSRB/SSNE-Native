#include "gc/zip.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "gc/decompress.h"

namespace fs = std::filesystem;

namespace gc::zip {

namespace {
struct File {
    FILE* f = nullptr;
    File(const fs::path& path, const char* mode) {
#ifdef _WIN32
        std::wstring m(mode, mode + strlen(mode));
        f = _wfopen(path.c_str(), m.c_str());
#else
        f = fopen(path.c_str(), mode);
#endif
    }
    ~File() { if (f) fclose(f); }
    bool seek(uint64_t off) {
#ifdef _WIN32
        return _fseeki64(f, int64_t(off), SEEK_SET) == 0;
#else
        return fseeko(f, off_t(off), SEEK_SET) == 0;
#endif
    }
    uint64_t size() {
#ifdef _WIN32
        _fseeki64(f, 0, SEEK_END);
        return uint64_t(_ftelli64(f));
#else
        fseeko(f, 0, SEEK_END);
        return uint64_t(ftello(f));
#endif
    }
    bool read(void* p, size_t n) { return fread(p, 1, n, f) == n; }
};
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }
} // namespace

uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool init = [] {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return true;
    }();
    (void)init;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool list(const std::string& zip_path, std::vector<Entry>& out, std::string& error) {
    File f(fs::u8path(zip_path), "rb");
    if (!f.f) { error = "cannot open " + zip_path; return false; }
    uint64_t size = f.size();
    // End of central directory: search the last 64 KiB + 22 bytes.
    uint64_t tail = std::min<uint64_t>(size, 65557);
    std::vector<uint8_t> t(tail);
    if (!f.seek(size - tail) || !f.read(t.data(), tail)) { error = "cannot read zip"; return false; }
    int64_t eocd = -1;
    for (int64_t i = int64_t(tail) - 22; i >= 0; i--)
        if (le32(&t[i]) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { error = "not a zip file"; return false; }
    uint64_t count = le16(&t[eocd + 10]), cd_size = le32(&t[eocd + 12]), cd_off = le32(&t[eocd + 16]);
    if (count == 0xFFFF || cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu) {  // Zip64 locator precedes the EOCD
        if (eocd < 20 || le32(&t[eocd - 20]) != 0x07064b50) { error = "bad zip64 locator"; return false; }
        uint64_t z64 = le64(&t[eocd - 20 + 8]);
        uint8_t z[56];
        if (!f.seek(z64) || !f.read(z, 56) || le32(z) != 0x06064b50) { error = "bad zip64 record"; return false; }
        count = le64(z + 32); cd_size = le64(z + 40); cd_off = le64(z + 48);
    }
    std::vector<uint8_t> cd(cd_size);
    if (!f.seek(cd_off) || !f.read(cd.data(), cd_size)) { error = "cannot read central directory"; return false; }
    size_t p = 0;
    out.clear();
    for (uint64_t i = 0; i < count; i++) {
        if (p + 46 > cd.size() || le32(&cd[p]) != 0x02014b50) { error = "corrupt central directory"; return false; }
        Entry e;
        e.method = le16(&cd[p + 10]);
        e.crc = le32(&cd[p + 16]);
        e.compressed = le32(&cd[p + 20]);
        e.size = le32(&cd[p + 24]);
        uint16_t nlen = le16(&cd[p + 28]), xlen = le16(&cd[p + 30]), clen = le16(&cd[p + 32]);
        e.local_offset = le32(&cd[p + 42]);
        if (p + 46 + nlen + xlen + clen > cd.size()) { error = "corrupt central directory"; return false; }
        e.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), nlen);
        for (auto& c : e.name) if (c == '\\') c = '/';
        // Zip64 extra field
        size_t x = p + 46 + nlen, xe = x + xlen;
        while (x + 4 <= xe) {
            uint16_t id = le16(&cd[x]), sz = le16(&cd[x + 2]);
            if (id == 0x0001) {
                size_t q = x + 4;
                if (e.size == 0xFFFFFFFFu && q + 8 <= x + 4 + sz) { e.size = le64(&cd[q]); q += 8; }
                if (e.compressed == 0xFFFFFFFFu && q + 8 <= x + 4 + sz) { e.compressed = le64(&cd[q]); q += 8; }
                if (e.local_offset == 0xFFFFFFFFu && q + 8 <= x + 4 + sz) { e.local_offset = le64(&cd[q]); q += 8; }
            }
            x += 4 + sz;
        }
        out.push_back(std::move(e));
        p += 46 + nlen + xlen + clen;
    }
    return true;
}

bool extract(const std::string& zip_path, const std::string& out_dir,
             const std::function<std::string(const std::string&)>& map,
             const std::function<bool(uint64_t, uint64_t)>& progress, std::string& error) {
    std::vector<Entry> entries;
    if (!list(zip_path, entries, error)) return false;
    uint64_t total = 0, done = 0;
    std::vector<std::pair<const Entry*, std::string>> todo;
    for (auto& e : entries) {
        if (e.is_dir()) continue;
        std::string rel = map(e.name);
        if (rel.empty()) continue;
        if (rel.find("..") != std::string::npos || rel[0] == '/' || rel.find(':') != std::string::npos) {
            error = "unsafe path in zip: " + e.name;
            return false;
        }
        todo.push_back({&e, rel});
        total += e.size;
    }
    File f(fs::u8path(zip_path), "rb");
    if (!f.f) { error = "cannot open " + zip_path; return false; }
    fs::path root = fs::u8path(out_dir);
    std::vector<uint8_t> comp, data;
    for (auto& [e, rel] : todo) {
        uint8_t lh[30];
        if (!f.seek(e->local_offset) || !f.read(lh, 30) || le32(lh) != 0x04034b50) { error = "corrupt entry " + e->name; return false; }
        uint64_t start = e->local_offset + 30 + le16(lh + 26) + le16(lh + 28);
        comp.resize(e->compressed);
        if (!f.seek(start) || (e->compressed && !f.read(comp.data(), e->compressed))) { error = "cannot read " + e->name; return false; }
        data.resize(e->size);
        if (e->method == 0) {
            if (e->compressed != e->size) { error = "bad stored entry " + e->name; return false; }
            data = comp;
        } else if (e->method == 8) {
            std::string derr;
            if (e->size && !decomp::inflate(comp.data(), comp.size(), data.data(), data.size(), false, &derr)) {
                error = "cannot decompress " + e->name + ": " + derr;
                return false;
            }
        } else {
            error = "unsupported compression method " + std::to_string(e->method) + " in " + e->name;
            return false;
        }
        if (crc32(0, data.data(), data.size()) != e->crc) { error = "CRC mismatch in " + e->name; return false; }
        fs::path out = root / fs::u8path(rel);
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        File o(out, "wb");
        if (!o.f || (data.size() && fwrite(data.data(), 1, data.size(), o.f) != data.size())) {
            error = "cannot write " + rel;
            return false;
        }
        done += e->size;
        if (progress && !progress(done, total)) { error = "cancelled"; return false; }
    }
    return true;
}

} // namespace gc::zip
