// LZMA / LZMA2 decoder (after Igor Pavlov's public-domain LZMA specification), used for WIA/RVZ images.
#include <cstring>
#include <vector>

#include "gc/decompress.h"

namespace gc::decomp {

namespace {

struct Error {};

struct Range {
    const uint8_t* p;
    const uint8_t* end;
    uint32_t range = 0xFFFFFFFFu, code = 0;
    uint8_t next() { if (p >= end) throw Error(); return *p++; }
    void init(const uint8_t* s, const uint8_t* e) {
        p = s; end = e; range = 0xFFFFFFFFu; code = 0;
        if (next() != 0) throw Error();
        for (int i = 0; i < 4; i++) code = code << 8 | next();
    }
    void norm() { if (range < (1u << 24)) { range <<= 8; code = code << 8 | next(); } }
    int bit(uint16_t& prob) {
        uint32_t bound = (range >> 11) * prob;
        int b;
        if (code < bound) { range = bound; prob = uint16_t(prob + ((2048 - prob) >> 5)); b = 0; }
        else { range -= bound; code -= bound; prob = uint16_t(prob - (prob >> 5)); b = 1; }
        norm();
        return b;
    }
    uint32_t direct(int n) {
        uint32_t res = 0;
        while (n--) {
            range >>= 1;
            code -= range;
            uint32_t t = 0 - (code >> 31);
            code += range & t;
            norm();
            res = (res << 1) + (t + 1);
        }
        return res;
    }
};

uint32_t tree(Range& rc, uint16_t* probs, int bits) {
    uint32_t m = 1;
    for (int i = 0; i < bits; i++) m = (m << 1) + uint32_t(rc.bit(probs[m]));
    return m - (1u << bits);
}
uint32_t rtree(Range& rc, uint16_t* probs, int bits) {
    uint32_t m = 1, sym = 0;
    for (int i = 0; i < bits; i++) {
        uint32_t b = uint32_t(rc.bit(probs[m]));
        m = (m << 1) + b;
        sym |= b << i;
    }
    return sym;
}

struct LenDec {
    uint16_t choice, choice2, low[16][8], mid[16][8], high[256];
    void init() {
        choice = choice2 = 1024;
        for (auto& a : low) for (auto& x : a) x = 1024;
        for (auto& a : mid) for (auto& x : a) x = 1024;
        for (auto& x : high) x = 1024;
    }
    uint32_t decode(Range& rc, uint32_t ps) {
        if (!rc.bit(choice)) return tree(rc, low[ps], 3);
        if (!rc.bit(choice2)) return 8 + tree(rc, mid[ps], 3);
        return 16 + tree(rc, high, 8);
    }
};

struct Lzma {
    int lc = 3, lp = 0, pb = 2;
    std::vector<uint16_t> lit;
    uint16_t is_match[12 << 4], is_rep[12], rep_g0[12], rep_g1[12], rep_g2[12], rep0_long[12 << 4];
    uint16_t pos_slot[4][64], pos_dec[1 + 114], align[16];
    LenDec len, rep_len;
    uint32_t state = 0, rep[4] = {};
    uint64_t pos = 0;  // bytes since the last state reset (LZMA SDK "processedPos")

    bool set_props(uint8_t d) {
        if (d >= 9 * 5 * 5) return false;
        lc = d % 9; d /= 9; lp = d % 5; pb = d / 5;
        return true;
    }
    void reset() {
        lit.assign(size_t(0x300) << (lc + lp), 1024);
        auto fill = [](uint16_t* a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = 1024; };
        fill(is_match, sizeof(is_match) / 2); fill(is_rep, 12); fill(rep_g0, 12); fill(rep_g1, 12); fill(rep_g2, 12);
        fill(rep0_long, sizeof(rep0_long) / 2); fill(&pos_slot[0][0], 4 * 64); fill(pos_dec, 115); fill(align, 16);
        len.init(); rep_len.init();
        state = 0; rep[0] = rep[1] = rep[2] = rep[3] = 0; pos = 0;
    }

