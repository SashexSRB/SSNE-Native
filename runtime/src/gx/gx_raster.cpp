// Software rasterizer + TEV + pixel engine, operating on an emulated 640x528 EFB.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "gx_internal.h"
#include "ssne/gx.h"
#include "ssne/runtime.h"
#include "ssne/ppc_context.h"

namespace ssne::gx {

namespace {

uint32_t g_efb[EFB_W * EFB_H];     // RGBA8, R in low byte

// Debug switches (environment): SSNE_GX_NOZ, SSNE_GX_NOCULL, SSNE_GX_NOATEST, SSNE_GX_STATS
struct Debug {
    bool noz = getenv("SSNE_GX_NOZ"), nocull = getenv("SSNE_GX_NOCULL"), noatest = getenv("SSNE_GX_NOATEST");
    bool stats = getenv("SSNE_GX_STATS");
    bool nodraw = getenv("SSNE_GX_NODRAW");
    long trace_frame = getenv("SSNE_GX_TRACE_FRAME") ? atol(getenv("SSNE_GX_TRACE_FRAME")) : -1;
    double trace_at = getenv("SSNE_GX_TRACE_AT") ? atof(getenv("SSNE_GX_TRACE_AT")) : -1;  // seconds after start
} dbg;
struct FrameStats { uint64_t tris, culled, clipped_away, degenerate, offscreen, zfail, afail, written; } fs;
bool tracing() { return dbg.trace_frame >= 0 && long(counters.display_copies) == dbg.trace_frame; }
uint32_t g_zbuf[EFB_W * EFB_H];    // 24-bit depth

// TEV color registers (11-bit signed) and konst colors (8-bit); index 0 = PREV / K0.
int16_t g_tev_reg[4][4] = {};
int16_t g_konst[4][4] = {};

inline int sext11(uint32_t v) { return int(v << 21) >> 21; }

// ---- frame publishing
std::mutex g_frame_mutex;
std::vector<uint32_t> g_frame;
int g_frame_w = 0, g_frame_h = 0;
uint64_t g_frame_seq = 0;

struct Pix { int r, g, b, a; };

inline Pix unpack(uint32_t c) { return {int(c & 255), int((c >> 8) & 255), int((c >> 16) & 255), int(c >> 24)}; }
inline uint32_t clamp8(int v) { return uint32_t(v < 0 ? 0 : v > 255 ? 255 : v); }
inline uint32_t pack(const Pix& p) { return clamp8(p.r) | clamp8(p.g) << 8 | clamp8(p.b) << 16 | clamp8(p.a) << 24; }

inline bool compare(uint32_t func, int a, int b) {
    switch (func) {
    case 0: return false; case 1: return a < b; case 2: return a == b; case 3: return a <= b;
    case 4: return a > b; case 5: return a != b; case 6: return a >= b; default: return true;
    }
}

} // namespace

DrawCfg build_cfg() {
    DrawCfg c;
    build_cfg(c);
    return c;
}

void build_cfg(DrawCfg& c) {
    memset(&c, 0, sizeof(c));  // padding too: GPU batches compare configs with memcmp
    uint32_t genmode = g.bp[0x00];
    c.stages = int((genmode >> 10) & 0xF) + 1;
    int swaps[4][4];
    for (int t = 0; t < 4; t++) {
        uint32_t k0 = g.bp[0xF6 + 2 * t], k1 = g.bp[0xF6 + 2 * t + 1];
        swaps[t][0] = k0 & 3; swaps[t][1] = (k0 >> 2) & 3; swaps[t][2] = k1 & 3; swaps[t][3] = (k1 >> 2) & 3;
    }
    for (int s = 0; s < c.stages; s++) {
        StageCfg& st = c.st[s];
        st.cenv = g.bp[0xC0 + 2 * s];
        st.aenv = g.bp[0xC1 + 2 * s];
        uint32_t order = g.bp[0x28 + s / 2];
        if (s & 1) order >>= 12;
        st.texmap = int(order & 7);
        st.texcoord = int((order >> 3) & 7);
        st.tex_enabled = (order >> 6) & 1;
        uint32_t chan = (order >> 7) & 7;
        st.ras_chan = chan == 0 ? 0 : chan == 1 ? 1 : chan == 5 ? 2 : chan == 6 ? 3 : -1;
        uint32_t ksel = g.bp[0xF6 + s / 2];
        st.kcsel = (s & 1) ? (ksel >> 14) & 31 : (ksel >> 4) & 31;
        st.kasel = (s & 1) ? (ksel >> 19) & 31 : (ksel >> 9) & 31;
        int rs = int(st.aenv & 3), ts = int((st.aenv >> 2) & 3);
        for (int k = 0; k < 4; k++) { st.swap_ras[k] = swaps[rs][k]; st.swap_tex[k] = swaps[ts][k]; }
    }
    {   // indirect texturing
        c.nind = int((genmode >> 16) & 7);
        if (c.nind > 4) c.nind = 4;
        uint32_t iref = g.bp[0x27];
        for (int i = 0; i < c.nind; i++) {
            c.ind_map[i] = uint8_t((iref >> (6 * i)) & 7);
            c.ind_coord[i] = uint8_t((iref >> (6 * i + 3)) & 7);
            uint32_t ss = g.bp[0x25 + i / 2] >> ((i & 1) * 8);
            c.ind_ss[i] = uint8_t(ss & 15);
            c.ind_ts[i] = uint8_t((ss >> 4) & 15);
        }
        for (int m = 0; m < 3; m++) {
            uint32_t ra = g.bp[0x06 + 3 * m], rb = g.bp[0x07 + 3 * m], rc = g.bp[0x08 + 3 * m];
            auto s11 = [](uint32_t v) { return int16_t(int32_t(v << 21) >> 21); };
            c.ind_mtx[m][0] = s11(ra); c.ind_mtx[m][1] = s11(ra >> 11);
            c.ind_mtx[m][2] = s11(rb); c.ind_mtx[m][3] = s11(rb >> 11);
            c.ind_mtx[m][4] = s11(rc); c.ind_mtx[m][5] = s11(rc >> 11);
            int scale = int((ra >> 22) & 3) | int((rb >> 22) & 3) << 2 | int((rc >> 22) & 3) << 4;
            c.ind_shift[m] = int8_t(17 - scale);
        }
        for (int s = 0; s < c.stages; s++) {
            uint32_t cmd = g.bp[0x10 + s] & 0x1FFFFF;
            uint32_t bs = (cmd >> 7) & 3, mid = (cmd >> 9) & 15, sw = (cmd >> 13) & 7, tw = (cmd >> 16) & 7, fb = (cmd >> 20) & 1;
            bool active = bs || mid || sw || tw || fb;
            c.st[s].ind = active ? (cmd | 0x80000000u) : 0;
        }
        static const bool disable = getenv("SSNE_NO_IND") != nullptr;  // A/B testing
        if (disable) { c.nind = 0; for (int s = 0; s < c.stages; s++) c.st[s].ind = 0; }
        static const bool trace = getenv("SSNE_TRACE_IND") != nullptr;
        if (trace && c.nind) {
            static uint64_t last_frame = ~0ull;
            if (last_frame != counters.display_copies) {
                last_frame = counters.display_copies;
                fprintf(stdout, "[ind] frame %llu nind=%d iref=%06X ss0=%06X ss1=%06X stages=%d",
                        (unsigned long long)counters.display_copies, c.nind, iref, g.bp[0x25], g.bp[0x26], c.stages);
                for (int s = 0; s < c.stages; s++) fprintf(stdout, " cmd%d=%06X", s, g.bp[0x10 + s]);
                fprintf(stdout, " mtx0=%06X,%06X,%06X", g.bp[0x06], g.bp[0x07], g.bp[0x08]);
                if (const Texture* t = get_texture(c.ind_map[0])) {
                    int mn[4] = {255, 255, 255, 255}, mx[4] = {};
                    for (uint32_t p : t->rgba)
                        for (int k = 0; k < 4; k++) { int v = int(p >> (8 * k)) & 255; mn[k] = std::min(mn[k], v); mx[k] = std::max(mx[k], v); }
                    uint32_t mb = c.ind_map[0] < 4 ? 0x80 : 0xA0;
                    fprintf(stdout, " indtex %dx%d fmt%u r%d-%d g%d-%d b%d-%d a%d-%d", t->w, t->h, (g.bp[mb + 8 + (c.ind_map[0] & 3)] >> 20) & 15,
                            mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], mn[3], mx[3]);
                }
                fprintf(stdout, "\n");
            }
        }
    }
    for (int m = 0; m < 8; m++) {
        c.tex[m] = nullptr;
        uint32_t mode0 = g.bp[(m < 4 ? 0x80 : 0xA0) + (m & 3)];
        c.wrap_s[m] = mode0 & 3;
        c.wrap_t[m] = (mode0 >> 2) & 3;
        c.linear[m] = (mode0 >> 4) & 1;
        c.min_filter[m] = uint8_t((mode0 >> 5) & 7);
    }
    for (int s = 0; s < c.stages; s++)
        if (c.st[s].tex_enabled && !c.tex[c.st[s].texmap]) c.tex[c.st[s].texmap] = get_texture(c.st[s].texmap);
    for (int i = 0; i < c.nind; i++)
        if (!c.tex[c.ind_map[i]]) c.tex[c.ind_map[i]] = get_texture(c.ind_map[i]);
    for (int t = 0; t < 8; t++) {
        c.su_s[t] = float((g.bp[0x30 + 2 * t] & 0xFFFF) + 1);
        c.su_t[t] = float((g.bp[0x31 + 2 * t] & 0xFFFF) + 1);
    }
    c.alpha_cmp = g.bp[0xF3];
    {
        uint32_t ac = c.alpha_cmp, logic = (ac >> 22) & 3;
        bool a0 = ((ac >> 16) & 7) == 7, a1 = ((ac >> 19) & 7) == 7;
        c.atest_always = (logic == 0 && a0 && a1) || (logic == 1 && (a0 || a1));
    }
    uint32_t zmode = g.bp[0x40];
    c.z_enable = zmode & 1;
    c.z_func = (zmode >> 1) & 7;
    c.z_update = (zmode >> 4) & 1;
    c.blend = g.bp[0x41];
    c.const_alpha_en = (g.bp[0x42] >> 8) & 1;
    c.const_alpha = int(g.bp[0x42] & 0xFF);
    c.dst_has_alpha = (g.bp[0x43] & 7) == 1;  // RGBA6_Z24
    {
        uint32_t bm = c.blend, sf = (bm >> 8) & 7, df = (bm >> 5) & 7;
        if (!(bm & 1) && !(bm & 2)) c.blend_kind = BLEND_NONE;
        else if ((bm & 1) && !(bm & (1 << 11)) && sf == 4 && df == 5) c.blend_kind = BLEND_ALPHA;
        else if ((bm & 1) && !(bm & (1 << 11)) && sf == 1 && df == 1) c.blend_kind = BLEND_ADD;
        else c.blend_kind = BLEND_GENERIC;
        c.write_mask = ((bm & (1 << 3)) ? 0x00FFFFFFu : 0) | (((bm & (1 << 4)) && c.dst_has_alpha) ? 0xFF000000u : 0);
    }
    uint32_t tl = g.bp[0x20], br = g.bp[0x21], so = g.bp[0x59];
    c.xoff = float(int(so & 0x3FF) * 2 - 342);
    c.yoff = float(int((so >> 10) & 0x3FF) * 2 - 342);
    c.scissor_x0 = std::max(0, int((tl >> 12) & 0x7FF) - 342 - int(c.xoff));
    c.scissor_y0 = std::max(0, int(tl & 0x7FF) - 342 - int(c.yoff));
    c.scissor_x1 = std::min(EFB_W - 1, int((br >> 12) & 0x7FF) - 342 - int(c.xoff));
    c.scissor_y1 = std::min(EFB_H - 1, int(br & 0x7FF) - 342 - int(c.yoff));
    memcpy(c.tev_reg, g_tev_reg, sizeof(c.tev_reg));
    memcpy(c.konst, g_konst, sizeof(c.konst));
    c.ntex = int(std::min<uint32_t>(g.xf_reg[0x3F] & 0xF, 8));
    {   // fog: A and C are 20-bit floats (sign 1, exponent 8, mantissa 11)
        uint32_t f1 = g.bp[0xF1];
        auto f20 = [](uint32_t v) {
            uint32_t bits = ((v >> 19) & 1) << 31 | ((v >> 11) & 0xFF) << 23 | (v & 0x7FF) << 12;
            float f; memcpy(&f, &bits, 4); return f;
        };
        c.fog_sel = uint8_t((f1 >> 21) & 7);
        c.fog_proj = uint8_t((f1 >> 20) & 1);
        c.fog_a = f20(g.bp[0xEE]);
        c.fog_c = f20(f1 & 0xFFFFF);
        c.fog_bmag = g.bp[0xEF] & 0xFFFFFF;
        c.fog_bshift = g.bp[0xF0] & 0x1F;
        uint32_t fc = g.bp[0xF2];
        c.fog_color[0] = int((fc >> 16) & 0xFF); c.fog_color[1] = int((fc >> 8) & 0xFF); c.fog_color[2] = int(fc & 0xFF);
        static bool warned = false;
        if (c.fog_sel && (g.bp[0xE8] >> 10 & 1) && !warned) { warned = true; fprintf(stdout, "[gx] fog range adjustment is used (not emulated)\n"); }
    }
    for (int s = 0; s < c.stages; s++) {
        if (c.st[s].ras_chan == 0 || c.st[s].ras_chan == 1) c.need_ras[c.st[s].ras_chan] = true;
        if ((c.st[s].tex_enabled || c.st[s].ind) && c.st[s].texcoord < c.ntex) c.need_tex |= uint8_t(1u << c.st[s].texcoord);
    }
    for (int i = 0; i < c.nind; i++)
        if (c.ind_coord[i] < c.ntex) c.need_tex |= uint8_t(1u << c.ind_coord[i]);
}

