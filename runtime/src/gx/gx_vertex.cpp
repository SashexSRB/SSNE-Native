// Vertex loader + transform unit (XF): attribute decode, matrices, lighting, texgen.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include "gx_gpu_frame.h"
#include "gx_internal.h"
#include "ssne/runtime.h"
#include "ssne/ppc_context.h"

namespace ssne::gx {

namespace {

int comp_size(uint32_t fmt) { static const int s[8] = {1, 1, 2, 2, 4, 0, 0, 0}; return s[fmt & 7]; }
int color_size(uint32_t fmt) { static const int s[8] = {2, 3, 4, 2, 3, 4, 0, 0}; return s[fmt & 7]; }
int index_size(uint32_t type) { return type == 2 ? 1 : type == 3 ? 2 : 0; }

// VAT field locations for texcoords 0..7: (register 0=A 1=B 2=C, cnt bit, fmt bit, shift bit)
struct TexVat { int reg, cnt, fmt, shift; };
const TexVat kTexVat[8] = {{0, 21, 22, 25}, {1, 0, 1, 4}, {1, 9, 10, 13}, {1, 18, 19, 22},
                           {1, 27, 28, -1}, {2, 5, 6, 9}, {2, 14, 15, 18}, {2, 23, 24, 27}};

uint32_t vat_reg(int vat, int r) { return g.cp[(r == 0 ? 0x70 : r == 1 ? 0x80 : 0x90) + vat]; }
uint32_t tex_shift(int vat, int t) {
    if (t == 4) return vat_reg(vat, 2) & 31;  // TEX4 shift lives in VAT C bits 0-4
    return (vat_reg(vat, kTexVat[t].reg) >> kTexVat[t].shift) & 31;
}

// Reads one component from guest memory or the command stream.
struct Reader {
    const uint8_t* p = nullptr;  // command stream (direct data)
    uint32_t addr = 0;           // guest memory (indexed data)
    bool mem = false;
    uint8_t u8(uint32_t o) const { return mem ? mem_read8(addr + o) : p[o]; }
    uint16_t u16(uint32_t o) const {
        if (!mem) { uint16_t v; memcpy(&v, p + o, 2); return SSNE_BSWAP16(v); }
        return uint16_t(u8(o) << 8 | u8(o + 1));
    }
    uint32_t u32(uint32_t o) const {
        if (!mem) { uint32_t v; memcpy(&v, p + o, 4); return SSNE_BSWAP32(v); }
        return uint32_t(u16(o)) << 16 | u16(o + 2);
    }
    float comp(uint32_t fmt, uint32_t i, float scale) const {
        switch (fmt) {
        case 0: return u8(i) * scale;
        case 1: return int8_t(u8(i)) * scale;
        case 2: return u16(i * 2) * scale;
        case 3: return int16_t(u16(i * 2)) * scale;
        default: { uint32_t v = u32(i * 4); float f; memcpy(&f, &v, 4); return f; }
        }
    }
    void color(uint32_t fmt, float out[4]) const {
        switch (fmt) {
        case 0: { uint16_t v = u16(0); out[0] = float((v >> 11) * 255 / 31); out[1] = float(((v >> 5) & 63) * 255 / 63);
                  out[2] = float((v & 31) * 255 / 31); out[3] = 255; break; }
        case 1: case 2: out[0] = u8(0); out[1] = u8(1); out[2] = u8(2); out[3] = 255; break;
        case 3: { uint16_t v = u16(0); out[0] = float((v >> 12) * 17); out[1] = float(((v >> 8) & 15) * 17);
                  out[2] = float(((v >> 4) & 15) * 17); out[3] = float((v & 15) * 17); break; }
        case 4: { uint32_t v = uint32_t(u8(0)) << 16 | uint32_t(u8(1)) << 8 | u8(2);
                  out[0] = float(((v >> 18) & 63) * 255 / 63); out[1] = float(((v >> 12) & 63) * 255 / 63);
                  out[2] = float(((v >> 6) & 63) * 255 / 63); out[3] = float((v & 63) * 255 / 63); break; }
        default: out[0] = u8(0); out[1] = u8(1); out[2] = u8(2); out[3] = u8(3); break;
        }
    }
};

Reader indexed(int array, uint32_t index) {
    Reader r;
    r.mem = true;
    r.addr = (g.cp[0xA0 + array] & 0x03FFFFFF) + index * (g.cp[0xB0 + array] & 0xFF);
    // Array data lives in main RAM (physical addresses): read it through a host pointer instead of byte by byte
    // through the MMIO-checking accessors (same bytes; attributes are at most 36 bytes).
    if (r.addr < ssne::kRamSize - 64 && (r.addr >> 24) != 0xCC) { r.p = ssne::g_ram + r.addr; r.mem = false; }
    return r;
}

void mul34(uint32_t base, const float in[4], float out[3]) {
    for (int r = 0; r < 3; r++)
        out[r] = xf_f(base + r * 4) * in[0] + xf_f(base + r * 4 + 1) * in[1] + xf_f(base + r * 4 + 2) * in[2] +
                 xf_f(base + r * 4 + 3) * in[3];
}

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

void light_channel(int chan, const float view_pos[3], const float nrm[3], const float vcol[2][4], bool has_vcol[2],
                   float out[4]) {
    for (int part = 0; part < 2; part++) {  // 0 = color (rgb), 1 = alpha
        uint32_t ctrl = g.xf_reg[(part ? 0x10 : 0x0E) + chan];
        uint32_t matreg = g.xf_reg[0x0C + chan], ambreg = g.xf_reg[0x0A + chan];
        float mat[4], amb[4];
        for (int k = 0; k < 4; k++) {
            mat[k] = float((matreg >> (24 - 8 * k)) & 0xFF);
            amb[k] = float((ambreg >> (24 - 8 * k)) & 0xFF);
        }
        if (ctrl & 1) for (int k = 0; k < 4; k++) mat[k] = has_vcol[chan] ? vcol[chan][k] : 255.f;
        if (ctrl & 0x40) for (int k = 0; k < 4; k++) amb[k] = has_vcol[chan] ? vcol[chan][k] : 255.f;
        float res[4];
        if (!(ctrl & 2)) {
            memcpy(res, mat, sizeof(res));
        } else {
            float lit[4] = {amb[0], amb[1], amb[2], amb[3]};
            uint32_t mask = ((ctrl >> 2) & 0xF) | (((ctrl >> 11) & 0xF) << 4);
            uint32_t diffuse_fn = (ctrl >> 7) & 3;
            bool atten = (ctrl >> 9) & 1, spot = (ctrl >> 10) & 1;
            for (int l = 0; l < 8; l++) {
                if (!(mask & (1u << l))) continue;
                uint32_t base = 0x600 + l * 0x10;
                uint32_t lc = g.xf_mem[base + 3];
                float lpos[3] = {xf_f(base + 10), xf_f(base + 11), xf_f(base + 12)};
                float ldir[3] = {xf_f(base + 13), xf_f(base + 14), xf_f(base + 15)};
                float L[3] = {lpos[0] - view_pos[0], lpos[1] - view_pos[1], lpos[2] - view_pos[2]};
                float dist2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2];
                float dist = std::sqrt(dist2);
                if (dist > 0) for (float& c : L) c /= dist;
                float ndl = nrm[0] * L[0] + nrm[1] * L[1] + nrm[2] * L[2];
                float diff = diffuse_fn == 0 ? 1.f : diffuse_fn == 1 ? ndl : std::max(0.f, ndl);
                float att = 1.f;
                if (atten) {
                    float a0 = xf_f(base + 4), a1 = xf_f(base + 5), a2 = xf_f(base + 6);
                    float k0 = xf_f(base + 7), k1 = xf_f(base + 8), k2 = xf_f(base + 9);
                    if (spot) {
                        float cosang = std::max(0.f, -(L[0] * ldir[0] + L[1] * ldir[1] + L[2] * ldir[2]));
                        float num = std::max(0.f, a0 + a1 * cosang + a2 * cosang * cosang);
                        float den = k0 + k1 * dist + k2 * dist2;
                        att = den > 0 ? num / den : num;
                    } else {  // specular: direction holds the half-angle vector
                        float h = nrm[0] * ldir[0] + nrm[1] * ldir[1] + nrm[2] * ldir[2];
                        float c = ndl > 0 ? std::max(0.f, h) : 0.f;
                        float num = std::max(0.f, a0 + a1 * c + a2 * c * c);
                        float den = k0 + k1 * c + k2 * c * c;
                        att = den > 0 ? num / den : num;
                    }
                }
                for (int k = 0; k < 4; k++) lit[k] += float((lc >> (24 - 8 * k)) & 0xFF) * diff * att;
            }
            for (int k = 0; k < 4; k++) res[k] = mat[k] * clamp01(lit[k] / 255.f);
        }
        if (part == 0) { out[0] = res[0]; out[1] = res[1]; out[2] = res[2]; }
        else out[3] = res[3];
    }
}

} // namespace

