// Texture unit: decodes GX texture formats (tiled, big-endian) into RGBA8, with TLUT support.
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <unordered_map>

#include "gx_internal.h"
#include "ssne/ppc_context.h"

namespace ssne::gx {

namespace {

inline uint32_t rgba(uint32_t r, uint32_t g_, uint32_t b, uint32_t a) { return r | (g_ << 8) | (b << 16) | (a << 24); }
inline uint32_t exp5(uint32_t v) { return (v << 3) | (v >> 2); }
inline uint32_t exp6(uint32_t v) { return (v << 2) | (v >> 4); }
inline uint32_t exp4(uint32_t v) { return v * 17; }
inline uint32_t exp3(uint32_t v) { return (v << 5) | (v << 2) | (v >> 1); }

inline uint32_t from565(uint16_t v) { return rgba(exp5(v >> 11), exp6((v >> 5) & 63), exp5(v & 31), 255); }
inline uint32_t from5a3(uint16_t v) {
    if (v & 0x8000) return rgba(exp5((v >> 10) & 31), exp5((v >> 5) & 31), exp5(v & 31), 255);
    return rgba(exp4((v >> 8) & 15), exp4((v >> 4) & 15), exp4(v & 15), exp3((v >> 12) & 7));
}
inline uint32_t fromIA8(uint16_t v) { uint32_t i = v & 0xFF; return rgba(i, i, i, v >> 8); }

struct Source {
    const uint8_t* tmem = nullptr;  // preloaded texture in TMEM
    uint32_t addr = 0;              // or guest RAM
    uint8_t b(uint32_t o) const { return tmem ? tmem[o & 0xFFFFF] : mem_read8(addr + o); }
    uint16_t h(uint32_t o) const { return uint16_t(b(o) << 8 | b(o + 1)); }
};

uint32_t tlut_color(uint32_t tlut_reg, uint32_t index) {
    uint32_t base = (tlut_reg & 0x3FF) << 9, fmt = (tlut_reg >> 10) & 3;
    uint32_t o = (base + index * 2) & 0xFFFFF;
    uint16_t v = uint16_t(g.tmem[o] << 8 | g.tmem[(o + 1) & 0xFFFFF]);
    return fmt == 0 ? fromIA8(v) : fmt == 1 ? from565(v) : from5a3(v);
}

// Block dimensions per format (width, height in texels) and bytes per block.
void block_dims(uint32_t fmt, int& bw, int& bh) {
    switch (fmt) {
    case 0: case 8: case 14: bw = 8; bh = 8; break;       // I4, C4, CMPR
    case 1: case 2: case 9: bw = 8; bh = 4; break;        // I8, IA4, C8
    default: bw = 4; bh = 4; break;                       // IA8, RGB565, RGB5A3, RGBA8, C14X2
    }
}

void decode(const Source& src, uint32_t fmt, int w, int h, uint32_t tlut, std::vector<uint32_t>& out) {
    out.assign(size_t(w) * h, 0);
    int bw, bh;
    block_dims(fmt, bw, bh);
    int bx_n = (w + bw - 1) / bw, by_n = (h + bh - 1) / bh;
    uint32_t off = 0;
    auto put = [&](int x, int y, uint32_t c) { if (x < w && y < h) out[size_t(y) * w + x] = c; };
    for (int by = 0; by < by_n; by++)
        for (int bx = 0; bx < bx_n; bx++) {
            int x0 = bx * bw, y0 = by * bh;
            switch (fmt) {
            case 0:  // I4
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x += 2) {
                        uint8_t v = src.b(off++);
                        uint32_t i0 = exp4(v >> 4), i1 = exp4(v & 15);
                        put(x0 + x, y0 + y, rgba(i0, i0, i0, i0));
                        put(x0 + x + 1, y0 + y, rgba(i1, i1, i1, i1));
                    }
                break;
            case 1:  // I8
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 8; x++) { uint32_t i = src.b(off++); put(x0 + x, y0 + y, rgba(i, i, i, i)); }
                break;
            case 2:  // IA4
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 8; x++) {
                        uint8_t v = src.b(off++);
                        uint32_t i = exp4(v & 15);
                        put(x0 + x, y0 + y, rgba(i, i, i, exp4(v >> 4)));
                    }
                break;
            case 3:  // IA8
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) { put(x0 + x, y0 + y, fromIA8(src.h(off))); off += 2; }
                break;
            case 4:  // RGB565
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) { put(x0 + x, y0 + y, from565(src.h(off))); off += 2; }
                break;
            case 5:  // RGB5A3
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) { put(x0 + x, y0 + y, from5a3(src.h(off))); off += 2; }
                break;
            case 6:  // RGBA8: 32 bytes of AR pairs, then 32 bytes of GB pairs
                for (int k = 0; k < 16; k++) {
                    uint32_t a = src.b(off + 2 * k), r = src.b(off + 2 * k + 1);
                    uint32_t gg = src.b(off + 32 + 2 * k), b = src.b(off + 32 + 2 * k + 1);
                    put(x0 + (k & 3), y0 + (k >> 2), rgba(r, gg, b, a));
                }
                off += 64;
                break;
            case 8:  // C4
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x += 2) {
                        uint8_t v = src.b(off++);
                        put(x0 + x, y0 + y, tlut_color(tlut, v >> 4));
                        put(x0 + x + 1, y0 + y, tlut_color(tlut, v & 15));
                    }
                break;
            case 9:  // C8
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 8; x++) put(x0 + x, y0 + y, tlut_color(tlut, src.b(off++)));
                break;
            case 10:  // C14X2
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) { put(x0 + x, y0 + y, tlut_color(tlut, src.h(off) & 0x3FFF)); off += 2; }
                break;
            case 14:  // CMPR: 2x2 DXT1 sub-blocks
                for (int sb = 0; sb < 4; sb++) {
                    int sx = x0 + (sb & 1) * 4, sy = y0 + (sb >> 1) * 4;
                    uint16_t c0 = src.h(off), c1 = src.h(off + 2);
                    uint32_t pal[4];
                    pal[0] = from565(c0);
                    pal[1] = from565(c1);
                    auto ch = [](uint32_t c, int s) { return (c >> s) & 0xFF; };
                    if (c0 > c1) {
                        pal[2] = rgba((2 * ch(pal[0], 0) + ch(pal[1], 0)) / 3, (2 * ch(pal[0], 8) + ch(pal[1], 8)) / 3,
                                      (2 * ch(pal[0], 16) + ch(pal[1], 16)) / 3, 255);
                        pal[3] = rgba((ch(pal[0], 0) + 2 * ch(pal[1], 0)) / 3, (ch(pal[0], 8) + 2 * ch(pal[1], 8)) / 3,
                                      (ch(pal[0], 16) + 2 * ch(pal[1], 16)) / 3, 255);
                    } else {
                        pal[2] = rgba((ch(pal[0], 0) + ch(pal[1], 0)) / 2, (ch(pal[0], 8) + ch(pal[1], 8)) / 2,
                                      (ch(pal[0], 16) + ch(pal[1], 16)) / 2, 255);
                        pal[3] = 0;  // transparent black
                    }
                    for (int y = 0; y < 4; y++) {
                        uint8_t bits = src.b(off + 4 + y);
                        for (int x = 0; x < 4; x++) put(sx + x, sy + y, pal[(bits >> (6 - 2 * x)) & 3]);
                    }
                    off += 8;
                }
                break;
            default:
                off += 32;
                break;
            }
        }
}