namespace {

inline int ifloor(float f) { int i = int(f); return (f < float(i)) ? i - 1 : i; }

inline int wrap_coord(int v, int size, uint32_t mode) {
    if (mode == 1) {
        if ((size & (size - 1)) == 0) return v & (size - 1);
        v %= size; return v < 0 ? v + size : v;
    }
    if (mode == 2) {
        int p = size * 2;
        v %= p;
        if (v < 0) v += p;
        return v < size ? v : p - 1 - v;
    }
    return v < 0 ? 0 : v >= size ? size - 1 : v;
}

// u, v are texel coordinates (already scaled by the rasterizer's SU scale registers).
inline uint32_t sample(const DrawCfg& c, int map, float u, float v) {
    const Texture* tx = c.tex[map];
    if (!tx || tx->w == 0) return 0xFFFFFFFFu;
    const uint32_t* px = tx->rgba.data();
    if (!c.linear[map]) {
        int x = wrap_coord(ifloor(u), tx->w, c.wrap_s[map]);
        int y = wrap_coord(ifloor(v), tx->h, c.wrap_t[map]);
        return px[size_t(y) * tx->w + x];
    }
    u -= 0.5f; v -= 0.5f;
    int x0 = ifloor(u), y0 = ifloor(v);
    uint32_t fx = uint32_t((u - float(x0)) * 256), fy = uint32_t((v - float(y0)) * 256);
    int xa = wrap_coord(x0, tx->w, c.wrap_s[map]), xb = wrap_coord(x0 + 1, tx->w, c.wrap_s[map]);
    int ya = wrap_coord(y0, tx->h, c.wrap_t[map]), yb = wrap_coord(y0 + 1, tx->h, c.wrap_t[map]);
    uint32_t p00 = px[size_t(ya) * tx->w + xa], p10 = px[size_t(ya) * tx->w + xb];
    uint32_t p01 = px[size_t(yb) * tx->w + xa], p11 = px[size_t(yb) * tx->w + xb];
    // Two channels at a time (0x00FF00FF masks).
    auto lerp = [](uint32_t a, uint32_t b, uint32_t f) {
        uint32_t rb = ((a & 0x00FF00FF) * (256 - f) + (b & 0x00FF00FF) * f) >> 8;
        uint32_t ga = (((a >> 8) & 0x00FF00FF) * (256 - f) + ((b >> 8) & 0x00FF00FF) * f) >> 8;
        return (rb & 0x00FF00FF) | ((ga & 0x00FF00FF) << 8);
    };
    return lerp(lerp(p00, p10, fx), lerp(p01, p11, fx), fy);
}

inline int konst_c(const DrawCfg& c, uint32_t sel, int ch) {
    static const int fixed[8] = {255, 223, 191, 159, 128, 96, 64, 32};
    if (sel < 8) return fixed[sel];
    if (sel >= 12 && sel <= 15) return c.konst[sel - 12][ch];
    if (sel >= 16) return c.konst[(sel - 16) & 3][(sel - 16) >> 2];  // K0_R.. K3_A
    return 0;
}

inline int tev_combine(int a, int b, int cc, int d, uint32_t bias, uint32_t op, uint32_t scale) {
    int ccc = cc + (cc >> 7);
    int lerp = ((a << 8) + (b - a) * ccc + (op ? 127 : 128)) >> 8;
    int v = d + (op ? -lerp : lerp);
    if (bias == 1) v += 128; else if (bias == 2) v -= 128;
    switch (scale) { case 1: v <<= 1; break; case 2: v <<= 2; break; case 3: v >>= 1; break; default: break; }
    return v;
}

// Texture coordinate in the hardware's fixed-point format (1/128 texel), as used by indirect texturing.
inline void fixed_uv(const DrawCfg& c, const float (*tc)[3], int coord, int& u, int& v) {
    const float* t = tc[coord & 7];
    float q = t[2] != 0 ? t[2] : 1.f;
    u = int(t[0] / q * c.su_s[coord & 7] * 128.f);
    v = int(t[1] / q * c.su_t[coord & 7] * 128.f);
}

inline int ind_wrap(int v, uint32_t mode) {
    static const int size[8] = {0, 256 << 7, 128 << 7, 64 << 7, 32 << 7, 16 << 7, 0, 0};
    if (mode == 0 || mode == 7) return v;
    if (mode == 6) return 0;
    return v & (size[mode] - 1);
}

// Indirect stage: turns the indirect texture lookup into a texture coordinate offset (Dolphin's formulation).
// Returns the stage's final coordinate in `tev` (1/128 texel units) and updates `alphabump`.
inline void ind_stage(const DrawCfg& c, const StageCfg& st, const int (*iind)[3], const float (*tc)[3], int tev[2],
                      int& alphabump) {
    uint32_t cmd = st.ind;
    uint32_t bt = cmd & 3, fmt = (cmd >> 2) & 3, bias = (cmd >> 4) & 7, bs = (cmd >> 7) & 3, mid = (cmd >> 9) & 15;
    uint32_t sw = (cmd >> 13) & 7, tw = (cmd >> 16) & 7, fb = (cmd >> 20) & 1;
    int crd[3] = {0, 0, 0};
    if (int(bt) < c.nind) { crd[0] = iind[bt][0]; crd[1] = iind[bt][1]; crd[2] = iind[bt][2]; }
    static const int bump_mask[4] = {0xF8, 0xE0, 0xF0, 0xF8}, fmt_mask[4] = {0xFF, 0x1F, 0x0F, 0x07};
    if (bs) alphabump = crd[bs - 1] & bump_mask[fmt];
    for (int k = 0; k < 3; k++) {
        crd[k] &= fmt_mask[fmt];
        if (bias & (1u << k)) crd[k] += fmt == 0 ? -128 : 1;
    }
    int uv[2];
    fixed_uv(c, tc, st.texcoord, uv[0], uv[1]);
    int tr[2] = {0, 0};
    int m = -1;
    if (mid >= 1 && mid <= 3) {
        m = int(mid) - 1;
        const int16_t* x = c.ind_mtx[m];
        tr[0] = (x[0] * crd[0] + x[2] * crd[1] + x[4] * crd[2]) >> 3;
        tr[1] = (x[1] * crd[0] + x[3] * crd[1] + x[5] * crd[2]) >> 3;
    } else if (mid >= 5 && mid <= 7) {
        m = int(mid) - 5;
        tr[0] = int((int64_t(uv[0]) * crd[0]) >> 8);
        tr[1] = int((int64_t(uv[1]) * crd[0]) >> 8);
    } else if (mid >= 9 && mid <= 11) {
        m = int(mid) - 9;
        tr[0] = int((int64_t(uv[0]) * crd[1]) >> 8);
        tr[1] = int((int64_t(uv[1]) * crd[1]) >> 8);
    }
    if (m >= 0) {
        int sh = c.ind_shift[m];
        for (int k = 0; k < 2; k++) tr[k] = sh >= 0 ? tr[k] >> sh : int(uint32_t(tr[k]) << -sh);
    }
    int w0 = ind_wrap(uv[0], sw) + tr[0], w1 = ind_wrap(uv[1], tw) + tr[1];
    if (fb) { tev[0] += w0; tev[1] += w1; } else { tev[0] = w0; tev[1] = w1; }
}

// Runs all TEV stages for one pixel. Inputs are table-driven: cin[sel][ch] / ain[sel].
Pix run_tev(const DrawCfg& c, const float ras[2][4], const float (*tc)[3]) {
    int reg[4][4];
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) reg[r][k] = c.tev_reg[r][k];
    int iind[4][3] = {};
    for (int i = 0; i < c.nind; i++) {  // indirect texture lookups (S = alpha, T = blue, U = green)
        int u, v;
        fixed_uv(c, tc, c.ind_coord[i], u, v);
        u >>= c.ind_ss[i];
        v >>= c.ind_ts[i];
        uint32_t raw = sample(c, c.ind_map[i], float(u) * (1.f / 128.f), float(v) * (1.f / 128.f));
        iind[i][0] = int(raw >> 24); iind[i][1] = int((raw >> 16) & 255); iind[i][2] = int((raw >> 8) & 255);
    }
    int tev_uv[2] = {0, 0}, alphabump = 0;
    for (int s = 0; s < c.stages; s++) {
        const StageCfg& st = c.st[s];
        int tex[4] = {255, 255, 255, 255};
        if (st.ind) {
            ind_stage(c, st, iind, tc, tev_uv, alphabump);
        } else if (c.nind) {
            fixed_uv(c, tc, st.texcoord, tev_uv[0], tev_uv[1]);
        }
        if (st.tex_enabled && st.ind) {
            uint32_t raw = sample(c, st.texmap, float(tev_uv[0]) * (1.f / 128.f), float(tev_uv[1]) * (1.f / 128.f));
            int ch[4] = {int(raw & 255), int((raw >> 8) & 255), int((raw >> 16) & 255), int(raw >> 24)};
            for (int k = 0; k < 4; k++) tex[k] = ch[st.swap_tex[k]];
        } else if (st.tex_enabled) {
            const float* t = tc[st.texcoord];
            float q = t[2] != 0 ? t[2] : 1.f;
            float iq = 1.f / q;
            uint32_t raw = sample(c, st.texmap, t[0] * iq * c.su_s[st.texcoord], t[1] * iq * c.su_t[st.texcoord]);
            int ch[4] = {int(raw & 255), int((raw >> 8) & 255), int((raw >> 16) & 255), int(raw >> 24)};
            for (int k = 0; k < 4; k++) tex[k] = ch[st.swap_tex[k]];
        }
        int rc[4] = {0, 0, 0, 0};
        if (st.ras_chan >= 2) {
            int ab = st.ras_chan == 2 ? alphabump : (alphabump | (alphabump >> 5));
            rc[0] = rc[1] = rc[2] = rc[3] = ab;
        } else if (st.ras_chan >= 0) {
            const float* r = ras[st.ras_chan];
            int ch[4] = {int(r[0]), int(r[1]), int(r[2]), int(r[3])};
            for (int k = 0; k < 4; k++) rc[k] = ch[st.swap_ras[k]];
        }
        // Fill only the inputs this stage selects (4 color + 4 alpha operands).
        int cin[16][3];
        auto fill_c = [&](uint32_t sel) {
            int* o = cin[sel];
            switch (sel) {
            case 0: case 2: case 4: case 6: { const int* r = reg[sel >> 1]; o[0] = r[0]; o[1] = r[1]; o[2] = r[2]; break; }
            case 1: case 3: case 5: case 7: o[0] = o[1] = o[2] = reg[sel >> 1][3]; break;
            case 8: o[0] = tex[0]; o[1] = tex[1]; o[2] = tex[2]; break;
            case 9: o[0] = o[1] = o[2] = tex[3]; break;
            case 10: o[0] = rc[0]; o[1] = rc[1]; o[2] = rc[2]; break;
            case 11: o[0] = o[1] = o[2] = rc[3]; break;
            case 12: o[0] = o[1] = o[2] = 255; break;
            case 13: o[0] = o[1] = o[2] = 128; break;
            case 14: for (int k = 0; k < 3; k++) o[k] = konst_c(c, st.kcsel, k); break;
            default: o[0] = o[1] = o[2] = 0; break;
            }
        };
        {
            uint32_t ce0 = st.cenv;
            fill_c(ce0 & 15); fill_c((ce0 >> 4) & 15); fill_c((ce0 >> 8) & 15); fill_c((ce0 >> 12) & 15);
        }
        int ain[8];
        {
            uint32_t ae0 = st.aenv;
            uint32_t sels[4] = {(ae0 >> 4) & 7, (ae0 >> 7) & 7, (ae0 >> 10) & 7, (ae0 >> 13) & 7};
            for (uint32_t sel : sels) {
                switch (sel) {
                case 0: case 1: case 2: case 3: ain[sel] = reg[sel][3]; break;
                case 4: ain[sel] = tex[3]; break;
                case 5: ain[sel] = rc[3]; break;
                case 6: ain[sel] = konst_c(c, st.kasel, 3); break;
                default: ain[sel] = 0; break;
                }
            }
        }

        uint32_t ce = st.cenv, ae = st.aenv;
        int cres[3];
        {
            uint32_t sd = ce & 15, sc = (ce >> 4) & 15, sb = (ce >> 8) & 15, sa = (ce >> 12) & 15;
            uint32_t bias = (ce >> 16) & 3, op = (ce >> 18) & 1, clamp = (ce >> 19) & 1, scale = (ce >> 20) & 3;
            if (bias == 3) {  // compare modes
                uint32_t mode = (scale << 1) | op;
                int ar = cin[sa][0] & 255, ag = cin[sa][1] & 255, ab = cin[sa][2] & 255;
                int br = cin[sb][0] & 255, bg = cin[sb][1] & 255, bb = cin[sb][2] & 255;
                bool cond_all = false;
                switch (mode) {
                case 0: cond_all = ar > br; break;
                case 1: cond_all = ar == br; break;
                case 2: cond_all = (ag << 8 | ar) > (bg << 8 | br); break;
                case 3: cond_all = (ag << 8 | ar) == (bg << 8 | br); break;
                case 4: cond_all = (ab << 16 | ag << 8 | ar) > (bb << 16 | bg << 8 | br); break;
                case 5: cond_all = (ab << 16 | ag << 8 | ar) == (bb << 16 | bg << 8 | br); break;
                default: break;
                }
                for (int k = 0; k < 3; k++) {
                    bool cond = mode >= 6 ? (mode == 6 ? (cin[sa][k] & 255) > (cin[sb][k] & 255)
                                                       : (cin[sa][k] & 255) == (cin[sb][k] & 255))
                                          : cond_all;
                    int v = cin[sd][k] + (cond ? (cin[sc][k] & 255) : 0);
                    cres[k] = clamp ? std::clamp(v, 0, 255) : std::clamp(v, -1024, 1023);
                }
            } else {
                for (int k = 0; k < 3; k++) {
                    int v = tev_combine(cin[sa][k] & 255, cin[sb][k] & 255, cin[sc][k] & 255, cin[sd][k], bias, op, scale);
                    cres[k] = clamp ? std::clamp(v, 0, 255) : std::clamp(v, -1024, 1023);
                }
            }
        }
        int ares;
        {
            uint32_t sd = (ae >> 4) & 7, sc = (ae >> 7) & 7, sb = (ae >> 10) & 7, sa = (ae >> 13) & 7;
            uint32_t bias = (ae >> 16) & 3, op = (ae >> 18) & 1, clamp = (ae >> 19) & 1, scale = (ae >> 20) & 3;
            int a = ain[sa] & 255, b = ain[sb] & 255, cc = ain[sc] & 255, d = ain[sd];
            int v;
            if (bias == 3) {
                uint32_t mode = (scale << 1) | op;
                bool cond = (mode & 1) ? a == b : a > b;
                v = d + (cond ? cc : 0);
            } else {
                v = tev_combine(a, b, cc, d, bias, op, scale);
            }
            ares = clamp ? std::clamp(v, 0, 255) : std::clamp(v, -1024, 1023);
        }
        int cdest = int((ce >> 22) & 3), adest = int((ae >> 22) & 3);
        reg[cdest][0] = cres[0]; reg[cdest][1] = cres[1]; reg[cdest][2] = cres[2];
        reg[adest][3] = ares;
    }
    return {std::clamp(reg[0][0], 0, 255), std::clamp(reg[0][1], 0, 255), std::clamp(reg[0][2], 0, 255),
            std::clamp(reg[0][3], 0, 255)};
}