uint32_t vertex_size(int vat) {
    uint32_t lo = g.cp[0x50], hi = g.cp[0x60];
    uint32_t a = vat_reg(vat, 0);
    uint32_t size = 0;
    for (int i = 0; i < 9; i++) size += (lo >> i) & 1;
    uint32_t pos = (lo >> 9) & 3;
    if (pos == 1) size += ((a & 1) ? 3 : 2) * comp_size((a >> 1) & 7);
    else size += index_size(pos);
    uint32_t nrm = (lo >> 11) & 3;
    if (nrm) {
        bool nbt = (a >> 9) & 1, idx3 = (a >> 31) & 1;
        if (nrm == 1) size += (nbt ? 9 : 3) * comp_size((a >> 10) & 7);
        else size += index_size(nrm) * ((nbt && idx3) ? 3 : 1);
    }
    for (int k = 0; k < 2; k++) {
        uint32_t col = (lo >> (13 + 2 * k)) & 3;
        uint32_t fmt = k == 0 ? (a >> 14) & 7 : (a >> 18) & 7;
        if (col == 1) size += color_size(fmt);
        else size += index_size(col);
    }
    for (int t = 0; t < 8; t++) {
        uint32_t tc = (hi >> (2 * t)) & 3;
        if (!tc) continue;
        uint32_t reg = vat_reg(vat, kTexVat[t].reg);
        if (tc == 1) size += (((reg >> kTexVat[t].cnt) & 1) ? 2 : 1) * comp_size((reg >> kTexVat[t].fmt) & 7);
        else size += index_size(tc);
    }
    return size;
}

void load_indexed_xf(int array, uint32_t v) {
    uint32_t index = v >> 16, count = ((v >> 12) & 0xF) + 1, addr = v & 0xFFF;
    uint32_t src = (g.cp[0xA0 + array] & 0x03FFFFFF) + index * (g.cp[0xB0 + array] & 0xFF);
    g_xf_dirty = true;
    for (uint32_t k = 0; k < count; k++) g.xf_mem[(addr + k) & 0xFFF] = mem_read32(src + 4 * k);
}

