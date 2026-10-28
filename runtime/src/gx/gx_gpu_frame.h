// A recorded GX frame for the GPU renderer (produced on the guest thread, executed on the render thread).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gx_internal.h"

namespace ssne::gx {

// GPU vertex processing (GpuVcfg::mode 1): the CPU reads the attributes and transforms positions to clip space
// (needed for depth snapping and the widescreen HUD); texture coordinate generation and lighting run in the vertex
// shader with these per-batch parameters, mirroring the CPU's math (gx_vertex.cpp, decode_vertices_fast).
// Mode 0: vertices transformed and lit on the CPU (draws the shader path doesn't cover, points/lines, SSNE_GPU_TL=0).
enum : uint32_t {
    VA_VCOL0 = 1, VA_VCOL1 = 2,  // vertex colors present (else 255 where a channel uses them)
    VA_GEO = 4,                  // view-space position + normalized view-space normal (lighting with lights)
    VA_RPOS = 8, VA_RNRM = 16,   // untransformed position / normal (texgen sources 0 / 1)
    VA_TC = 256,                 // << i: texcoord i (texgen source 5 + i)
};
struct GpuVcfg {
    uint32_t mode;      // 0 = CPU-transformed vertices, 1 = GPU vertex processing
    uint32_t attrs;     // VA_* bits: what each vertex carries (layout: gpu_vtx_layout)
    uint32_t ntg;       // texgens (= texcoords the fragment shader reads)
    uint32_t tg[8];     // per texgen: type | src << 3 | stq << 8 | abc1 << 9 | dual_norm << 10 | emboss source << 11
    uint32_t dual;      // dual texture transform (post matrices)
    uint32_t lchans;    // lit channels (1 or 2; channel 1 is zero otherwise)
    uint32_t lp[2][2];  // [channel][0 rgb, 1 alpha]: lit | mat_vc << 1 | amb_vc << 2 | diffuse_fn << 3 | atten << 5 |
                        // spot << 6 | light mask << 8
};
struct GpuXf {
    float tm[8][12];    // texgen matrices (rows s, t, q), type 0 texgens
    float pm[8][12];    // post matrices (dual transform)
    float mat[2][4], amb[2][4];                       // channel material / ambient register colors (0..255)
    float lpos[8][4], ldir[8][4], lcol[8][4], latt[8][8];  // lights; latt = a0 a1 a2 k0 | k1 k2
};
// Byte offsets of each attribute in a mode 1 vertex (-1: absent) and the stride.
struct GpuVtxLayout { int pos = 0, col0 = 16, col1 = 20, vpos = -1, vnrm = -1, rpos = -1, rnrm = -1, tc[8]; uint32_t stride; };
inline GpuVtxLayout gpu_vtx_layout(uint32_t attrs) {
    GpuVtxLayout l;
    int o = 24;
    if (attrs & VA_GEO) { l.vpos = o; l.vnrm = o + 12; o += 24; }
    if (attrs & VA_RPOS) { l.rpos = o; o += 12; }
    if (attrs & VA_RNRM) { l.rnrm = o; o += 12; }
    for (int i = 0; i < 8; i++) {
        l.tc[i] = (attrs & (VA_TC << i)) ? o : -1;
        if (l.tc[i] >= 0) o += 8;
    }
    l.stride = uint32_t(o);
    return l;
}

struct GpuState {
    DrawCfg cfg;       // TEV / blend / depth / scissor snapshot (padding zeroed: compared with memcmp)
    float vp[6];       // XF viewport: scale x, y, z, offset x, y, z
    uint32_t cull;     // GENMODE cull mode
    GpuVcfg vcfg;      // vertex processing (all zero in mode 0)
    GpuXf xf;
};

struct GpuCmd {
    enum Type : uint8_t { DRAW, CLEAR, COPY_TEX, COPY_XFB } type;
    // DRAW: vertex block at vtx_off (stride = 24 + 12 * ntex bytes: pos f32x4, color0/1 u8x4, texcoords f32x3),
    //       indices [idx_first, idx_first + idx_count) relative to the block
    uint32_t state, vtx_off, stride, ntex, vcount, idx_first, idx_count;
    // DRAW: some vertex lies on/beyond the near or far plane, so fragment depth may need snapping to the plane
    // (the shader then writes gl_FragDepth, which disables early depth testing: only done when needed)
    bool zsnap;
    // CLEAR / COPY_*: EFB rectangle
    int x, y, w, h;
    // CLEAR
    uint32_t color, z;
    bool cu, au, zu;
    // COPY_TEX
    bool half;
    uint64_t copy_id;
    int out_w, out_h;
    uint8_t copy_fmt;       // copy format (0-15, texture-copy numbering) and intensity flag
    bool intensity;
    bool copy_fmt_known;    // false only for frames loaded from old captures
    // COPY_TEX into part of an earlier copy's texture (base_copy_id != 0): the result is a base_w x base_h copy that
    // keeps the base copy's texels and has this copy (out_w x out_h) in rows dst_row.., as the console's memory would
    uint64_t base_copy_id;
    int base_w, base_h, dst_row;
};

// Allocator that leaves new elements uninitialized (vertex data is written right after the buffer grows).
template <class T> struct NoInitAlloc : std::allocator<T> {
    template <class U> struct rebind { using other = NoInitAlloc<U>; };
    NoInitAlloc() = default;
    template <class U> NoInitAlloc(const NoInitAlloc<U>&) {}
    template <class U> void construct(U* p) noexcept { ::new (static_cast<void*>(p)) U; }
    template <class U, class... A> void construct(U* p, A&&... a) { ::new (static_cast<void*>(p)) U(std::forward<A>(a)...); }
};

struct GpuFrame {
    uint64_t seq = 0;
    float wide = 1.0f;  // widescreen factor this frame was rendered with
    std::vector<GpuCmd> cmds;
    std::vector<GpuState> states;
    std::vector<uint8_t, NoInitAlloc<uint8_t>> vtx;
    std::vector<uint32_t> idx;
    std::vector<std::shared_ptr<const Texture>> textures;  // keeps decoded textures alive until executed
    std::unordered_set<const Texture*> kept;
};

#define SSNE_GPU_CMD_HAS_ZSNAP 1

// GPU vertex processing (recorder side): the vertex code writes a draw's vertices at gpu_vtx_reserve() (the end of
// the frame's vertex buffer), then gpu_record_vertices() batches them. vx_version identifies vcfg + xf (changes
// whenever their contents do), so unchanged parameters cost no comparison.
uint8_t* gpu_vtx_reserve(size_t bytes);
void gpu_record_vertices(uint8_t prim, uint32_t n, const GpuVcfg& vcfg, const GpuXf& xf, uint64_t vx_version, bool zsnap);
bool gpu_vertex_shading();  // GPU vertex processing on (SSNE_GPU_TL=0: off)
// Draws with GPU vertex processing; false if this draw needs the CPU path (gx_vertex.cpp).
bool gpu_draw(uint8_t prim, int vat, const uint8_t* data, uint32_t count);

std::unique_ptr<GpuFrame> gpu_take_frame();  // render thread: next recorded frame, or null
void gpu_push_frame(std::unique_ptr<GpuFrame> f);  // queue a frame for the render thread (replay tool)
void gpu_recycle_frame(std::unique_ptr<GpuFrame> f);  // render thread: an executed frame's buffers can be reused
// Regression testing (gx_gpu_capture.cpp): save submitted frames when SSNE_GPU_CAPTURE is set; load them back.
void gpu_capture_frame(const GpuFrame& f);
void gpu_capture_frame_to(const GpuFrame& f, const std::string& path);
// SSNE_GPU_TL_COMPARE (testing): record each draw with CPU (context 0) and GPU (context 1) vertex processing.
bool gpu_compare_on();
void gpu_select_ctx(int k);
std::unique_ptr<GpuFrame> gpu_load_captured_frame(const std::string& path, std::string& err);
size_t gpu_frames_queued();
bool gpu_wait_frame(int timeout_ms);  // render thread: block until a frame is queued (or the timeout)
void gpu_set_enabled(bool on);

} // namespace ssne::gx