    // Decodes into dst[out..out_end). `dict_start` is the oldest byte matches may reference.
    // Returns false if an end marker was hit before out_end.
    bool decode(Range& rc, uint8_t* dst, size_t& out, size_t out_end, size_t dict_start) {
        const uint32_t pb_mask = (1u << pb) - 1, lp_mask = (1u << lp) - 1;
        while (out < out_end) {
            uint32_t ps = uint32_t(pos) & pb_mask;
            if (!rc.bit(is_match[(state << 4) + ps])) {
                uint8_t prev = out > dict_start ? dst[out - 1] : 0;
                uint16_t* probs = &lit[size_t(0x300) * (((uint32_t(pos) & lp_mask) << lc) + (prev >> (8 - lc)))];
                uint32_t sym = 1;
                if (state >= 7) {
                    if (size_t(rep[0]) + 1 > out - dict_start) throw Error();
                    uint32_t mb = dst[out - rep[0] - 1];
                    do {
                        uint32_t mbit = (mb >> 7) & 1;
                        mb <<= 1;
                        uint32_t b = uint32_t(rc.bit(probs[((1 + mbit) << 8) + sym]));
                        sym = (sym << 1) | b;
                        if (mbit != b) break;
                    } while (sym < 0x100);
                }
                while (sym < 0x100) sym = (sym << 1) | uint32_t(rc.bit(probs[sym]));
                dst[out++] = uint8_t(sym);
                pos++;
                state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
                continue;
            }
            uint32_t l;
            if (!rc.bit(is_rep[state])) {
                rep[3] = rep[2]; rep[2] = rep[1]; rep[1] = rep[0];
                l = len.decode(rc, ps);
                state = state < 7 ? 7 : 10;
                uint32_t ls = l < 3 ? l : 3;
                uint32_t slot = tree(rc, pos_slot[ls], 6);
                uint32_t dist;
                if (slot < 4) dist = slot;
                else {
                    int nd = int(slot >> 1) - 1;
                    dist = (2 | (slot & 1)) << nd;
                    if (slot < 14) dist += rtree(rc, pos_dec + dist - slot, nd);
                    else {
                        dist += rc.direct(nd - 4) << 4;
                        dist += rtree(rc, align, 4);
                    }
                }
                if (dist == 0xFFFFFFFFu) return false;  // end marker
                rep[0] = dist;
            } else {
                if (!rc.bit(rep_g0[state])) {
                    if (!rc.bit(rep0_long[(state << 4) + ps])) {
                        if (out == dict_start || size_t(rep[0]) + 1 > out - dict_start) throw Error();
                        state = state < 7 ? 9 : 11;
                        dst[out] = dst[out - rep[0] - 1];
                        out++; pos++;
                        continue;
                    }
                } else {
                    uint32_t d;
                    if (!rc.bit(rep_g1[state])) d = rep[1];
                    else {
                        if (!rc.bit(rep_g2[state])) d = rep[2];
                        else { d = rep[3]; rep[3] = rep[2]; }
                        rep[2] = rep[1];
                    }
                    rep[1] = rep[0];
                    rep[0] = d;
                }
                l = rep_len.decode(rc, ps);
                state = state < 7 ? 8 : 11;
            }
            size_t n = l + 2;
            size_t d = size_t(rep[0]) + 1;
            if (d > out - dict_start) throw Error();
            if (n > out_end - out) throw Error();
            uint8_t* o = dst + out;
            const uint8_t* from = o - d;
            for (size_t i = 0; i < n; i++) o[i] = from[i];
            out += n;
            pos += n;
        }
        return true;
    }
};

bool fail(std::string* e, const char* m) { if (e) *e = m; return false; }

} // namespace

bool lzma(const uint8_t props[5], const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len, std::string* error) {
    Lzma z;
    if (!z.set_props(props[0])) return fail(error, "lzma: bad properties");
    try {
        z.reset();
        Range rc;
        rc.init(src, src + src_len);
        size_t out = 0;
        if (!z.decode(rc, dst, out, dst_len, 0)) return fail(error, "lzma: stream shorter than expected");
    } catch (const Error&) {
        return fail(error, "lzma: corrupt data");
    }
    return true;
}

bool lzma2(uint8_t dict_prop, const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len, std::string* error) {
    if (dict_prop > 40) return fail(error, "lzma2: bad dictionary size");
    Lzma z;
    size_t in = 0, out = 0, dict_start = 0;
    bool need_dict = true, need_props = true;
    try {
        for (;;) {
            if (in >= src_len) throw Error();
            uint8_t c = src[in++];
            if (c == 0) break;
            if (c == 1 || c == 2) {
                if (src_len - in < 2) throw Error();
                size_t n = (size_t(src[in]) << 8 | src[in + 1]) + 1;
                in += 2;
                if (c == 1) { dict_start = out; need_dict = false; }
                else if (need_dict) throw Error();
                if (src_len - in < n || dst_len - out < n) throw Error();
                memcpy(dst + out, src + in, n);
                in += n; out += n;
                z.pos += n;
                continue;
            }
            if (c < 0x80) throw Error();
            if (src_len - in < 4) throw Error();
            size_t unpacked = (size_t(c & 0x1F) << 16) + (size_t(src[in]) << 8 | src[in + 1]) + 1;
            size_t packed = (size_t(src[in + 2]) << 8 | src[in + 3]) + 1;
            in += 4;
            int mode = (c >> 5) & 3;
            if (mode == 3) { dict_start = out; need_dict = false; }
            else if (need_dict) throw Error();
            if (mode >= 2) {
                if (in >= src_len || !z.set_props(src[in++])) throw Error();
                if (z.lc + z.lp > 4) throw Error();
                need_props = false;
            } else if (need_props) throw Error();
            if (mode >= 1) z.reset();
            if (src_len - in < packed || dst_len - out < unpacked) throw Error();
            Range rc;
            rc.init(src + in, src + in + packed);
            if (!z.decode(rc, dst, out, out + unpacked, dict_start)) throw Error();
            in += packed;
            if (out == dst_len) break;
        }
    } catch (const Error&) {
        return fail(error, "lzma2: corrupt data");
    }
    if (out != dst_len) return fail(error, "lzma2: stream shorter than expected");
    return true;
}

} // namespace gc::decomp