// GX fog: distance from the 24-bit screen z, fog curve, then an 8-bit blend toward the fog color.
inline void apply_fog(const DrawCfg& c, Pix& p, uint32_t zi) {
    if (c.fog_sel < 2) return;
    float ze;
    if (c.fog_proj == 0) {
        float den = float(int64_t(c.fog_bmag) - int64_t(zi >> c.fog_bshift));
        ze = den != 0 ? (c.fog_a * 16777216.f) / den : 0.f;
    } else {
        ze = c.fog_a * float(zi) / 16777216.f;
    }
    float f = std::clamp(ze - c.fog_c, 0.f, 1.f);
    switch (c.fog_sel) {
    case 4: f = 1.f - std::exp2(-8.f * f); break;
    case 5: f = 1.f - std::exp2(-8.f * f * f); break;
    case 6: f = std::exp2(-8.f * (1.f - f)); break;
    case 7: f = std::exp2(-8.f * (1.f - f) * (1.f - f)); break;
    default: break;
    }
    int ifog = int(std::lround(f * 256.f));
    p.r = (p.r * (256 - ifog) + c.fog_color[0] * ifog) >> 8;
    p.g = (p.g * (256 - ifog) + c.fog_color[1] * ifog) >> 8;
    p.b = (p.b * (256 - ifog) + c.fog_color[2] * ifog) >> 8;
}

