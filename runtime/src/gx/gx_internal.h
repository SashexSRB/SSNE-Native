// Internal GX state shared by the command processor, vertex pipeline, texture unit and rasterizer.
// Register layouts follow the hardware (YAGCD / libogc / Dolphin naming).
#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace ssne::gx {

constexpr int EFB_W = 640, EFB_H = 528;

struct State {
    uint32_t cp[256] = {};          // command processor: VCD/VAT, array bases/strides, matrix indices
    uint32_t xf_mem[0x1000] = {};   // XF memory: matrices (0x000), normal mtx (0x400), post-tex (0x500), lights (0x600)
    uint32_t xf_reg[0x100] = {};    // XF registers (0x1000 + n)
    uint32_t bp[256] = {};          // blitting processor / TEV / PE registers
    uint32_t bp_mask = 0xFFFFFF;
    uint8_t tmem[1 << 20] = {};     // texture memory (upper half holds TLUTs)
};

extern State g;

inline float xf_f(uint32_t addr) { float f; memcpy(&f, &g.xf_mem[addr & 0xFFF], 4); return f; }
inline float xfr_f(uint32_t reg) { float f; memcpy(&f, &g.xf_reg[reg & 0xFF], 4); return f; }

// ---- vertex pipeline output
struct Vtx {
    float x, y, z, w;        // clip space
    float sx, sy, sz;        // screen (EFB) space after divide + viewport; sz = depth 0..1
    float color[2][4];       // rasterized color channels 0..255
    float tex[8][3];         // texture coordinates (s, t, q)
};

// Primitive types (opcode & 0xF8)
enum Prim : uint8_t {
    PRIM_QUADS = 0x80, PRIM_QUADS2 = 0x88, PRIM_TRIANGLES = 0x90, PRIM_TRISTRIP = 0x98,
    PRIM_TRIFAN = 0xA0, PRIM_LINES = 0xA8, PRIM_LINESTRIP = 0xB0, PRIM_POINTS = 0xB8,
};

uint32_t vertex_size(int vat);
// Decodes `count` vertices of format `vat` from `data` and transforms them.
void decode_vertices(int vat, const uint8_t* data, uint32_t count, std::vector<Vtx>& out);
void load_indexed_xf(int array, uint32_t value);   // LOAD_INDX_A..D

// Rasterizer / pixel engine
void draw(uint8_t prim, const std::vector<Vtx>& v);
void efb_copy(uint32_t cmd);                        // BP 0x52
void load_tlut(uint32_t v);                         // BP 0x65
void invalidate_textures();
void texture_cache_clear();                          // drop every decoded texture (save state loaded)
void tev_regs_access(int16_t regs[4][4], int16_t konst[4][4], bool write);
void texture_cache_trim();                          // safe point to evict (no queued triangles)
void bp_written(uint32_t reg, uint32_t value);      // hooks for texture cache invalidation etc.
void tev_reg_write(uint32_t reg, uint32_t value);  // BP 0xE0-0xE7 (TEV color registers / konst)

// Texture unit
struct Texture : std::enable_shared_from_this<Texture> {
    int w = 0, h = 0;
    std::vector<uint32_t> rgba;  // decoded RGBA8 (R in low byte)
    std::vector<std::vector<uint32_t>> mips;  // GPU renderer: mip levels 1..n (level k is max(1, w >> k) wide)
    uint64_t id = 0;             // unique per decoded texture (GPU texture cache key)
    // GPU renderer: texture backed by an EFB copy that lives on the GPU (rgba is empty).
    int64_t efb_copy = -1;       // copy id
    uint8_t copy_fmt = 0, tex_fmt = 0;
    bool intensity = false;
};
const Texture* get_texture(int map);                // texture bound to map 0..7 (decoded, cached)
// Button prompts (gx_prompts.cpp): cache-key variant for a texture with this source fingerprint (0 = none), and
// repainting of a freshly decoded font atlas (returns the variant it was painted for).
uint32_t prompt_variant(uint32_t sample);
uint32_t prompt_apply(Texture& t, uint32_t sample);
bool prompt_candidate_size(int w, int h);
std::shared_ptr<const Texture> texture_ref(const Texture* t);  // keep a bound texture alive (GPU frames)

// ---- per-draw TEV / pixel engine snapshot (shared by the software and GPU renderers)
struct StageCfg {
    uint32_t cenv, aenv;
    int texmap, texcoord;
    bool tex_enabled;
    int ras_chan;          // 0 color0, 1 color1, 2 alpha bump, 3 normalized alpha bump, else zero
    uint32_t ind;          // IND_CMD (BP 0x10+s) when the stage's indirect path is active, else 0
    uint32_t kcsel, kasel;
    int swap_ras[4], swap_tex[4];
};

enum BlendKind { BLEND_NONE, BLEND_ALPHA, BLEND_ADD, BLEND_GENERIC };

struct DrawCfg {
    int stages;
    StageCfg st[16];
    const Texture* tex[8];
    uint32_t wrap_s[8], wrap_t[8];
    bool linear[8];
    uint8_t min_filter[8];    // TX_SETMODE0 min filter (0 near, 1/2 near+mip, 4 linear, 5/6 linear+mip)
    float su_s[8], su_t[8];   // rasterizer texcoord scale (BP 0x30+2i / 0x31+2i): coords -> texel units
    uint32_t alpha_cmp;
    bool atest_always;
    bool z_enable, z_update;
    uint32_t z_func;
    uint32_t blend;
    BlendKind blend_kind;
    uint32_t write_mask;
    bool const_alpha_en;
    int const_alpha;
    bool dst_has_alpha;
    int scissor_x0, scissor_y0, scissor_x1, scissor_y1;
    float xoff, yoff;
    int16_t tev_reg[4][4];
    int16_t konst[4][4];
    // fog (BP 0xEE-0xF2)
    uint8_t fog_sel;          // 0 off, 2 linear, 4 exp, 5 exp2, 6 backward exp, 7 backward exp2
    uint8_t fog_proj;         // 0 perspective, 1 orthographic
    float fog_a, fog_c;
    uint32_t fog_bmag, fog_bshift;
    int fog_color[3];
    // indirect texturing (GENMODE nbindstages, IREF 0x27, IND_SS 0x25/0x26, IND_MTX 0x06-0x0E)
    int nind;
    uint8_t ind_map[4], ind_coord[4], ind_ss[4], ind_ts[4];
    int16_t ind_mtx[3][6];    // ma, mb, mc, md, me, mf (s11; row s = ma mc me, row t = mb md mf)
    int8_t ind_shift[3];      // 17 - scale exponent
    int ntex;
    bool need_ras[2];
    uint8_t need_tex;   // bitmask of texcoord slots read by TEV stages
};
DrawCfg build_cfg();

// ---- GPU renderer (gx_gpu_record.cpp): when enabled, draws and EFB copies are recorded for the GPU
bool gpu_enabled();
void gpu_record_draw(uint8_t prim, const std::vector<Vtx>& v);
void gpu_record_efb_copy(uint32_t cmd);
const Texture* gpu_lookup_efb_copy(uint32_t addr, uint32_t fmt, int w, int h);  // GPU-resident EFB copy?

// Frame presentation (display copies)
void publish_frame(const uint32_t* efb, int x0, int y0, int w, int h);

struct Counters {
    uint64_t triangles = 0, pixels = 0, tex_copies = 0, display_copies = 0, textures_decoded = 0;
    uint64_t lines_skipped = 0;
};
extern Counters counters;

} // namespace ssne::gx
