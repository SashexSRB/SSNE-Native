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
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
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
std::unique_ptr<GpuFrame> g_cur;
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

GpuFrame& cur() {
    if (!g_cur) {
        {
            std::lock_guard<std::mutex> lk(g_pool_mutex);
            if (!g_pool.empty()) { g_cur = std::move(g_pool.back()); g_pool.pop_back(); }
        }
        if (!g_cur) {
            g_cur = std::make_unique<GpuFrame>();
            g_cur->vtx.reserve(1 << 20);
            g_cur->idx.reserve(1 << 18);
        }
    }
    return *g_cur;
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

void submit_frame() {
    if (!g_cur) return;
    g_cur->seq = ++g_frame_seq;
    g_cur->wide = widescreen_frame_factor();
    gpu_capture_frame(*g_cur);
    std::unique_lock<std::mutex> lk(g_q_mutex);
    // Pace the game to the renderer (2 frames in flight). If nothing consumes frames (no renderer running),
    // don't stall the game: drop the oldest.
    if (!g_q_cv.wait_for(lk, std::chrono::milliseconds(g_consumer_seen ? 250 : 0), [] { return g_queue.size() < 2; })) {
        g_queue.pop_front();
        g_dropped++;
    }
    g_queue.push_back(std::move(g_cur));
    g_q_cv.notify_all();
}

} // namespace

bool gpu_enabled() { return g_enabled; }

void gpu_record_draw(uint8_t prim, const std::vector<Vtx>& v) {
    if (prim == PRIM_LINES || prim == PRIM_LINESTRIP || prim == PRIM_POINTS) { counters.lines_skipped++; return; }
    GpuFrame& f = cur();
    GpuState st;
    memset(&st, 0, sizeof(st));
    DrawCfg cfg = build_cfg();
    memcpy(&st.cfg, &cfg, sizeof(cfg));
    for (int i = 0; i < 6; i++) st.vp[i] = xfr_f(0x1A + i);
    st.cull = (g.bp[0x00] >> 14) & 3;
    int ntex = std::clamp(cfg.ntex, 0, 8);
    // Triangulate (same winding rules as the software rasterizer).
    size_t n = v.size();
    uint32_t tris = 0;
    switch (prim) {
    case PRIM_QUADS: case PRIM_QUADS2: tris = uint32_t(n / 4) * 2; break;
    case PRIM_TRIANGLES: tris = uint32_t(n / 3); break;
    case PRIM_TRISTRIP: case PRIM_TRIFAN: tris = n >= 3 ? uint32_t(n - 2) : 0; break;
    default: break;
    }
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

    // Merge into the previous batch if the state is identical.
    bool merge = false;
    if (!f.cmds.empty() && f.cmds.back().type == GpuCmd::DRAW) {
        const GpuCmd& last = f.cmds.back();
        merge = last.ntex == uint32_t(ntex) && last.zsnap == zsnap && memcmp(&f.states[last.state], &st, sizeof(st)) == 0 &&
                last.vcount + n < (1u << 30);
    }
    if (!merge) {
        for (int m = 0; m < 8; m++) keep(f, cfg.tex[m]);
        f.states.push_back(st);
        GpuCmd c{};
        c.type = GpuCmd::DRAW;
        c.state = uint32_t(f.states.size() - 1);
        c.ntex = uint32_t(ntex);
        c.stride = 16 + 8 + 12 * uint32_t(ntex);
        c.zsnap = zsnap;
        c.vtx_off = uint32_t((f.vtx.size() + 15) & ~size_t(15));
        c.idx_first = uint32_t(f.idx.size());
        f.vtx.resize(c.vtx_off);
        f.cmds.push_back(c);
    }
    GpuCmd& c = f.cmds.back();
    uint32_t base = c.vcount;
    size_t off = f.vtx.size();
    f.vtx.resize(off + n * c.stride);
    uint8_t* p = f.vtx.data() + off;
    for (size_t i = 0; i < n; i++, p += c.stride) {
        const Vtx& s = v[i];
        float pos[4] = {s.x, s.y, s.z, s.w};
        memcpy(p, pos, 16);
        for (int ch = 0; ch < 2; ch++)
            for (int k = 0; k < 4; k++)
                p[16 + ch * 4 + k] = uint8_t(std::clamp(int(s.color[ch][k] + 0.5f), 0, 255));
        if (ntex) memcpy(p + 24, s.tex, size_t(ntex) * 12);
    }
    c.vcount += uint32_t(n);
    auto tri = [&](size_t a, size_t b, size_t cc) {
        f.idx.push_back(base + uint32_t(a)); f.idx.push_back(base + uint32_t(b)); f.idx.push_back(base + uint32_t(cc));
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

void gpu_record_efb_copy(uint32_t cmd) {
    GpuFrame& f = cur();
    uint32_t tl = g.bp[0x49], wh = g.bp[0x4A];
    int x0 = int(tl & 0x3FF), y0 = int((tl >> 10) & 0x3FF);
    int w = int(wh & 0x3FF) + 1, h = int((wh >> 10) & 0x3FF) + 1;
    bool xfb = (cmd >> 14) & 1;
    GpuCmd c{};
    c.x = x0; c.y = y0; c.w = w; c.h = h;
    if (xfb) {
        counters.display_copies++;
        c.type = GpuCmd::COPY_XFB;
        f.cmds.push_back(c);
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
        c.out_w = g_copies[dst].out_w;
        c.out_h = g_copies[dst].out_h;
        f.cmds.push_back(c);
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
        f.cmds.push_back(k);
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