uint32_t data_size(uint32_t fmt, int w, int h) {
    int bw, bh;
    block_dims(fmt, bw, bh);
    uint32_t blocks = uint32_t((w + bw - 1) / bw) * uint32_t((h + bh - 1) / bh);
    return blocks * (fmt == 6 ? 64 : 32);
}

struct CacheKey {
    uint32_t addr, fmt, w, h, tlut, sample, variant = 0;  // variant: button prompt repaint (gx_prompts.cpp)
    bool operator==(const CacheKey& o) const {
        return addr == o.addr && fmt == o.fmt && w == o.w && h == o.h && tlut == o.tlut && sample == o.sample &&
               variant == o.variant;
    }
};
struct KeyHash {
    size_t operator()(const CacheKey& k) const {
        return (size_t(k.addr) * 0x9E3779B1u) ^ (k.fmt << 24) ^ (k.w << 12) ^ k.h ^ (size_t(k.tlut) << 32) ^ (size_t(k.sample) << 20) ^ (size_t(k.variant) << 40);
    }
};
std::unordered_map<CacheKey, std::shared_ptr<Texture>, KeyHash> g_cache;
uint64_t g_next_tex_id = 1;
const Texture* g_bound[8] = {};

// Cheap content fingerprint: a strided sample of the texture data (and the TLUT for palette formats).
uint32_t fingerprint(const Source& src, uint32_t size, uint32_t fmt, uint32_t tlut) {
    uint32_t hsh = 2166136261u;
    uint32_t step = std::max<uint32_t>(4, size / 64) & ~3u;
    for (uint32_t o = 0; o < size; o += step) {
        uint32_t v = uint32_t(src.b(o)) << 24 | uint32_t(src.b(o + 1)) << 16 | uint32_t(src.b(o + 2)) << 8 | src.b(o + 3);
        hsh = (hsh ^ v) * 16777619u;
    }
    if (fmt == 8 || fmt == 9 || fmt == 10) {
        uint32_t base = (tlut & 0x3FF) << 9;
        for (uint32_t o = 0; o < 512; o += 16) hsh = (hsh ^ g.tmem[(base + o) & 0xFFFFF]) * 16777619u;
    }
    return hsh;
}

} // namespace

std::shared_ptr<const Texture> texture_ref(const Texture* t) {
    return t ? t->shared_from_this() : nullptr;
}

void invalidate_textures() {
    for (auto& b : g_bound) b = nullptr;
}