// Reference implementation (decodes every register per vertex). Kept for SSNE_CHECK_VERTICES=1, which runs both
// decoders and reports any difference; decode_vertices() below is the fast one.
void decode_vertices_ref(int vat, const uint8_t* data, uint32_t count, std::vector<Vtx>& out) {
    out.resize(count);
    uint32_t lo = g.cp[0x50], hi = g.cp[0x60];
    uint32_t a = vat_reg(vat, 0);
    uint32_t vsize = vertex_size(vat);
    uint32_t mia = g.xf_reg[0x18], mib = g.xf_reg[0x19];
    float p0 = xfr_f(0x20), p1 = xfr_f(0x21), p2 = xfr_f(0x22), p3 = xfr_f(0x23), p4 = xfr_f(0x24), p5 = xfr_f(0x25);
    bool ortho = g.xf_reg[0x26] & 1;
    uint32_t ntexgen = std::min<uint32_t>(g.xf_reg[0x3F] & 0xF, 8);
    uint32_t nchan = std::min<uint32_t>(g.xf_reg[0x09] & 3, 2);

    for (uint32_t v = 0; v < count; v++) {
        const uint8_t* p = data + size_t(v) * vsize;
        uint32_t off = 0;
        uint32_t pos_idx = mia & 0x3F;
        uint32_t tex_idx[8];
        for (int t = 0; t < 8; t++) tex_idx[t] = t < 4 ? (mia >> (6 + 6 * t)) & 0x3F : (mib >> (6 * (t - 4))) & 0x3F;
        if (lo & 1) pos_idx = p[off++] & 0x3F;
        for (int t = 0; t < 8; t++) if ((lo >> (1 + t)) & 1) tex_idx[t] = p[off++] & 0x3F;

        auto attr_reader = [&](uint32_t type, int array, uint32_t direct_size) {
            Reader r;
            if (type == 1) { r.p = p + off; off += direct_size; }
            else if (type == 2) { r = indexed(array, p[off]); off += 1; }
            else { r = indexed(array, uint32_t(p[off]) << 8 | p[off + 1]); off += 2; }
            return r;
        };

        // position
        float pos[4] = {0, 0, 0, 1};
        uint32_t ptype = (lo >> 9) & 3;
        if (ptype) {
            uint32_t fmt = (a >> 1) & 7, n = (a & 1) ? 3 : 2;
            float scale = fmt == 4 ? 1.f : 1.f / float(1u << ((a >> 4) & 31));
            Reader r = attr_reader(ptype, 0, n * comp_size(fmt));
            for (uint32_t k = 0; k < n; k++) pos[k] = r.comp(fmt, k, scale);
        }
        // normal
        float nrm[3] = {0, 0, 1};
        uint32_t ntype = (lo >> 11) & 3;
        if (ntype) {
            uint32_t fmt = (a >> 10) & 7;
            bool nbt = (a >> 9) & 1, idx3 = (a >> 31) & 1;
            static const float nscale[5] = {1.f / 128, 1.f / 64, 1.f / 32768, 1.f / 16384, 1.f};
            float scale = nscale[std::min<uint32_t>(fmt, 4)];
            uint32_t n = nbt ? 9 : 3;
            Reader r = attr_reader(ntype, 1, n * comp_size(fmt));
            for (int k = 0; k < 3; k++) nrm[k] = r.comp(fmt, uint32_t(k), scale);
            if (ntype != 1 && nbt && idx3) off += 2 * index_size(ntype);  // B and T indices (unused)
        }
        // colors
        float vcol[2][4] = {};
        bool has_vcol[2] = {false, false};
        for (int c = 0; c < 2; c++) {
            uint32_t ctype = (lo >> (13 + 2 * c)) & 3;
            if (!ctype) continue;
            uint32_t fmt = c == 0 ? (a >> 14) & 7 : (a >> 18) & 7;
            Reader r = attr_reader(ctype, 2 + c, color_size(fmt));
            r.color(fmt, vcol[c]);
            has_vcol[c] = true;
        }
        // texcoords
        float tc[8][2] = {};
        for (int t = 0; t < 8; t++) {
            uint32_t ttype = (hi >> (2 * t)) & 3;
            if (!ttype) continue;
            uint32_t reg = vat_reg(vat, kTexVat[t].reg);
            uint32_t fmt = (reg >> kTexVat[t].fmt) & 7, n = ((reg >> kTexVat[t].cnt) & 1) ? 2 : 1;
            float scale = fmt == 4 ? 1.f : 1.f / float(1u << tex_shift(vat, t));
            Reader r = attr_reader(ttype, 4 + t, n * comp_size(fmt));
            for (uint32_t k = 0; k < n; k++) tc[t][k] = r.comp(fmt, k, scale);
        }

        // ---- transform
        Vtx& o = out[v];
        float view[3];
        mul34(pos_idx * 4, pos, view);
        float vn[3] = {0, 0, 1};
        {
            uint32_t nb = 0x400 + (pos_idx & 31) * 3;
            for (int r = 0; r < 3; r++)
                vn[r] = xf_f(nb + r * 3) * nrm[0] + xf_f(nb + r * 3 + 1) * nrm[1] + xf_f(nb + r * 3 + 2) * nrm[2];
            float len = std::sqrt(vn[0] * vn[0] + vn[1] * vn[1] + vn[2] * vn[2]);
            if (len > 0) for (float& c : vn) c /= len;
        }
        if (ortho) { o.x = p0 * view[0] + p1; o.y = p2 * view[1] + p3; o.z = p4 * view[2] + p5; o.w = 1; }
        else { o.x = p0 * view[0] + p1 * view[2]; o.y = p2 * view[1] + p3 * view[2]; o.z = p4 * view[2] + p5; o.w = -view[2]; }

        for (uint32_t c = 0; c < 2; c++) {
            if (c < nchan || c == 0) light_channel(int(c), view, vn, vcol, has_vcol, o.color[c]);
            else for (int k = 0; k < 4; k++) o.color[c][k] = 0;
        }

        for (uint32_t t = 0; t < 8; t++) {
            o.tex[t][0] = o.tex[t][1] = 0; o.tex[t][2] = 1;
            if (t >= ntexgen) continue;
            uint32_t tg = g.xf_reg[0x40 + t];
            bool stq = (tg >> 1) & 1, abc1 = (tg >> 2) & 1;
            uint32_t type = (tg >> 4) & 7, src = (tg >> 7) & 31;
            float in[4] = {0, 0, 1, 1};
            if (src == 0) { in[0] = pos[0]; in[1] = pos[1]; in[2] = pos[2]; }
            else if (src == 1) { in[0] = nrm[0]; in[1] = nrm[1]; in[2] = nrm[2]; }
            else if (src >= 5 && src <= 12) { in[0] = tc[src - 5][0]; in[1] = tc[src - 5][1]; }
            if (!abc1) in[2] = 1.f;
            float s = 0, tt = 0, q = 1;
            if (type == 0) {
                uint32_t base = tex_idx[t] * 4;
                s = xf_f(base) * in[0] + xf_f(base + 1) * in[1] + xf_f(base + 2) * in[2] + xf_f(base + 3) * in[3];
                tt = xf_f(base + 4) * in[0] + xf_f(base + 5) * in[1] + xf_f(base + 6) * in[2] + xf_f(base + 7) * in[3];
                if (stq) q = xf_f(base + 8) * in[0] + xf_f(base + 9) * in[1] + xf_f(base + 10) * in[2] + xf_f(base + 11) * in[3];
            } else if (type == 1) {  // emboss: reuse the source texgen (bump offset ignored)
                uint32_t es = (tg >> 12) & 7;
                s = o.tex[es][0]; tt = o.tex[es][1]; q = o.tex[es][2];
            } else {                 // color -> texcoord
                s = o.color[0][0] / 255.f; tt = o.color[0][1] / 255.f;
            }
            if (g.xf_reg[0x12] & 1) {  // dual texture transform (post matrix)
                uint32_t pt = g.xf_reg[0x50 + t];
                if ((pt >> 8) & 1) {
                    float len = std::sqrt(s * s + tt * tt + q * q);
                    if (len > 0) { s /= len; tt /= len; q /= len; }
                }
                uint32_t base = 0x500 + (pt & 0x3F) * 4;
                float in2[4] = {s, tt, q, 1};
                float r3[3];
                mul34(base, in2, r3);
                s = r3[0]; tt = r3[1]; q = stq ? r3[2] : 1.f;
            }
            o.tex[t][0] = s; o.tex[t][1] = tt; o.tex[t][2] = q;
        }
    }
}

