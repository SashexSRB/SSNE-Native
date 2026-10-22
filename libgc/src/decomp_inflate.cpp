// Deflate / zlib decoder (RFC 1950/1951), used for GCZ images.
#include <cstring>
#include <vector>

#include "gc/decompress.h"

namespace gc::decomp {

namespace {

struct Bits {
    const uint8_t* p;
    const uint8_t* end;
    uint64_t buf = 0;
    int cnt = 0;
    size_t pad = 0;  // zero bytes fed past the end of the input
    bool overrun = false;
    void fill() {
        while (cnt <= 56) {
            uint64_t b = 0;
            if (p < end) b = *p++; else pad++;
            buf |= b << cnt;
            cnt += 8;
        }
        // Bits consumed past the real end of the input mean a truncated stream.
        if (pad * 8 > size_t(cnt)) overrun = true;
    }
    uint32_t peek(int n) { if (cnt < n) fill(); return uint32_t(buf & ((1ull << n) - 1)); }
    void drop(int n) { buf >>= n; cnt -= n; }
    uint32_t get(int n) { if (n == 0) return 0; uint32_t v = peek(n); drop(n); return v; }
    void align() { drop(cnt & 7); }
};

// Full lookup table for a canonical Huffman code: index = next max_len bits (LSB first), entry = sym << 4 | len.
struct Huff {
    std::vector<uint32_t> table;
    int max_len = 0;
    bool build(const uint8_t* lens, int n) {
        int count[16] = {};
        max_len = 0;
        for (int i = 0; i < n; i++) { count[lens[i]]++; if (lens[i] > max_len) max_len = lens[i]; }
        if (max_len == 0) { max_len = 1; table.assign(2, 0); return true; }  // empty code (e.g. no distances)
        count[0] = 0;
        int next[16] = {};
        int code = 0;
        for (int l = 1; l <= 15; l++) { code = (code + count[l - 1]) << 1; next[l] = code; }
        table.assign(size_t(1) << max_len, 0);
        for (int s = 0; s < n; s++) {
            int l = lens[s];
            if (!l) continue;
            uint32_t c = uint32_t(next[l]++);
            if (c >= (1u << l)) return false;  // over-subscribed
            uint32_t r = 0;
            for (int i = 0; i < l; i++) r |= ((c >> i) & 1) << (l - 1 - i);
            for (uint32_t k = r; k < table.size(); k += 1u << l) table[k] = uint32_t(s) << 4 | uint32_t(l);
        }
        return true;
    }
    int decode(Bits& b) const {
        uint32_t e = table[b.peek(max_len)];
        int l = int(e & 15);
        if (!l) return -1;
        b.drop(l);
        return int(e >> 4);
    }
};

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool fail(std::string* e, const char* m) { if (e) *e = m; return false; }

} // namespace

bool inflate(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len, bool zlib_header, std::string* error) {
    if (zlib_header) {
        if (src_len < 2 || (src[0] & 15) != 8 || ((src[0] << 8) | src[1]) % 31 != 0)
            return fail(error, "bad zlib header");
        if (src[1] & 0x20) return fail(error, "zlib preset dictionary not supported");
        src += 2; src_len -= 2;
    }
    Bits b{src, src + src_len};
    size_t out = 0;
    Huff lit, dist;
    bool last = false;
    while (!last) {
        last = b.get(1);
        int type = int(b.get(2));
        if (type == 0) {
            b.align();
            uint32_t len = b.get(16), nlen = b.get(16);
            if ((len ^ 0xFFFF) != nlen) return fail(error, "deflate: bad stored block");
            for (uint32_t i = 0; i < len; i++) {
                if (out >= dst_len) return fail(error, "deflate: output overflow");
                dst[out++] = uint8_t(b.get(8));
            }
            if (b.overrun) return fail(error, "deflate: truncated");
            continue;
        }
        if (type == 3) return fail(error, "deflate: bad block type");
        if (type == 1) {
            uint8_t l[288], d[30];
            for (int i = 0; i < 144; i++) l[i] = 8;
            for (int i = 144; i < 256; i++) l[i] = 9;
            for (int i = 256; i < 280; i++) l[i] = 7;
            for (int i = 280; i < 288; i++) l[i] = 8;
            for (int i = 0; i < 30; i++) d[i] = 5;
            lit.build(l, 288); dist.build(d, 30);
        } else {
            int hlit = int(b.get(5)) + 257, hdist = int(b.get(5)) + 1, hclen = int(b.get(4)) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19] = {};
            for (int i = 0; i < hclen; i++) cl[order[i]] = uint8_t(b.get(3));
            Huff clh;
            if (!clh.build(cl, 19)) return fail(error, "deflate: bad code lengths");
            uint8_t lens[320] = {};
            int n = 0;
            while (n < hlit + hdist) {
                int s = clh.decode(b);
                if (s < 0) return fail(error, "deflate: bad code length code");
                if (s < 16) { lens[n++] = uint8_t(s); continue; }
                int rep, val = 0;
                if (s == 16) { if (!n) return fail(error, "deflate: repeat with no previous"); val = lens[n - 1]; rep = 3 + int(b.get(2)); }
                else if (s == 17) rep = 3 + int(b.get(3));
                else rep = 11 + int(b.get(7));
                if (n + rep > hlit + hdist) return fail(error, "deflate: too many lengths");
                while (rep--) lens[n++] = uint8_t(val);
            }
            if (!lit.build(lens, hlit) || !dist.build(lens + hlit, hdist)) return fail(error, "deflate: bad codes");
        }
        for (;;) {
            int s = lit.decode(b);
            if (s < 0 || b.overrun) return fail(error, "deflate: bad literal/length code");
            if (s < 256) {
                if (out >= dst_len) return fail(error, "deflate: output overflow");
                dst[out++] = uint8_t(s);
                continue;
            }
            if (s == 256) break;
            s -= 257;
            if (s >= 29) return fail(error, "deflate: bad length symbol");
            size_t len = kLenBase[s] + b.get(kLenExtra[s]);
            int ds = dist.decode(b);
            if (ds < 0 || ds >= 30) return fail(error, "deflate: bad distance symbol");
            size_t d = kDistBase[ds] + b.get(kDistExtra[ds]);
            if (d > out) return fail(error, "deflate: distance too far back");
            if (out + len > dst_len) return fail(error, "deflate: output overflow");
            uint8_t* o = dst + out;
            const uint8_t* from = o - d;
            for (size_t i = 0; i < len; i++) o[i] = from[i];
            out += len;
        }
    }
    if (out != dst_len) return fail(error, "deflate: stream shorter than expected");
    return true;
}

} // namespace gc::decomp