void texture_cache_clear() {
    g_cache.clear();
    for (auto& b : g_bound) b = nullptr;
}

void texture_cache_trim() {
    if (g_cache.size() > 2048) {
        g_cache.clear();
        for (auto& b : g_bound) b = nullptr;
    }
}

void load_tlut(uint32_t v1) {
    uint32_t src = (g.bp[0x64] & 0x1FFFFF) << 5;
    uint32_t tmem_addr = (v1 & 0x3FF) << 9;
    uint32_t count = ((v1 >> 10) & 0x7FF) * 16;  // 16-bit entries, in units of 16
    for (uint32_t i = 0; i < count * 2; i++) g.tmem[(tmem_addr + i) & 0xFFFFF] = mem_read8(src + i);
    invalidate_textures();
}

void bp_written(uint32_t reg, uint32_t v) {
    if (reg >= 0xE0 && reg <= 0xE7) { tev_reg_write(reg, v); return; }
    if ((reg >= 0x80 && reg <= 0xBB)) invalidate_textures();
    if (reg == 0x63) {  // TMEM preload: copy lines of 32 bytes from RAM to TMEM
        uint32_t src = (g.bp[0x60] & 0x1FFFFF) << 5;
        uint32_t tmem_even = (g.bp[0x61] & 0x7FFF) << 5;
        uint32_t lines = v & 0x7FFF;
        for (uint32_t i = 0; i < lines * 32; i++) g.tmem[(tmem_even + i) & 0xFFFFF] = mem_read8(src + i);
        invalidate_textures();
    }
}

const Texture* get_texture(int map) {
    if (map < 0 || map > 7) return nullptr;
    if (g_bound[map]) return g_bound[map];
    int i = map & 3;
    uint32_t base = map < 4 ? 0x80 : 0xA0;
    uint32_t img0 = g.bp[base + 0x08 + i], img1 = g.bp[base + 0x0C + i], img3 = g.bp[base + 0x14 + i];
    uint32_t tlut = g.bp[base + 0x18 + i];
    int w = int(img0 & 0x3FF) + 1, h = int((img0 >> 10) & 0x3FF) + 1;
    uint32_t fmt = (img0 >> 20) & 0xF;
    if (gpu_enabled() && !((img1 >> 21) & 1)) {
        // An EFB copy that stays on the GPU (not encoded to RAM)?
        if (const Texture* t = gpu_lookup_efb_copy((img3 & 0xFFFFFF) << 5, fmt, w, h)) { g_bound[map] = t; return t; }
    }
    Source src;
    bool preloaded = (img1 >> 21) & 1;
    if (preloaded) src.tmem = g.tmem + ((img1 & 0x7FFF) << 5);
    else src.addr = (img3 & 0xFFFFFF) << 5;
    uint32_t size = data_size(fmt, w, h);
    // Mip levels (GPU renderer only; the software rasterizer samples the base level): decoded when the min
    // filter uses mipmaps and the max LOD allows more than the base level. Levels follow the base in memory.
    int levels = 0;
    if (gpu_enabled() && !preloaded) {
        uint32_t mode0 = g.bp[base + i], mode1 = g.bp[base + 4 + i];
        uint32_t minf = (mode0 >> 5) & 7;
        uint32_t max_lod = (mode1 >> 8) & 0xFF;  // 4.4 fixed point
        if (minf == 1 || minf == 2 || minf == 5 || minf == 6) {
            int possible = 0;
            for (int d = std::max(w, h); d > 1; d >>= 1) possible++;
            levels = std::min<int>(possible, int(max_lod >> 4));
        }
    }
    uint32_t total = size;
    for (int k = 1; k <= levels; k++) total += data_size(fmt, std::max(1, w >> k), std::max(1, h >> k));
    CacheKey key{preloaded ? 0xFFFFFFFFu - (img1 & 0x7FFF) : src.addr, fmt | uint32_t(levels) << 8, uint32_t(w), uint32_t(h),
                 tlut, fingerprint(src, total, fmt, tlut)};
    if (prompt_candidate_size(w, h)) key.variant = prompt_variant(key.sample);
    auto it = g_cache.find(key);
    if (it == g_cache.end()) {
        auto t = std::make_shared<Texture>();
        t->w = w; t->h = h;
        t->id = g_next_tex_id++;
        decode(src, fmt, w, h, tlut, t->rgba);
        uint32_t off = size;
        for (int k = 1; k <= levels; k++) {
            int lw = std::max(1, w >> k), lh = std::max(1, h >> k);
            Source ls = src;
            ls.addr += off;
            t->mips.emplace_back();
            decode(ls, fmt, lw, lh, tlut, t->mips.back());
            off += data_size(fmt, lw, lh);
        }
        counters.textures_decoded++;
        if (prompt_candidate_size(w, h) && levels == 0) {
            key.variant = prompt_apply(*t, key.sample);
        }
        it = g_cache.insert_or_assign(key, std::move(t)).first;
    }
    g_bound[map] = it->second.get();
    return g_bound[map];
}

} // namespace ssne::gx