inline bool alpha_test(uint32_t ac, int a) {
    int ref0 = int(ac & 0xFF), ref1 = int((ac >> 8) & 0xFF);
    bool r0 = compare((ac >> 16) & 7, a, ref0), r1 = compare((ac >> 19) & 7, a, ref1);
    switch ((ac >> 22) & 3) { case 0: return r0 && r1; case 1: return r0 || r1; case 2: return r0 != r1; default: return r0 == r1; }
}

inline int blend_factor(uint32_t f, bool is_src, const Pix& s, const Pix& d, int ch) {
    int sc = ch == 0 ? s.r : ch == 1 ? s.g : ch == 2 ? s.b : s.a;
    int dc = ch == 0 ? d.r : ch == 1 ? d.g : ch == 2 ? d.b : d.a;
    switch (f) {
    case 0: return 0; case 1: return 255;
    case 2: return is_src ? dc : sc; case 3: return 255 - (is_src ? dc : sc);
    case 4: return s.a; case 5: return 255 - s.a; case 6: return d.a; default: return 255 - d.a;
    }
}

inline int div255(int v) { return (v + 128 + ((v + 128) >> 8)) >> 8; }

uint32_t blend(const DrawCfg& c, const Pix& s, uint32_t dst_raw) {
    uint32_t bm = c.blend;
    Pix out = s;
    switch (c.blend_kind) {
    case BLEND_NONE: break;
    case BLEND_ALPHA: {
        Pix d = unpack(dst_raw);
        if (!c.dst_has_alpha) d.a = 255;
        int a = std::clamp(s.a, 0, 255), ia = 255 - a;
        out = {div255(s.r * a + d.r * ia), div255(s.g * a + d.g * ia), div255(s.b * a + d.b * ia), div255(s.a * a + d.a * ia)};
        break;
    }
    case BLEND_ADD: {
        Pix d = unpack(dst_raw);
        out = {s.r + d.r, s.g + d.g, s.b + d.b, s.a + d.a};
        break;
    }
    default: {
        Pix d = unpack(dst_raw);
        if (!c.dst_has_alpha) d.a = 255;
        if (bm & 1) {
            if (bm & (1 << 11)) {
                out = {d.r - s.r, d.g - s.g, d.b - s.b, d.a - s.a};
            } else {
                uint32_t sf = (bm >> 8) & 7, df = (bm >> 5) & 7;
                int o[4];
                for (int ch = 0; ch < 4; ch++) {
                    int sv = ch == 0 ? s.r : ch == 1 ? s.g : ch == 2 ? s.b : s.a;
                    int dv = ch == 0 ? d.r : ch == 1 ? d.g : ch == 2 ? d.b : d.a;
                    o[ch] = div255(sv * blend_factor(sf, true, s, d, ch) + dv * blend_factor(df, false, s, d, ch));
                }
                out = {o[0], o[1], o[2], o[3]};
            }
        } else if (bm & 2) {  // logic op
            uint32_t sv = pack(s), dv = dst_raw, r;
            switch ((bm >> 12) & 15) {
            case 0: r = 0; break; case 1: r = sv & dv; break; case 2: r = sv & ~dv; break; case 3: r = sv; break;
            case 4: r = ~sv & dv; break; case 5: r = dv; break; case 6: r = sv ^ dv; break; case 7: r = sv | dv; break;
            case 8: r = ~(sv | dv); break; case 9: r = ~(sv ^ dv); break; case 10: r = ~dv; break; case 11: r = sv | ~dv; break;
            case 12: r = ~sv; break; case 13: r = ~sv | dv; break; case 14: r = ~(sv & dv); break; default: r = ~0u; break;
            }
            out = unpack(r);
        }
    }
    }
    if (c.const_alpha_en) out.a = c.const_alpha;
    return (pack(out) & c.write_mask) | (dst_raw & ~c.write_mask);
}

