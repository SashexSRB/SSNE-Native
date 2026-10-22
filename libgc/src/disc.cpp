#include "gc/disc.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "gc/bytes.h"
#include "gc/dol.h"
#include "gc/sha1.h"

namespace fs = std::filesystem;

namespace gc {

namespace {

fs::path u8path(const std::string& s) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}

std::string trim_cstr(const uint8_t* p, size_t max) {
    size_t n = 0;
    while (n < max && p[n]) n++;
    std::string s(reinterpret_cast<const char*>(p), n);
    while (!s.empty() && (s.back() == ' ')) s.pop_back();
    return s;
}

} // namespace

std::unique_ptr<Disc> Disc::open(const std::string& path, std::string& error) {
    std::unique_ptr<Disc> d(new Disc());
    d->blob_ = open_blob(path, error);
    if (!d->blob_) return nullptr;
    if (d->blob_->format() != ImageFormat::RawGcm) {
        // Containers carry their own headers; check the disc magic of the image inside.
        uint8_t h[0x20];
        if (!d->blob_->read(0, h, sizeof(h))) { error = "Could not read the disc header: " + d->blob_->error(); return nullptr; }
        if (be32(h + 0x1C) != kDiscMagic) { error = "Not a GameCube disc image (missing disc magic)."; return nullptr; }
    }
    d->size_ = d->blob_->size();
    if (!d->parse(error)) return nullptr;
    return d;
}

bool Disc::read(uint64_t offset, void* dst, size_t len) {
    if (offset > size_ || len > size_ - offset) return false;
    return blob_->read(offset, dst, len);
}

std::vector<uint8_t> Disc::read_vec(uint64_t offset, size_t len) {
    std::vector<uint8_t> v(len);
    if (!read(offset, v.data(), len)) v.clear();
    return v;
}

bool Disc::parse(std::string& error) {
    uint8_t h[0x440];
    if (!read(0, h, sizeof(h))) { error = "Image is truncated (no boot header)."; return false; }
    header_.game_id = std::string(reinterpret_cast<char*>(h), 6);
    header_.disc_number = h[6];
    header_.version = h[7];
    header_.magic = be32(h + 0x1C);
    header_.title = trim_cstr(h + 0x20, 0x3E0);
    header_.dol_offset = be32(h + 0x420);
    header_.fst_offset = be32(h + 0x424);
    header_.fst_size = be32(h + 0x428);

    if (header_.fst_size == 0 || header_.fst_offset + uint64_t(header_.fst_size) > size_) {
        error = "File system table is out of range (bad or truncated dump).";
        return false;
    }
    std::vector<uint8_t> fst = read_vec(header_.fst_offset, header_.fst_size);
    if (fst.size() < 12) { error = "Could not read file system table."; return false; }

    uint32_t count = be32(fst.data() + 8);
    if (uint64_t(count) * 12 > fst.size()) { error = "File system table is corrupt."; return false; }
    const uint8_t* strtab = fst.data() + count * 12;
    size_t strtab_len = fst.size() - count * 12;

    // Walk entries; track the directory stack by each dir's "next" index.
    struct Dir { uint32_t end; std::string path; };
    std::vector<Dir> stack{{count, ""}};
    files_.clear();
    for (uint32_t i = 1; i < count; i++) {
        while (stack.size() > 1 && i >= stack.back().end) stack.pop_back();
        const uint8_t* e = fst.data() + i * 12;
        bool dir = e[0] == 1;
        uint32_t name_off = be32(e) & 0x00FFFFFF;
        if (name_off >= strtab_len) { error = "File name offset out of range."; return false; }
        std::string name = trim_cstr(strtab + name_off, strtab_len - name_off);
        std::string full = stack.back().path.empty() ? name : stack.back().path + "/" + name;
        FstEntry fe;
        fe.path = full;
        fe.is_dir = dir;
        if (dir) {
            stack.push_back({be32(e + 8), full});
        } else {
            fe.offset = be32(e + 4);
            fe.size = be32(e + 8);
            if (fe.offset + uint64_t(fe.size) > size_) {
                error = "File '" + full + "' lies beyond the end of the image (truncated dump?).";
                return false;
            }
        }
        files_.push_back(fe);
    }
    return true;
}

