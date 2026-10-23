// Minimal PNG writer for screenshots: 8-bit RGB, Paeth/Sub/Up row filters (picked per row), and a small deflate
// encoder (LZ77 with hash chains + the fixed Huffman code). Good enough compression for game images without
// pulling in a PNG library.
#include "png_write.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace port {
namespace {

uint32_t crc_table[256];
void make_crc() {
    static bool done = false;
    if (done) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    done = true;
}
uint32_t crc(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t acc = 0;
    int n = 0;
    void put(uint32_t bits, int count) {  // LSB first
        acc |= bits << n;
        n += count;
        while (n >= 8) { out.push_back(uint8_t(acc)); acc >>= 8; n -= 8; }
    }
    void put_rev(uint32_t code, int len) {  // Huffman codes are sent MSB first
        uint32_t r = 0;
        for (int i = 0; i < len; i++) r |= ((code >> i) & 1) << (len - 1 - i);
        put(r, len);
    }
    void flush() { if (n) { out.push_back(uint8_t(acc)); acc = 0; n = 0; } }
};

void put_literal(BitWriter& bw, int v) {  // fixed Huffman literal/length codes
    if (v < 144) bw.put_rev(0x30 + v, 8);
    else if (v < 256) bw.put_rev(0x190 + (v - 144), 9);
    else if (v < 280) bw.put_rev(v - 256, 7);
    else bw.put_rev(0xC0 + (v - 280), 8);
}

const int kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
                          131, 163, 195, 227, 258};
const int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
                           2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void put_match(BitWriter& bw, int len, int dist) {
    int li = 28;
    while (kLenBase[li] > len) li--;
    put_literal(bw, 257 + li);
    if (kLenExtra[li]) bw.put(uint32_t(len - kLenBase[li]), kLenExtra[li]);
    int di = 29;
    while (kDistBase[di] > dist) di--;
    bw.put_rev(uint32_t(di), 5);
    if (kDistExtra[di]) bw.put(uint32_t(dist - kDistBase[di]), kDistExtra[di]);
}

// zlib stream (deflate, one fixed-Huffman block) of `in`.
std::vector<uint8_t> zlib_compress(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out = {0x78, 0x01};
    BitWriter bw{out};
    bw.put(1, 1);  // final block
    bw.put(1, 2);  // fixed Huffman
    constexpr int kWindow = 32768, kHashBits = 15, kMaxChain = 32;
    std::vector<int> head(1 << kHashBits, -1), prev(in.size(), -1);
    auto hash = [&](size_t i) { return ((in[i] << 10) ^ (in[i + 1] << 5) ^ in[i + 2]) & ((1 << kHashBits) - 1); };
    size_t i = 0, n = in.size();
    auto insert = [&](size_t p) { if (p + 2 < n) { int h = hash(p); prev[p] = head[h]; head[h] = int(p); } };
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + 2 < n) {
            int cand = head[hash(i)];
            for (int chain = 0; cand >= 0 && chain < kMaxChain && int(i) - cand <= kWindow; chain++, cand = prev[cand]) {
                int len = 0, max = int(std::min<size_t>(258, n - i));
                while (len < max && in[size_t(cand) + len] == in[i + len]) len++;
                if (len > best_len) { best_len = len; best_dist = int(i) - cand; if (len == max) break; }
            }
        }
        if (best_len >= 3) {
            put_match(bw, best_len, best_dist);
            for (int k = 0; k < best_len; k++) insert(i + k);
            i += size_t(best_len);
        } else {
            put_literal(bw, in[i]);
            insert(i);
            i++;
        }
    }
    put_literal(bw, 256);  // end of block
    bw.flush();
    uint32_t a = 1, b = 0;  // Adler-32
    for (uint8_t c : in) { a = (a + c) % 65521; b = (b + a) % 65521; }
    uint32_t adler = b << 16 | a;
    for (int k = 3; k >= 0; k--) out.push_back(uint8_t(adler >> (8 * k)));
    return out;
}

void chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
    uint32_t len = uint32_t(data.size());
    for (int k = 3; k >= 0; k--) png.push_back(uint8_t(len >> (8 * k)));
    size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    uint32_t c = crc(png.data() + start, png.size() - start) ^ 0xFFFFFFFFu;
    for (int k = 3; k >= 0; k--) png.push_back(uint8_t(c >> (8 * k)));
}

int paeth(int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

} // namespace

bool write_png(const std::string& path, const uint32_t* rgba, int w, int h) {
    make_crc();
    // Filtered scanlines: each row gets the filter with the smallest sum of absolute values (standard heuristic).
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
    std::vector<uint8_t> cur(size_t(w) * 3), up(size_t(w) * 3, 0), cand[5];
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t c = rgba[size_t(y) * w + x];
            cur[x * 3] = uint8_t(c); cur[x * 3 + 1] = uint8_t(c >> 8); cur[x * 3 + 2] = uint8_t(c >> 16);
        }
        int best = 0;
        long best_sum = -1;
        for (int f = 0; f < 5; f++) {
            cand[f].resize(cur.size());
            long sum = 0;
            for (size_t i = 0; i < cur.size(); i++) {
                int a = i >= 3 ? cur[i - 3] : 0, b = up[i], c = i >= 3 ? up[i - 3] : 0;
                int pred = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : paeth(a, b, c);
                uint8_t v = uint8_t(cur[i] - pred);
                cand[f][i] = v;
                sum += v < 128 ? v : 256 - v;
            }
            if (best_sum < 0 || sum < best_sum) { best_sum = sum; best = f; }
        }
        raw.push_back(uint8_t(best));
        raw.insert(raw.end(), cand[best].begin(), cand[best].end());
        up.swap(cur);
    }
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr(13, 0);
    for (int k = 0; k < 4; k++) { ihdr[k] = uint8_t(uint32_t(w) >> (24 - 8 * k)); ihdr[4 + k] = uint8_t(uint32_t(h) >> (24 - 8 * k)); }
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 2;   // RGB
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", zlib_compress(raw));
    chunk(png, "IEND", {});
    std::error_code ec;
    auto p = std::filesystem::u8path(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    return bool(f);
}

} // namespace port
