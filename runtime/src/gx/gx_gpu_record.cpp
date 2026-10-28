// GPU renderer, guest side: records each frame (between display copies) as a list of commands for the
// OpenGL executor (gx_gpu.cpp), which runs on the host's render thread.
//
// Vertices arrive already transformed/lit by the XF emulation (clip-space position, two color channels,
// up to 8 texture coordinates). Draws with identical TEV/PE state are merged into one batch.
// EFB->texture copies are not encoded into guest RAM: the copy stays on the GPU (at the internal resolution)
// and a small marker is written at the destination address, so a later texture lookup at that address can be
// resolved to the GPU copy (and a CPU overwrite of the memory invalidates it).
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "gx_gpu_frame.h"
#include "gx_internal.h"
#include "ssne/gx.h"
#include "ssne/runtime.h"
#include "ssne/ppc_context.h"

namespace ssne::gx {
namespace {

bool g_enabled = false;
uint64_t g_frame_seq = 0;
uint64_t g_next_copy_id = 1;

std::mutex g_q_mutex;
std::condition_variable g_q_cv;
std::deque<std::unique_ptr<GpuFrame>> g_queue;
std::atomic<bool> g_consumer_seen{false};
uint64_t g_dropped = 0;

// Registry of GPU-resident EFB copies by destination address.
struct CopyEntry {
    uint64_t id;
    uint32_t addr, fmt;    // copy format (0-15, texture-copy numbering)
    bool intensity, half;
    int out_w, out_h;
    std::unordered_map<uint32_t, std::shared_ptr<Texture>> views;  // by texture format
};
std::unordered_map<uint32_t, CopyEntry> g_copies;
constexpr uint32_t kMarker0 = 0x5353454E, kMarker1 = 0x45464221;  // "SSNEEFB!"

// Executed frames come back from the render thread and are reused: their buffers keep their capacity, so
// recording a frame doesn't allocate (and page-fault in) a few MB of fresh memory 60 times a second.
std::mutex g_pool_mutex;
std::vector<std::unique_ptr<GpuFrame>> g_pool;

// Draw state snapshot, rebuilt only when g_cfg_dirty says its inputs changed (most draws repeat the previous
// draw's state, and building + comparing the ~1.4 KB snapshot for each of ~2,000 draws per frame was a large share
// of the game thread). R->state_index: entry of the current frame's `states` equal to R->state (none: ~0u).
// A widescreen HUD draw of the frame being recorded: its vertices (clip position first), NDC extent, and whether it
// is text (a glyph of a font atlas).
struct HudPiece { size_t off; uint32_t n, stride; float mn, mx, ymn, ymx, k; bool glyph; };
struct RecCtx {
    std::unique_ptr<GpuFrame> cur;  // frame being recorded
    std::vector<HudPiece> hud;      // its widescreen HUD draws, placed when it's submitted (place_hud)
    GpuState state;
    uint32_t state_index = ~0u;
    uint64_t state_vx = 0;  // version of state.vcfg / xf (gpu_record_vertices); 0: mode 0, zeroed
};
// SSNE_GPU_TL_COMPARE=dir:first:count (testing GPU vertex processing): every draw is recorded twice, in context 0
// with the CPU's vertex processing and in context 1 with the GPU's; display frames first.. of both are saved as
// captures in dir/cpu and dir/gpu (gpu-replay them and compare the images). Context 1's frames are displayed.
RecCtx g_rec[2];
RecCtx* R = &g_rec[0];
struct Compare { bool on = false; std::string dir; long first = 0, count = 0, seen = 0; };
Compare g_cmp = [] {
    Compare c;
    if (const char* e = getenv("SSNE_GPU_TL_COMPARE")) {
        std::string v = e;
        size_t a = v.find(':'), b = a == std::string::npos ? a : v.find(':', a + 1);
        c.on = true;
        c.dir = v.substr(0, a);
        c.first = a == std::string::npos ? 0 : atol(v.substr(a + 1).c_str());
        c.count = b == std::string::npos ? 10 : atol(v.substr(b + 1).c_str());
    }
    return c;
}();

GpuFrame& frame_of(RecCtx& r) {
    if (!r.cur) {
        {
            std::lock_guard<std::mutex> lk(g_pool_mutex);
            if (!g_pool.empty()) { r.cur = std::move(g_pool.back()); g_pool.pop_back(); }
        }
        if (!r.cur) {
            r.cur = std::make_unique<GpuFrame>();
            r.cur->vtx.reserve(1 << 20);
            r.cur->idx.reserve(1 << 18);
        }
    }
    return *r.cur;
}
GpuFrame& cur() { return frame_of(*R); }

// Copies and clears go into every recording context (both in compare mode).
void push_cmd(const GpuCmd& c) {
    if (g_cmp.on) { frame_of(g_rec[0]).cmds.push_back(c); frame_of(g_rec[1]).cmds.push_back(c); }
    else cur().cmds.push_back(c);
}

void keep(GpuFrame& f, const Texture* t) {
    if (!t) return;
    if (f.kept.insert(t).second) f.textures.push_back(texture_ref(t));
}

// Bits per texel class: copies and textures can only be reinterpreted within the same class.
int copy_bpp(uint32_t fmt) {
    switch (fmt) {
    case 0: return 4;
    case 1: case 2: case 7: case 8: case 9: case 10: return 8;
    case 3: case 4: case 5: case 11: case 12: return 16;
    case 6: return 32;
    default: return 0;
    }
}
int tex_bpp(uint32_t fmt) {
    switch (fmt) {
    case 0: return 4;
    case 1: case 2: return 8;
    case 3: case 4: case 5: return 16;
    case 6: return 32;
    default: return 0;  // palette / compressed formats aren't produced by EFB copies
    }
}

// Widescreen HUD (see widescreen_hud_left): a recorded 2D draw is noted with its extent and placed when the frame is
// submitted. Text (hints, pickup messages, the combo counter) is drawn one glyph per draw, twice (shadow, then the
// text), and animated glyph by glyph: hints fly in letter by letter from the bottom of the screen, through the HUD.
// Placing glyphs by position splits words, lines and their shadows, and grouping them by proximity drags the HUD
// along with letters passing by. So text is never moved to a side: every glyph is just scaled toward the middle, as
// in the 4:3 layout. The rest (icons, bars, frames) is placed in groups: an element and the draws right after it that
// touch or nearly touch it. Glyphs right after it that lie over it (the health and ammo numbers) follow the group, but
// never decide its place: a pickup message ("Armor +100") drawn right after the health bars and next to them had
// pulled the group to the middle. Only groups in a corner
// band (health, ammo, the NETRICSA envelope) or at the far side of the 4:3 area (the weapon bar) move out to the
// sides: the weapon wheel's slots and icons lie apart from each other around the middle and stay centered with it.
void note_hud(GpuFrame& f, size_t off, uint32_t n, uint32_t stride) {
    float k = widescreen_frame_factor();
    if (k <= 1.001f || !(g.xf_reg[0x26] & 1)) return;
    const DrawCfg& c = R->state.cfg;
    const Texture* t0 = c.stages > 0 && c.st[0].tex_enabled ? c.tex[c.st[0].texmap & 7] : nullptr;
    HudPiece h{off, n, stride, 1e30f, -1e30f, 1e30f, -1e30f, k, t0 && t0->font};
    const uint8_t* v = f.vtx.data() + off;
    for (uint32_t i = 0; i < n; i++) {
        float p[4];
        memcpy(p, v + size_t(i) * stride, 16);
        if (!(p[3] > 0)) return;
        float x = p[0] / p[3], y = p[1] / p[3];
        h.mn = std::min(h.mn, x); h.mx = std::max(h.mx, x);
        h.ymn = std::min(h.ymn, y); h.ymx = std::max(h.ymx, y);
    }
    if (h.mx - h.mn >= 1.9f) return;  // spans the screen (scene redraws, fades): left alone
    R->hud.push_back(h);
}

void place_hud(RecCtx& r) {
    if (!r.cur) { r.hud.clear(); return; }
    GpuFrame& f = *r.cur;
    const bool sides = hud_edges();
    constexpr float kGapX = 0.08f, kGapY = 0.06f;  // NDC: about a glyph's width / height at 448 lines
    constexpr float kBand = 0.55f, kOuter = 0.8f;  // 4:3 NDC: top / bottom bands, far sides
    auto place = [&](size_t from, size_t to, float mn, float nmn, float k) {
        for (size_t q = from; q < to; q++) {
            const HudPiece& p = r.hud[q];
            uint8_t* v = f.vtx.data() + p.off;
            for (uint32_t m = 0; m < p.n; m++, v += p.stride) {
                float c[4];
                memcpy(c, v, 16);
                c[0] = (nmn + (c[0] / c[3] - mn) / k) * c[3];
                memcpy(v, c, 4);
            }
        }
    };
    for (size_t i = 0; i < r.hud.size();) {
        const HudPiece& first = r.hud[i];
        const float k = first.k;
        if (first.glyph) {  // text: scaled toward the middle
            place(i, i + 1, 0.f, 0.f, k);
            i++;
            continue;
        }
        float mn = first.mn, mx = first.mx, ymn = first.ymn, ymx = first.ymx;
        size_t j = i + 1;
        for (; j < r.hud.size(); j++) {
            const HudPiece& p = r.hud[j];
            if (p.k != k || std::max(p.ymn - ymx, ymn - p.ymx) > kGapY) break;
            if (p.glyph) {  // follows the group only within its height and right after the glyph before it (the
                            // health number); it doesn't change the group's extent
                const HudPiece& q = r.hud[j - 1];
                float c = (p.mn + p.mx) * 0.5f;
                if (c < mn - kGapX || c > mx + kGapX || p.ymn < ymn - 0.01f || p.ymx > ymx + 0.01f) break;
                if (q.glyph && std::max(p.mn - q.mx, q.mn - p.mx) > kGapX) break;
                continue;
            }
            if (std::max(p.mn - mx, mn - p.mx) > kGapX) break;
            mn = std::min(mn, p.mn); mx = std::max(mx, p.mx);
            ymn = std::min(ymn, p.ymn); ymx = std::max(ymx, p.ymx);
        }
        bool corner = ymx < -kBand || ymn > kBand || mn < -kOuter || mx > kOuter;
        place(i, j, mn, widescreen_hud_left(mn, mx, k, sides && corner), k);
        i = j;
    }
    r.hud.clear();
}

void submit_frame() {
    if (g_cmp.on) {
        long n = g_cmp.seen++;
        for (int k = 0; k < 2; k++) {
            place_hud(g_rec[k]);
            GpuFrame& f = frame_of(g_rec[k]);
            f.wide = widescreen_frame_factor();
            if (n >= g_cmp.first && n < g_cmp.first + g_cmp.count) {
                char name[64];
                snprintf(name, sizeof(name), "/frame_%05ld.bin", n - g_cmp.first);
                gpu_capture_frame_to(f, g_cmp.dir + (k ? "/gpu" : "/cpu") + name);
            }
            g_rec[k].state_index = ~0u;
        }
        gpu_recycle_frame(std::move(g_rec[0].cur));
        R = &g_rec[1];
    }
    place_hud(*R);
    if (!R->cur) return;
    R->cur->seq = ++g_frame_seq;
    R->cur->wide = widescreen_frame_factor();
    gpu_capture_frame(*R->cur);
    R->state_index = ~0u;
    std::unique_lock<std::mutex> lk(g_q_mutex);
    // Pace the game to the renderer (2 frames in flight). If nothing consumes frames (no renderer running),
    // don't stall the game: drop the oldest.
    if (!g_q_cv.wait_for(lk, std::chrono::milliseconds(g_consumer_seen ? 250 : 0), [] { return g_queue.size() < 2; })) {
        g_queue.pop_front();
        g_dropped++;
    }
    g_queue.push_back(std::move(R->cur));
    g_q_cv.notify_all();
    if (g_cmp.on) R = &g_rec[0];
}

} // namespace

bool gpu_enabled() { return g_enabled; }
bool gpu_compare_on() { return g_cmp.on; }
void gpu_select_ctx(int k) { R = &g_rec[k & 1]; }

namespace {

// Brings the TEV / PE part of R->state up to date (when g_cfg_dirty says its inputs changed).
void refresh_state() {
    GpuState& st = R->state;
    if (g_cfg_dirty) {
        build_cfg(st.cfg);  // clears cfg (padding included); vp and cull follow it without padding
        for (int i = 0; i < 6; i++) st.vp[i] = xfr_f(0x1A + i);
        st.cull = (g.bp[0x00] >> 14) & 3;
        g_cfg_dirty = false;
        R->state_index = ~0u;
    }
    static const bool check = getenv("SSNE_CHECK_STATE") != nullptr;
    if (check) {  // debugging: compare the reused snapshot with a fresh one
        static GpuState fresh;
        static uint64_t checked = 0, bad = 0;
        memset(&fresh, 0, sizeof(fresh));
        build_cfg(fresh.cfg);
        for (int i = 0; i < 6; i++) fresh.vp[i] = xfr_f(0x1A + i);
        fresh.cull = (g.bp[0x00] >> 14) & 3;
        if (memcmp(&fresh, &st, offsetof(GpuState, vcfg)) && bad++ < 10)
            fprintf(stderr, "[gpu] stale draw state (draw %llu)\n", (unsigned long long)checked);
        if ((++checked & 0x3FFFF) == 0)
            fprintf(stderr, "[gpu] draw state check: %llu draws, %llu stale\n", (unsigned long long)checked, (unsigned long long)bad);
    }
}

uint32_t triangle_count(uint8_t prim, size_t n) {
    switch (prim) {
    case PRIM_QUADS: case PRIM_QUADS2: return uint32_t(n / 4) * 2;
    case PRIM_TRIANGLES: return uint32_t(n / 3);
    case PRIM_TRISTRIP: case PRIM_TRIFAN: return n >= 3 ? uint32_t(n - 2) : 0;
    default: return 0;
    }
}

// The draw command n vertices at vtx_off (stride bytes each) join: the previous one if the state is identical and
// the data follows it (`contiguous`), else a new command. Returns the index of their first vertex in the command.
uint32_t batch(GpuFrame& f, uint32_t ntex, uint32_t stride, bool zsnap, size_t vtx_off, size_t n, bool contiguous) {
    const GpuState& st = R->state;
    bool merge = false;
    if (!f.cmds.empty() && f.cmds.back().type == GpuCmd::DRAW) {
        const GpuCmd& last = f.cmds.back();
        if (R->state_index == ~0u && memcmp(&f.states[last.state], &st, sizeof(st)) == 0) R->state_index = last.state;
        merge = last.ntex == ntex && last.zsnap == zsnap && last.state == R->state_index && last.stride == stride &&
                last.vcount + n < (1u << 30) &&
                (!contiguous || size_t(last.vtx_off) + size_t(last.vcount) * last.stride == vtx_off);
    }
    if (!merge) {
        if (R->state_index == ~0u) {
            for (int m = 0; m < 8; m++) keep(f, st.cfg.tex[m]);
            f.states.push_back(st);
            R->state_index = uint32_t(f.states.size() - 1);
        }
        GpuCmd c{};
        c.type = GpuCmd::DRAW;
        c.state = R->state_index;
        c.ntex = ntex;
        c.stride = stride;
        c.zsnap = zsnap;
        c.vtx_off = uint32_t(vtx_off);
        c.idx_first = uint32_t(f.idx.size());
        f.cmds.push_back(c);
    }
    GpuCmd& c = f.cmds.back();
    uint32_t base = c.vcount;
    c.vcount += uint32_t(n);
    return base;
}

// Indices of a primitive's triangles (same winding rules as the software rasterizer), relative to `base`.
void add_indices(GpuFrame& f, uint8_t prim, size_t n, uint32_t tris, uint32_t base) {
    GpuCmd& c = f.cmds.back();
    size_t first = f.idx.size();
    f.idx.resize(first + size_t(tris) * 3);
    uint32_t* ip = f.idx.data() + first;
    auto tri = [&](size_t a, size_t b, size_t cc) {
        ip[0] = base + uint32_t(a); ip[1] = base + uint32_t(b); ip[2] = base + uint32_t(cc); ip += 3;
    };
    switch (prim) {
    case PRIM_QUADS: case PRIM_QUADS2:
        for (size_t i = 0; i + 3 < n; i += 4) { tri(i, i + 1, i + 2); tri(i, i + 2, i + 3); }
        break;
    case PRIM_TRIANGLES:
        for (size_t i = 0; i + 2 < n; i += 3) tri(i, i + 1, i + 2);
        break;
    case PRIM_TRISTRIP:
        for (size_t i = 0; i + 2 < n; i++) { if (i & 1) tri(i + 1, i, i + 2); else tri(i, i + 1, i + 2); }
        break;
    case PRIM_TRIFAN:
        for (size_t i = 1; i + 1 < n; i++) tri(0, i, i + 1);
        break;
    default: break;
    }
    c.idx_count = uint32_t(f.idx.size()) - c.idx_first;
}

} // namespace

// Mode 0: vertices transformed and lit on the CPU.
void gpu_record_draw(uint8_t prim, const std::vector<Vtx>& v) {
    if (prim == PRIM_LINES || prim == PRIM_LINESTRIP || prim == PRIM_POINTS) { counters.lines_skipped++; return; }
    GpuFrame& f = cur();
    GpuState& st = R->state;
    refresh_state();
    if (R->state_vx != 0) {  // R->state holds GPU vertex processing parameters: back to mode 0
        memset(&st.vcfg, 0, sizeof(st.vcfg));
        memset(&st.xf, 0, sizeof(st.xf));
        R->state_vx = 0;
        R->state_index = ~0u;
    }
    const DrawCfg& cfg = st.cfg;
    int ntex = std::clamp(cfg.ntex, 0, 8);
    size_t n = v.size();
    uint32_t tris = triangle_count(prim, n);
    if (!tris) return;
    counters.triangles += tris;

    // Does any vertex lie on (or beyond) the near/far plane? Only then can a fragment's depth come within the
    // shader's snapping distance of a plane (window depth is linear in screen space, so fragments of a triangle
    // whose vertices are all strictly inside stay strictly inside). Other draws skip gl_FragDepth, which keeps
    // the GPU's early depth test (and hierarchical Z) working.
    bool zsnap = false;
    {
        double zn = (double(st.vp[5]) - double(st.vp[2])) / 16777216.0, zf = double(st.vp[5]) / 16777216.0;
        double lo = std::min(zn, zf) + 1e-5, hi = std::max(zn, zf) - 1e-5;
        lo = std::max(lo, 1e-5); hi = std::min(hi, 1.0 - 1e-5);
        for (size_t i = 0; i < n && !zsnap; i++) {
            const Vtx& s = v[i];
            if (!(s.w > 1e-5f)) { zsnap = true; break; }
            double d = (double(s.z) / double(s.w) * double(st.vp[2]) + double(st.vp[5])) / 16777216.0;
            if (!(d > lo && d < hi)) zsnap = true;
        }
    }

    uint32_t stride = 16 + 8 + 12 * uint32_t(ntex);
    bool new_cmd = f.cmds.empty() || f.cmds.back().type != GpuCmd::DRAW;
    size_t off = f.vtx.size();
    // A new command starts 16-byte aligned; a merge continues right after the previous vertices.
    size_t aligned = (off + 15) & ~size_t(15);
    uint32_t base = batch(f, uint32_t(ntex), stride, zsnap, aligned, n, false);
    if (base == 0 || new_cmd) off = f.cmds.back().vtx_off;
    else off = size_t(f.cmds.back().vtx_off) + size_t(base) * stride;
    f.vtx.resize(off + n * stride);
    uint8_t* p = f.vtx.data() + off;
    for (size_t i = 0; i < n; i++, p += stride) {
        const Vtx& s = v[i];
        float pos[4] = {s.x, s.y, s.z, s.w};
        memcpy(p, pos, 16);
        for (int ch = 0; ch < 2; ch++)
            for (int k = 0; k < 4; k++)
                p[16 + ch * 4 + k] = uint8_t(std::clamp(int(s.color[ch][k] + 0.5f), 0, 255));
        if (ntex) memcpy(p + 24, s.tex, size_t(ntex) * 12);
    }
    add_indices(f, prim, n, tris, base);
    note_hud(f, off, uint32_t(n), stride);
}

uint8_t* gpu_vtx_reserve(size_t bytes) {
    GpuFrame& f = cur();
    size_t off = f.vtx.size();
    f.vtx.resize(off + bytes);
    return f.vtx.data() + off;
}

// Mode 1: the draw's vertices were written at gpu_vtx_reserve() (the end of the frame's vertex buffer).
void gpu_record_vertices(uint8_t prim, uint32_t n, const GpuVcfg& vcfg, const GpuXf& xf, uint64_t vx_version, bool zsnap) {
    GpuFrame& f = cur();
    uint32_t stride = gpu_vtx_layout(vcfg.attrs).stride;
    size_t off = f.vtx.size() - size_t(n) * stride;
    uint32_t tris = triangle_count(prim, n);
    if (!tris) { f.vtx.resize(off); return; }
    counters.triangles += tris;
    refresh_state();
    if (R->state_vx != vx_version) {
        R->state.vcfg = vcfg;
        R->state.xf = xf;
        R->state_vx = vx_version;
        R->state_index = ~0u;
    }
    uint32_t base = batch(f, vcfg.ntg, stride, zsnap, off, n, true);
    add_indices(f, prim, n, tris, base);
    note_hud(f, off, n, stride);
}

void gpu_record_efb_copy(uint32_t cmd) {
    uint32_t tl = g.bp[0x49], wh = g.bp[0x4A];
    int x0 = int(tl & 0x3FF), y0 = int((tl >> 10) & 0x3FF);
    int w = int(wh & 0x3FF) + 1, h = int((wh >> 10) & 0x3FF) + 1;
    bool xfb = (cmd >> 14) & 1;
    GpuCmd c{};
    c.x = x0; c.y = y0; c.w = w; c.h = h;
    if (xfb) {
        counters.display_copies++;
        c.type = GpuCmd::COPY_XFB;
        push_cmd(c);
    } else {
        counters.tex_copies++;
        uint32_t dst = (g.bp[0x4B] & 0xFFFFFF) << 5;
        uint32_t tpf = (cmd >> 3) & 0xF;
        uint32_t fmt = tpf / 2 + (tpf & 1) * 8;
        bool half = (cmd >> 9) & 1;
        CopyEntry e;
        e.id = g_next_copy_id++;
        e.addr = dst;
        e.fmt = fmt;
        e.intensity = (cmd >> 15) & 1;
        e.half = half;
        e.out_w = half ? w / 2 : w;
        e.out_h = half ? h / 2 : h;
        // A copy into the rows of an earlier copy's texture: cutscenes render the letterboxed band only and copy it
        // 72 rows into the full-frame scene texture, which the game then samples whole from its start. On the console
        // the copy overwrites those rows in place; here it becomes a new copy of the enclosing texture that keeps the
        // earlier copy's other rows (the renderer composites the two). Otherwise the lookup at the texture's start
        // would keep finding the last full-frame copy, and cutscenes showed that frozen frame.
        if (int bpp = copy_bpp(fmt)) {
            int tile_w = bpp == 32 || bpp == 16 ? 4 : 8, tile_h = bpp == 4 ? 8 : 4;
            uint32_t row_bytes = uint32_t(tile_h * ((e.out_w + tile_w - 1) / tile_w * tile_w) * bpp / 8);
            for (auto& [addr, b] : g_copies) {
                if (addr >= dst || b.fmt != fmt || b.intensity != e.intensity || b.out_w != e.out_w) continue;
                uint32_t off = dst - addr;
                if (off % row_bytes) continue;
                int row = int(off / row_bytes) * tile_h;
                if (row + e.out_h > b.out_h) continue;
                if (mem_read32(addr) != kMarker0 || mem_read32(addr + 4) != kMarker1 ||
                    mem_read32(addr + 8) != uint32_t(b.id >> 32) || mem_read32(addr + 12) != uint32_t(b.id)) continue;
                c.base_copy_id = b.id;
                c.base_w = b.out_w;
                c.base_h = b.out_h;
                c.dst_row = row;
                e.addr = addr;
                e.out_w = b.out_w;
                e.out_h = b.out_h;
                break;
            }
        }
        if (c.base_copy_id) {
            c.out_w = half ? w / 2 : w;  // the copied band (the composite's size is base_w x base_h)
            c.out_h = half ? h / 2 : h;
            dst = e.addr;
        }
        g_copies[dst] = std::move(e);
        mem_write32(dst, kMarker0);
        mem_write32(dst + 4, kMarker1);
        mem_write32(dst + 8, uint32_t(g_copies[dst].id >> 32));
        mem_write32(dst + 12, uint32_t(g_copies[dst].id));
        c.type = GpuCmd::COPY_TEX;
        c.half = half;
        c.copy_id = g_copies[dst].id;
        c.copy_fmt = uint8_t(fmt);
        c.intensity = (cmd >> 15) & 1;
        c.copy_fmt_known = true;
        if (!c.base_copy_id) {
            c.out_w = g_copies[dst].out_w;
            c.out_h = g_copies[dst].out_h;
        }
        push_cmd(c);
        invalidate_textures();
    }
    if ((cmd >> 11) & 1) {  // clear the copied rectangle
        GpuCmd k{};
        k.type = GpuCmd::CLEAR;
        k.x = x0; k.y = y0; k.w = w; k.h = h;
        uint32_t ar = g.bp[0x4F], gb = g.bp[0x50];
        k.color = (ar & 0xFF) | ((gb >> 8) & 0xFF) << 8 | (gb & 0xFF) << 16 | ((ar >> 8) & 0xFF) << 24;
        k.z = g.bp[0x51] & 0xFFFFFF;
        uint32_t bm = g.bp[0x41];
        k.cu = bm & (1 << 3);
        k.au = bm & (1 << 4);
        k.zu = (g.bp[0x40] >> 4) & 1;
        push_cmd(k);
    }
    if (xfb) {
        submit_frame();
        texture_cache_trim();
    }
}

const Texture* gpu_lookup_efb_copy(uint32_t addr, uint32_t fmt, int w, int h) {
    auto it = g_copies.find(addr);
    if (it == g_copies.end()) return nullptr;
    CopyEntry& e = it->second;
    if (mem_read32(addr) != kMarker0 || mem_read32(addr + 4) != kMarker1 ||
        mem_read32(addr + 8) != uint32_t(e.id >> 32) || mem_read32(addr + 12) != uint32_t(e.id)) {
        g_copies.erase(it);  // memory was reused by the game
        return nullptr;
    }
    if (e.out_w != w || e.out_h != h || copy_bpp(e.fmt) != tex_bpp(fmt) || !tex_bpp(fmt)) {
        static int warned = 0;
        if (warned++ < 8)
            fprintf(stdout, "[gpu] EFB copy at %08X (fmt %u, %dx%d) sampled as fmt %u %dx%d: not supported\n", addr, e.fmt,
                    e.out_w, e.out_h, fmt, w, h);
        return nullptr;
    }
    auto& view = e.views[fmt];
    if (!view) {
        view = std::make_shared<Texture>();
        view->w = w;
        view->h = h;
        view->efb_copy = int64_t(e.id);
        view->copy_fmt = uint8_t(e.fmt);
        view->tex_fmt = uint8_t(fmt);
        view->intensity = e.intensity;
    }
    return view.get();
}

// ---- consumer side (render thread)

std::unique_ptr<GpuFrame> gpu_take_frame() {
    std::lock_guard<std::mutex> lk(g_q_mutex);
    g_consumer_seen = true;
    if (g_queue.empty()) return nullptr;
    auto f = std::move(g_queue.front());
    g_queue.pop_front();
    g_q_cv.notify_all();
    return f;
}

void gpu_recycle_frame(std::unique_ptr<GpuFrame> f) {
    if (!f) return;
    f->cmds.clear();
    f->states.clear();
    f->vtx.clear();
    f->idx.clear();
    f->textures.clear();  // drops the frame's texture references here, on the render thread
    f->kept.clear();
    f->seq = 0;
    f->wide = 1.0f;
    std::lock_guard<std::mutex> lk(g_pool_mutex);
    if (g_pool.size() < 4) g_pool.push_back(std::move(f));
}

void gpu_push_frame(std::unique_ptr<GpuFrame> f) {
    std::lock_guard<std::mutex> lk(g_q_mutex);
    f->seq = ++g_frame_seq;
    g_queue.push_back(std::move(f));
    g_q_cv.notify_all();
}

bool gpu_wait_frame(int timeout_ms) {
    std::unique_lock<std::mutex> lk(g_q_mutex);
    return g_q_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [] { return !g_queue.empty(); });
}

size_t gpu_frames_queued() {
    std::lock_guard<std::mutex> lk(g_q_mutex);
    return g_queue.size();
}

void gpu_set_enabled(bool on) { g_enabled = on; }

} // namespace ssne::gx

namespace ssne {
uint64_t gpu_dropped_frames() {
    std::lock_guard<std::mutex> lk(gx::g_q_mutex);
    return gx::g_dropped;
}
} // namespace ssne

namespace ssne::gx {

} // namespace ssne::gx