// ---- queued triangles (rasterized in parallel bands at flush time)

struct SV {  // screen-space vertex; attributes are pre-multiplied by 1/w for perspective correction
    float x, y, z, invw;
    float col[2][4];
    float tex[8][3];
};

struct Tri {
    const DrawCfg* cfg;
    SV v[3];
    int minx, maxx, miny, maxy;
};

std::deque<DrawCfg> g_cfgs;   // stable addresses while queued
std::vector<Tri> g_tris;

struct alignas(64) BandStats { uint64_t zfail = 0, afail = 0, written = 0; };

// Rows are dealt to workers in interleaved 4-row stripes (stripe % nworkers == worker) for load balance.
void raster_band(const Tri& t, int worker, int nworkers, BandStats& out_stats) {
    BandStats bs;  // local: avoids cache-line ping-pong between workers
    struct Commit { BandStats& o; BandStats& l; ~Commit() { o.zfail += l.zfail; o.afail += l.afail; o.written += l.written; } } commit{out_stats, bs};
    const DrawCfg& c = *t.cfg;
    int miny = t.miny, maxy = t.maxy;
    const SV &v0 = t.v[0], &v1 = t.v[1], &v2 = t.v[2];
    float area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
    float inv_area = 1.f / area;
    // w0(px,py) = ((v1.x-px)(v2.y-py) - (v2.x-px)(v1.y-py)) / area  -> linear in px, py
    float dw0dx = (v1.y - v2.y) * inv_area, dw1dx = (v2.y - v0.y) * inv_area;
    int ntex = c.ntex;
    bool atest = !c.atest_always && !dbg.noatest;
    bool ztest = c.z_enable && !dbg.noz;
    float ras[2][4];
    float tc[8][3];
    const float eps = -1e-6f;
    for (int y = miny; y <= maxy; y++) {
        if (nworkers > 1 && ((y >> 2) % nworkers) != worker) continue;
        float py = float(y) + 0.5f;
        float px0 = float(t.minx) + 0.5f;
        float w0s = ((v1.x - px0) * (v2.y - py) - (v2.x - px0) * (v1.y - py)) * inv_area;
        float w1s = ((v2.x - px0) * (v0.y - py) - (v0.x - px0) * (v2.y - py)) * inv_area;
        float w2s = 1.f - w0s - w1s, dw2dx = -dw0dx - dw1dx;
        // Exact covered span on this row: every w_i(x) = w_i0 + dw_i * k >= eps.
        float kmin = 0.f, kmax = float(t.maxx - t.minx);
        auto limit = [&](float w, float dw) {
            if (dw > 0) kmin = std::max(kmin, std::ceil((eps - w) / dw));
            else if (dw < 0) kmax = std::min(kmax, std::floor((eps - w) / dw));
            else if (w < eps) kmax = -1.f;
        };
        limit(w0s, dw0dx); limit(w1s, dw1dx); limit(w2s, dw2dx);
        if (kmin > kmax) continue;
        int x0 = t.minx + int(kmin), x1 = t.minx + int(kmax);
        float w0 = w0s + dw0dx * kmin, w1 = w1s + dw1dx * kmin;
        uint32_t* crow = g_efb + size_t(y) * EFB_W;
        uint32_t* zrow = g_zbuf + size_t(y) * EFB_W;
        for (int x = x0; x <= x1; x++, w0 += dw0dx, w1 += dw1dx) {
            float w2 = 1.f - w0 - w1;
            if (w0 < eps || w1 < eps || w2 < eps) continue;  // float drift at span ends
            float z = w0 * v0.z + w1 * v1.z + w2 * v2.z;
            uint32_t zi = uint32_t(std::clamp(z, 0.f, 1.f) * 16777215.f);
            if (ztest && !compare(c.z_func, int(zi), int(zrow[x]))) { bs.zfail++; continue; }
            float iw = w0 * v0.invw + w1 * v1.invw + w2 * v2.invw;
            float rw = iw != 0 ? 1.f / iw : 0.f;
            float p0 = w0 * rw, p1 = w1 * rw, p2 = w2 * rw;
            for (int ch = 0; ch < 2; ch++)
                if (c.need_ras[ch])
                    for (int k = 0; k < 4; k++) ras[ch][k] = p0 * v0.col[ch][k] + p1 * v1.col[ch][k] + p2 * v2.col[ch][k];
            for (int tt = 0; tt < ntex; tt++)
                if (c.need_tex & (1u << tt))
                    for (int k = 0; k < 3; k++) tc[tt][k] = p0 * v0.tex[tt][k] + p1 * v1.tex[tt][k] + p2 * v2.tex[tt][k];
            Pix col = run_tev(c, ras, tc);
            if (atest && !alpha_test(c.alpha_cmp, col.a)) { bs.afail++; continue; }
            apply_fog(c, col, zi);
            if (ztest && c.z_update) zrow[x] = zi;
            crow[x] = blend(c, col, crow[x]);
            bs.written++;
        }
    }
}

// ---- worker pool: each worker owns a horizontal band of the EFB
struct Pool {
    std::vector<std::thread> threads;
    std::mutex m;
    std::condition_variable cv_start, cv_done;
    uint64_t generation = 0;
    int pending = 0;
    int n = 1;
    BandStats stats[64];
    bool started = false;

    void start() {
        if (started) return;
        started = true;
        unsigned hw = std::thread::hardware_concurrency();
        const char* env = getenv("SSNE_GX_THREADS");
        n = env ? atoi(env) : int(hw ? hw : 1);
        n = std::clamp(n, 1, 32);
        for (int i = 1; i < n; i++) threads.emplace_back([this, i] { worker(i); });
        for (auto& t : threads) t.detach();
    }
    void run_band(int i) {
        for (const Tri& t : g_tris) raster_band(t, i, n, stats[i]);
    }
    void worker(int i) {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m);
                cv_start.wait(lk, [&] { return generation != seen; });
                seen = generation;
            }
            run_band(i);
            std::lock_guard<std::mutex> lk(m);
            if (--pending == 0) cv_done.notify_all();
        }
    }
    void flush() {
        start();
        if (n > 1) {
            std::unique_lock<std::mutex> lk(m);
            pending = n - 1;
            generation++;
            cv_start.notify_all();
        }
        run_band(0);
        if (n > 1) {
            std::unique_lock<std::mutex> lk(m);
            cv_done.wait(lk, [&] { return pending == 0; });
        }
        for (int i = 0; i < n; i++) {
            fs.zfail += stats[i].zfail; fs.afail += stats[i].afail; fs.written += stats[i].written;
            stats[i] = BandStats{};
        }
    }
} g_pool;

double g_flush_ms = 0;
uint64_t g_flush_tris = 0;
void flush_tris() {
    if (!g_tris.empty()) {
        auto t0 = std::chrono::steady_clock::now();
        g_pool.flush();
        g_flush_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        g_flush_tris += g_tris.size();
    }
    g_tris.clear();
    g_cfgs.clear();
    texture_cache_trim();
}

void submit_triangle(const DrawCfg& c, const SV& v0, const SV& v1, const SV& v2) {
    float area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
    fs.tris++;
    if (std::fabs(area) < 1e-8f) { fs.degenerate++; return; }
    uint32_t cull = (g.bp[0x00] >> 14) & 3;
    if (dbg.nocull) cull = 0;
    // GX: front faces are clockwise on screen (y down => positive area here).
    if (cull == 3 || (cull == 1 && area < 0) || (cull == 2 && area > 0)) { fs.culled++; return; }
    Tri t;
    t.cfg = &c;
    t.minx = std::max(c.scissor_x0, ifloor(std::min({v0.x, v1.x, v2.x})));
    t.maxx = std::min(c.scissor_x1, int(std::ceil(std::max({v0.x, v1.x, v2.x}))));
    t.miny = std::max(c.scissor_y0, ifloor(std::min({v0.y, v1.y, v2.y})));
    t.maxy = std::min(c.scissor_y1, int(std::ceil(std::max({v0.y, v1.y, v2.y}))));
    if (t.minx > t.maxx || t.miny > t.maxy) { fs.offscreen++; return; }
    counters.triangles++;
    t.v[0] = v0; t.v[1] = v1; t.v[2] = v2;
    g_tris.push_back(t);
    if (g_tris.size() >= 262144) flush_tris();
}

