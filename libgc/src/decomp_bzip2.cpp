// bzip2 decoder, used for WIA/RVZ images compressed with bzip2.
#include <cstring>
#include <vector>

#include "gc/decompress.h"

namespace gc::decomp {

namespace {

struct Error {};

struct MsbBits {
    const uint8_t* p;
    const uint8_t* end;
    uint64_t buf = 0;
    int cnt = 0;
    uint32_t get(int n) {
        while (cnt < n) {
            if (p >= end) throw Error();
            buf = buf << 8 | *p++;
            cnt += 8;
        }
        cnt -= n;
        return uint32_t((buf >> cnt) & ((1ull << n) - 1));
    }
    void align() { cnt -= cnt & 7; }
    bool at_end() const { return p >= end && cnt < 8; }
};

constexpr int kMaxLen = 20;

struct Huff {
    int32_t limit[kMaxLen + 2], base[kMaxLen + 2];
    uint16_t perm[258];
    int min_len, max_len;
    void build(const uint8_t* len, int n) {
        min_len = 32; max_len = 0;
        for (int i = 0; i < n; i++) { if (len[i] > max_len) max_len = len[i]; if (len[i] < min_len) min_len = len[i]; }
        int pp = 0;
        for (int l = min_len; l <= max_len; l++)
            for (int j = 0; j < n; j++) if (len[j] == l) perm[pp++] = uint16_t(j);
        int32_t b[kMaxLen + 3] = {};
        for (int i = 0; i < n; i++) b[len[i] + 1]++;
        for (int i = 1; i < kMaxLen + 3; i++) b[i] += b[i - 1];
        for (auto& x : limit) x = -1;
        int32_t vec = 0;
        for (int l = min_len; l <= max_len; l++) {
            vec += b[l + 1] - b[l];
            limit[l] = vec - 1;
            vec <<= 1;
        }
        for (int l = 0; l <= kMaxLen + 1; l++) base[l] = b[l];
        for (int l = min_len + 1; l <= max_len; l++) base[l] = ((limit[l - 1] + 1) << 1) - b[l];
    }
    int decode(MsbBits& bits) const {
        int n = min_len;
        int32_t v = int32_t(bits.get(n));
        while (v > limit[n]) {
            if (++n > max_len) throw Error();
            v = (v << 1) | int32_t(bits.get(1));
        }
        int idx = v - base[n];
        if (idx < 0 || idx >= 258) throw Error();
        return perm[idx];
    }
};

} // namespace

bool bzip2(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len, std::string* error) {
    size_t out = 0;
    MsbBits bits{src, src + src_len};
    std::vector<uint32_t> tt;
    try {
        while (out < dst_len) {
            if (bits.get(8) != 'B' || bits.get(8) != 'Z' || bits.get(8) != 'h') throw Error();
            uint32_t level = bits.get(8);
            if (level < '1' || level > '9') throw Error();
            size_t max_block = (level - '0') * 100000;
            tt.resize(max_block);
            for (;;) {
                uint64_t magic = uint64_t(bits.get(24)) << 24 | bits.get(24);
                bits.get(32);  // block / stream CRC (not checked)
                if (magic == 0x177245385090ull) { bits.align(); break; }
                if (magic != 0x314159265359ull) throw Error();
                if (bits.get(1)) throw Error();  // randomized blocks (obsolete)
                uint32_t orig = bits.get(24);
                // Symbol map
                uint8_t unseq[256];
                int in_use = 0;
                uint32_t groups = bits.get(16);
                for (int i = 0; i < 16; i++)
                    if (groups & (0x8000u >> i)) {
                        uint32_t m = bits.get(16);
                        for (int j = 0; j < 16; j++) if (m & (0x8000u >> j)) unseq[in_use++] = uint8_t(i * 16 + j);
                    }
                if (!in_use) throw Error();
                int alpha = in_use + 2;
                int ngroups = int(bits.get(3));
                if (ngroups < 2 || ngroups > 6) throw Error();
                int nsel = int(bits.get(15));
                if (!nsel) throw Error();
                std::vector<uint8_t> sel(static_cast<size_t>(nsel));
                uint8_t mtfg[6] = {0, 1, 2, 3, 4, 5};
                for (int i = 0; i < nsel; i++) {
                    int j = 0;
                    while (bits.get(1)) if (++j >= ngroups) throw Error();
                    uint8_t v = mtfg[j];
                    for (; j > 0; j--) mtfg[j] = mtfg[j - 1];
                    mtfg[0] = v;
                    sel[size_t(i)] = v;
                }
                Huff h[6];
                for (int g = 0; g < ngroups; g++) {
                    uint8_t len[258];
                    int cur = int(bits.get(5));
                    for (int s = 0; s < alpha; s++) {
                        for (;;) {
                            if (cur < 1 || cur > kMaxLen) throw Error();
                            if (!bits.get(1)) break;
                            cur += bits.get(1) ? -1 : 1;
                        }
                        len[s] = uint8_t(cur);
                    }
                    h[g].build(len, alpha);
                }
                // Huffman + RUNA/RUNB + MTF
                uint8_t mtf[256];
                for (int i = 0; i < 256; i++) mtf[i] = uint8_t(i);
                uint32_t counts[256] = {};
                size_t n = 0;
                int gsel = -1, gleft = 0;
                const Huff* cur_h = nullptr;
                auto next_sym = [&]() {
                    if (gleft == 0) {
                        if (++gsel >= nsel) throw Error();
                        cur_h = &h[sel[size_t(gsel)]];
                        gleft = 50;
                    }
                    gleft--;
                    return cur_h->decode(bits);
                };
                int sym = next_sym();
                for (;;) {
                    if (sym == alpha - 1) break;
                    if (sym <= 1) {
                        size_t run = 0, w = 1;
                        do {
                            run += w << sym;  // RUNA adds w, RUNB adds 2w
                            w <<= 1;
                            if (w > (1u << 24)) throw Error();
                            sym = next_sym();
                        } while (sym <= 1);
                        uint8_t c = unseq[mtf[0]];
                        if (n + run > max_block) throw Error();
                        counts[c] += uint32_t(run);
                        for (size_t i = 0; i < run; i++) tt[n++] = c;
                        continue;
                    }
                    int idx = sym - 1;
                    if (idx >= in_use) throw Error();
                    uint8_t v = mtf[idx];
                    memmove(mtf + 1, mtf, size_t(idx));
                    mtf[0] = v;
                    uint8_t c = unseq[v];
                    if (n >= max_block) throw Error();
                    counts[c]++;
                    tt[n++] = c;
                    sym = next_sym();
                }
                if (orig >= n) throw Error();
                // Inverse BWT
                uint32_t cf[256], sum = 0;
                for (int i = 0; i < 256; i++) { cf[i] = sum; sum += counts[i]; }
                for (size_t i = 0; i < n; i++) {
                    uint8_t c = uint8_t(tt[i] & 0xFF);
                    tt[cf[c]++] |= uint32_t(i) << 8;
                }
                uint32_t t = tt[orig] >> 8;
                // Output with the initial run-length decoding (4 equal bytes + repeat count)
                int last = -1, run = 0;
                for (size_t i = 0; i < n; i++) {
                    t = tt[t];
                    uint8_t c = uint8_t(t & 0xFF);
                    t >>= 8;
                    if (run == 4) {
                        if (out + c > dst_len) throw Error();
                        memset(dst + out, last, c);
                        out += c;
                        run = 0;
                        last = -1;
                        continue;
                    }
                    if (c == last) run++; else { run = 1; last = c; }
                    if (out >= dst_len) throw Error();
                    dst[out++] = c;
                }
            }
            if (bits.at_end()) break;
        }
    } catch (const Error&) {
        if (error) *error = "bzip2: corrupt data";
        return false;
    }
    if (out != dst_len) { if (error) *error = "bzip2: stream shorter than expected"; return false; }
    return true;
}

} // namespace gc::decomp