namespace {

// Per-call attribute layout: everything decode_vertices_ref re-derives from the CP/XF registers for every vertex.
struct AttrDesc { uint32_t type = 0, fmt = 0, n = 0, direct = 0; float scale = 1; int array = 0; };

inline Reader attr_at(const AttrDesc& d, const uint8_t* p, uint32_t& off) {
    Reader r;
    if (d.type == 1) { r.p = p + off; off += d.direct; }
    else if (d.type == 2) { r = indexed(d.array, p[off]); off += 1; }
    else { r = indexed(d.array, uint32_t(p[off]) << 8 | p[off + 1]); off += 2; }
    return r;
}

// Host pointer to an attribute's bytes (command stream, or array data in plain RAM, as `indexed` decides), or
// nullptr when the data must be read through the MMIO-checking accessors (then `slow` is set up for that).
inline const uint8_t* attr_ptr(const AttrDesc& d, const uint8_t* p, uint32_t& off, Reader& slow) {
    if (d.type == 1) { const uint8_t* q = p + off; off += d.direct; return q; }
    uint32_t index = d.type == 2 ? uint32_t(p[off]) : uint32_t(p[off]) << 8 | p[off + 1];
    off += d.type == 2 ? 1 : 2;
    uint32_t addr = (g.cp[0xA0 + d.array] & 0x03FFFFFF) + index * (g.cp[0xB0 + d.array] & 0xFF);
    if (addr < ssne::kRamSize - 64 && (addr >> 24) != 0xCC) return ssne::g_ram + addr;
    slow = indexed(d.array, index);
    return nullptr;
}

// Reader::comp / Reader::color on a host pointer (same arithmetic).
inline float comp_at(const uint8_t* q, uint32_t fmt, uint32_t i, float scale) {
    switch (fmt) {
    case 0: return q[i] * scale;
    case 1: return int8_t(q[i]) * scale;
    case 2: { uint16_t v; memcpy(&v, q + i * 2, 2); return SSNE_BSWAP16(v) * scale; }
    case 3: { uint16_t v; memcpy(&v, q + i * 2, 2); return int16_t(SSNE_BSWAP16(v)) * scale; }
    default: { uint32_t v; memcpy(&v, q + i * 4, 4); v = SSNE_BSWAP32(v); float f; memcpy(&f, &v, 4); return f; }
    }
}

// Lighting set up once per draw (light_channel re-derives all of this per vertex). One entry per channel and part
// (0 = color rgb, 1 = alpha); the per-vertex math below repeats light_channel's expressions in the same order, so
// the results are bit-identical (SSNE_CHECK_VERTICES compares against it).
struct LightPart {
    bool lit = false, mat_vc = false, amb_vc = false;
    float mat[4], amb[4];
    uint32_t mask = 0, diffuse_fn = 0;
    bool atten = false, spot = false;
};
struct LightConst { float lpos[3], ldir[3], col[4], a0, a1, a2, k0, k1, k2; };
// Per-vertex light geometry, shared by every part that uses the light.
struct LightGeo { float L[3], dist2, dist, ndl; };

void setup_light_part(int chan, int part, LightPart& lp) {
    uint32_t ctrl = g.xf_reg[(part ? 0x10 : 0x0E) + chan];
    uint32_t matreg = g.xf_reg[0x0C + chan], ambreg = g.xf_reg[0x0A + chan];
    for (int k = 0; k < 4; k++) {
        lp.mat[k] = float((matreg >> (24 - 8 * k)) & 0xFF);
        lp.amb[k] = float((ambreg >> (24 - 8 * k)) & 0xFF);
    }
    lp.mat_vc = ctrl & 1;
    lp.amb_vc = ctrl & 0x40;
    lp.lit = ctrl & 2;
    lp.mask = ((ctrl >> 2) & 0xF) | (((ctrl >> 11) & 0xF) << 4);
    lp.diffuse_fn = (ctrl >> 7) & 3;
    lp.atten = (ctrl >> 9) & 1;
    lp.spot = (ctrl >> 10) & 1;
}

void setup_light_const(int l, LightConst& c) {
    uint32_t base = 0x600 + l * 0x10;
    uint32_t lc = g.xf_mem[base + 3];
    for (int k = 0; k < 3; k++) { c.lpos[k] = xf_f(base + 10 + k); c.ldir[k] = xf_f(base + 13 + k); }
    for (int k = 0; k < 4; k++) c.col[k] = float((lc >> (24 - 8 * k)) & 0xFF);
    c.a0 = xf_f(base + 4); c.a1 = xf_f(base + 5); c.a2 = xf_f(base + 6);
    c.k0 = xf_f(base + 7); c.k1 = xf_f(base + 8); c.k2 = xf_f(base + 9);
}

inline void light_geo(const LightConst& c, const float view_pos[3], const float nrm[3], LightGeo& o) {
    float L[3] = {c.lpos[0] - view_pos[0], c.lpos[1] - view_pos[1], c.lpos[2] - view_pos[2]};
    float dist2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2];
    float dist = std::sqrt(dist2);
    if (dist > 0) for (float& x : L) x /= dist;
    o.L[0] = L[0]; o.L[1] = L[1]; o.L[2] = L[2];
    o.dist2 = dist2; o.dist = dist;
    o.ndl = nrm[0] * L[0] + nrm[1] * L[1] + nrm[2] * L[2];
}

// One part of light_channel for one vertex: writes out[k0..k1) (rgb: 0..3, alpha: 3..4). The range is a template
// parameter: with a runtime range the compiler turned the short copy loops into memcpy calls.
template <int k0, int k1>
inline void light_part(const LightPart& lp, const LightConst* lc, const LightGeo* geo, const float nrm[3],
                       const float* vcol, bool has_vcol, float out[4]) {
    float mat[4], amb[4];
    for (int k = k0; k < k1; k++) {
        mat[k] = lp.mat_vc ? (has_vcol ? vcol[k] : 255.f) : lp.mat[k];
        amb[k] = lp.amb_vc ? (has_vcol ? vcol[k] : 255.f) : lp.amb[k];
    }
    if (!lp.lit) { for (int k = k0; k < k1; k++) out[k] = mat[k]; return; }
    float lit[4];
    for (int k = k0; k < k1; k++) lit[k] = amb[k];
    for (uint32_t m = lp.mask; m; m &= m - 1) {
        int l = std::countr_zero(m);
        const LightConst& c = lc[l];
        const LightGeo& o = geo[l];
        float ndl = o.ndl;
        float diff = lp.diffuse_fn == 0 ? 1.f : lp.diffuse_fn == 1 ? ndl : std::max(0.f, ndl);
        float att = 1.f;
        if (lp.atten) {
            if (lp.spot) {
                float cosang = std::max(0.f, -(o.L[0] * c.ldir[0] + o.L[1] * c.ldir[1] + o.L[2] * c.ldir[2]));
                float num = std::max(0.f, c.a0 + c.a1 * cosang + c.a2 * cosang * cosang);
                float den = c.k0 + c.k1 * o.dist + c.k2 * o.dist2;
                att = den > 0 ? num / den : num;
            } else {
                float h = nrm[0] * c.ldir[0] + nrm[1] * c.ldir[1] + nrm[2] * c.ldir[2];
                float cc = ndl > 0 ? std::max(0.f, h) : 0.f;
                float num = std::max(0.f, c.a0 + c.a1 * cc + c.a2 * cc * cc);
                float den = c.k0 + c.k1 * cc + c.k2 * cc * cc;
                att = den > 0 ? num / den : num;
            }
        }
        for (int k = k0; k < k1; k++) lit[k] += c.col[k] * diff * att;
    }
    for (int k = k0; k < k1; k++) out[k] = mat[k] * clamp01(lit[k] / 255.f);
}

struct TexGen { bool stq, abc1, dual_norm; uint32_t type, src, es, post_base; };

// Everything a draw's vertices are decoded with, from the CP / XF registers (decode_vertices_fast, gpu_draw).
struct DrawSetup {
    uint32_t lo, hi, a, vsize, mia, mib;
    float p0, p1, p2, p3, p4, p5;  // projection
    bool ortho, nbt, dual;
    uint32_t ntexgen, nchan, lchans, used_lights, tex_mi_mask, nrm_skip;
    uint32_t def_tex_idx[8];
    AttrDesc pos_d, nrm_d, col_d[2], tex_d[8];
    TexGen tgs[8];
    LightPart lparts[2][2];
};

