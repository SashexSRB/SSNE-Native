// GX command processor: consumes the command stream the game writes to the write-gather pipe
// (0xCC008000) and to display lists, tracks CP/XF/BP register state, and signals the pixel engine
// events the SDK waits for (draw-sync tokens, draw done). This is the input side of the future
// renderer: every primitive and EFB copy flows through here.
#pragma once
#include <cstdint>
#include <vector>

namespace ssne {

void gx_fifo_write(uint32_t value, int size);   // CPU write to the WG pipe (1/2/4 bytes)
// The command stream is processed on a separate GX thread (like the console's GPU, it runs in parallel with the
// game's code); SSNE_GX_SYNC=1 or gx_set_threaded(false) before the first write processes it on the writing thread.
void gx_set_threaded(bool on);
// Waits until the GX thread has processed everything written so far (save states; game-thread state that the
// command processing reads, such as the frame's widescreen factor, changing). No-op when not threaded.
void gx_sync();
uint16_t gx_pe_read16(uint32_t off);             // 0xCC001000 block
void gx_pe_write16(uint32_t off, uint16_t v);
uint16_t gx_cp_read16(uint32_t off);             // 0xCC000000 block
void gx_cp_write16(uint32_t off, uint16_t v);

struct GxStats {
    uint64_t bytes, commands, primitives, vertices, display_lists, efb_copies, draw_done, tokens;
    uint32_t last_token, unknown_opcode;
};
GxStats gx_stats();

// Latest displayed frame (from the most recent EFB->XFB copy). Returns false if nothing new since `seq`.
bool gx_get_frame(std::vector<uint32_t>& rgba, int& w, int& h, uint64_t& seq);

struct GxRenderStats { uint64_t triangles, pixels, display_copies, tex_copies, textures_decoded, lines_skipped; };
GxRenderStats gx_render_stats();

} // namespace ssne
