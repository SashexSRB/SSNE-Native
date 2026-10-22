// Disc image containers. Formats follow Dolphin's implementations (Source/Core/DiscIO: CISOBlob, CompressedBlob,
// WIABlob and docs/WiaAndRvz.md), reimplemented here with the built-in decompressors.
#include "gc/blob.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include "gc/bytes.h"
#include "gc/decompress.h"
#include "gc/disc.h"

namespace fs = std::filesystem;

namespace gc {

const char* to_string(ImageFormat f) {
    switch (f) {
    case ImageFormat::RawGcm: return "ISO";
    case ImageFormat::Ciso: return "CISO";
    case ImageFormat::Gcz: return "GCZ";
    case ImageFormat::Wia: return "WIA";
    case ImageFormat::Rvz: return "RVZ";
    case ImageFormat::Wbfs: return "WBFS";
    default: return "unknown";
    }
}

namespace {

uint32_t le32(const uint8_t* p) { return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t le64(const uint8_t* p) { return le32(p) | uint64_t(le32(p + 4)) << 32; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

class File {
public:
    ~File() { if (f_) fclose(f_); }
    bool open(const std::string& path) {
#ifdef _WIN32
#if defined(__cpp_char8_t)
        fs::path p(std::u8string(path.begin(), path.end()));
#else
        fs::path p = fs::u8path(path);
#endif
        f_ = _wfopen(p.c_str(), L"rb");
#else
        f_ = fopen(path.c_str(), "rb");
#endif
        if (!f_) return false;
#ifdef _WIN32
        _fseeki64(f_, 0, SEEK_END);
        size_ = uint64_t(_ftelli64(f_));
#else
        fseeko(f_, 0, SEEK_END);
        size_ = uint64_t(ftello(f_));
#endif
        return true;
    }
    uint64_t size() const { return size_; }
    bool read(uint64_t off, void* dst, size_t len) {
        if (off > size_ || len > size_ - off) return false;
#ifdef _WIN32
        if (_fseeki64(f_, int64_t(off), SEEK_SET) != 0) return false;
#else
        if (fseeko(f_, off_t(off), SEEK_SET) != 0) return false;
#endif
        return fread(dst, 1, len, f_) == len;
    }

private:
    FILE* f_ = nullptr;
    uint64_t size_ = 0;
};

std::string kib(uint64_t n) {
    if (n >= (1u << 20) && n % (1u << 20) == 0) return std::to_string(n >> 20) + " MiB";
    return std::to_string(n >> 10) + " KiB";
}

// ---------------------------------------------------------------------------------------------------------------
class PlainBlob : public BlobReader {
public:
    File file;
    ImageFormat format() const override { return ImageFormat::RawGcm; }
    uint64_t size() const override { return file.size(); }
    bool read(uint64_t off, void* dst, size_t len) override {
        if (!file.read(off, dst, len)) { error_ = "read error"; return false; }
        return true;
    }
    std::string describe() const override { return "ISO (uncompressed)"; }
};

// ---------------------------------------------------------------------------------------------------------------
// CISO: 0x8000-byte header ("CISO", u32 LE block size, one byte per block: present or not), then the present
// blocks in order. Missing blocks read as zeros.
class CisoBlob : public BlobReader {
public:
    File file;
    uint32_t block = 0;
    std::vector<int64_t> map;  // block -> index in file, or -1
    uint64_t size_ = 0;

    bool init(std::string& err) {
        std::vector<uint8_t> h(0x8000);
        if (!file.read(0, h.data(), h.size())) { err = "CISO header is truncated."; return false; }
        block = le32(&h[4]);
        if (block == 0 || (block & (block - 1)) || block > (64u << 20)) { err = "CISO block size is invalid."; return false; }
        map.resize(0x8000 - 8);
        int64_t used = 0, last = -1;
        for (size_t i = 0; i < map.size(); i++) {
            if (h[8 + i] > 1) { err = "CISO block map is corrupt."; return false; }
            map[i] = h[8 + i] ? used++ : -1;
            if (h[8 + i]) last = int64_t(i);
        }
        uint64_t full = uint64_t(map.size()) * block;
        // Dolphin reports the whole map range; a GameCube disc is exactly kFullDiscSize, so cut it there.
        size_ = full >= kFullDiscSize ? kFullDiscSize : uint64_t(last + 1) * block;
        if (0x8000 + uint64_t(used) * block > file.size() + block) { err = "CISO file is truncated."; return false; }
        return true;
    }
    ImageFormat format() const override { return ImageFormat::Ciso; }
    uint64_t size() const override { return size_; }
    std::string describe() const override { return "CISO, " + kib(block) + " blocks"; }
    bool read(uint64_t off, void* dst, size_t len) override {
        uint8_t* o = static_cast<uint8_t*>(dst);
        if (off + len > size_) { error_ = "read past end of image"; return false; }
        while (len) {
            uint64_t b = off / block, in = off % block;
            size_t n = size_t(std::min<uint64_t>(len, block - in));
            if (b >= map.size() || map[b] < 0) memset(o, 0, n);
            else {
                uint64_t fo = 0x8000 + uint64_t(map[b]) * block + in;
                // The last stored block may be cut short in the file.
                uint64_t avail = fo < file.size() ? std::min<uint64_t>(n, file.size() - fo) : 0;
                if (avail && !file.read(fo, o, size_t(avail))) { error_ = "read error"; return false; }
                memset(o + avail, 0, n - size_t(avail));
            }
            o += n; off += n; len -= n;
        }
        return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// GCZ (Dolphin "compressed GC/Wii image"): LE header, u64 block pointers (bit 63 = stored uncompressed),
// u32 Adler-32 hashes, then zlib-compressed blocks.
class GczBlob : public BlobReader {
public:
    File file;
    uint64_t comp_size = 0, data_size = 0, data_off = 0;
    uint32_t block = 0, nblocks = 0;
    std::vector<uint64_t> ptrs;
    int64_t cached = -1;
    std::vector<uint8_t> cache, comp;

    bool init(std::string& err) {
        uint8_t h[32];
        if (!file.read(0, h, 32)) { err = "GCZ header is truncated."; return false; }
        comp_size = le64(h + 8);
        data_size = le64(h + 16);
        block = le32(h + 24);
        nblocks = le32(h + 28);
        if (!block || block > (64u << 20) || !nblocks || uint64_t(nblocks) * block < data_size) {
            err = "GCZ header is invalid.";
            return false;
        }
        std::vector<uint8_t> p(size_t(nblocks) * 8);
        if (!file.read(32, p.data(), p.size())) { err = "GCZ block table is truncated."; return false; }
        ptrs.resize(nblocks);
        for (uint32_t i = 0; i < nblocks; i++) ptrs[i] = le64(&p[i * 8]);
        data_off = 32 + uint64_t(nblocks) * 12;
        return true;
    }
    ImageFormat format() const override { return ImageFormat::Gcz; }
    uint64_t size() const override { return data_size; }
    std::string describe() const override { return "GCZ (zlib), " + kib(block) + " blocks"; }
    bool load(uint32_t b) {
        if (cached == int64_t(b)) return true;
        const uint64_t kRaw = 1ull << 63;
        uint64_t start = ptrs[b] & ~kRaw;
        uint64_t end = b + 1 < nblocks ? (ptrs[b + 1] & ~kRaw) : comp_size;
        if (end < start || end - start > block + (block >> 3) + 1024) { error_ = "GCZ block table is corrupt"; return false; }
        comp.resize(size_t(end - start));
        if (!file.read(data_off + start, comp.data(), comp.size())) { error_ = "read error"; return false; }
        cache.assign(block, 0);
        size_t want = size_t(std::min<uint64_t>(block, data_size - uint64_t(b) * block));
        if (ptrs[b] & kRaw) {
            memcpy(cache.data(), comp.data(), std::min(comp.size(), size_t(block)));
        } else {
            std::string e;
            // Dolphin compresses whole blocks; accept a short final block too.
            if (!decomp::inflate(comp.data(), comp.size(), cache.data(), block, true, &e) &&
                (want == block || !decomp::inflate(comp.data(), comp.size(), cache.data(), want, true, &e))) {
                error_ = "GCZ block " + std::to_string(b) + ": " + e;
                cached = -1;
                return false;
            }
        }
        cached = b;
        return true;
    }
    bool read(uint64_t off, void* dst, size_t len) override {
        uint8_t* o = static_cast<uint8_t*>(dst);
        if (off + len > data_size) { error_ = "read past end of image"; return false; }
        while (len) {
            uint32_t b = uint32_t(off / block);
            uint32_t in = uint32_t(off % block);
            size_t n = std::min<size_t>(len, block - in);
            if (!load(b)) return false;
            memcpy(o, cache.data() + in, n);
            o += n; off += n; len -= n;
        }
        return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Lagged Fibonacci generator that produces GameCube/Wii disc padding ("junk") data, as used by RVZ.
class JunkGen {
public:
    static constexpr int K = 521, J = 32, SEED = 17;
    void seed(const uint8_t* s) {
        pos_ = 0;
        for (int i = 0; i < SEED; i++) buf_[i] = be32(s + 4 * i);
        for (int i = SEED; i < K; i++) buf_[i] = (buf_[i - 17] << 23) ^ (buf_[i - 16] >> 9) ^ buf_[i - 1];
        // Apply the "shift by 18 instead of 16" quirk and store big-endian so output is a plain byte copy.
        for (auto& x : buf_) {
            uint32_t v = (x & 0xFF00FFFFu) | ((x >> 2) & 0x00FF0000u);
            uint8_t b[4];
            put_be32(b, v);
            memcpy(&x, b, 4);
        }
        for (int i = 0; i < 4; i++) step();
    }
    void forward(size_t n) {
        pos_ += n;
        while (pos_ >= K * 4) { step(); pos_ -= K * 4; }
    }
    void get(uint8_t* out, size_t n) {
        while (n) {
            size_t len = std::min(n, size_t(K * 4) - pos_);
            memcpy(out, reinterpret_cast<const uint8_t*>(buf_) + pos_, len);
            pos_ += len; out += len; n -= len;
            if (pos_ == K * 4) { step(); pos_ = 0; }
        }
    }

private:
    void step() {
        for (int i = 0; i < J; i++) buf_[i] ^= buf_[i + K - J];
        for (int i = J; i < K; i++) buf_[i] ^= buf_[i - J];
    }
    uint32_t buf_[K];
    size_t pos_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// WIA and RVZ. File head (0x48) -> disc struct -> raw-data and group tables (compressed with the image's method).
// Each group holds one chunk of disc data; RVZ groups may additionally be "packed" (runs of real data and
// seeds for regenerated padding).
class WiaBlob : public BlobReader {
public:
    enum Compression { NONE = 0, PURGE = 1, BZIP2 = 2, LZMA = 3, LZMA2 = 4, ZSTD = 5 };
    File file;
    bool rvz = false;
    uint64_t iso_size = 0;
    uint32_t compression = 0, chunk = 0;
    int32_t level = 0;
    uint8_t dhead[0x80] = {};
    uint8_t cdata[7] = {};
    uint32_t cdata_len = 0;
    struct RawData { uint64_t off, size; uint32_t group, ngroups; };
    struct Group { uint64_t off; uint32_t size; bool compressed; uint32_t packed; };
    std::vector<RawData> raw;
    std::vector<Group> groups;
    struct Cached { int64_t group = -1; std::vector<uint8_t> data; uint64_t use = 0; };
    Cached cache[3];
    uint64_t tick = 0;
    std::vector<uint8_t> comp, unpack_buf;

    static const char* method_name(uint32_t c) {
        switch (c) {
        case NONE: return "no compression";
        case PURGE: return "purge";
        case BZIP2: return "bzip2";
        case LZMA: return "LZMA";
        case LZMA2: return "LZMA2";
        case ZSTD: return "zstd";
        }
        return "?";
    }

    bool decompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len, std::string& e) {
        switch (compression) {
        case NONE:
            if (in_len < out_len) { e = "stored data too short"; return false; }
            memcpy(out, in, out_len);
            return true;
        case PURGE: {
            // Segments {u32 BE offset, u32 BE size, data} followed by a SHA-1; gaps are zeros.
            if (in_len < 20) { e = "purge data too short"; return false; }
            memset(out, 0, out_len);
            size_t p = 0, end = in_len - 20;
            while (p < end) {
                if (end - p < 8) { e = "purge segment header truncated"; return false; }
                uint32_t so = be32(in + p), ss = be32(in + p + 4);
                p += 8;
                if (ss > end - p || so > out_len || ss > out_len - so) { e = "purge segment out of range"; return false; }
                memcpy(out + so, in + p, ss);
                p += ss;
            }
            return true;
        }
        case BZIP2: return decomp::bzip2(in, in_len, out, out_len, &e);
        case LZMA:
            if (cdata_len < 5) { e = "missing LZMA properties"; return false; }
            return decomp::lzma(cdata, in, in_len, out, out_len, &e);
        case LZMA2:
            if (cdata_len < 1) { e = "missing LZMA2 properties"; return false; }
            return decomp::lzma2(cdata[0], in, in_len, out, out_len, &e);
        case ZSTD: return decomp::zstd(in, in_len, out, out_len, &e);
        }
        e = "unknown compression method";
        return false;
    }

    bool read_table(uint64_t off, uint32_t csize, std::vector<uint8_t>& out, std::string& err) {
        std::vector<uint8_t> c(csize);
        if (!file.read(off, c.data(), c.size())) { err = "table is truncated"; return false; }
        return decompress(c.data(), c.size(), out.data(), out.size(), err);
    }

    bool init(std::string& err) {
        uint8_t h[0x48];
        if (!file.read(0, h, sizeof(h))) { err = "WIA/RVZ header is truncated."; return false; }
        rvz = !memcmp(h, "RVZ\x01", 4);
        uint32_t disc_size = be32(h + 12);
        iso_size = be64(h + 0x24);
        if (disc_size < 0xDC || disc_size > 0x10000) { err = "WIA/RVZ disc header is invalid."; return false; }
        std::vector<uint8_t> d(disc_size);
        if (!file.read(0x48, d.data(), d.size())) { err = "WIA/RVZ disc header is truncated."; return false; }
        uint32_t disc_type = be32(&d[0]);
        compression = be32(&d[4]);
        level = int32_t(be32(&d[8]));
        chunk = be32(&d[12]);
        memcpy(dhead, &d[16], 0x80);
        uint32_t n_part = be32(&d[0x90]);
        uint32_t n_raw = be32(&d[0xB4]);
        uint64_t raw_off = be64(&d[0xB8]);
        uint32_t raw_size = be32(&d[0xC0]);
        uint32_t n_groups = be32(&d[0xC4]);
        uint64_t group_off = be64(&d[0xC8]);
        uint32_t group_size = be32(&d[0xD0]);
        cdata_len = std::min<uint32_t>(d[0xD4], 7);
        memcpy(cdata, &d[0xD5], 7);
        if (disc_type == 2 || n_part) { err = "This is a Wii disc image, not a GameCube one."; return false; }
        if (disc_type != 1) { err = "Unknown disc type in WIA/RVZ header."; return false; }
        if (compression > (rvz ? 5u : 4u) || (rvz && compression == PURGE)) {
            err = "Unsupported WIA/RVZ compression method.";
            return false;
        }
        if (chunk < 0x8000 || chunk % 0x8000 || chunk > (256u << 20)) { err = "WIA/RVZ chunk size is invalid."; return false; }
        if (iso_size < 0x440 || iso_size > (16ull << 30) || n_raw > 1024 || n_groups > (1u << 24)) {
            err = "WIA/RVZ header is invalid.";
            return false;
        }
        std::vector<uint8_t> rt(size_t(n_raw) * 24), gt(size_t(n_groups) * (rvz ? 12 : 8));
        std::string e;
        if (!read_table(raw_off, raw_size, rt, e)) { err = "WIA/RVZ raw data table: " + e; return false; }
        if (!read_table(group_off, group_size, gt, e)) { err = "WIA/RVZ group table: " + e; return false; }
        for (uint32_t i = 0; i < n_raw; i++) {
            const uint8_t* p = &rt[i * 24];
            RawData r{be64(p), be64(p + 8), be32(p + 16), be32(p + 20)};
            uint64_t skip = r.off % 0x8000;  // groups start at 32 KiB boundaries
            r.off -= skip;
            r.size += skip;
            if (uint64_t(r.group) + r.ngroups > n_groups || (r.size + chunk - 1) / chunk > r.ngroups) {
                err = "WIA/RVZ raw data table is corrupt.";
                return false;
            }
            raw.push_back(r);
        }
        for (uint32_t i = 0; i < n_groups; i++) {
            const uint8_t* p = &gt[i * (rvz ? 12 : 8)];
            Group g;
            g.off = uint64_t(be32(p)) << 2;
            uint32_t s = be32(p + 4);
            if (rvz) {
                g.size = s & 0x7FFFFFFFu;
                g.compressed = (s & 0x80000000u) != 0;
                g.packed = be32(p + 8);
            } else {
                g.size = s;
                g.compressed = compression != NONE;
                g.packed = 0;
            }
            groups.push_back(g);
        }
        return true;
    }

    ImageFormat format() const override { return rvz ? ImageFormat::Rvz : ImageFormat::Wia; }
    uint64_t size() const override { return iso_size; }
    std::string describe() const override {
        std::string s = rvz ? "RVZ, " : "WIA, ";
        s += method_name(compression);
        if (compression >= BZIP2) s += " level " + std::to_string(level);
        return s + ", " + kib(chunk) + " chunks";
    }

    // Unpacks RVZ packed data: runs of {u32 BE size (bit 31 = junk), data or 68-byte junk seed}.
    bool unpack(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len, uint64_t disc_off, std::string& e) {
        size_t ip = 0, op = 0;
        JunkGen lfg;
        while (op < out_len) {
            if (in_len - ip < 4) { e = "packed data truncated"; return false; }
            uint32_t sz = be32(in + ip);
            ip += 4;
            bool junk = sz & 0x80000000u;
            sz &= 0x7FFFFFFFu;
            if (sz > out_len - op) { e = "packed run too long"; return false; }
            if (junk) {
                if (in_len - ip < 68) { e = "junk seed truncated"; return false; }
                lfg.seed(in + ip);
                ip += 68;
                lfg.forward(size_t((disc_off + op) % 0x8000));
                lfg.get(out + op, sz);
            } else {
                if (in_len - ip < sz) { e = "packed data truncated"; return false; }
                memcpy(out + op, in + ip, sz);
                ip += sz;
            }
            op += sz;
        }
        return true;
    }

    const std::vector<uint8_t>* load(uint32_t gi, uint64_t disc_off, size_t len) {
        for (auto& c : cache)
            if (c.group == int64_t(gi)) { c.use = ++tick; return &c.data; }
        Cached* slot = &cache[0];
        for (auto& c : cache) if (c.use < slot->use) slot = &c;
        slot->group = -1;
        slot->data.assign(len, 0);
        const Group& g = groups[gi];
        std::string e;
        if (g.size) {
            comp.resize(g.size);
            if (!file.read(g.off, comp.data(), comp.size())) { error_ = "read error (file truncated?)"; return nullptr; }
            size_t target = g.packed ? g.packed : len;
            uint8_t* dst = slot->data.data();
            if (g.packed) { unpack_buf.assign(target, 0); dst = unpack_buf.data(); }
            bool ok;
            if (g.compressed) ok = decompress(comp.data(), comp.size(), dst, target, e);
            else if (comp.size() < target) { e = "stored data too short"; ok = false; }
            else { memcpy(dst, comp.data(), target); ok = true; }
            if (ok && g.packed) ok = unpack(unpack_buf.data(), target, slot->data.data(), len, disc_off, e);
            if (!ok) {
                error_ = std::string(rvz ? "RVZ" : "WIA") + " chunk " + std::to_string(gi) + ": " + e;
                return nullptr;
            }
        }
        slot->group = gi;
        slot->use = ++tick;
        return &slot->data;
    }

    bool read(uint64_t off, void* dst, size_t len) override {
        uint8_t* o = static_cast<uint8_t*>(dst);
        if (off + len > iso_size) { error_ = "read past end of image"; return false; }
        while (len) {
            if (off < 0x80) {
                size_t n = std::min<size_t>(len, size_t(0x80 - off));
                memcpy(o, dhead + off, n);
                o += n; off += n; len -= n;
                continue;
            }
            const RawData* r = nullptr;
            uint64_t next = iso_size;
            for (const auto& x : raw) {
                if (off >= x.off && off < x.off + x.size) { r = &x; break; }
                if (x.off > off) next = std::min(next, x.off);
            }
            if (!r) {  // not covered by any raw data entry: zeros
                size_t n = size_t(std::min<uint64_t>(len, next - off));
                memset(o, 0, n);
                o += n; off += n; len -= n;
                continue;
            }
            uint64_t rel = off - r->off;
            uint32_t gi = uint32_t(rel / chunk);
            uint64_t gstart = r->off + uint64_t(gi) * chunk;
            size_t glen = size_t(std::min<uint64_t>(chunk, r->off + r->size - gstart));
            const std::vector<uint8_t>* data = load(r->group + gi, gstart, glen);
            if (!data) return false;
            size_t in = size_t(off - gstart);
            size_t n = std::min(len, glen - in);
            memcpy(o, data->data() + in, n);
            o += n; off += n; len -= n;
        }
        return true;
    }
};

} // namespace

ImageFormat sniff_image(const std::string& path) {
    File f;
    if (!f.open(path)) return ImageFormat::Unknown;
    uint8_t h[0x20] = {};
    if (!f.read(0, h, sizeof(h))) return ImageFormat::Unknown;
    if (!memcmp(h, "CISO", 4)) return ImageFormat::Ciso;
    if (!memcmp(h, "RVZ\x01", 4)) return ImageFormat::Rvz;
    if (!memcmp(h, "WIA\x01", 4)) return ImageFormat::Wia;
    if (!memcmp(h, "WBFS", 4)) return ImageFormat::Wbfs;
    if (le32(h) == 0xB10BC001) return ImageFormat::Gcz;
    if (be32(h + 0x1C) == kDiscMagic) return ImageFormat::RawGcm;
    return ImageFormat::Unknown;
}

std::unique_ptr<BlobReader> open_blob(const std::string& path, std::string& error) {
    ImageFormat fmt = sniff_image(path);
    auto open_file = [&](File& f) {
        if (!f.open(path)) { error = "Could not open file."; return false; }
        return true;
    };
    switch (fmt) {
    case ImageFormat::RawGcm: {
        auto b = std::make_unique<PlainBlob>();
        if (!open_file(b->file)) return nullptr;
        return b;
    }
    case ImageFormat::Ciso: {
        auto b = std::make_unique<CisoBlob>();
        if (!open_file(b->file) || !b->init(error)) return nullptr;
        return b;
    }
    case ImageFormat::Gcz: {
        auto b = std::make_unique<GczBlob>();
        if (!open_file(b->file) || !b->init(error)) return nullptr;
        return b;
    }
    case ImageFormat::Wia: case ImageFormat::Rvz: {
        auto b = std::make_unique<WiaBlob>();
        if (!open_file(b->file) || !b->init(error)) return nullptr;
        return b;
    }
    case ImageFormat::Wbfs:
        error = "WBFS images are for Wii games. Use the GameCube disc as .iso, .rvz, .wia, .gcz or .ciso.";
        return nullptr;
    default: {
        File f;
        if (!f.open(path)) error = "Could not open file.";
        else error = "Not a GameCube disc image (unrecognized format).";
        return nullptr;
    }
    }
}

} // namespace gc
