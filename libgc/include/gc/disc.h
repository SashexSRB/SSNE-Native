// GameCube disc image reader (plain .iso / .gcm, or a CISO/GCZ/WIA/RVZ container via BlobReader).
//
// Layout reference (all big-endian):
//   0x0000 boot.bin  (0x440 bytes): game code, maker, disc#, version, magic 0xC2339F3D @0x1C, title @0x20
//            0x420: main.dol offset, 0x424: FST offset, 0x428: FST size
//   0x0440 bi2.bin   (0x2000 bytes)
//   0x2440 apploader (0x20 header, size @+0x14, trailer @+0x18)
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gc/blob.h"

namespace gc {

constexpr uint32_t kDiscMagic = 0xC2339F3D;
constexpr uint64_t kFullDiscSize = 1459978240ull; // standard 1.4 GB mini-DVD image

struct DiscHeader {
    std::string game_id;   // 6 chars, e.g. "GXXE8G"
    uint8_t disc_number = 0;
    uint8_t version = 0;
    uint32_t magic = 0;
    std::string title;
    uint32_t dol_offset = 0;
    uint32_t fst_offset = 0;
    uint32_t fst_size = 0;
};

struct FstEntry {
    std::string path;      // full path, '/'-separated, no leading slash
    bool is_dir = false;
    uint32_t offset = 0;   // file: disc offset
    uint32_t size = 0;     // file: byte size
};

class Disc {
public:
    // Opens and parses the image. On failure returns nullptr and sets `error`.
    static std::unique_ptr<Disc> open(const std::string& path, std::string& error);
    static ImageFormat sniff(const std::string& path) { return sniff_image(path); }

    ImageFormat format() const { return blob_->format(); }
    std::string describe() const { return blob_->describe(); }
    const std::string& read_error() const { return blob_->error(); }

    const DiscHeader& header() const { return header_; }
    const std::vector<FstEntry>& files() const { return files_; }
    uint64_t size() const { return size_; }

    bool read(uint64_t offset, void* dst, size_t len);
    std::vector<uint8_t> read_vec(uint64_t offset, size_t len);
    std::vector<uint8_t> read_dol();          // main.dol (size computed from its header)
    std::vector<uint8_t> read_apploader();
    const FstEntry* find(const std::string& path) const;

    using Progress = std::function<bool(uint64_t done, uint64_t total)>; // return false to cancel
    // Streams the whole image through SHA-1.
    std::string sha1(const Progress& progress = {});
    // Dolphin-style extraction: <out>/sys/{boot.bin,bi2.bin,apploader.img,main.dol,fst.bin}, <out>/files/...
    bool extract(const std::string& out_dir, std::string& error, const Progress& progress = {});

private:
    Disc() = default;
    bool parse(std::string& error);
    std::unique_ptr<BlobReader> blob_;
    uint64_t size_ = 0;
    DiscHeader header_;
    std::vector<FstEntry> files_;
};

} // namespace gc
