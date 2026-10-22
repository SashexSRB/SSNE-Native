// A recorded GX frame for the GPU renderer (produced on the guest thread, executed on the render thread).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "gx_internal.h"

namespace ssne::gx {

struct GpuState {
    DrawCfg cfg;       // TEV / blend / depth / scissor snapshot (padding zeroed: compared with memcmp)
    float vp[6];       // XF viewport: scale x, y, z, offset x, y, z
    uint32_t cull;     // GENMODE cull mode
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

struct GpuFrame {
    uint64_t seq = 0;
    float wide = 1.0f;  // widescreen factor this frame was rendered with
    std::vector<GpuCmd> cmds;
    std::vector<GpuState> states;
    std::vector<uint8_t> vtx;
    std::vector<uint32_t> idx;
    std::vector<std::shared_ptr<const Texture>> textures;  // keeps decoded textures alive until executed
    std::unordered_set<const Texture*> kept;
};

#define SSNE_GPU_CMD_HAS_ZSNAP 1

std::unique_ptr<GpuFrame> gpu_take_frame();  // render thread: next recorded frame, or null
void gpu_push_frame(std::unique_ptr<GpuFrame> f);  // queue a frame for the render thread (replay tool)
void gpu_recycle_frame(std::unique_ptr<GpuFrame> f);  // render thread: an executed frame's buffers can be reused
// Regression testing (gx_gpu_capture.cpp): save submitted frames when SSNE_GPU_CAPTURE is set; load them back.
void gpu_capture_frame(const GpuFrame& f);
std::unique_ptr<GpuFrame> gpu_load_captured_frame(const std::string& path, std::string& err);
size_t gpu_frames_queued();
bool gpu_wait_frame(int timeout_ms);  // render thread: block until a frame is queued (or the timeout)
void gpu_set_enabled(bool on);

} // namespace ssne::gx