void setup_draw(int vat, DrawSetup& S) {
    uint32_t lo = g.cp[0x50], hi = g.cp[0x60];
    uint32_t a = vat_reg(vat, 0);
    uint32_t vsize = vertex_size(vat);
    uint32_t mia = g.xf_reg[0x18], mib = g.xf_reg[0x19];
    float p0 = xfr_f(0x20), p1 = xfr_f(0x21), p2 = xfr_f(0x22), p3 = xfr_f(0x23), p4 = xfr_f(0x24), p5 = xfr_f(0x25);
    bool ortho = g.xf_reg[0x26] & 1;
    uint32_t ntexgen = std::min<uint32_t>(g.xf_reg[0x3F] & 0xF, 8);
    uint32_t nchan = std::min<uint32_t>(g.xf_reg[0x09] & 3, 2);

    uint32_t def_tex_idx[8];
    for (int t = 0; t < 8; t++) def_tex_idx[t] = t < 4 ? (mia >> (6 + 6 * t)) & 0x3F : (mib >> (6 * (t - 4))) & 0x3F;
    uint32_t tex_mi_mask = (lo >> 1) & 0xFF;  // which texcoords carry a per-vertex matrix index byte
    AttrDesc pos_d, nrm_d, col_d[2], tex_d[8];
    pos_d.type = (lo >> 9) & 3;
    if (pos_d.type) {
        pos_d.fmt = (a >> 1) & 7; pos_d.n = (a & 1) ? 3 : 2;
        pos_d.scale = pos_d.fmt == 4 ? 1.f : 1.f / float(1u << ((a >> 4) & 31));
        pos_d.direct = pos_d.n * uint32_t(comp_size(pos_d.fmt)); pos_d.array = 0;
    }
    nrm_d.type = (lo >> 11) & 3;
    bool nbt = (a >> 9) & 1, idx3 = (a >> 31) & 1;
    uint32_t nrm_skip = 0;
    if (nrm_d.type) {
        static const float nscale[5] = {1.f / 128, 1.f / 64, 1.f / 32768, 1.f / 16384, 1.f};
        nrm_d.fmt = (a >> 10) & 7;
        nrm_d.scale = nscale[std::min<uint32_t>(nrm_d.fmt, 4)];
        nrm_d.direct = (nbt ? 9 : 3) * uint32_t(comp_size(nrm_d.fmt)); nrm_d.array = 1;
        if (nrm_d.type != 1 && nbt && idx3) nrm_skip = 2 * uint32_t(index_size(nrm_d.type));
    }
    for (int c = 0; c < 2; c++) {
        col_d[c].type = (lo >> (13 + 2 * c)) & 3;
        col_d[c].fmt = c == 0 ? (a >> 14) & 7 : (a >> 18) & 7;
        col_d[c].direct = uint32_t(color_size(col_d[c].fmt)); col_d[c].array = 2 + c;
    }
    for (int t = 0; t < 8; t++) {
        AttrDesc& d = tex_d[t];
        d.type = (hi >> (2 * t)) & 3;
        if (!d.type) continue;
        uint32_t reg = vat_reg(vat, kTexVat[t].reg);
        d.fmt = (reg >> kTexVat[t].fmt) & 7; d.n = ((reg >> kTexVat[t].cnt) & 1) ? 2 : 1;
        d.scale = d.fmt == 4 ? 1.f : 1.f / float(1u << tex_shift(vat, t));
        d.direct = d.n * uint32_t(comp_size(d.fmt)); d.array = 4 + t;
    }
    // texgen setup
    TexGen tgs[8];
    bool dual = g.xf_reg[0x12] & 1;
    for (uint32_t t = 0; t < ntexgen; t++) {
        uint32_t tg = g.xf_reg[0x40 + t];
        TexGen& x = tgs[t];
        x.stq = (tg >> 1) & 1; x.abc1 = (tg >> 2) & 1; x.type = (tg >> 4) & 7; x.src = (tg >> 7) & 31; x.es = (tg >> 12) & 7;
        uint32_t pt = g.xf_reg[0x50 + t];
        x.dual_norm = (pt >> 8) & 1;
        x.post_base = 0x500 + (pt & 0x3F) * 4;
    }
    // Lighting: channel 0 is always evaluated, channel 1 when enabled (as before). The transformed normal is only
    // needed when some evaluated part is lit by at least one light; light geometry once per light per vertex.
    uint32_t lchans = nchan > 1 ? 2 : 1;
    LightPart lparts[2][2];
    uint32_t used_lights = 0;
    for (uint32_t c = 0; c < lchans; c++)
        for (int part = 0; part < 2; part++) {
            setup_light_part(int(c), part, lparts[c][part]);
            if (lparts[c][part].lit) used_lights |= lparts[c][part].mask;
        }
    S.lo = lo; S.hi = hi; S.a = a; S.vsize = vsize; S.mia = mia; S.mib = mib;
    S.p0 = p0; S.p1 = p1; S.p2 = p2; S.p3 = p3; S.p4 = p4; S.p5 = p5;
    S.ortho = ortho; S.nbt = nbt; S.dual = dual;
    S.ntexgen = ntexgen; S.nchan = nchan; S.lchans = lchans; S.used_lights = used_lights; S.tex_mi_mask = tex_mi_mask;
    S.nrm_skip = nrm_skip;
    memcpy(S.def_tex_idx, def_tex_idx, sizeof(def_tex_idx));
    S.pos_d = pos_d; S.nrm_d = nrm_d; S.col_d[0] = col_d[0]; S.col_d[1] = col_d[1];
    for (int t = 0; t < 8; t++) S.tex_d[t] = tex_d[t];
    for (int t = 0; t < 8; t++) S.tgs[t] = tgs[t];
    for (int c = 0; c < 2; c++) for (int k = 0; k < 2; k++) S.lparts[c][k] = lparts[c][k];
}

} // namespace

