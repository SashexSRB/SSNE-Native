// Zstandard decoder (RFC 8878), used for RVZ images. Decodes whole frames into a caller buffer, so the window is
// simply the output decoded so far.
#include <cstring>
#include <vector>

#include "gc/decompress.h"

namespace gc::decomp {

namespace {

struct Error {};

int highbit(uint32_t v) { return 31 - __builtin_clz(v); }

uint32_t le16(const uint8_t* p) { return p[0] | uint32_t(p[1]) << 8; }
uint32_t le24(const uint8_t* p) { return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }
uint32_t le32(const uint8_t* p) { return le24(p) | uint32_t(p[3]) << 24; }

// Backward bit stream: starts at the final set bit of the last byte and reads toward the first byte.
struct BackBits {
    const uint8_t* s = nullptr;
    size_t n = 0;
    int64_t pos = 0;  // bits not yet consumed
    void init(const uint8_t* p, size_t len) {
        if (!len || !p[len - 1]) throw Error();
        s = p; n = len;
        pos = int64_t(len - 1) * 8 + highbit(p[len - 1]);
    }
    uint64_t bits_at(int64_t lo, int cnt) const {
        if (cnt == 0) return 0;
        if (lo < 0) {
            if (lo + cnt <= 0) return 0;
            return bits_at(0, int(cnt + lo)) << (-lo);
        }
        size_t byte = size_t(lo >> 3);
        uint64_t x = 0;
        if (byte + 8 <= n) memcpy(&x, s + byte, 8);
        else for (size_t i = 0; byte + i < n; i++) x |= uint64_t(s[byte + i]) << (8 * i);
        return (x >> (lo & 7)) & ((1ull << cnt) - 1);
    }
    uint32_t read(int cnt) { pos -= cnt; return uint32_t(bits_at(pos, cnt)); }
    uint32_t peek(int cnt) const { return uint32_t(bits_at(pos - cnt, cnt)); }
    void skip(int cnt) { pos -= cnt; }
};

struct FseEntry { uint8_t sym; uint8_t bits; uint16_t base; };

struct FseTable {
    std::vector<FseEntry> t;
    int log = 0;
    void build(const int16_t* norm, int max_sym, int table_log) {
        log = table_log;
        uint32_t size = 1u << table_log;
        t.assign(size, {});
        std::vector<uint16_t> next(size_t(max_sym) + 1);
        uint32_t high = size - 1;
        for (int s = 0; s <= max_sym; s++) {
            if (norm[s] == -1) { t[high--].sym = uint8_t(s); next[s] = 1; }
            else next[s] = uint16_t(norm[s] < 0 ? 0 : norm[s]);
        }
        uint32_t step = (size >> 1) + (size >> 3) + 3, mask = size - 1, p = 0;
        for (int s = 0; s <= max_sym; s++)
            for (int i = 0; i < norm[s]; i++) {
                t[p].sym = uint8_t(s);
                do p = (p + step) & mask; while (p > high);
            }
        if (p != 0) throw Error();
        for (uint32_t u = 0; u < size; u++) {
            uint32_t ns = next[t[u].sym]++;
            if (!ns) throw Error();
            int nb = table_log - highbit(ns);
            t[u].bits = uint8_t(nb);
            t[u].base = uint16_t((ns << nb) - size);
        }
    }
    void rle(uint8_t sym) { log = 0; t.assign(1, {sym, 0, 0}); }
};

// Reads an FSE normalized-count header from a forward bit stream. Returns bytes consumed.
size_t read_ncount(const uint8_t* p, size_t len, int16_t* norm, int& max_sym, int& table_log, int max_log) {
    size_t bitpos = 0;
    auto bits = [&](int cnt) -> uint32_t {  // peek
        uint64_t x = 0;
        size_t byte = bitpos >> 3;
        for (int i = 0; i < 8 && byte + i < len; i++) x |= uint64_t(p[byte + i]) << (8 * i);
        return uint32_t((x >> (bitpos & 7)) & ((1ull << cnt) - 1));
    };
    if (!len) throw Error();
    table_log = int(bits(4)) + 5;
    bitpos += 4;
    if (table_log > max_log) throw Error();
    int remaining = (1 << table_log) + 1, threshold = 1 << table_log, nb = table_log + 1;
    int sym = 0;
    bool prev0 = false;
    int limit = max_sym;
    while (remaining > 1 && sym <= limit) {
        if (prev0) {
            int n0 = sym;
            while (bits(2) == 3) { n0 += 3; bitpos += 2; }
            n0 += int(bits(2)); bitpos += 2;
            if (n0 > limit + 1) throw Error();
            while (sym < n0) norm[sym++] = 0;
            if (sym > limit) break;
        }
        int maxv = (2 * threshold - 1) - remaining;
        int count;
        if (int(bits(nb - 1) & uint32_t(threshold - 1)) < maxv) {
            count = int(bits(nb - 1) & uint32_t(threshold - 1));
            bitpos += size_t(nb - 1);
        } else {
            count = int(bits(nb) & uint32_t(2 * threshold - 1));
            if (count >= threshold) count -= maxv;
            bitpos += size_t(nb);
        }
        count--;
        remaining -= count < 0 ? -count : count;
        norm[sym++] = int16_t(count);
        prev0 = count == 0;
        while (remaining < threshold) { nb--; threshold >>= 1; }
        if ((bitpos >> 3) > len) throw Error();
    }
    if (remaining != 1) throw Error();
    max_sym = sym - 1;
    size_t used = (bitpos + 7) >> 3;
    if (used > len) throw Error();
    return used;
}

struct HufEntry { uint8_t sym; uint8_t bits; };

struct HufTable {
    std::vector<HufEntry> t;
    int max_bits = 0;
    bool valid = false;
};

// Parses a Huffman tree description; returns bytes consumed.
size_t read_huffman(const uint8_t* p, size_t len, HufTable& h) {
    if (!len) throw Error();
    uint8_t w[256] = {};
    int nw = 0;
    size_t used;
    uint32_t hdr = p[0];
    if (hdr < 128) {
        used = 1 + hdr;
        if (used > len || hdr == 0) throw Error();
        int16_t norm[256];
        int max_sym = 255, log = 0;
        size_t n = read_ncount(p + 1, hdr, norm, max_sym, log, 6);
        FseTable ft;
        ft.build(norm, max_sym, log);
        BackBits b;
        b.init(p + 1 + n, hdr - n);
        uint32_t s1 = b.read(log), s2 = b.read(log);
        for (;;) {
            if (nw >= 255) throw Error();
            w[nw++] = ft.t[s1].sym;
            s1 = ft.t[s1].base + b.read(ft.t[s1].bits);
            if (b.pos < 0) { w[nw++] = ft.t[s2].sym; break; }
            if (nw >= 255) throw Error();
            w[nw++] = ft.t[s2].sym;
            s2 = ft.t[s2].base + b.read(ft.t[s2].bits);
            if (b.pos < 0) { w[nw++] = ft.t[s1].sym; break; }
        }
    } else {
        nw = int(hdr - 127);
        used = 1 + size_t(nw + 1) / 2;
        if (used > len) throw Error();
        for (int i = 0; i < nw; i++) w[i] = (i & 1) ? (p[1 + i / 2] & 15) : (p[1 + i / 2] >> 4);
    }
    uint32_t sum = 0;
    for (int i = 0; i < nw; i++) {
        if (w[i] > 11) throw Error();
        if (w[i]) sum += 1u << (w[i] - 1);
    }
    if (!sum) throw Error();
    int max_bits = highbit(sum) + 1;
    uint32_t left = (1u << max_bits) - sum;
    if (left & (left - 1)) throw Error();  // must be a power of two
    w[nw++] = uint8_t(highbit(left) + 1);
    if (max_bits > 11) throw Error();
    // Table: weights in increasing order occupy increasing index ranges.
    uint32_t rank[13] = {};
    for (int i = 0; i < nw; i++) rank[w[i]]++;
    uint32_t start[13] = {}, next = 0;
    for (int k = 1; k <= max_bits; k++) { start[k] = next; next += rank[k] << (k - 1); }
    h.t.assign(size_t(1) << max_bits, {});
    for (int s = 0; s < nw; s++) {
        if (!w[s]) continue;
        uint32_t cnt = (1u << w[s]) >> 1;
        HufEntry e{uint8_t(s), uint8_t(max_bits + 1 - w[s])};
        for (uint32_t i = 0; i < cnt; i++) h.t[start[w[s]] + i] = e;
        start[w[s]] += cnt;
    }
    h.max_bits = max_bits;
    h.valid = true;
    return used;
}

void huf_stream(const HufTable& h, const uint8_t* p, size_t len, uint8_t* out, size_t n) {
    BackBits b;
    b.init(p, len);
    for (size_t i = 0; i < n; i++) {
        const HufEntry& e = h.t[b.peek(h.max_bits)];
        out[i] = e.sym;
        b.skip(e.bits);
    }
    if (b.pos != 0) throw Error();
}

const uint32_t kLLBase[36] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 18,
                              20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
const uint8_t kLLBits[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                             1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const uint32_t kMLBase[53] = {3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
                              21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 39, 41,
                              43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
const uint8_t kMLBits[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                             0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const int16_t kLLDefault[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2,
                                2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
const int16_t kMLDefault[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
const int16_t kOFDefault[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

struct FrameState {
    HufTable huf;
    FseTable ll, of, ml;
    bool have_ll = false, have_of = false, have_ml = false;
    uint32_t rep[3] = {1, 4, 8};
    std::vector<uint8_t> lit;
};

void decode_table(FseTable& t, bool& have, int mode, const uint8_t*& p, const uint8_t* end, const int16_t* def,
                  int def_max, int def_log, int max_sym, int max_log) {
    switch (mode) {
    case 0: t.build(def, def_max, def_log); break;
    case 1:
        if (p >= end) throw Error();
        if (*p > max_sym) throw Error();
        t.rle(*p++);
        break;
    case 2: {
        int16_t norm[64];
        int ms = max_sym, log = 0;
        p += read_ncount(p, size_t(end - p), norm, ms, log, max_log);
        t.build(norm, ms, log);
        break;
    }
    case 3:
        if (!have) throw Error();
        break;
    }
    have = true;
}

// Decodes one compressed block into dst[out..]. Returns the new output position.
size_t decode_block(FrameState& fs, const uint8_t* p, size_t len, uint8_t* dst, size_t out, size_t dst_len,
                    size_t frame_start) {
    const uint8_t* end = p + len;
    if (len < 1) throw Error();
    // --- Literals section
    int ltype = p[0] & 3, sf = (p[0] >> 2) & 3;
    size_t regen = 0, csize = 0, hsize = 0;
    int streams = 1;
    if (ltype < 2) {
        if (sf == 0 || sf == 2) { regen = p[0] >> 3; hsize = 1; }
        else if (sf == 1) { if (len < 2) throw Error(); regen = (p[0] >> 4) + (uint32_t(p[1]) << 4); hsize = 2; }
        else { if (len < 3) throw Error(); regen = (p[0] >> 4) + (uint32_t(p[1]) << 4) + (uint32_t(p[2]) << 12); hsize = 3; }
    } else {
        int bits;
        if (sf == 0) { hsize = 3; bits = 10; streams = 1; }
        else if (sf == 1) { hsize = 3; bits = 10; streams = 4; }
        else if (sf == 2) { hsize = 4; bits = 14; streams = 4; }
        else { hsize = 5; bits = 18; streams = 4; }
        if (len < hsize) throw Error();
        uint64_t h = 0;
        for (size_t i = 0; i < hsize; i++) h |= uint64_t(p[i]) << (8 * i);
        regen = (h >> 4) & ((1u << bits) - 1);
        csize = (h >> (4 + bits)) & ((1u << bits) - 1);
    }
    p += hsize;
    if (regen > (1u << 17)) throw Error();
    fs.lit.resize(regen);
    uint8_t* lit = fs.lit.data();
    if (ltype == 0) {
        if (size_t(end - p) < regen) throw Error();
        memcpy(lit, p, regen);
        p += regen;
    } else if (ltype == 1) {
        if (p >= end) throw Error();
        memset(lit, *p++, regen);
    } else {
        if (size_t(end - p) < csize) throw Error();
        const uint8_t* lp = p;
        size_t lleft = csize;
        if (ltype == 2) {
            size_t n = read_huffman(lp, lleft, fs.huf);
            lp += n; lleft -= n;
        } else if (!fs.huf.valid) throw Error();
        if (streams == 1) {
            huf_stream(fs.huf, lp, lleft, lit, regen);
        } else {
            if (lleft < 6) throw Error();
            size_t s1 = le16(lp), s2 = le16(lp + 2), s3 = le16(lp + 4);
            if (s1 + s2 + s3 + 6 > lleft) throw Error();
            size_t s4 = lleft - 6 - s1 - s2 - s3;
            size_t seg = (regen + 3) / 4;
            if (seg * 3 > regen) throw Error();
            const uint8_t* q = lp + 6;
            huf_stream(fs.huf, q, s1, lit, seg); q += s1;
            huf_stream(fs.huf, q, s2, lit + seg, seg); q += s2;
            huf_stream(fs.huf, q, s3, lit + 2 * seg, seg); q += s3;
            huf_stream(fs.huf, q, s4, lit + 3 * seg, regen - 3 * seg);
        }
        p += csize;
    }
    // --- Sequences section
    if (p >= end) throw Error();
    uint32_t nseq = p[0];
    if (nseq == 0) {
        p++;
        if (out + regen > dst_len) throw Error();
        memcpy(dst + out, lit, regen);
        return out + regen;
    }
    if (nseq < 128) p++;
    else if (nseq < 255) { if (end - p < 2) throw Error(); nseq = ((nseq - 128) << 8) + p[1]; p += 2; }
    else { if (end - p < 3) throw Error(); nseq = p[1] + (uint32_t(p[2]) << 8) + 0x7F00; p += 3; }
    if (p >= end) throw Error();
    uint8_t modes = *p++;
    if (modes & 3) throw Error();
    decode_table(fs.ll, fs.have_ll, modes >> 6, p, end, kLLDefault, 35, 6, 35, 9);
    decode_table(fs.of, fs.have_of, (modes >> 4) & 3, p, end, kOFDefault, 28, 5, 31, 8);
    decode_table(fs.ml, fs.have_ml, (modes >> 2) & 3, p, end, kMLDefault, 52, 6, 52, 9);
    BackBits b;
    b.init(p, size_t(end - p));
    uint32_t sll = b.read(fs.ll.log), sof = b.read(fs.of.log), sml = b.read(fs.ml.log);
    size_t lpos = 0;
    uint32_t* rep = fs.rep;
    for (uint32_t i = 0; i < nseq; i++) {
        uint32_t llc = fs.ll.t[sll].sym, ofc = fs.of.t[sof].sym, mlc = fs.ml.t[sml].sym;
        if (llc > 35 || mlc > 52 || ofc > 31) throw Error();
        uint32_t ofv = (1u << ofc) + b.read(int(ofc));
        uint32_t ml = kMLBase[mlc] + b.read(kMLBits[mlc]);
        uint32_t ll = kLLBase[llc] + b.read(kLLBits[llc]);
        uint32_t offset;
        if (ofv > 3) {
            offset = ofv - 3;
            rep[2] = rep[1]; rep[1] = rep[0]; rep[0] = offset;
        } else {
            uint32_t idx = ofv - 1 + (ll == 0 ? 1 : 0);
            if (idx == 0) offset = rep[0];
            else {
                offset = idx == 3 ? rep[0] - 1 : rep[idx];
                if (idx > 1) rep[2] = rep[1];
                rep[1] = rep[0];
                rep[0] = offset;
            }
        }
        if (i + 1 < nseq) {
            sll = fs.ll.t[sll].base + b.read(fs.ll.t[sll].bits);
            sml = fs.ml.t[sml].base + b.read(fs.ml.t[sml].bits);
            sof = fs.of.t[sof].base + b.read(fs.of.t[sof].bits);
        }
        if (lpos + ll > regen || out + ll + ml > dst_len) throw Error();
        memcpy(dst + out, lit + lpos, ll);
        out += ll; lpos += ll;
        if (offset == 0 || offset > out - frame_start) throw Error();
        uint8_t* o = dst + out;
        const uint8_t* from = o - offset;
        if (offset >= ml) memcpy(o, from, ml);
        else for (uint32_t k = 0; k < ml; k++) o[k] = from[k];
        out += ml;
    }
    if (b.pos != 0) throw Error();
    size_t rest = regen - lpos;
    if (out + rest > dst_len) throw Error();
    memcpy(dst + out, lit + lpos, rest);
    return out + rest;
}

} // namespace

bool zstd(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len, std::string* error) {
    size_t in = 0, out = 0;
    try {
        while (in < src_len && out < dst_len) {
            if (src_len - in < 4) throw Error();
            uint32_t magic = le32(src + in);
            in += 4;
            if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) {
                if (src_len - in < 4) throw Error();
                size_t n = le32(src + in);
                in += 4 + n;
                continue;
            }
            if (magic != 0xFD2FB528u) { if (error) *error = "zstd: bad frame magic"; return false; }
            if (in >= src_len) throw Error();
            uint8_t d = src[in++];
            int fcs_flag = d >> 6, single = (d >> 5) & 1, checksum = (d >> 2) & 1, dict_flag = d & 3;
            if (d & 8) throw Error();
            if (!single) in++;  // window descriptor (the whole output is our window)
            static const int dict_sz[4] = {0, 1, 2, 4};
            in += size_t(dict_sz[dict_flag]);
            if (dict_flag) {
                // A dictionary ID of 0 means "no dictionary"; anything else we can't decode.
                uint32_t id = 0;
                for (int i = 0; i < dict_sz[dict_flag]; i++) id |= uint32_t(src[in - size_t(dict_sz[dict_flag]) + size_t(i)]) << (8 * i);
                if (id) { if (error) *error = "zstd: dictionaries not supported"; return false; }
            }
            static const int fcs_sz[4] = {0, 2, 4, 8};
            int fs_bytes = fcs_flag == 0 ? (single ? 1 : 0) : fcs_sz[fcs_flag];
            in += size_t(fs_bytes);
            if (in > src_len) throw Error();
            FrameState fs;
            size_t frame_start = out;
            for (;;) {
                if (src_len - in < 3) throw Error();
                uint32_t bh = le24(src + in);
                in += 3;
                bool last = bh & 1;
                int type = (bh >> 1) & 3;
                size_t size = bh >> 3;
                if (type == 0) {
                    if (src_len - in < size || dst_len - out < size) throw Error();
                    memcpy(dst + out, src + in, size);
                    in += size; out += size;
                } else if (type == 1) {
                    if (in >= src_len || dst_len - out < size) throw Error();
                    memset(dst + out, src[in], size);
                    in += 1; out += size;
                } else if (type == 2) {
                    if (src_len - in < size) throw Error();
                    out = decode_block(fs, src + in, size, dst, out, dst_len, frame_start);
                    in += size;
                } else throw Error();
                if (last) break;
            }
            if (checksum) in += 4;
        }
    } catch (const Error&) {
        if (error) *error = "zstd: corrupt data";
        return false;
    }
    if (out != dst_len) { if (error) *error = "zstd: stream shorter than expected"; return false; }
    return true;
}

} // namespace gc::decomp