// Clip against near (z >= -w), far (z <= 0) and w > 0 in clip space; then screen-transform and queue.
void clip_and_submit(const DrawCfg& c, const Vtx* in) {
    Vtx poly_a[12], poly_b[12];
    int n = 3;
    for (int i = 0; i < 3; i++) poly_a[i] = in[i];
    Vtx* poly = poly_a;
    Vtx* tmp = poly_b;
    auto clip_plane = [&](int plane) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            const Vtx& a = poly[i];
            const Vtx& b = poly[(i + 1) % n];
            float da = plane == 0 ? a.z + a.w : plane == 1 ? -a.z : a.w - 1e-5f;
            float db = plane == 0 ? b.z + b.w : plane == 1 ? -b.z : b.w - 1e-5f;
            if (da >= 0 && m < 12) tmp[m++] = a;
            if ((da >= 0) != (db >= 0) && m < 12) {
                float t = da / (da - db);
                Vtx& o = tmp[m++];
                o.x = a.x + (b.x - a.x) * t; o.y = a.y + (b.y - a.y) * t;
                o.z = a.z + (b.z - a.z) * t; o.w = a.w + (b.w - a.w) * t;
                for (int ch = 0; ch < 2; ch++)
                    for (int k = 0; k < 4; k++) o.color[ch][k] = a.color[ch][k] + (b.color[ch][k] - a.color[ch][k]) * t;
                for (int tt = 0; tt < c.ntex; tt++)
                    for (int k = 0; k < 3; k++) o.tex[tt][k] = a.tex[tt][k] + (b.tex[tt][k] - a.tex[tt][k]) * t;
            }
        }
        std::swap(poly, tmp);
        n = m;
    };
    bool need_clip = false;
    for (int i = 0; i < 3; i++)
        if (in[i].z + in[i].w < 0 || in[i].z > 0 || in[i].w <= 1e-5f) need_clip = true;
    if (need_clip) {
        clip_plane(0);
        if (n >= 3) clip_plane(1);
        if (n >= 3) clip_plane(2);
    }
    if (n < 3) { fs.clipped_away++; return; }

    float sx = xfr_f(0x1A), sy = xfr_f(0x1B), sz = xfr_f(0x1C), ox = xfr_f(0x1D), oy = xfr_f(0x1E), oz = xfr_f(0x1F);
    SV sv[12];
    for (int i = 0; i < n; i++) {
        const Vtx& v = poly[i];
        float iw = 1.f / v.w;
        SV& s = sv[i];
        s.x = v.x * iw * sx + ox - 342.f - c.xoff;
        s.y = v.y * iw * sy + oy - 342.f - c.yoff;
        s.z = (v.z * iw * sz + oz) / 16777216.f;
        s.invw = iw;
        for (int ch = 0; ch < 2; ch++)
            for (int k = 0; k < 4; k++) s.col[ch][k] = v.color[ch][k] * iw;
        for (int tt = 0; tt < c.ntex; tt++)
            for (int k = 0; k < 3; k++) s.tex[tt][k] = v.tex[tt][k] * iw;
    }
    for (int i = 1; i + 1 < n; i++) submit_triangle(c, sv[0], sv[i], sv[i + 1]);
}

} // namespace

void tev_reg_write(uint32_t reg, uint32_t v) {
    int i = int((reg - 0xE0) >> 1);
    bool konst = (v >> 23) & 1;
    if (((reg - 0xE0) & 1) == 0) {  // RA
        if (konst) { g_konst[i][0] = int16_t(v & 0xFF); g_konst[i][3] = int16_t((v >> 12) & 0xFF); }
        else { g_tev_reg[i][0] = int16_t(sext11(v & 0x7FF)); g_tev_reg[i][3] = int16_t(sext11((v >> 12) & 0x7FF)); }
    } else {                         // BG
        if (konst) { g_konst[i][2] = int16_t(v & 0xFF); g_konst[i][1] = int16_t((v >> 12) & 0xFF); }
        else { g_tev_reg[i][2] = int16_t(sext11(v & 0x7FF)); g_tev_reg[i][1] = int16_t(sext11((v >> 12) & 0x7FF)); }
    }
}

// Widescreen HUD: in widescreen gameplay frames, 2D (orthographic) draws keep their 4:3 proportions. Everything
// is scaled around the screen center (the HUD stays inside the original 4:3 area, laid out exactly as designed)
// except elements that touch the left or right screen edge, which stay attached to that edge. Draws spanning the
// whole screen (scene redraws, fades) are left alone. This is the software renderer's per-draw version; the GPU
// recorder places whole groups of draws at the end of the frame (gx_gpu_record.cpp, place_hud), which also allows
// moving the HUD out to the screen edges (hud_edges): per draw, that splits text, which is drawn one glyph per draw.
const std::vector<Vtx>& widescreen_hud(const std::vector<Vtx>& v, std::vector<Vtx>& tmp) {
    float k = widescreen_frame_factor();
    if (k <= 1.001f || !(g.xf_reg[0x26] & 1)) return v;
    float mn = 1e30f, mx = -1e30f;
    for (const Vtx& p : v) {
        if (p.w <= 0) return v;
        float x = p.x / p.w;
        mn = std::min(mn, x); mx = std::max(mx, x);
    }
    if (mx - mn >= 1.9f) return v;
    float nmn = widescreen_hud_left(mn, mx, k, false);
    tmp = v;
    for (Vtx& p : tmp) {
        float x = p.x / p.w;
        p.x = (nmn + (x - mn) / k) * p.w;
    }
    return tmp;
}

// Points and lines are rasterized by GX as screen-aligned quads: points are size x size pixels (SU_LPSIZE, in
// 1/6 pixel units) around the vertex, lines are widened across their minor axis. Texture coordinates with the
// point/line offset enabled (SU_SSIZE bits 19/18) get the SU_LPSIZE texcoord offset added across the quad, which
// makes textured point sprites (the game's particles). They're never culled.
void expand_points_lines(uint8_t prim, const std::vector<Vtx>& v, std::vector<Vtx>& out) {
    static const float kTexOff[8] = {0.f, 1.f / 16, 1.f / 8, 1.f / 4, 1.f / 2, 1.f, 1.f, 1.f};
    uint32_t lp = g.bp[0x22];
    float sx = xfr_f(0x1A), sy = xfr_f(0x1B);
    if (sx == 0 || sy == 0) return;
    int ntex = int(std::min<uint32_t>(g.xf_reg[0x3F] & 0xF, 8));
    out.clear();
    auto push_quad = [&](const Vtx* c, const float dx[4], const float dy[4], const float ts[4], const float tt[4], uint32_t enable_bit) {
        for (int k = 0; k < 4; k++) {
            Vtx q = *c;
            // screen pixel offset -> clip space (screen = x/w * sx + ox)
            q.x += dx[k] / sx * q.w;
            q.y += dy[k] / sy * q.w;
            for (int t = 0; t < ntex; t++)
                if ((g.bp[0x30 + 2 * t] >> enable_bit) & 1) {
                    float qq = q.tex[t][2] != 0 ? q.tex[t][2] : 1.f;
                    q.tex[t][0] += ts[k] * qq;
                    q.tex[t][1] += tt[k] * qq;
                }
            out.push_back(q);
        }
    };
    if (prim == PRIM_POINTS) {
        float h = float((lp >> 8) & 0xFF) / 6.f * 0.5f;
        float off = kTexOff[(lp >> 19) & 7];
        // clockwise with y down: top-left, top-right, bottom-right, bottom-left
        const float dx[4] = {-h, h, h, -h}, dy[4] = {-h, -h, h, h};
        const float ts[4] = {0, off, off, 0}, tt[4] = {0, 0, off, off};
        for (const Vtx& p : v) push_quad(&p, dx, dy, ts, tt, 19);
        return;
    }
    float h = float(lp & 0xFF) / 6.f * 0.5f;
    float off = kTexOff[(lp >> 16) & 7];
    size_t step = prim == PRIM_LINES ? 2 : 1;
    for (size_t i = 0; i + 1 < v.size(); i += step) {
        const Vtx &a = v[i], &b = v[i + 1];
        if (a.w <= 0 || b.w <= 0) continue;
        float ax = a.x / a.w * sx, ay = a.y / a.w * sy, bx = b.x / b.w * sx, by = b.y / b.w * sy;
        bool horizontal = std::fabs(bx - ax) >= std::fabs(by - ay);
        // widen across the minor axis; texcoord offset goes across the width
        float ox = horizontal ? 0.f : h, oy = horizontal ? h : 0.f;
        float ts2 = horizontal ? 0.f : off, tt2 = horizontal ? off : 0.f;
        const float dA[2][2] = {{-ox, -oy}, {ox, oy}};
        Vtx quad[4] = {a, b, b, a};
        const float sideA[4] = {0, 0, 1, 1};  // vertices 0,1 on the "-" side, 2,3 on the "+" side
        for (int k = 0; k < 4; k++) {
            Vtx& q = quad[k];
            int side = int(sideA[k]);
            q.x += dA[side][0] / sx * q.w;
            q.y += dA[side][1] / sy * q.w;
            for (int t = 0; t < ntex; t++)
                if ((g.bp[0x30 + 2 * t] >> 18) & 1) {
                    float qq = q.tex[t][2] != 0 ? q.tex[t][2] : 1.f;
                    q.tex[t][0] += side * ts2 * qq;
                    q.tex[t][1] += side * tt2 * qq;
                }
            out.push_back(q);
        }
    }
}