void decode_vertices_fast(int vat, const uint8_t* data, uint32_t count, std::vector<Vtx>& out) {
    out.resize(count);
    DrawSetup S;
    setup_draw(vat, S);
    const uint32_t lo = S.lo, vsize = S.vsize, mia = S.mia, ntexgen = S.ntexgen, tex_mi_mask = S.tex_mi_mask;
    const uint32_t nrm_skip = S.nrm_skip, lchans = S.lchans, used_lights = S.used_lights;
    const float p0 = S.p0, p1 = S.p1, p2 = S.p2, p3 = S.p3, p4 = S.p4, p5 = S.p5;
    const bool ortho = S.ortho, dual = S.dual;
    const uint32_t* def_tex_idx = S.def_tex_idx;
    const AttrDesc &pos_d = S.pos_d, &nrm_d = S.nrm_d;
    const AttrDesc* col_d = S.col_d;
    const AttrDesc* tex_d = S.tex_d;
    const TexGen* tgs = S.tgs;
    const LightPart (&lparts)[2][2] = S.lparts;
    LightConst lconst[8];
    for (uint32_t m = used_lights; m; m &= m - 1) setup_light_const(std::countr_zero(m), lconst[std::countr_zero(m)]);
    const bool need_vn = used_lights != 0;

    for (uint32_t v = 0; v < count; v++) {
        const uint8_t* p = data + size_t(v) * vsize;
        uint32_t off = 0;
        uint32_t pos_idx = mia & 0x3F;
        uint32_t tex_idx[8];
        memcpy(tex_idx, def_tex_idx, sizeof(tex_idx));
        if (lo & 1) pos_idx = p[off++] & 0x3F;
        if (tex_mi_mask)
            for (int t = 0; t < 8; t++) if ((tex_mi_mask >> t) & 1) tex_idx[t] = p[off++] & 0x3F;

        Reader slow;
        float pos[4] = {0, 0, 0, 1};
        if (pos_d.type) {
            if (const uint8_t* q = attr_ptr(pos_d, p, off, slow))
                for (uint32_t k = 0; k < pos_d.n; k++) pos[k] = comp_at(q, pos_d.fmt, k, pos_d.scale);
            else
                for (uint32_t k = 0; k < pos_d.n; k++) pos[k] = slow.comp(pos_d.fmt, k, pos_d.scale);
        }
        float nrm[3] = {0, 0, 1};
        if (nrm_d.type) {
            if (const uint8_t* q = attr_ptr(nrm_d, p, off, slow))
                for (int k = 0; k < 3; k++) nrm[k] = comp_at(q, nrm_d.fmt, uint32_t(k), nrm_d.scale);
            else
                for (int k = 0; k < 3; k++) nrm[k] = slow.comp(nrm_d.fmt, uint32_t(k), nrm_d.scale);
            off += nrm_skip;
        }
        float vcol[2][4] = {};
        bool has_vcol[2] = {false, false};
        for (int c = 0; c < 2; c++) {
            if (!col_d[c].type) continue;
            if (const uint8_t* q = attr_ptr(col_d[c], p, off, slow)) { Reader r; r.p = q; r.color(col_d[c].fmt, vcol[c]); }
            else slow.color(col_d[c].fmt, vcol[c]);
            has_vcol[c] = true;
        }
        float tc[8][2] = {};
        for (int t = 0; t < 8; t++) {
            const AttrDesc& d = tex_d[t];
            if (!d.type) continue;
            if (const uint8_t* q = attr_ptr(d, p, off, slow))
                for (uint32_t k = 0; k < d.n; k++) tc[t][k] = comp_at(q, d.fmt, k, d.scale);
            else
                for (uint32_t k = 0; k < d.n; k++) tc[t][k] = slow.comp(d.fmt, k, d.scale);
        }

        // ---- transform (same arithmetic as decode_vertices_ref)
        Vtx& o = out[v];
        float view[3];
        mul34(pos_idx * 4, pos, view);
        float vn[3] = {0, 0, 1};
        if (need_vn) {
            uint32_t nb = 0x400 + (pos_idx & 31) * 3;
            for (int r = 0; r < 3; r++)
                vn[r] = xf_f(nb + r * 3) * nrm[0] + xf_f(nb + r * 3 + 1) * nrm[1] + xf_f(nb + r * 3 + 2) * nrm[2];
            float len = std::sqrt(vn[0] * vn[0] + vn[1] * vn[1] + vn[2] * vn[2]);
            if (len > 0) for (float& c : vn) c /= len;
        }
        if (ortho) { o.x = p0 * view[0] + p1; o.y = p2 * view[1] + p3; o.z = p4 * view[2] + p5; o.w = 1; }
        else { o.x = p0 * view[0] + p1 * view[2]; o.y = p2 * view[1] + p3 * view[2]; o.z = p4 * view[2] + p5; o.w = -view[2]; }

        LightGeo geo[8];
        for (uint32_t m = used_lights; m; m &= m - 1) {
            int l = std::countr_zero(m);
            light_geo(lconst[l], view, vn, geo[l]);
        }
        for (uint32_t c = 0; c < 2; c++) {
            if (c < lchans) {
                light_part<0, 3>(lparts[c][0], lconst, geo, vn, vcol[c], has_vcol[c], o.color[c]);
                light_part<3, 4>(lparts[c][1], lconst, geo, vn, vcol[c], has_vcol[c], o.color[c]);
            } else for (int k = 0; k < 4; k++) o.color[c][k] = 0;
        }

        for (uint32_t t = 0; t < 8; t++) {
            o.tex[t][0] = o.tex[t][1] = 0; o.tex[t][2] = 1;
            if (t >= ntexgen) continue;
            const TexGen& x = tgs[t];
            float in[4] = {0, 0, 1, 1};
            if (x.src == 0) { in[0] = pos[0]; in[1] = pos[1]; in[2] = pos[2]; }
            else if (x.src == 1) { in[0] = nrm[0]; in[1] = nrm[1]; in[2] = nrm[2]; }
            else if (x.src >= 5 && x.src <= 12) { in[0] = tc[x.src - 5][0]; in[1] = tc[x.src - 5][1]; }
            if (!x.abc1) in[2] = 1.f;
            float s = 0, tt = 0, q = 1;
            if (x.type == 0) {
                uint32_t base = tex_idx[t] * 4;
                s = xf_f(base) * in[0] + xf_f(base + 1) * in[1] + xf_f(base + 2) * in[2] + xf_f(base + 3) * in[3];
                tt = xf_f(base + 4) * in[0] + xf_f(base + 5) * in[1] + xf_f(base + 6) * in[2] + xf_f(base + 7) * in[3];
                if (x.stq) q = xf_f(base + 8) * in[0] + xf_f(base + 9) * in[1] + xf_f(base + 10) * in[2] + xf_f(base + 11) * in[3];
            } else if (x.type == 1) {
                s = o.tex[x.es][0]; tt = o.tex[x.es][1]; q = o.tex[x.es][2];
            } else {
                s = o.color[0][0] / 255.f; tt = o.color[0][1] / 255.f;
            }
            if (dual) {
                if (x.dual_norm) {
                    float len = std::sqrt(s * s + tt * tt + q * q);
                    if (len > 0) { s /= len; tt /= len; q /= len; }
                }
                float in2[4] = {s, tt, q, 1};
                float r3[3];
                mul34(x.post_base, in2, r3);
                s = r3[0]; tt = r3[1]; q = x.stq ? r3[2] : 1.f;
            }
            o.tex[t][0] = s; o.tex[t][1] = tt; o.tex[t][2] = q;
        }
    }
}

// ---- GPU vertex processing (GpuVcfg mode 1): the CPU part. Reads the attributes and transforms positions (as
// decode_vertices_fast does); texture coordinates and lighting are left to the vertex shader (gx_gpu.cpp gen_vs),
// which gets the sources it needs per vertex and the matrices, colors and lights as per-batch parameters.
namespace {

GpuVcfg g_vc_last{};
GpuXf g_xf_last{};
uint64_t g_vx_version = 0;  // changes whenever g_vc_last / g_xf_last do (0: none yet)

uint32_t pack_light_part(const LightPart& lp) {
    return uint32_t(lp.lit) | uint32_t(lp.mat_vc) << 1 | uint32_t(lp.amb_vc) << 2 | (lp.diffuse_fn & 3) << 3 |
           uint32_t(lp.atten) << 5 | uint32_t(lp.spot) << 6 | (lp.mask & 0xFF) << 8;
}

// The widescreen HUD adjustment (gx_raster.cpp widescreen_hud) on a draw's written clip positions.
void widescreen_hud_inplace(uint8_t* v, uint32_t n, uint32_t stride) {
    float k = widescreen_frame_factor();
    if (k <= 1.001f || !(g.xf_reg[0x26] & 1)) return;
    float mn = 1e30f, mx = -1e30f;
    for (uint32_t i = 0; i < n; i++) {
        float p[4];
        memcpy(p, v + size_t(i) * stride, 16);
        if (p[3] <= 0) return;
        float x = p[0] / p[3];
        mn = std::min(mn, x); mx = std::max(mx, x);
    }
    if (mx - mn >= 1.9f) return;
    constexpr float kEdge = 0.97f;
    float nmn;
    if (mn <= -kEdge) nmn = -1.f + (mn + 1.f) / k;
    else if (mx >= kEdge) nmn = 1.f - (1.f - mx) / k - (mx - mn) / k;
    else nmn = mn / k;
    for (uint32_t i = 0; i < n; i++) {
        float p[4];
        memcpy(p, v + size_t(i) * stride, 16);
        float x = p[0] / p[3];
        p[0] = (nmn + (x - mn) / k) * p[3];
        memcpy(v + size_t(i) * stride, p, 4);
    }
}

} // namespace