const FstEntry* Disc::find(const std::string& path) const {
    for (const auto& f : files_)
        if (!f.is_dir && f.path == path) return &f;
    return nullptr;
}

std::vector<uint8_t> Disc::read_dol() {
    uint8_t h[0x100];
    if (!read(header_.dol_offset, h, sizeof(h))) return {};
    return read_vec(header_.dol_offset, Dol::file_size_from_header(h));
}

std::vector<uint8_t> Disc::read_apploader() {
    uint8_t h[0x20];
    if (!read(0x2440, h, sizeof(h))) return {};
    uint32_t total = 0x20 + be32(h + 0x14) + be32(h + 0x18);
    return read_vec(0x2440, total);
}

std::string Disc::sha1(const Progress& progress) {
    Sha1 s;
    std::vector<uint8_t> buf(4 << 20);
    uint64_t done = 0;
    while (done < size_) {
        size_t n = size_t(std::min<uint64_t>(buf.size(), size_ - done));
        if (!blob_->read(done, buf.data(), n)) return {};
        s.update(buf.data(), n);
        done += n;
        if (progress && !progress(done, size_)) return {};
    }
    return Sha1::hex(s.finish());
}

bool Disc::extract(const std::string& out_dir, std::string& error, const Progress& progress) {
    fs::path root = u8path(out_dir);
    std::error_code ec;
    fs::create_directories(root / "sys", ec);
    fs::create_directories(root / "files", ec);
    if (ec) { error = "Could not create output folder: " + ec.message(); return false; }

    auto write_file = [&](const fs::path& p, const void* data, size_t len) {
#ifdef _WIN32
        FILE* f = _wfopen(p.c_str(), L"wb");
#else
        FILE* f = fopen(p.c_str(), "wb");
#endif
        if (!f) return false;
        bool ok = fwrite(data, 1, len, f) == len;
        fclose(f);
        return ok;
    };

    auto boot = read_vec(0, 0x440), bi2 = read_vec(0x440, 0x2000);
    auto app = read_apploader(), dol = read_dol();
    auto fst = read_vec(header_.fst_offset, header_.fst_size);
    if (boot.empty() || bi2.empty() || app.empty() || dol.empty() || fst.empty()) {
        error = "Failed to read system area of the disc.";
        return false;
    }
    if (!write_file(root / "sys/boot.bin", boot.data(), boot.size()) ||
        !write_file(root / "sys/bi2.bin", bi2.data(), bi2.size()) ||
        !write_file(root / "sys/apploader.img", app.data(), app.size()) ||
        !write_file(root / "sys/main.dol", dol.data(), dol.size()) ||
        !write_file(root / "sys/fst.bin", fst.data(), fst.size())) {
        error = "Failed to write system files.";
        return false;
    }

    uint64_t total = 0, done = 0;
    for (const auto& f : files_) total += f.size;
    std::vector<uint8_t> buf(4 << 20);
    for (const auto& f : files_) {
        fs::path p = root / "files" / u8path(f.path);
        if (f.is_dir) { fs::create_directories(p, ec); continue; }
        fs::create_directories(p.parent_path(), ec);
#ifdef _WIN32
        FILE* out = _wfopen(p.c_str(), L"wb");
#else
        FILE* out = fopen(p.c_str(), "wb");
#endif
        if (!out) { error = "Could not create " + f.path; return false; }
        uint64_t left = f.size, off = f.offset;
        while (left) {
            size_t n = left < buf.size() ? size_t(left) : buf.size();
            if (!read(off, buf.data(), n) || fwrite(buf.data(), 1, n, out) != n) {
                fclose(out);
                error = "I/O error while extracting " + f.path + (blob_->error().empty() ? "" : " (" + blob_->error() + ")");
                return false;
            }
            left -= n; off += n; done += n;
            if (progress && !progress(done, total)) { fclose(out); error = "Cancelled."; return false; }
        }
        fclose(out);
    }
    return true;
}

} // namespace gc