void draw_prims(uint8_t prim, const std::vector<Vtx>& v);

void draw(uint8_t prim, const std::vector<Vtx>& v_in) {
    if (v_in.empty()) return;
    // Headless timing tests: skip drawing (game logic only); SSNE_NORENDER=<seconds> resumes drawing after that time.
    static const double norender = getenv("SSNE_NORENDER") ? atof(getenv("SSNE_NORENDER")) : -1.0;
    if (norender >= 0) {
        static const auto t0 = std::chrono::steady_clock::now();
        if ((norender < 2 || std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < norender) && !tracing()) return;
    }
    if (prim == PRIM_LINES || prim == PRIM_LINESTRIP || prim == PRIM_POINTS) {
        static std::vector<Vtx> quads;
        expand_points_lines(prim, v_in, quads);
        if (quads.empty()) return;
        counters.lines_skipped++;  // counted as point/line primitives now
        uint32_t gm = g.bp[0x00];
        g.bp[0x00] = gm & ~(3u << 14);  // no culling for points/lines
        g_cfg_dirty = true;
        draw_prims(PRIM_QUADS, quads);
        g.bp[0x00] = gm;
        g_cfg_dirty = true;
        return;
    }
    draw_prims(prim, v_in);
}

void draw_prims(uint8_t prim, const std::vector<Vtx>& v_in) {
    if (gpu_enabled()) { gpu_record_draw(prim, v_in); return; }  // (the recorder places the widescreen HUD)
    static std::vector<Vtx> hud_tmp;
    const std::vector<Vtx>& v = widescreen_hud(v_in, hud_tmp);
    if (dbg.nodraw) return;
    g_cfgs.push_back(build_cfg());
    const DrawCfg& c = g_cfgs.back();
    if (tracing()) {
        fprintf(stdout, "[trace] prim %02X n=%zu stages=%d blend=%06X z=%d/%d/%d acmp=%06X", prim, v.size(), c.stages, c.blend,
                c.z_enable, c.z_func, c.z_update, c.alpha_cmp);
        if ((g.bp[0x00] >> 16) & 7)
            fprintf(stdout, " IND n=%u iref=%06X cmd0=%06X", (g.bp[0x00] >> 16) & 7, g.bp[0x27], g.bp[0x10]);
        {
            float mn = 1e9f, mx = -1e9f, ymn = 1e9f, ymx = -1e9f;
            for (const Vtx& p : v_in) if (p.w > 0) { mn = std::min(mn, p.x / p.w); mx = std::max(mx, p.x / p.w); ymn = std::min(ymn, p.y / p.w); ymx = std::max(ymx, p.y / p.w); }
            fprintf(stdout, " %s x[%.3f,%.3f] y[%.3f,%.3f]", (g.xf_reg[0x26] & 1) ? "ORTHO" : "persp", mn, mx, ymn, ymx);
            uint32_t pm = (g.cp[0x30] & 0x3F) * 4;
            fprintf(stdout, " pm%u=[%.3f %.3f %.3f %.2f | %.3f %.3f %.3f %.2f | %.3f %.3f %.3f %.2f]", pm / 4,
                    xf_f(pm), xf_f(pm + 1), xf_f(pm + 2), xf_f(pm + 3), xf_f(pm + 4), xf_f(pm + 5), xf_f(pm + 6), xf_f(pm + 7),
                    xf_f(pm + 8), xf_f(pm + 9), xf_f(pm + 10), xf_f(pm + 11));
        }
        for (int s2 = 0; s2 < c.stages; s2++) {
            const StageCfg& st = c.st[s2];
            fprintf(stdout, " | s%d c=%06X a=%06X", s2, st.cenv, st.aenv);
            if (st.tex_enabled && c.tex[st.texmap]) {
                int m = st.texmap & 3; uint32_t b = st.texmap < 4 ? 0x80 : 0xA0;
                fprintf(stdout, " tex%d[%dx%d fmt%u @%08X]", st.texmap, c.tex[st.texmap]->w, c.tex[st.texmap]->h,
                        (g.bp[b + 8 + m] >> 20) & 15, (g.bp[b + 0x14 + m] & 0xFFFFFF) << 5);
            }
        }
        fprintf(stdout, "\n");
    }
    Vtx tri[3];
    auto emit = [&](size_t a, size_t b, size_t cc) { tri[0] = v[a]; tri[1] = v[b]; tri[2] = v[cc]; clip_and_submit(c, tri); };
    size_t n = v.size();
    switch (prim) {
    case PRIM_QUADS: case PRIM_QUADS2:
        for (size_t i = 0; i + 3 < n; i += 4) { emit(i, i + 1, i + 2); emit(i, i + 2, i + 3); }
        break;
    case PRIM_TRIANGLES:
        for (size_t i = 0; i + 2 < n; i += 3) emit(i, i + 1, i + 2);
        break;
    case PRIM_TRISTRIP:
        for (size_t i = 0; i + 2 < n; i++) { if (i & 1) emit(i + 1, i, i + 2); else emit(i, i + 1, i + 2); }
        break;
    case PRIM_TRIFAN:
        for (size_t i = 1; i + 1 < n; i++) emit(0, i, i + 1);
        break;
    default: break;
    }
}

void publish_frame(const uint32_t* efb, int x0, int y0, int w, int h) {
    std::lock_guard<std::mutex> lk(g_frame_mutex);
    g_frame.resize(size_t(w) * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int sx = std::min(x0 + x, EFB_W - 1), sy = std::min(y0 + y, EFB_H - 1);
            g_frame[size_t(y) * w + x] = efb[size_t(sy) * EFB_W + sx] | 0xFF000000u;
        }
    g_frame_w = w;
    g_frame_h = h;
    g_frame_seq++;
}

// ---- EFB copies