bool gpu_vertex_shading() {
    static const bool on = !(getenv("SSNE_GPU_TL") && getenv("SSNE_GPU_TL")[0] == '0');
    return on;
}

namespace {

// A vertex format's setup for GPU vertex processing, kept until a CP or XF register write (g_vsetup_dirty).
struct GpuDrawFmt {
    bool valid = false, ok = false;  // ok: the shader path covers it
    DrawSetup S;
    GpuVcfg vc;
    // indexed attributes, for prefetching a few vertices ahead: offset in the vertex, index bytes, array
    int npf = 0;
    uint8_t pf_off[12], pf_size[12], pf_array[12];
};
GpuDrawFmt g_fmts[8];

void build_fmt(int vat, GpuDrawFmt& F) {
    F.valid = true;
    F.ok = false;
    DrawSetup& S = F.S;
    setup_draw(vat, S);
    F.npf = 0;
    {
        uint32_t off = (S.lo & 1) + uint32_t(std::popcount(S.tex_mi_mask));
        auto add = [&](const AttrDesc& d, uint32_t direct) {
            if (d.type == 1) { off += direct; return; }
            if (d.type >= 2) {
                F.pf_off[F.npf] = uint8_t(off); F.pf_size[F.npf] = uint8_t(d.type == 2 ? 1 : 2); F.pf_array[F.npf] = uint8_t(d.array);
                F.npf++;
                off += d.type == 2 ? 1 : 2;
            }
        };
        if (S.pos_d.type) add(S.pos_d, S.pos_d.direct);
        if (S.nrm_d.type) { add(S.nrm_d, S.nrm_d.direct); off += S.nrm_skip; }
        for (int c = 0; c < 2; c++) if (S.col_d[c].type) add(S.col_d[c], S.col_d[c].direct);
        for (int t = 0; t < 8; t++) if (S.tex_d[t].type) add(S.tex_d[t], S.tex_d[t].direct);
    }
    if (S.tex_mi_mask) return;  // per-vertex texture matrix indices: CPU path
    GpuVcfg& vc = F.vc;
    memset(&vc, 0, sizeof(vc));
    vc.mode = 1;
    vc.ntg = S.ntexgen;
    vc.dual = S.dual;
    vc.lchans = S.lchans;
    uint32_t attrs = 0;
    if (S.col_d[0].type) attrs |= VA_VCOL0;
    if (S.col_d[1].type) attrs |= VA_VCOL1;
    if (S.used_lights) attrs |= VA_GEO;
    for (uint32_t t = 0; t < S.ntexgen; t++) {
        const TexGen& x = S.tgs[t];
        if (x.type == 1 && x.es >= t) return;  // emboss from a later texgen (stale values on the CPU path)
        if (x.type == 0) {
            if (x.src == 0) attrs |= VA_RPOS;
            else if (x.src == 1) attrs |= VA_RNRM;
            else if (x.src >= 5 && x.src <= 12 && S.tex_d[x.src - 5].type) attrs |= VA_TC << (x.src - 5);
        }
        vc.tg[t] = x.type | (x.src & 31) << 3 | uint32_t(x.stq) << 8 | uint32_t(x.abc1) << 9 | uint32_t(x.dual_norm) << 10 |
                   (x.es & 7) << 11;
    }
    for (uint32_t c = 0; c < S.lchans; c++)
        for (int part = 0; part < 2; part++) vc.lp[c][part] = pack_light_part(S.lparts[c][part]);
    vc.attrs = attrs;
    F.ok = true;
}

} // namespace