namespace {

void clear_rect(int x0, int y0, int w, int h) {
    uint32_t ar = g.bp[0x4F], gb = g.bp[0x50];
    uint32_t color = (ar & 0xFF) | ((gb >> 8) & 0xFF) << 8 | (gb & 0xFF) << 16 | ((ar >> 8) & 0xFF) << 24;
    uint32_t z = g.bp[0x51] & 0xFFFFFF;
    uint32_t bm = g.bp[0x41];
    bool cu = bm & (1 << 3), au = bm & (1 << 4), zu = (g.bp[0x40] >> 4) & 1;
    uint32_t mask = (cu ? 0x00FFFFFFu : 0) | (au ? 0xFF000000u : 0);
    for (int y = y0; y < std::min(EFB_H, y0 + h); y++)
        for (int x = x0; x < std::min(EFB_W, x0 + w); x++) {
            size_t i = size_t(y) * EFB_W + x;
            g_efb[i] = (color & mask) | (g_efb[i] & ~mask);
            if (zu) g_zbuf[i] = z;
        }
}

// Encode an EFB rectangle into a GX texture format in guest RAM.
void encode_texture(uint32_t cmd, int x0, int y0, int w, int h) {
    uint32_t dst = (g.bp[0x4B] & 0xFFFFFF) << 5;
    uint32_t stride = (g.bp[0x4D] & 0x3FF) << 5;
    uint32_t tpf = (cmd >> 3) & 0xF;
    uint32_t fmt = tpf / 2 + (tpf & 1) * 8;
    bool intensity = (cmd >> 15) & 1;
    bool half = (cmd >> 9) & 1;
    int ow = half ? w / 2 : w, oh = half ? h / 2 : h;
    auto px = [&](int x, int y) -> Pix {
        if (!half) return unpack(g_efb[size_t(std::min(y0 + y, EFB_H - 1)) * EFB_W + std::min(x0 + x, EFB_W - 1)]);
        int sr = 0, sg = 0, sb = 0, sa = 0;
        for (int dy = 0; dy < 2; dy++)
            for (int dx = 0; dx < 2; dx++) {
                Pix p = unpack(g_efb[size_t(std::min(y0 + 2 * y + dy, EFB_H - 1)) * EFB_W + std::min(x0 + 2 * x + dx, EFB_W - 1)]);
                sr += p.r; sg += p.g; sb += p.b; sa += p.a;
            }
        return {sr / 4, sg / 4, sb / 4, sa / 4};
    };
    auto luma = [](const Pix& p) { return std::clamp((66 * p.r + 129 * p.g + 25 * p.b + 128) / 256 + 16, 0, 255); };
    int bw = 4, bh = 4, bytes = 32;
    switch (fmt) {
    case 0: bw = 8; bh = 8; break;                        // R4/I4
    case 1: case 2: case 7: case 8: case 9: case 10: bw = 8; bh = 4; break;  // 8-bit formats
    default: break;
    }
    if (fmt == 6) bytes = 64;
    int bxn = (ow + bw - 1) / bw, byn = (oh + bh - 1) / bh;
    for (int by = 0; by < byn; by++)
        for (int bx = 0; bx < bxn; bx++) {
            uint32_t o = dst + uint32_t(by) * stride + uint32_t(bx) * uint32_t(bytes);
            for (int k = 0; k < bw * bh; k++) {
                int x = bx * bw + k % bw, y = by * bh + k / bw;
                Pix p = px(std::min(x, ow - 1), std::min(y, oh - 1));
                int I = intensity ? luma(p) : p.r;
                switch (fmt) {
                case 0: {  // 4-bit
                    uint8_t prev = mem_read8(o + k / 2);
                    uint8_t nib = uint8_t(I >> 4);
                    mem_write8(o + k / 2, (k & 1) ? uint8_t((prev & 0xF0) | nib) : uint8_t((prev & 0x0F) | (nib << 4)));
                    break;
                }
                case 1: case 8: mem_write8(o + k, uint8_t(I)); break;
                case 2: mem_write8(o + k, uint8_t((p.a >> 4) << 4 | (I >> 4))); break;
                case 7: mem_write8(o + k, uint8_t(p.a)); break;
                case 9: mem_write8(o + k, uint8_t(p.g)); break;
                case 10: mem_write8(o + k, uint8_t(p.b)); break;
                case 3: mem_write16(o + 2 * k, uint16_t(p.a << 8 | I)); break;
                case 11: mem_write16(o + 2 * k, uint16_t(p.g << 8 | p.r)); break;
                case 12: mem_write16(o + 2 * k, uint16_t(p.b << 8 | p.g)); break;
                case 4: mem_write16(o + 2 * k, uint16_t((p.r >> 3) << 11 | (p.g >> 2) << 5 | (p.b >> 3))); break;
                case 5:
                    if (p.a >= 224) mem_write16(o + 2 * k, uint16_t(0x8000 | (p.r >> 3) << 10 | (p.g >> 3) << 5 | (p.b >> 3)));
                    else mem_write16(o + 2 * k, uint16_t((p.a >> 5) << 12 | (p.r >> 4) << 8 | (p.g >> 4) << 4 | (p.b >> 4)));
                    break;
                case 6:
                    mem_write8(o + 2 * k, uint8_t(p.a)); mem_write8(o + 2 * k + 1, uint8_t(p.r));
                    mem_write8(o + 32 + 2 * k, uint8_t(p.g)); mem_write8(o + 32 + 2 * k + 1, uint8_t(p.b));
                    break;
                default: break;
                }
            }
        }
    invalidate_textures();
}

} // namespace

void efb_copy(uint32_t cmd) {
    if ((cmd >> 14) & 1) frame_limiter_wait();
    if (dbg.trace_at >= 0 && dbg.trace_frame < 0 && ((cmd >> 14) & 1)) {
        static const auto t0 = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= dbg.trace_at)
            dbg.trace_frame = long(counters.display_copies) + 2;
    }
    if (gpu_enabled()) { gpu_record_efb_copy(cmd); return; }
    flush_tris();
    uint32_t tl = g.bp[0x49], wh = g.bp[0x4A];
    int x0 = int(tl & 0x3FF), y0 = int((tl >> 10) & 0x3FF);
    int w = int(wh & 0x3FF) + 1, h = int((wh >> 10) & 0x3FF) + 1;
    if (tracing())
        fprintf(stdout, "[trace] EFB copy cmd=%06X %s rect=(%d,%d %dx%d) dst=%08X stride=%u fmt=%u clear=%d\n", cmd,
                (cmd >> 14) & 1 ? "XFB" : "TEX", x0, y0, w, h, (g.bp[0x4B] & 0xFFFFFF) << 5, (g.bp[0x4D] & 0x3FF) << 5,
                ((cmd >> 3) & 15) / 2 + (((cmd >> 3) & 15) & 1) * 8, (cmd >> 11) & 1);
    if (tracing()) {  // snapshot EFB color + depth at every copy of the traced frame
        static int n = 0;
        char name[128];
        snprintf(name, sizeof(name), "/tmp/claude-0/efb_%02d.ppm", n);
        if (FILE* f = fopen(name, "wb")) {
            fprintf(f, "P6\n%d %d\n255\n", EFB_W, EFB_H);
            for (uint32_t c2 : g_efb) { uint8_t rgb[3] = {uint8_t(c2), uint8_t(c2 >> 8), uint8_t(c2 >> 16)}; fwrite(rgb, 1, 3, f); }
            fclose(f);
        }
        snprintf(name, sizeof(name), "/tmp/claude-0/zbuf_%02d.ppm", n++);
        if (FILE* f = fopen(name, "wb")) {
            fprintf(f, "P5\n%d %d\n255\n", EFB_W, EFB_H);
            for (uint32_t z : g_zbuf) { uint8_t b = uint8_t(z >> 16); fwrite(&b, 1, 1, f); }
            fclose(f);
        }
        fprintf(stdout, "[trace] pe_control=%06X zmode=%06X\n", g.bp[0x43], g.bp[0x40]);
    }
    if ((cmd >> 14) & 1) {
        counters.display_copies++;
        if (dbg.stats && counters.display_copies % 90 == 0)
            fprintf(stdout, "[gx] frame %llu: tris %llu culled %llu clipped %llu offscreen %llu | px written %llu zfail %llu afail %llu | raster %.1f ms (%llu tris)\n",
                    (unsigned long long)counters.display_copies, (unsigned long long)fs.tris, (unsigned long long)fs.culled,
                    (unsigned long long)fs.clipped_away, (unsigned long long)fs.offscreen,
                    (unsigned long long)fs.written, (unsigned long long)fs.zfail, (unsigned long long)fs.afail, g_flush_ms,
                    (unsigned long long)g_flush_tris);
        g_flush_ms = 0;
        g_flush_tris = 0;
        fs = FrameStats{};
        publish_frame(g_efb, x0, y0, w, h);
    } else {
        counters.tex_copies++;
        encode_texture(cmd, x0, y0, w, h);
    }
    if ((cmd >> 11) & 1) clear_rect(x0, y0, w, h);
}

// Save states: TEV color / konst registers (set by BP writes, kept here).
void tev_regs_access(int16_t regs[4][4], int16_t konst[4][4], bool write) {
    if (write) { memcpy(g_tev_reg, regs, sizeof(g_tev_reg)); memcpy(g_konst, konst, sizeof(g_konst)); g_cfg_dirty = true; }
    else { memcpy(regs, g_tev_reg, sizeof(g_tev_reg)); memcpy(konst, g_konst, sizeof(g_konst)); }
}

bool g_hud_edges = true;
bool hud_edges() { return g_hud_edges; }

// Where a 4:3 HUD piece spanning [mn, mx] (NDC of the widened frame) starts once compressed back to its 4:3 width
// (factor k): pieces at a screen edge stay there; with `sides`, pieces left / right of the middle keep their margin
// from the left / right edge of the screen instead of the 4:3 area (health and ammo move out to the corners, as in a
// widescreen game); pieces around the middle stay centered.
float widescreen_hud_left(float mn, float mx, float k, bool sides) {
    constexpr float kEdge = 0.97f, kSide = 1.f / 3.f;
    float c = (mn + mx) * 0.5f;
    bool left = mn <= -kEdge || (sides && c < -kSide), right = mx >= kEdge || (sides && c > kSide);
    if (left) return -1.f + (mn + 1.f) / k;
    if (right) return 1.f - (1.f - mx) / k - (mx - mn) / k;
    return mn / k;  // centered 4:3 layout
}

} // namespace ssne::gx

namespace ssne {
void set_hud_edges(bool on) { gx::g_hud_edges = on; }

bool gx_get_frame(std::vector<uint32_t>& rgba, int& w, int& h, uint64_t& seq) {
    std::lock_guard<std::mutex> lk(gx::g_frame_mutex);
    if (!gx::g_frame_seq || gx::g_frame_seq == seq) return false;
    rgba = gx::g_frame;
    w = gx::g_frame_w;
    h = gx::g_frame_h;
    seq = gx::g_frame_seq;
    return true;
}

GxRenderStats gx_render_stats() {
    return {gx::counters.triangles, gx::counters.pixels, gx::counters.display_copies, gx::counters.tex_copies,
            gx::counters.textures_decoded, gx::counters.lines_skipped};
}

} // namespace ssne