bool gpu_draw(uint8_t prim, int vat, const uint8_t* data, uint32_t count) {
    if (prim == PRIM_LINES || prim == PRIM_LINESTRIP || prim == PRIM_POINTS) return false;  // expanded on the CPU
    if (g_vsetup_dirty) {
        for (GpuDrawFmt& f : g_fmts) f.valid = false;
        g_vsetup_dirty = false;
    }
    GpuDrawFmt& F = g_fmts[vat & 7];
    if (!F.valid) build_fmt(vat & 7, F);
    if (!F.ok) return false;
    const DrawSetup& S = F.S;
    const GpuVcfg& vc = F.vc;
    const uint32_t attrs = vc.attrs;

    // Per-batch parameters: only the rows / lights the shader reads (other values would split batches for nothing).
    bool vc_changed = memcmp(&vc, &g_vc_last, sizeof(vc)) != 0;
    if (g_xf_dirty || vc_changed || !g_vx_version) {
        GpuXf xf;
        memset(&xf, 0, sizeof(xf));
        for (uint32_t t = 0; t < S.ntexgen; t++) {
            const TexGen& x = S.tgs[t];
            int rows = x.stq ? 3 : 2;
            if (x.type == 0)
                for (int k = 0; k < rows * 4; k++) xf.tm[t][k] = xf_f(S.def_tex_idx[t] * 4 + uint32_t(k));
            if (S.dual)
                for (int k = 0; k < rows * 4; k++) xf.pm[t][k] = xf_f(x.post_base + uint32_t(k));
        }
        for (uint32_t c = 0; c < S.lchans; c++)
            for (int k = 0; k < 4; k++) { xf.mat[c][k] = S.lparts[c][0].mat[k]; xf.amb[c][k] = S.lparts[c][0].amb[k]; }
        for (uint32_t m = S.used_lights; m; m &= m - 1) {
            int l = std::countr_zero(m);
            LightConst lc;
            setup_light_const(l, lc);
            for (int k = 0; k < 3; k++) { xf.lpos[l][k] = lc.lpos[k]; xf.ldir[l][k] = lc.ldir[k]; }
            for (int k = 0; k < 4; k++) xf.lcol[l][k] = lc.col[k];
            float att[6] = {lc.a0, lc.a1, lc.a2, lc.k0, lc.k1, lc.k2};
            memcpy(xf.latt[l], att, sizeof(att));
        }
        g_xf_dirty = false;
        if (vc_changed || memcmp(&xf, &g_xf_last, sizeof(xf)) != 0) {
            g_vc_last = vc;
            g_xf_last = xf;
            g_vx_version++;
        }
    }

    const GpuVtxLayout L = gpu_vtx_layout(attrs);
    uint8_t* out = gpu_vtx_reserve(size_t(count) * L.stride);
    const bool need_nrm = attrs & (VA_GEO | VA_RNRM);
    double zn = (double(xfr_f(0x1F)) - double(xfr_f(0x1C))) / 16777216.0, zf = double(xfr_f(0x1F)) / 16777216.0;
    double zlo = std::max(std::min(zn, zf) + 1e-5, 1e-5), zhi = std::min(std::max(zn, zf) - 1e-5, 1.0 - 1e-5);
    double vp2 = double(xfr_f(0x1C)), vp5 = double(xfr_f(0x1F));
    bool zsnap = false;
    constexpr uint32_t kAhead = 6;  // indexed attributes are scattered in RAM: fetch them a few vertices early
    auto prefetch = [&](uint32_t v) {
        const uint8_t* p = data + size_t(v) * S.vsize;
        for (int i = 0; i < F.npf; i++) {
            uint32_t idx = F.pf_size[i] == 1 ? p[F.pf_off[i]] : uint32_t(p[F.pf_off[i]]) << 8 | p[F.pf_off[i] + 1];
            uint32_t a = F.pf_array[i];
            uint32_t addr = (g.cp[0xA0 + a] & 0x03FFFFFF) + idx * (g.cp[0xB0 + a] & 0xFF);
            if (addr < ssne::kRamSize - 64) __builtin_prefetch(ssne::g_ram + addr);
        }
    };
    for (uint32_t v = 0; v < count && v < kAhead; v++) prefetch(v);
    for (uint32_t v = 0; v < count; v++) {
        if (v + kAhead < count) prefetch(v + kAhead);
        const uint8_t* p = data + size_t(v) * S.vsize;
        uint8_t* o = out + size_t(v) * L.stride;
        uint32_t off = 0;
        uint32_t pos_idx = S.mia & 0x3F;
        if (S.lo & 1) pos_idx = p[off++] & 0x3F;
        Reader slow;
        float pos[4] = {0, 0, 0, 1};
        if (S.pos_d.type) {
            if (const uint8_t* q = attr_ptr(S.pos_d, p, off, slow))
                for (uint32_t k = 0; k < S.pos_d.n; k++) pos[k] = comp_at(q, S.pos_d.fmt, k, S.pos_d.scale);
            else
                for (uint32_t k = 0; k < S.pos_d.n; k++) pos[k] = slow.comp(S.pos_d.fmt, k, S.pos_d.scale);
        }
        float nrm[3] = {0, 0, 1};
        if (S.nrm_d.type) {
            const uint8_t* q = attr_ptr(S.nrm_d, p, off, slow);
            if (need_nrm) {
                if (q) for (int k = 0; k < 3; k++) nrm[k] = comp_at(q, S.nrm_d.fmt, uint32_t(k), S.nrm_d.scale);
                else for (int k = 0; k < 3; k++) nrm[k] = slow.comp(S.nrm_d.fmt, uint32_t(k), S.nrm_d.scale);
            }
            off += S.nrm_skip;
        }
        for (int c = 0; c < 2; c++) {
            uint8_t rgba[4] = {0, 0, 0, 0};
            if (S.col_d[c].type) {
                const uint32_t fmt = S.col_d[c].fmt;
                const uint8_t* q = attr_ptr(S.col_d[c], p, off, slow);
                if (q && fmt == 5) memcpy(rgba, q, 4);                                          // RGBA8
                else if (q && (fmt == 1 || fmt == 2)) { rgba[0] = q[0]; rgba[1] = q[1]; rgba[2] = q[2]; rgba[3] = 255; }  // RGB8 / RGBX8
                else {
                    float vcol[4];
                    if (q) { Reader r; r.p = q; r.color(fmt, vcol); }
                    else slow.color(fmt, vcol);
                    for (int k = 0; k < 4; k++) rgba[k] = uint8_t(vcol[k]);  // whole numbers 0..255
                }
            }
            memcpy(o + (c ? L.col1 : L.col0), rgba, 4);
        }
        for (int t = 0; t < 8; t++) {
            const AttrDesc& d = S.tex_d[t];
            if (!d.type) continue;
            const uint8_t* q = attr_ptr(d, p, off, slow);
            if (L.tc[t] < 0) continue;
            float tc[2] = {0, 0};
            if (q) for (uint32_t k = 0; k < d.n; k++) tc[k] = comp_at(q, d.fmt, k, d.scale);
            else for (uint32_t k = 0; k < d.n; k++) tc[k] = slow.comp(d.fmt, k, d.scale);
            memcpy(o + L.tc[t], tc, 8);
        }

        // ---- position (same arithmetic as decode_vertices_fast)
        float view[3];
        mul34(pos_idx * 4, pos, view);
        float clip[4];
        if (S.ortho) { clip[0] = S.p0 * view[0] + S.p1; clip[1] = S.p2 * view[1] + S.p3; clip[2] = S.p4 * view[2] + S.p5; clip[3] = 1; }
        else {
            clip[0] = S.p0 * view[0] + S.p1 * view[2]; clip[1] = S.p2 * view[1] + S.p3 * view[2];
            clip[2] = S.p4 * view[2] + S.p5; clip[3] = -view[2];
        }
        memcpy(o, clip, 16);
        if (attrs & VA_GEO) {
            float vn[3];
            uint32_t nb = 0x400 + (pos_idx & 31) * 3;
            for (int r = 0; r < 3; r++)
                vn[r] = xf_f(nb + r * 3) * nrm[0] + xf_f(nb + r * 3 + 1) * nrm[1] + xf_f(nb + r * 3 + 2) * nrm[2];
            float len = std::sqrt(vn[0] * vn[0] + vn[1] * vn[1] + vn[2] * vn[2]);
            if (len > 0) for (float& c : vn) c /= len;
            memcpy(o + L.vpos, view, 12);
            memcpy(o + L.vnrm, vn, 12);
        }
        if (attrs & VA_RPOS) memcpy(o + L.rpos, pos, 12);
        if (attrs & VA_RNRM) memcpy(o + L.rnrm, nrm, 12);
        if (!zsnap) {
            if (!(clip[3] > 1e-5f)) zsnap = true;
            else {
                double d = (double(clip[2]) / double(clip[3]) * vp2 + vp5) / 16777216.0;
                if (!(d > zlo && d < zhi)) zsnap = true;
            }
        }
    }
    widescreen_hud_inplace(out, count, L.stride);
    gpu_record_vertices(prim, count, g_vc_last, g_xf_last, g_vx_version, zsnap);
    return true;
}

void decode_vertices(int vat, const uint8_t* data, uint32_t count, std::vector<Vtx>& out) {
    static const bool check = getenv("SSNE_CHECK_VERTICES") != nullptr;
    decode_vertices_fast(vat, data, count, out);
    if (check) {  // debugging: compare with the reference decoder bit for bit
        static std::vector<Vtx> ref;
        static uint64_t checked = 0, bad = 0;
        decode_vertices_ref(vat, data, count, ref);
        for (uint32_t i = 0; i < count; i++)
            if (memcmp(&ref[i], &out[i], offsetof(Vtx, sx)) || memcmp(ref[i].color, out[i].color, sizeof(out[i].color)) ||
                memcmp(ref[i].tex, out[i].tex, sizeof(out[i].tex))) {
                if (bad++ < 10) fprintf(stderr, "[gx] vertex decoder mismatch (vat %d, vertex %u of %u)\n", vat, i, count);
            }
        if (((checked += count) & 0xFFFFF) < count)
            fprintf(stderr, "[gx] vertex decoder check: %llu vertices, %llu mismatches\n", (unsigned long long)checked,
                    (unsigned long long)bad);
    }
}

} // namespace ssne::gx
