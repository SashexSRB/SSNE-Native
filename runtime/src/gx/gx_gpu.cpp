// GPU renderer, render-thread side: executes recorded GX frames with OpenGL 3.3 core.
//
// The EFB is a framebuffer at N x 640x528 (internal resolution scale N). Each draw batch uses a GLSL program
// generated from its TEV configuration; the TEV math is done in integers exactly like the software renderer
// (gx_raster.cpp), which remains the reference. EFB->texture copies are rendered into textures at the internal
// resolution and later sampled directly (optionally reinterpreted into the texture format the game reads them
// as). Display (XFB) copies produce the output texture the port presents.
//
// Row convention: GL framebuffer/texture row y == EFB row y (top at row 0), so presenting flips vertically.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "gx_gl.h"
#include "gx_gpu_frame.h"
#include "ssne/gx_gpu.h"
#include "ssne/runtime.h"

namespace ssne::gl {
#define SSNE_GL_DEFINE(ret, name, args) PFN_##name name = nullptr;
SSNE_GL_FUNCS(SSNE_GL_DEFINE)
SSNE_GL_OPTIONAL_FUNCS(SSNE_GL_DEFINE)
#undef SSNE_GL_DEFINE

bool load(void* (*getproc)(const char*), const char** missing) {
#define SSNE_GL_LOAD(ret, name, args)                                         \
    name = reinterpret_cast<PFN_##name>(getproc("gl" #name));                  \
    if (!name) { if (missing) *missing = "gl" #name; return false; }
    SSNE_GL_FUNCS(SSNE_GL_LOAD)
#undef SSNE_GL_LOAD
#define SSNE_GL_LOAD_OPT(ret, name, args) name = reinterpret_cast<PFN_##name>(getproc("gl" #name));
    SSNE_GL_OPTIONAL_FUNCS(SSNE_GL_LOAD_OPT)
#undef SSNE_GL_LOAD_OPT
    return true;
}
} // namespace ssne::gl

namespace ssne {

using namespace gl;
using gx::DrawCfg;
using gx::GpuCmd;
using gx::GpuFrame;
using gx::GpuState;
using gx::StageCfg;
using gx::Texture;

namespace {

constexpr int EFB_W = gx::EFB_W, EFB_H = gx::EFB_H;

bool g_ready = false;
bool g_gpu_mode = false;
int g_scale = 2;
float g_wide = 1.0f;  // widescreen: the EFB is rendered this much wider (anamorphic), the display stretches it back
// EFB pixel coordinate -> internal framebuffer coordinate
inline int SX(int x) { return int(std::lround(double(x) * g_scale * g_wide)); }
inline int SY(int y) { return y * g_scale; }
std::string g_info;
bool g_trace = getenv("SSNE_GPU_TRACE") != nullptr;

GLuint g_efb_fbo = 0, g_efb_color = 0, g_efb_depth = 0;
GLuint g_out_fbo = 0, g_out_tex = 0;
// Anti-aliasing. MSAA: the EFB is multisampled and resolved (into g_resolve_*) whenever the game copies from it.
// FXAA: a post pass over the display image. Smooth downscaling: the final image gets mipmaps so a display smaller
// than the internal resolution (supersampling) is filtered properly instead of skipping pixels.
int g_msaa = 1, g_msaa_max = 1;
bool g_fxaa = false, g_smooth = false;
GLuint g_resolve_fbo = 0, g_resolve_tex = 0;
GLuint g_fxaa_fbo = 0, g_fxaa_tex = 0, g_fxaa_prog = 0;
GLint g_fxaa_u_rcp = -1;
int g_fxaa_w = 0, g_fxaa_h = 0;
GLuint g_final_tex = 0, g_final_fbo = 0;  // what the port presents (output, or its FXAA version)
int g_out_w = 0, g_out_h = 0;
uint64_t g_out_seq = 0;
float g_out_wide = 1.0f;
GLuint g_vao = 0, g_vbo = 0, g_ibo = 0;
GLuint g_white = 0;
std::unordered_map<uint32_t, GLuint> g_samplers;  // by (min filter, mag filter, wrap s, wrap t)
GLuint g_tmp_fbo = 0;

// ------------------------------------------------------------------ GL state cache
// Consecutive batches mostly share blend/depth/cull/texture state; skipping redundant GL calls saves driver CPU
// time (noticeable on slower CPUs and some drivers). Only the render thread's own draws go through the cache;
// it is invalidated at the start of every frame because the port's UI draws with the same context in between.
struct StateCache {
    int8_t caps[9];
    GLenum blend_eq[2], blend_f[4], logic, depth_func, cull, front;
    int color_mask, depth_mask;
    GLint scissor[4];
    GLuint program;
    int active;
    GLuint tex[8], smp[8];
    int8_t attrib[11];
} g_sc;

void sc_invalidate() {
    memset(&g_sc, 0xFF, sizeof(g_sc));  // "unknown" everywhere: the next call of each kind always reaches GL
}
int cap_index(GLenum cap) {
    switch (cap) {
    case SCISSOR_TEST: return 0; case BLEND: return 1; case COLOR_LOGIC_OP: return 2; case DEPTH_TEST: return 3;
    case CULL_FACE: return 4; case CLIP_DISTANCE0: return 5; case CLIP_DISTANCE1: return 6;
    case CLIP_DISTANCE0 + 2: return 7; case DEPTH_CLAMP: return 8; default: return -1;
    }
}
void sc_cap(GLenum cap, bool on) {
    int i = cap_index(cap);
    if (i >= 0 && g_sc.caps[i] == int8_t(on)) return;
    if (on) Enable(cap); else Disable(cap);
    if (i >= 0) g_sc.caps[i] = int8_t(on);
}
void sc_blend_eq(GLenum rgb, GLenum a) {
    if (g_sc.blend_eq[0] == rgb && g_sc.blend_eq[1] == a) return;
    BlendEquationSeparate(rgb, a);
    g_sc.blend_eq[0] = rgb; g_sc.blend_eq[1] = a;
}
void sc_blend_func(GLenum sr, GLenum dr, GLenum sa, GLenum da) {
    if (g_sc.blend_f[0] == sr && g_sc.blend_f[1] == dr && g_sc.blend_f[2] == sa && g_sc.blend_f[3] == da) return;
    BlendFuncSeparate(sr, dr, sa, da);
    g_sc.blend_f[0] = sr; g_sc.blend_f[1] = dr; g_sc.blend_f[2] = sa; g_sc.blend_f[3] = da;
}
void sc_logic(GLenum op) { if (g_sc.logic != op) { LogicOp(op); g_sc.logic = op; } }
void sc_color_mask(bool r, bool g, bool b, bool a) {
    int m = int(r) | int(g) << 1 | int(b) << 2 | int(a) << 3;
    if (g_sc.color_mask == m) return;
    ColorMask(r, g, b, a);
    g_sc.color_mask = m;
}
void sc_depth_mask(bool on) { if (g_sc.depth_mask != int(on)) { DepthMask(on); g_sc.depth_mask = int(on); } }
void sc_depth_func(GLenum f) { if (g_sc.depth_func != f) { DepthFunc(f); g_sc.depth_func = f; } }
void sc_cull(GLenum mode) { if (g_sc.cull != mode) { CullFace(mode); g_sc.cull = mode; } }
void sc_front(GLenum mode) { if (g_sc.front != mode) { FrontFace(mode); g_sc.front = mode; } }
void sc_scissor(GLint x, GLint y, GLint w, GLint h) {
    if (g_sc.scissor[0] == x && g_sc.scissor[1] == y && g_sc.scissor[2] == w && g_sc.scissor[3] == h) return;
    Scissor(x, y, w, h);
    g_sc.scissor[0] = x; g_sc.scissor[1] = y; g_sc.scissor[2] = w; g_sc.scissor[3] = h;
}
void sc_program(GLuint p) { if (g_sc.program != p) { UseProgram(p); g_sc.program = p; } }
void sc_active(int unit) { if (g_sc.active != unit) { ActiveTexture(TEXTURE0 + GLenum(unit)); g_sc.active = unit; } }
void sc_texture(int unit, GLuint t) {
    if (g_sc.tex[unit] == t) return;
    sc_active(unit);
    BindTexture(TEXTURE_2D, t);
    g_sc.tex[unit] = t;
}
void sc_sampler(int unit, GLuint smp) { if (g_sc.smp[unit] != smp) { BindSampler(GLuint(unit), smp); g_sc.smp[unit] = smp; } }
void sc_attrib(int i, bool on) {
    if (g_sc.attrib[i] == int8_t(on)) return;
    if (on) EnableVertexAttribArray(GLuint(i)); else DisableVertexAttribArray(GLuint(i));
    g_sc.attrib[i] = int8_t(on);
}
// Deleted texture names are unbound by GL (and may be reused by the next GenTextures).
void sc_delete_texture(GLuint t) {
    DeleteTextures(1, &t);
    for (GLuint& b : g_sc.tex) if (b == t) b = 0;
}

// What the render thread is doing (read by the port's watchdog to report hangs).
std::atomic<const char*> g_phase{"idle"};
std::atomic<uint64_t> g_phase_frame{0};
std::atomic<uint32_t> g_phase_cmd{0}, g_phase_ncmds{0};
double g_max_frame_ms = 0;
uint64_t g_frame_no = 0;
// GPU time per executed frame (TIME_ELAPSED queries, read back a few frames later so the CPU never waits).
constexpr int kTimerQueries = 4;
GLuint g_tq[kTimerQueries] = {};
int g_tq_first = 0, g_tq_pending = 0;
bool g_tq_ok = false;
double g_gpu_ms_total = 0;
int g_gpu_frames = 0;

void poll_timer_queries() {
    while (g_tq_ok && g_tq_pending > 0) {
        GLuint q = g_tq[g_tq_first];
        GLint avail = 0;
        GetQueryObjectiv(q, QUERY_RESULT_AVAILABLE, &avail);
        if (!avail) break;
        uint64_t ns = 0;
        GetQueryObjectui64v(q, QUERY_RESULT, &ns);
        if (ns > 0 && ns < 2000000000ull) {  // ignore nonsense from drivers with broken timers
            g_gpu_ms_total += double(ns) * 1e-6;
            g_gpu_frames++;
        }
        g_tq_first = (g_tq_first + 1) % kTimerQueries;
        g_tq_pending--;
    }
}
GpuStats g_stats{};
// SSNE_GPU_PROFILE=1 (debugging, slow): glFinish around every command to attribute GPU time by command kind.
const bool g_prof = getenv("SSNE_GPU_PROFILE") != nullptr;
double g_prof_ms[6] = {};  // draw, clear, copy, display copy, reinterpret, draw (depth-writing)
uint64_t g_prof_n[6] = {};
double prof_now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ------------------------------------------------------------------ helpers
GLuint compile(GLenum type, const std::string& src, std::string& log) {
    GLuint s = CreateShader(type);
    const char* p = src.c_str();
    ShaderSource(s, 1, &p, nullptr);
    CompileShader(s);
    GLint ok = 0;
    GetShaderiv(s, COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[4096];
        GLsizei n = 0;
        GetShaderInfoLog(s, sizeof(buf), &n, buf);
        log.assign(buf, size_t(n));
        DeleteShader(s);
        return 0;
    }
    return s;
}

GLuint link(const std::string& vs, const std::string& fs, bool retrievable = false) {
    std::string log;
    GLuint v = compile(VERTEX_SHADER, vs, log);
    if (!v) { log_message("[gpu] vertex shader error:\n" + log + "\n" + vs); return 0; }
    GLuint f = compile(FRAGMENT_SHADER, fs, log);
    if (!f) { log_message("[gpu] fragment shader error:\n" + log + "\n" + fs); DeleteShader(v); return 0; }
    GLuint p = CreateProgram();
    AttachShader(p, v);
    AttachShader(p, f);
    if (retrievable) ProgramParameteri(p, PROGRAM_BINARY_RETRIEVABLE_HINT, 1);
    LinkProgram(p);
    DeleteShader(v);
    DeleteShader(f);
    GLint ok = 0;
    GetProgramiv(p, LINK_STATUS, &ok);
    if (!ok) {
        char buf[4096];
        GLsizei n = 0;
        GetProgramInfoLog(p, sizeof(buf), &n, buf);
        log_message("[gpu] shader link error: " + std::string(buf, size_t(n)));
        DeleteProgram(p);
        return 0;
    }
    return p;
}

GLuint make_tex(int w, int h, GLenum ifmt, GLenum fmt, GLenum type, const void* data, bool linear = false) {
    GLuint t = 0;
    GenTextures(1, &t);
    ActiveTexture(TEXTURE0);  // always bound for real (a new name may equal a stale cached one)
    BindTexture(TEXTURE_2D, t);
    g_sc.active = 0;
    g_sc.tex[0] = t;
    TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, linear ? LINEAR : NEAREST);
    TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, linear ? LINEAR : NEAREST);
    TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
    TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
    TexParameteri(TEXTURE_2D, TEXTURE_MAX_LEVEL, 0);
    PixelStorei(UNPACK_ALIGNMENT, 4);
    TexImage2D(TEXTURE_2D, 0, GLint(ifmt), w, h, 0, fmt, type, data);
    return t;
}

GLuint fbo_for(GLuint color, GLuint depth = 0) {
    GLuint f = 0;
    GenFramebuffers(1, &f);
    BindFramebuffer(FRAMEBUFFER, f);
    FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, color, 0);
    if (depth) FramebufferTexture2D(FRAMEBUFFER, DEPTH_STENCIL_ATTACHMENT, TEXTURE_2D, depth, 0);
    if (CheckFramebufferStatus(FRAMEBUFFER) != FRAMEBUFFER_COMPLETE) log_message("[gpu] framebuffer incomplete");
    return f;
}

// Fixed-function state for internal full-screen passes (copies, reinterpretation). Clip distances must be off:
// these shaders don't write gl_ClipDistance, and enabled-but-unwritten clip distances are undefined (NVIDIA
// clips everything, Mesa doesn't).
void pass_state() {
    sc_cap(SCISSOR_TEST, false); sc_cap(BLEND, false); sc_cap(COLOR_LOGIC_OP, false); sc_cap(DEPTH_TEST, false);
    sc_cap(CULL_FACE, false);
    sc_cap(CLIP_DISTANCE0, false); sc_cap(CLIP_DISTANCE1, false); sc_cap(GLenum(CLIP_DISTANCE0 + 2), false);
    sc_color_mask(1, 1, 1, 1);
    sc_depth_mask(0);
}

// ------------------------------------------------------------------ frame capture (debugging)
std::string g_dump_dir;
std::atomic<bool> g_dump_req{false};
bool g_dumping = false;
int g_dump_n = 0;
FILE* g_dump_log = nullptr;

void write_bmp(const std::string& path, int w, int h, const std::vector<uint32_t>& px) {  // px row 0 = top
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    int row = (w * 3 + 3) & ~3;
    uint32_t size = 54 + uint32_t(row) * uint32_t(h);
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; i++) hdr[o + i] = uint8_t(v >> (8 * i)); };
    put32(2, size); put32(10, 54); put32(14, 40); put32(18, uint32_t(w)); put32(22, uint32_t(h));
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> line(size_t(row), 0);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint32_t c = px[size_t(y) * w + x];
            line[size_t(x) * 3] = uint8_t(c >> 16); line[size_t(x) * 3 + 1] = uint8_t(c >> 8); line[size_t(x) * 3 + 2] = uint8_t(c);
        }
        fwrite(line.data(), 1, line.size(), f);
    }
    fclose(f);
}

void dump_fbo(GLuint fbo, int w, int h, const std::string& tag) {
    if (!g_dumping) return;
    std::vector<uint32_t> px(size_t(w) * h);
    BindFramebuffer(FRAMEBUFFER, fbo);
    PixelStorei(PACK_ALIGNMENT, 4);
    ReadPixels(0, 0, w, h, RGBA, UNSIGNED_BYTE, px.data());
    char name[64];
    snprintf(name, sizeof(name), "%03d_", g_dump_n++);
    write_bmp(g_dump_dir + "/" + name + tag + ".bmp", w, h, px);
    if (g_dump_log) {
        uint64_t sum = 0; uint32_t nonblack = 0;
        for (uint32_t c : px) { sum += (c & 255) + ((c >> 8) & 255) + ((c >> 16) & 255); nonblack += (c & 0xFFFFFF) != 0; }
        fprintf(g_dump_log, "  -> %s%s.bmp %dx%d avg %.1f nonblack %.1f%%\n", name, tag.c_str(), w, h,
                double(sum) / (3.0 * double(px.size())), 100.0 * nonblack / double(px.size()));
    }
}

void dump_depth(const std::string& tag) {
    if (!g_dumping) return;
    int w = SX(EFB_W), h = SY(EFB_H);
    std::vector<float> d(size_t(w) * h);
    BindFramebuffer(FRAMEBUFFER, g_efb_fbo);
    ReadPixels(0, 0, w, h, DEPTH_COMPONENT, FLOAT, d.data());
    std::vector<uint32_t> px(d.size());
    float mn = 1, mx = 0;
    for (size_t i = 0; i < d.size(); i++) {
        uint32_t v = uint32_t(std::clamp(d[i], 0.f, 1.f) * 255.f);
        px[i] = v | v << 8 | v << 16;
        mn = std::min(mn, d[i]); mx = std::max(mx, d[i]);
    }
    char name[64];
    snprintf(name, sizeof(name), "%03d_", g_dump_n++);
    write_bmp(g_dump_dir + "/" + name + tag + ".bmp", w, h, px);
    if (g_dump_log) fprintf(g_dump_log, "  -> %s%s.bmp depth min %.6f max %.6f\n", name, tag.c_str(), mn, mx);
}

// ------------------------------------------------------------------ samplers
// Anisotropic filtering (EXT/ARB_texture_filter_anisotropic, core in GL 4.6): 1 = off. Applied only to the game's
// mipmapped textures with linear filtering (floors and walls seen at grazing angles), never to EFB copies or
// point-filtered textures, so screen effects and pixel-exact 2D art are unchanged.
int g_aniso = 1, g_aniso_max = 1;

GLuint sampler_for(uint32_t minf, bool mag_linear, uint32_t ws, uint32_t wt, int aniso = 1) {
    uint32_t key = minf | uint32_t(mag_linear) << 3 | ws << 4 | wt << 6 | uint32_t(aniso) << 8;
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) return it->second;
    static const GLenum wraps[3] = {CLAMP_TO_EDGE, REPEAT, MIRRORED_REPEAT};
    GLenum min;
    switch (minf) {
    case 0: min = NEAREST; break;
    case 1: min = NEAREST_MIPMAP_NEAREST; break;
    case 2: min = NEAREST_MIPMAP_LINEAR; break;
    case 5: min = LINEAR_MIPMAP_NEAREST; break;
    case 6: min = LINEAR_MIPMAP_LINEAR; break;
    default: min = LINEAR; break;
    }
    GLuint smp = 0;
    GenSamplers(1, &smp);
    SamplerParameteri(smp, TEXTURE_MIN_FILTER, GLint(min));
    SamplerParameteri(smp, TEXTURE_MAG_FILTER, mag_linear ? LINEAR : NEAREST);
    SamplerParameteri(smp, TEXTURE_WRAP_S, GLint(wraps[ws]));
    SamplerParameteri(smp, TEXTURE_WRAP_T, GLint(wraps[wt]));
    if (aniso > 1 && SamplerParameterf) SamplerParameterf(smp, TEXTURE_MAX_ANISOTROPY, float(aniso));
    g_samplers[key] = smp;
    return smp;
}

// ------------------------------------------------------------------ render-target pool
// EFB copies and reinterpreted views are full-size render targets created every frame by the game's effects.
// They are recycled (same size -> same texture + framebuffer) instead of being allocated and deleted each frame,
// which makes some drivers (NVIDIA) synchronize.
struct RT { GLuint tex = 0, fbo = 0; int w = 0, h = 0; };
std::vector<RT> g_rt_free;

RT rt_acquire(int w, int h) {
    for (size_t i = 0; i < g_rt_free.size(); i++)
        if (g_rt_free[i].w == w && g_rt_free[i].h == h) {
            RT r = g_rt_free[i];
            g_rt_free.erase(g_rt_free.begin() + long(i));
            return r;
        }
    RT r;
    r.w = w; r.h = h;
    r.tex = make_tex(w, h, RGBA8, RGBA, UNSIGNED_BYTE, nullptr);
    r.fbo = fbo_for(r.tex);
    g_stats.render_targets++;
    return r;
}
void rt_release(const RT& r) {
    g_rt_free.push_back(r);
    if (g_rt_free.size() > 24) {  // cap the pool
        RT old = g_rt_free.front();
        g_rt_free.erase(g_rt_free.begin());
        DeleteFramebuffers(1, &old.fbo);
        sc_delete_texture(old.tex);
    }
}

// ------------------------------------------------------------------ textures
struct GlTex { GLuint tex; int w, h; uint64_t last_used; };
std::unordered_map<uint64_t, GlTex> g_textures;     // decoded RAM textures by Texture::id
struct PooledTex { RT rt; uint64_t last_used; RT conv{}; int conv_fmt = -1; };
// EFB copies by copy id: raw RGBA8 at internal res, plus (conv) the copy already encoded/decoded as the texture
// format it is normally read as, made in the same pass (saves a full-size reinterpretation pass per copy).
std::unordered_map<uint64_t, PooledTex> g_copies;
std::unordered_map<uint64_t, PooledTex> g_views;    // reinterpreted copies: key = copy id * 16 + tex format

GLuint g_copy_prog = 0, g_reint_prog = 0, g_copy_mrt_prog = 0, g_mrt_fbo = 0;
GLint g_copy_mrt_u_rect = -1, g_copy_mrt_u_mode = -1;

// The texture format a game reads an EFB copy format back as (same bits): I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8 for
// the matching copy formats, I8 for the single-channel copies (A8, R8, G8, B8), IA8 for RG8/GB8. -1: none.
int natural_tex_fmt(uint32_t copy_fmt) {
    static const int8_t m[16] = {0, 1, 2, 3, 4, 5, 6, 1, 1, 1, 1, 3, 3, -1, -1, -1};
    return copy_fmt < 16 ? m[copy_fmt] : -1;
}
void release_copy(PooledTex& p) {
    rt_release(p.rt);
    if (p.conv.tex) rt_release(p.conv);
}
GLint g_copy_u_rect = -1, g_copy_u_half = -1;
GLint g_reint_u_mode = -1;

GLuint make_ms_tex(int w, int h, GLenum ifmt) {
    GLuint t = 0;
    GenTextures(1, &t);
    BindTexture(TEXTURE_2D_MULTISAMPLE, t);
    TexImage2DMultisample(TEXTURE_2D_MULTISAMPLE, g_msaa, ifmt, w, h, 1);
    BindTexture(TEXTURE_2D_MULTISAMPLE, 0);
    return t;
}

void create_efb() {
    int w = SX(EFB_W), h = SY(EFB_H);
    if (g_efb_fbo) { DeleteFramebuffers(1, &g_efb_fbo); sc_delete_texture(g_efb_color); sc_delete_texture(g_efb_depth); }
    if (g_resolve_fbo) { DeleteFramebuffers(1, &g_resolve_fbo); sc_delete_texture(g_resolve_tex); g_resolve_fbo = g_resolve_tex = 0; }
    if (g_msaa > 1) {
        g_efb_color = make_ms_tex(w, h, RGBA8);
        g_efb_depth = make_ms_tex(w, h, DEPTH24_STENCIL8);
        GenFramebuffers(1, &g_efb_fbo);
        BindFramebuffer(FRAMEBUFFER, g_efb_fbo);
        FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D_MULTISAMPLE, g_efb_color, 0);
        FramebufferTexture2D(FRAMEBUFFER, DEPTH_STENCIL_ATTACHMENT, TEXTURE_2D_MULTISAMPLE, g_efb_depth, 0);
        if (CheckFramebufferStatus(FRAMEBUFFER) != FRAMEBUFFER_COMPLETE) {
            log_message("[gpu] multisampled EFB not supported by the driver: anti-aliasing (MSAA) off");
            DeleteFramebuffers(1, &g_efb_fbo); sc_delete_texture(g_efb_color); sc_delete_texture(g_efb_depth);
            g_msaa = 1;
            create_efb();
            return;
        }
        g_resolve_tex = make_tex(w, h, RGBA8, RGBA, UNSIGNED_BYTE, nullptr, true);
        g_resolve_fbo = fbo_for(g_resolve_tex);
    } else {
        g_efb_color = make_tex(w, h, RGBA8, RGBA, UNSIGNED_BYTE, nullptr, true);
        g_efb_depth = make_tex(w, h, DEPTH24_STENCIL8, DEPTH_STENCIL, UNSIGNED_INT_24_8, nullptr);
        g_efb_fbo = fbo_for(g_efb_color, g_efb_depth);
    }
    BindFramebuffer(FRAMEBUFFER, g_efb_fbo);
    ClearColor(0, 0, 0, 0);
    ClearDepth(1.0);
    sc_cap(SCISSOR_TEST, false);
    sc_color_mask(1, 1, 1, 1);
    sc_depth_mask(1);
    Clear(COLOR_BUFFER_BIT | DEPTH_BUFFER_BIT);
}

// The EFB color as a plain texture for reading (copies): with MSAA, resolve the given internal-pixel rectangle.
GLuint efb_color_for_read(int x0, int y0, int x1, int y1) {
    if (g_msaa <= 1) return g_efb_color;
    pass_state();  // the scissor test also clips blits
    BindFramebuffer(READ_FRAMEBUFFER, g_efb_fbo);
    BindFramebuffer(DRAW_FRAMEBUFFER, g_resolve_fbo);
    BlitFramebuffer(x0, y0, x1, y1, x0, y0, x1, y1, COLOR_BUFFER_BIT, NEAREST);
    return g_resolve_tex;
}

GLuint texture_for(const Texture* t) {
    if (!t || t->w == 0) return g_white;
    if (t->efb_copy >= 0) {
        uint64_t key = uint64_t(t->efb_copy) * 16 + t->tex_fmt;
        auto it = g_views.find(key);
        if (it != g_views.end()) { it->second.last_used = g_frame_no; return it->second.rt.tex; }
        auto ct = g_copies.find(uint64_t(t->efb_copy));
        if (ct == g_copies.end()) return g_white;
        // Already converted by the copy pass, or a format whose reinterpretation is the identity (RGBA8 as RGBA8).
        if (ct->second.conv.tex && ct->second.conv_fmt == int(t->tex_fmt)) { ct->second.last_used = g_frame_no; return ct->second.conv.tex; }
        if (t->copy_fmt == 6 && t->tex_fmt == 6) { ct->second.last_used = g_frame_no; return ct->second.rt.tex; }
        // Reinterpret: encode each texel as the copy format, decode it as the texture format.
        g_phase = "reinterpret";
        double prof_t0 = 0;
        if (g_prof) { Finish(); prof_t0 = prof_now(); }
        RT out = rt_acquire(ct->second.rt.w, ct->second.rt.h);
        BindFramebuffer(FRAMEBUFFER, out.fbo);
        Viewport(0, 0, out.w, out.h);
        pass_state();
        sc_program(g_reint_prog);
        GLint mode[4] = {t->copy_fmt, t->tex_fmt, t->intensity ? 1 : 0, 0};
        Uniform4iv(g_reint_u_mode, 1, mode);
        sc_texture(0, ct->second.rt.tex);
        sc_sampler(0, 0);
        DrawArrays(TRIANGLES, 0, 3);
        BindFramebuffer(FRAMEBUFFER, g_efb_fbo);
        Viewport(0, 0, SX(EFB_W), SY(EFB_H));
        g_views[key] = {out, g_frame_no};
        if (g_dumping) {
            if (g_dump_log) fprintf(g_dump_log, "  reinterpret copy %lld fmt %u -> tex fmt %u (intensity %d)\n", (long long)t->efb_copy,
                                    t->copy_fmt, t->tex_fmt, t->intensity);
            dump_fbo(out.fbo, out.w, out.h, "reinterpreted");
            BindFramebuffer(FRAMEBUFFER, g_efb_fbo);
        }
        g_stats.reinterpretations++;
        if (g_prof) { Finish(); g_prof_ms[4] += prof_now() - prof_t0; g_prof_n[4]++; g_prof_ms[0] -= prof_now() - prof_t0; }
        g_phase = "draw";
        return out.tex;
    }
    auto it = g_textures.find(t->id);
    if (it != g_textures.end()) { it->second.last_used = g_frame_no; return it->second.tex; }
    const char* prev_phase = g_phase;
    g_phase = "texture upload";
    GLuint tex = make_tex(t->w, t->h, RGBA8, RGBA, UNSIGNED_BYTE, t->rgba.data());
    g_phase = prev_phase;
    if (!t->mips.empty()) {
        for (size_t k = 0; k < t->mips.size(); k++)
            TexImage2D(TEXTURE_2D, GLint(k + 1), GLint(RGBA8), std::max(1, t->w >> (k + 1)), std::max(1, t->h >> (k + 1)), 0,
                       RGBA, UNSIGNED_BYTE, t->mips[k].data());
        TexParameteri(TEXTURE_2D, TEXTURE_MAX_LEVEL, GLint(t->mips.size()));
    }
    g_textures[t->id] = {tex, t->w, t->h, g_frame_no};
    g_stats.uploads++;
    return tex;
}

void evict() {
    auto sweep = [](auto& m, uint64_t age) {
        for (auto it = m.begin(); it != m.end();) {
            if (g_frame_no - it->second.last_used > age) { sc_delete_texture(it->second.tex); it = m.erase(it); }
            else ++it;
        }
    };
    sweep(g_textures, 600);
    auto sweep_rt = [](auto& m, uint64_t age) {
        for (auto it = m.begin(); it != m.end();) {
            if (g_frame_no - it->second.last_used > age) {
                rt_release(it->second.rt);
                if (it->second.conv.tex) rt_release(it->second.conv);
                it = m.erase(it);
            } else ++it;
        }
    };
    sweep_rt(g_views, 3);
    sweep_rt(g_copies, 6);
}

// ------------------------------------------------------------------ TEV program generation
// Last values uploaded to each uniform of a program (GL keeps uniforms per program): unchanged ones are skipped.
struct UniformShadow {
    float vp0[4], vp1[4], efb[2], sts[32], calpha, texsz[16], su[16], fog[4], zr[2];
    GLint reg[16], konst[16], aref[4], indmtx[24], fogc[4];
};
struct Program {
    GLuint prog = 0;
    GLint u_vp0, u_vp1, u_efb, u_reg, u_konst, u_aref, u_stscale, u_calpha, u_zrange, u_fog, u_fogcolor;
    GLint u_indmtx, u_texsz, u_su;
    std::shared_ptr<UniformShadow> shadow;  // null until the first upload
};
template <size_t N, class T> bool changed(T (&cache)[N], const T* v, bool force) {
    if (!force && memcmp(cache, v, sizeof(cache)) == 0) return false;
    memcpy(cache, v, sizeof(cache));
    return true;
}
std::unordered_map<std::string, Program> g_programs;

const char* kFixedK[8] = {"255", "223", "191", "159", "128", "96", "64", "32"};

std::string konst_color(uint32_t sel) {
    if (sel < 8) return std::string("ivec3(") + kFixedK[sel] + ")";
    if (sel >= 12 && sel <= 15) return "u_konst[" + std::to_string(sel - 12) + "].rgb";
    if (sel >= 16) return "ivec3(u_konst[" + std::to_string((sel - 16) & 3) + "][" + std::to_string((sel - 16) >> 2) + "])";
    return "ivec3(0)";
}
std::string konst_alpha(uint32_t sel) {
    if (sel < 8) return kFixedK[sel];
    if (sel >= 12 && sel <= 15) return "u_konst[" + std::to_string(sel - 12) + "].a";
    if (sel >= 16) return "u_konst[" + std::to_string((sel - 16) & 3) + "][" + std::to_string((sel - 16) >> 2) + "]";
    return "0";
}

std::string color_in(uint32_t sel, const StageCfg& st) {
    switch (sel) {
    case 0: return "R[0].rgb"; case 1: return "R[0].aaa";
    case 2: return "R[1].rgb"; case 3: return "R[1].aaa";
    case 4: return "R[2].rgb"; case 5: return "R[2].aaa";
    case 6: return "R[3].rgb"; case 7: return "R[3].aaa";
    case 8: return "tx.rgb"; case 9: return "tx.aaa";
    case 10: return "rc.rgb"; case 11: return "rc.aaa";
    case 12: return "ivec3(255)"; case 13: return "ivec3(128)";
    case 14: return konst_color(st.kcsel);
    default: return "ivec3(0)";
    }
}
std::string alpha_in(uint32_t sel, const StageCfg& st) {
    switch (sel) {
    case 0: return "R[0].a"; case 1: return "R[1].a"; case 2: return "R[2].a"; case 3: return "R[3].a";
    case 4: return "tx.a"; case 5: return "rc.a"; case 6: return konst_alpha(st.kasel);
    default: return "0";
    }
}

const char* kComp[8] = {"false", "(a < r)", "(a == r)", "(a <= r)", "(a > r)", "(a != r)", "(a >= r)", "true"};

std::string gen_vs(int ntex) {
    std::string s = "#version 330 core\n"
                    "layout(location=0) in vec4 a_pos;\nlayout(location=1) in vec4 a_col0;\nlayout(location=2) in vec4 a_col1;\n";
    for (int i = 0; i < ntex; i++) s += "layout(location=" + std::to_string(3 + i) + ") in vec3 a_tex" + std::to_string(i) + ";\n";
    s += "uniform vec4 u_vp0, u_vp1;\nuniform vec2 u_efb;\nout vec4 v_col0, v_col1;\n";
    for (int i = 0; i < ntex; i++) s += "out vec3 v_tex" + std::to_string(i) + ";\n";
    s += "void main() {\n"
         "  vec4 p = a_pos;\n"
         "  float px = p.x * u_vp0.x + u_vp1.x * p.w;\n"
         "  float py = p.y * u_vp0.y + u_vp1.y * p.w;\n"
         "  float pz = (p.z * u_vp0.z + u_vp1.z * p.w) * (1.0 / 16777216.0);\n"
         "  gl_Position = vec4(px * 2.0 / u_efb.x - p.w, py * 2.0 / u_efb.y - p.w, pz * 2.0 - p.w, p.w);\n"
         "  // GX clips z < -w and z > 0; geometry exactly on a plane (common for full-screen quads) must survive\n"
         "  gl_ClipDistance[0] = p.z + p.w + abs(p.w) * 1e-5;\n  gl_ClipDistance[1] = -p.z + abs(p.w) * 1e-5;\n"
         "  gl_ClipDistance[2] = p.w - 1e-5;\n"
         "  v_col0 = a_col0 * 255.0;\n  v_col1 = a_col1 * 255.0;\n";
    for (int i = 0; i < ntex; i++) s += "  v_tex" + std::to_string(i) + " = a_tex" + std::to_string(i) + ";\n";
    s += "}\n";
    return s;
}

std::string gen_fs(const DrawCfg& c, int ntex, bool zsnap) {
    std::string s = "#version 330 core\n";
    s += "in vec4 v_col0, v_col1;\n";
    for (int i = 0; i < ntex; i++) s += "in vec3 v_tex" + std::to_string(i) + ";\n";
    s += "uniform sampler2D u_samp[8];\nuniform ivec4 u_reg[4];\nuniform ivec4 u_konst[4];\nuniform ivec4 u_aref;\n"
         "uniform vec2 u_stscale[16];\nuniform float u_calpha;\nuniform vec2 u_zrange;\n"
         "uniform vec4 u_fog;   // a, c, b magnitude, b shift\nuniform ivec4 u_fogcolor;\n"
         "uniform ivec4 u_indmtx[6];  // per matrix: (ma, mc, me, shift), (mb, md, mf, 0)\n"
         "uniform vec2 u_texsz[8];    // texture size per map (game texels)\nuniform vec2 u_su[8];  // SU scale per texcoord\n"
         "layout(location=0, index=0) out vec4 o_col;\nlayout(location=0, index=1) out vec4 o_src1;\n"
         "void main() {\n"
         "  ivec4 R[4];\n  R[0] = u_reg[0]; R[1] = u_reg[1]; R[2] = u_reg[2]; R[3] = u_reg[3];\n"
         "  ivec4 ras0 = ivec4(floor(v_col0 + 0.5)), ras1 = ivec4(floor(v_col1 + 0.5));\n"
         "  float zsnap = gl_FragCoord.z;\n";
    if (zsnap)  // only for draws touching a clip plane (see gx_gpu_record.cpp): writing depth disables early z
        s += "  // Depth on the near/far clip planes is snapped exactly to the plane, so geometry lying on a plane (screen\n"
             "  // quads) and geometry clipped by it compare equal, like on the console.\n"
             "  if (abs(zsnap - u_zrange.x) < 4e-6) zsnap = u_zrange.x; else if (abs(zsnap - u_zrange.y) < 4e-6) zsnap = u_zrange.y;\n"
             "  zsnap = clamp(zsnap, 0.0, 1.0);\n"
             "  gl_FragDepth = zsnap;\n";
    s += "  ivec4 tx, rc;\n";
    const char* ch = "rgba";
    auto fixuv = [&](int coord) {  // fixed-point (1/128 texel) texture coordinate, as the hardware computes it
        if (coord >= ntex) return std::string("ivec2(0)");
        std::string v = "v_tex" + std::to_string(coord), n = std::to_string(coord);
        return "ivec2(" + v + ".xy / (" + v + ".z != 0.0 ? " + v + ".z : 1.0) * u_su[" + n + "] * 128.0)";
    };
    if (c.nind) {
        s += "  ivec3 iind[4]; ivec2 tevc = ivec2(0); int abump = 0;\n";
        for (int i = 0; i < c.nind; i++) {
            std::string m = std::to_string(c.ind_map[i]);
            s += "  { ivec2 u = " + fixuv(c.ind_coord[i]) + "; u = ivec2(u.x >> " + std::to_string(c.ind_ss[i]) + ", u.y >> " +
                 std::to_string(c.ind_ts[i]) + ");\n    ivec4 r = ivec4(floor(texture(u_samp[" + m + "], vec2(u) / 128.0 / u_texsz[" + m +
                 "]) * 255.0 + 0.5));\n    iind[" + std::to_string(i) + "] = ivec3(r.a, r.b, r.g); }\n";
        }
    }
    for (int si = 0; si < c.stages; si++) {
        const StageCfg& st = c.st[si];
        s += "  // stage " + std::to_string(si) + "\n";
        if (st.ind) {
            uint32_t cmd = st.ind;
            uint32_t bt = cmd & 3, fmt = (cmd >> 2) & 3, bias = (cmd >> 4) & 7, bs = (cmd >> 7) & 3, mid = (cmd >> 9) & 15;
            uint32_t sw = (cmd >> 13) & 7, tw = (cmd >> 16) & 7, fb = (cmd >> 20) & 1;
            static const char* bump_mask[4] = {"248", "224", "240", "248"};
            static const char* fmt_mask[4] = {"255", "31", "15", "7"};
            s += "  { ivec3 crd = " + (int(bt) < c.nind ? "iind[" + std::to_string(bt) + "]" : std::string("ivec3(0)")) + ";\n";
            if (bs) s += std::string("    abump = crd.") + "xyz"[bs - 1] + " & " + bump_mask[fmt] + ";\n";
            s += std::string("    crd &= ") + fmt_mask[fmt] + ";\n";
            for (int k = 0; k < 3; k++)
                if (bias & (1u << k)) s += std::string("    crd.") + "xyz"[k] + (fmt == 0 ? " -= 128;\n" : " += 1;\n");
            s += "    ivec2 uv = " + fixuv(st.texcoord) + ";\n    ivec2 tr = ivec2(0);\n";
            int m = -1;
            if (mid >= 1 && mid <= 3) {
                m = int(mid) - 1;
                std::string a = "u_indmtx[" + std::to_string(2 * m) + "]", b = "u_indmtx[" + std::to_string(2 * m + 1) + "]";
                s += "    tr = ivec2(" + a + ".x * crd.x + " + a + ".y * crd.y + " + a + ".z * crd.z, " + b + ".x * crd.x + " + b +
                     ".y * crd.y + " + b + ".z * crd.z) >> 3;\n";
            } else if (mid >= 5 && mid <= 7) {
                m = int(mid) - 5;
                s += "    tr = (uv * crd.x) >> 8;\n";
            } else if (mid >= 9 && mid <= 11) {
                m = int(mid) - 9;
                s += "    tr = (uv * crd.y) >> 8;\n";
            }
            if (m >= 0) {
                std::string sh = "u_indmtx[" + std::to_string(2 * m) + "].w";
                s += "    tr = " + sh + " >= 0 ? (tr >> " + sh + ") : (tr << -" + sh + ");\n";
            }
            auto wrap = [](const std::string& v, uint32_t mode) -> std::string {
                static const char* sz[6] = {"", "32767", "16383", "8191", "4095", "2047"};
                if (mode == 0 || mode == 7) return v;
                if (mode == 6) return "0";
                return "(" + v + " & " + sz[mode] + ")";
            };
            s += "    ivec2 w = ivec2(" + wrap("uv.x", sw) + ", " + wrap("uv.y", tw) + ") + tr;\n";
            s += fb ? "    tevc += w; }\n" : "    tevc = w; }\n";
        } else if (c.nind) {
            s += "  tevc = " + fixuv(st.texcoord) + ";\n";
        }
        if (st.tex_enabled && st.ind) {
            std::string m = std::to_string(st.texmap);
            s += "  { ivec4 raw = ivec4(floor(texture(u_samp[" + m + "], vec2(tevc) / 128.0 / u_texsz[" + m + "]) * 255.0 + 0.5));\n"
                 "    tx = ivec4(raw." + ch[st.swap_tex[0]] + ", raw." + ch[st.swap_tex[1]] + ", raw." + ch[st.swap_tex[2]] +
                 ", raw." + ch[st.swap_tex[3]] + "); }\n";
        } else if (st.tex_enabled) {
            std::string tcv = st.texcoord < ntex ? "v_tex" + std::to_string(st.texcoord) : "vec3(0.0)";
            s += "  { vec3 t = " + tcv + "; float q = t.z != 0.0 ? t.z : 1.0;\n"
                 "    ivec4 raw = ivec4(floor(texture(u_samp[" + std::to_string(st.texmap) + "], t.xy / q * u_stscale[" +
                 std::to_string(si) + "]) * 255.0 + 0.5));\n"
                 "    tx = ivec4(raw." + ch[st.swap_tex[0]] + ", raw." + ch[st.swap_tex[1]] + ", raw." + ch[st.swap_tex[2]] +
                 ", raw." + ch[st.swap_tex[3]] + "); }\n";
        } else {
            s += "  tx = ivec4(255);\n";
        }
        if (st.ras_chan >= 2) {
            s += st.ras_chan == 2 ? "  rc = ivec4(abump);\n" : "  rc = ivec4(abump | (abump >> 5));\n";
        } else if (st.ras_chan >= 0) {
            std::string r = st.ras_chan == 0 ? "ras0" : "ras1";
            s += "  rc = ivec4(" + r + "." + ch[st.swap_ras[0]] + ", " + r + "." + ch[st.swap_ras[1]] + ", " + r + "." +
                 ch[st.swap_ras[2]] + ", " + r + "." + ch[st.swap_ras[3]] + ");\n";
        } else {
            s += "  rc = ivec4(0);\n";
        }
        // color
        uint32_t ce = st.cenv, ae = st.aenv;
        {
            uint32_t sd = ce & 15, sc = (ce >> 4) & 15, sb = (ce >> 8) & 15, sa = (ce >> 12) & 15;
            uint32_t bias = (ce >> 16) & 3, op = (ce >> 18) & 1, clampf = (ce >> 19) & 1, scale = (ce >> 20) & 3;
            uint32_t dest = (ce >> 22) & 3;
            s += "  { ivec3 a = " + color_in(sa, st) + " & 255, b = " + color_in(sb, st) + " & 255, cc = " + color_in(sc, st) +
                 " & 255, d = " + color_in(sd, st) + ";\n    ivec3 v;\n";
            if (bias == 3) {
                uint32_t mode = (scale << 1) | op;
                switch (mode) {
                case 0: s += "    bool cnd = a.r > b.r; v = d + (cnd ? cc : ivec3(0));\n"; break;
                case 1: s += "    bool cnd = a.r == b.r; v = d + (cnd ? cc : ivec3(0));\n"; break;
                case 2: s += "    bool cnd = (a.g * 256 + a.r) > (b.g * 256 + b.r); v = d + (cnd ? cc : ivec3(0));\n"; break;
                case 3: s += "    bool cnd = (a.g * 256 + a.r) == (b.g * 256 + b.r); v = d + (cnd ? cc : ivec3(0));\n"; break;
                case 4: s += "    bool cnd = (a.b * 65536 + a.g * 256 + a.r) > (b.b * 65536 + b.g * 256 + b.r); v = d + (cnd ? cc : ivec3(0));\n"; break;
                case 5: s += "    bool cnd = (a.b * 65536 + a.g * 256 + a.r) == (b.b * 65536 + b.g * 256 + b.r); v = d + (cnd ? cc : ivec3(0));\n"; break;
                case 6: s += "    v = d + ivec3(greaterThan(a, b)) * cc;\n"; break;
                default: s += "    v = d + ivec3(equal(a, b)) * cc;\n"; break;
                }
            } else {
                s += "    ivec3 c2 = cc + (cc >> 7);\n";
                s += std::string("    ivec3 lrp = ((a << 8) + (b - a) * c2 + ") + (op ? "127" : "128") + ") >> 8;\n";
                s += std::string("    v = d ") + (op ? "- lrp" : "+ lrp") + ";\n";
                if (bias == 1) s += "    v += 128;\n"; else if (bias == 2) s += "    v -= 128;\n";
                if (scale == 1) s += "    v *= 2;\n"; else if (scale == 2) s += "    v *= 4;\n"; else if (scale == 3) s += "    v >>= 1;\n";
            }
            s += clampf ? "    v = clamp(v, 0, 255);\n" : "    v = clamp(v, -1024, 1023);\n";
            s += "    ivec3 cres = v;\n";
            // alpha
            uint32_t ad = (ae >> 4) & 7, ac = (ae >> 7) & 7, ab = (ae >> 10) & 7, aa = (ae >> 13) & 7;
            uint32_t abias = (ae >> 16) & 3, aop = (ae >> 18) & 1, aclamp = (ae >> 19) & 1, ascale = (ae >> 20) & 3;
            uint32_t adest = (ae >> 22) & 3;
            s += "    int aa = " + alpha_in(aa, st) + " & 255, ab = " + alpha_in(ab, st) + " & 255, ac = " + alpha_in(ac, st) +
                 " & 255, ad = " + alpha_in(ad, st) + ";\n    int av;\n";
            if (abias == 3) {
                uint32_t mode = (ascale << 1) | aop;
                s += (mode & 1) ? "    av = ad + ((aa == ab) ? ac : 0);\n" : "    av = ad + ((aa > ab) ? ac : 0);\n";
            } else {
                s += "    int ac2 = ac + (ac >> 7);\n";
                s += std::string("    int alrp = ((aa << 8) + (ab - aa) * ac2 + ") + (aop ? "127" : "128") + ") >> 8;\n";
                s += std::string("    av = ad ") + (aop ? "- alrp" : "+ alrp") + ";\n";
                if (abias == 1) s += "    av += 128;\n"; else if (abias == 2) s += "    av -= 128;\n";
                if (ascale == 1) s += "    av *= 2;\n"; else if (ascale == 2) s += "    av *= 4;\n"; else if (ascale == 3) s += "    av >>= 1;\n";
            }
            s += aclamp ? "    av = clamp(av, 0, 255);\n" : "    av = clamp(av, -1024, 1023);\n";
            s += "    R[" + std::to_string(dest) + "].rgb = cres;\n";
            s += "    R[" + std::to_string(adest) + "].a = av; }\n";
        }
    }
    s += "  ivec4 o = clamp(R[0], 0, 255);\n";
    if (!c.atest_always) {
        uint32_t ac = c.alpha_cmp;
        uint32_t f0 = (ac >> 16) & 7, f1 = (ac >> 19) & 7, logic = (ac >> 22) & 3;
        std::string c0 = kComp[f0], c1 = kComp[f1];
        auto sub = [](std::string e, const char* ref) {
            for (size_t p; (p = e.find(" r)")) != std::string::npos;) e.replace(p, 3, std::string(" ") + ref + ")");
            for (size_t p; (p = e.find("(a ")) != std::string::npos;) e.replace(p, 3, "(o.a ");
            return e;
        };
        std::string r0 = sub(c0, "u_aref.x"), r1 = sub(c1, "u_aref.y");
        const char* ops[4] = {"&&", "||", "!=", "=="};
        s += "  if (!((" + r0 + ") " + ops[logic] + " (" + r1 + "))) discard;\n";
    }
    if (c.fog_sel >= 2) {  // GX fog (same math as the software renderer)
        s += "  { int zi = int(zsnap * 16777215.0 + 0.5);\n";
        s += c.fog_proj == 0 ? "    float den = u_fog.z - float(zi >> int(u_fog.w));\n    float ze = den != 0.0 ? (u_fog.x * 16777216.0) / den : 0.0;\n"
                             : "    float ze = u_fog.x * float(zi) / 16777216.0;\n";
        s += "    float f = clamp(ze - u_fog.y, 0.0, 1.0);\n";
        switch (c.fog_sel) {
        case 4: s += "    f = 1.0 - exp2(-8.0 * f);\n"; break;
        case 5: s += "    f = 1.0 - exp2(-8.0 * f * f);\n"; break;
        case 6: s += "    f = exp2(-8.0 * (1.0 - f));\n"; break;
        case 7: s += "    f = exp2(-8.0 * (1.0 - f) * (1.0 - f));\n"; break;
        default: break;
        }
        s += "    int ifog = int(floor(f * 256.0 + 0.5));\n    o.rgb = (o.rgb * (256 - ifog) + u_fogcolor.rgb * ifog) >> 8; }\n";
    }
    s += c.const_alpha_en ? "  o_col = vec4(vec3(o.rgb) / 255.0, u_calpha);\n" : "  o_col = vec4(o) / 255.0;\n";
    s += "  o_src1 = vec4(float(o.a) / 255.0);\n}\n";
    return s;
}

void program_key(std::string& k, const DrawCfg& c, int ntex, bool zsnap) {
    k.clear();
    auto put = [&](uint32_t v) { k.append(reinterpret_cast<const char*>(&v), 4); };
    put(uint32_t(c.stages)); put(uint32_t(ntex));
    for (int i = 0; i < c.stages; i++) {
        const StageCfg& st = c.st[i];
        put(st.cenv); put(st.aenv);
        put(uint32_t(st.texmap) | uint32_t(st.texcoord) << 4 | uint32_t(st.tex_enabled) << 8 | uint32_t(st.ras_chan + 1) << 9);
        put(st.kcsel | st.kasel << 8);
        uint32_t sw = 0;
        for (int j = 0; j < 4; j++) sw |= uint32_t(st.swap_ras[j]) << (j * 2) | uint32_t(st.swap_tex[j]) << (8 + j * 2);
        put(sw);
        put(st.ind);
    }
    put(uint32_t(c.nind));
    for (int i = 0; i < c.nind; i++)
        put(uint32_t(c.ind_map[i]) | uint32_t(c.ind_coord[i]) << 4 | uint32_t(c.ind_ss[i]) << 8 | uint32_t(c.ind_ts[i]) << 12);
    put(c.atest_always ? 0xFFFFFFFFu : (c.alpha_cmp >> 16));
    put(c.const_alpha_en);
    put(uint32_t(c.fog_sel) | uint32_t(c.fog_proj) << 8 | uint32_t(zsnap) << 16);
}

// ------------------------------------------------------------------ shader cache
// Compiling a TEV program takes tens of milliseconds on some drivers, and the game meets new TEV setups mid-level
// (a visible hitch each time). Linked programs are saved as driver binaries (ARB_get_program_binary) under
// <exe dir>/shadercache, named by a hash of the GLSL source plus the driver identity, and loaded instead of
// compiled on later runs. A binary the driver rejects (driver update) is simply recompiled and replaced.
std::string g_cache_dir;
uint64_t g_driver_hash = 0;
bool g_cache_ok = false;

uint64_t fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

std::string cache_file(uint64_t h) {
    char name[40];
    snprintf(name, sizeof(name), "/%016llx.bin", (unsigned long long)h);
    return g_cache_dir + name;
}

GLuint load_cached_program(uint64_t h) {
    if (!g_cache_ok) return 0;
    FILE* f = fopen(cache_file(h).c_str(), "rb");
    if (!f) return 0;
    char magic[8];
    uint64_t drv = 0;
    uint32_t fmt = 0, len = 0;
    std::vector<uint8_t> bin;
    bool ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "SSNESHD1", 8) && fread(&drv, 8, 1, f) == 1 && drv == g_driver_hash &&
              fread(&fmt, 4, 1, f) == 1 && fread(&len, 4, 1, f) == 1 && len > 0 && len < (64u << 20);
    if (ok) { bin.resize(len); ok = fread(bin.data(), 1, len, f) == len; }
    fclose(f);
    if (!ok) return 0;
    GLuint p = CreateProgram();
    ProgramBinary(p, GLenum(fmt), bin.data(), GLsizei(len));
    GLint linked = 0;
    GetProgramiv(p, LINK_STATUS, &linked);
    if (!linked) { DeleteProgram(p); return 0; }
    return p;
}

void save_program(GLuint p, uint64_t h) {
    if (!g_cache_ok) return;
    GLint len = 0;
    GetProgramiv(p, PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0) return;
    std::vector<uint8_t> bin(static_cast<size_t>(len));
    GLenum fmt = 0;
    GLsizei got = 0;
    GetProgramBinary(p, len, &got, &fmt, bin.data());
    if (got <= 0) return;
    std::string path = cache_file(h), tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    uint32_t f32 = fmt, l32 = uint32_t(got);
    bool ok = fwrite("SSNESHD1", 1, 8, f) == 8 && fwrite(&g_driver_hash, 8, 1, f) == 1 && fwrite(&f32, 4, 1, f) == 1 &&
              fwrite(&l32, 4, 1, f) == 1 && fwrite(bin.data(), 1, size_t(got), f) == size_t(got);
    ok = (fclose(f) == 0) && ok;
    if (ok) { std::remove(path.c_str()); ok = std::rename(tmp.c_str(), path.c_str()) == 0; }
    if (!ok) std::remove(tmp.c_str());
}

void init_shader_cache() {
    g_cache_ok = false;
    if (g_cache_dir.empty() || !GetProgramBinary || !ProgramBinary || !ProgramParameteri) return;
    GLint formats = 0;
    GetIntegerv(NUM_PROGRAM_BINARY_FORMATS, &formats);
    if (formats <= 0) return;
    std::string id;
    for (GLenum e : {VENDOR, RENDERER, VERSION}) {
        const GLubyte* v = GetString(e);
        id += v ? reinterpret_cast<const char*>(v) : "?";
        id += '|';
    }
    g_driver_hash = fnv1a(id.data(), id.size());
    g_cache_ok = true;
}

Program* program_for(const DrawCfg& c, int ntex, bool zsnap) {
    static std::string key;  // render thread only; reused so a lookup doesn't allocate
    program_key(key, c, ntex, zsnap);
    auto it = g_programs.find(key);
    if (it != g_programs.end()) return it->second.prog ? &it->second : nullptr;
    Program p;
    const char* prev_phase = g_phase;
    g_phase = "compile shader";
    auto t0 = std::chrono::steady_clock::now();
    std::string vs = gen_vs(ntex), fs = gen_fs(c, ntex, zsnap);
    uint64_t h = fnv1a(fs.data(), fs.size(), fnv1a(vs.data(), vs.size()));
    p.prog = load_cached_program(h);
    if (p.prog) g_stats.cached_programs++;
    else {
        p.prog = link(vs, fs, g_cache_ok);
        if (p.prog) save_program(p.prog, h);
    }
    g_stats.shader_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_stats.programs++;
    if (p.prog) {
        sc_program(p.prog);
        GLint units[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        Uniform1iv(GetUniformLocation(p.prog, "u_samp"), 8, units);
        p.u_vp0 = GetUniformLocation(p.prog, "u_vp0");
        p.u_vp1 = GetUniformLocation(p.prog, "u_vp1");
        p.u_efb = GetUniformLocation(p.prog, "u_efb");
        p.u_reg = GetUniformLocation(p.prog, "u_reg");
        p.u_konst = GetUniformLocation(p.prog, "u_konst");
        p.u_aref = GetUniformLocation(p.prog, "u_aref");
        p.u_stscale = GetUniformLocation(p.prog, "u_stscale");
        p.u_calpha = GetUniformLocation(p.prog, "u_calpha");
        p.u_zrange = GetUniformLocation(p.prog, "u_zrange");
        p.u_fog = GetUniformLocation(p.prog, "u_fog");
        p.u_fogcolor = GetUniformLocation(p.prog, "u_fogcolor");
        p.u_indmtx = GetUniformLocation(p.prog, "u_indmtx");
        p.u_texsz = GetUniformLocation(p.prog, "u_texsz");
        p.u_su = GetUniformLocation(p.prog, "u_su");
    }
    g_phase = prev_phase;
    auto& slot = g_programs[key];
    slot = p;
    return slot.prog ? &slot : nullptr;
}

// ------------------------------------------------------------------ fixed programs (copies, reinterpretation)
const char* kFullscreenVS =
    "#version 330 core\n"
    "void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";

const char* kCopyFS =
    "#version 330 core\n"
    "uniform sampler2D u_src;\nuniform ivec4 u_rect;  // source origin (internal pixels), half\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  ivec2 d = ivec2(gl_FragCoord.xy);\n"
    "  if (u_rect.z != 0) {\n"
    "    // 2x2 box filter over EFB pixels: at internal scale s, average the block's four quadrant centers\n"
    "    vec2 base = vec2(u_rect.xy) + vec2(d) * 2.0;\n"
    "    vec2 sz = vec2(textureSize(u_src, 0));\n"
    "    vec2 c = base + 1.0;\n"
    "    o = texture(u_src, c / sz);\n"
    "  } else {\n"
    "    o = texelFetch(u_src, u_rect.xy + d, 0);\n"
    "  }\n"
    "}\n";

// Encode an EFB texel (RGBA8) as copy format `copy`, then decode it as texture format `tex`, exactly like the
// CPU copy encoder and texture decoder do.
#define SSNE_REINT_FUNC \
    "int e4(int v) { return v * 17; }\nint e5(int v) { return (v << 3) | (v >> 2); }\nint e6(int v) { return (v << 2) | (v >> 4); }\n" \
    "int e3(int v) { return (v << 5) | (v << 2) | (v >> 1); }\n" \
    "vec4 reint(vec4 texel, ivec4 u_mode) {\n" \
    "  ivec4 p = ivec4(floor(texel * 255.0 + 0.5));\n" \
    "  int I = u_mode.z != 0 ? clamp((66 * p.r + 129 * p.g + 25 * p.b + 128) / 256 + 16, 0, 255) : p.r;\n" \
    "  int v = 0; ivec4 rgba8 = ivec4(0);\n" \
    "  switch (u_mode.x) {\n" \
    "    case 0: v = I >> 4; break;\n" \
    "    case 1: case 8: v = I; break;\n" \
    "    case 2: v = ((p.a >> 4) << 4) | (I >> 4); break;\n" \
    "    case 7: v = p.a; break;\n" \
    "    case 9: v = p.g; break;\n" \
    "    case 10: v = p.b; break;\n" \
    "    case 3: v = (p.a << 8) | I; break;\n" \
    "    case 11: v = (p.g << 8) | p.r; break;\n" \
    "    case 12: v = (p.b << 8) | p.g; break;\n" \
    "    case 4: v = ((p.r >> 3) << 11) | ((p.g >> 2) << 5) | (p.b >> 3); break;\n" \
    "    case 5: v = p.a >= 224 ? (0x8000 | ((p.r >> 3) << 10) | ((p.g >> 3) << 5) | (p.b >> 3))\n" \
    "                            : (((p.a >> 5) << 12) | ((p.r >> 4) << 8) | ((p.g >> 4) << 4) | (p.b >> 4)); break;\n" \
    "    default: rgba8 = p; break;\n" \
    "  }\n" \
    "  ivec4 r;\n" \
    "  switch (u_mode.y) {\n" \
    "    case 0: { int i = e4(v & 15); r = ivec4(i); break; }\n" \
    "    case 1: r = ivec4(v & 255); break;\n" \
    "    case 2: { int i = e4(v & 15); r = ivec4(i, i, i, e4((v >> 4) & 15)); break; }\n" \
    "    case 3: { int i = v & 255; r = ivec4(i, i, i, (v >> 8) & 255); break; }\n" \
    "    case 4: r = ivec4(e5((v >> 11) & 31), e6((v >> 5) & 63), e5(v & 31), 255); break;\n" \
    "    case 5: if ((v & 0x8000) != 0) r = ivec4(e5((v >> 10) & 31), e5((v >> 5) & 31), e5(v & 31), 255);\n" \
    "            else r = ivec4(e4((v >> 8) & 15), e4((v >> 4) & 15), e4(v & 15), e3((v >> 12) & 7)); break;\n" \
    "    default: r = rgba8; break;\n" \
    "  }\n" \
    "  return vec4(r) / 255.0;\n" \
    "}\n"

const char* kReinterpretFS =
    "#version 330 core\n"
    "uniform sampler2D u_src;\nuniform ivec4 u_mode;  // copy fmt, tex fmt, intensity\n"
    "out vec4 o;\n"
    SSNE_REINT_FUNC
    "void main() { o = reint(texelFetch(u_src, ivec2(gl_FragCoord.xy), 0), u_mode); }\n";

// Full-size EFB copy that also writes the copy converted to its natural texture format (second render target).
const char* kCopyMrtFS =
    "#version 330 core\n"
    "uniform sampler2D u_src;\nuniform ivec4 u_rect;  // source origin (internal pixels)\nuniform ivec4 u_mode;\n"
    "layout(location=0) out vec4 o;\nlayout(location=1) out vec4 o2;\n"
    SSNE_REINT_FUNC
    "void main() {\n"
    "  vec4 raw = texelFetch(u_src, u_rect.xy + ivec2(gl_FragCoord.xy), 0);\n"
    "  o = raw;\n"
    "  o2 = reint(raw, u_mode);\n"
    "}\n";

// FXAA (the classic "PC console" variant): blends along edges found from luma contrast, one pass at output size.
const char* kFxaaFS =
    "#version 330 core\n"
    "uniform sampler2D u_src;\nuniform vec2 u_rcp;\nout vec4 o;\n"
    "float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
    "void main() {\n"
    "  vec2 uv = gl_FragCoord.xy * u_rcp;\n"
    "  vec3 nw = texture(u_src, uv + vec2(-1.0, -1.0) * u_rcp).rgb, ne = texture(u_src, uv + vec2(1.0, -1.0) * u_rcp).rgb;\n"
    "  vec3 sw = texture(u_src, uv + vec2(-1.0, 1.0) * u_rcp).rgb, se = texture(u_src, uv + vec2(1.0, 1.0) * u_rcp).rgb;\n"
    "  vec3 m = texture(u_src, uv).rgb;\n"
    "  float lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se), lm = luma(m);\n"
    "  float lmin = min(lm, min(min(lnw, lne), min(lsw, lse))), lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));\n"
    "  if (lmax - lmin < max(0.0312, lmax * 0.125)) { o = vec4(m, 1.0); return; }  // no edge: keep the pixel\n"
    "  vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));\n"
    "  float reduce = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);\n"
    "  float rcpmin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);\n"
    "  dir = clamp(dir * rcpmin, vec2(-8.0), vec2(8.0)) * u_rcp;\n"
    "  vec3 a = 0.5 * (texture(u_src, uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture(u_src, uv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
    "  vec3 b = a * 0.5 + 0.25 * (texture(u_src, uv - dir * 0.5).rgb + texture(u_src, uv + dir * 0.5).rgb);\n"
    "  float lb = luma(b);\n"
    "  o = vec4((lb < lmin || lb > lmax) ? a : b, 1.0);\n"
    "}\n";

// ------------------------------------------------------------------ state mapping
GLenum blend_factor(uint32_t f, bool src, bool dst_alpha) {
    switch (f) {
    case 0: return ZERO;
    case 1: return ONE;
    case 2: return src ? DST_COLOR : SRC_COLOR;
    case 3: return src ? ONE_MINUS_DST_COLOR : ONE_MINUS_SRC_COLOR;
    case 4: return SRC1_ALPHA;
    case 5: return ONE_MINUS_SRC1_ALPHA;
    case 6: return dst_alpha ? DST_ALPHA : ONE;
    default: return dst_alpha ? ONE_MINUS_DST_ALPHA : ZERO;
    }
}

const GLenum kLogic[16] = {CLEAR, AND, AND_REVERSE, COPY, AND_INVERTED, NOOP, XOR, OR,
                           NOR, EQUIV, INVERT, OR_REVERSE, COPY_INVERTED, OR_INVERTED, NAND, SET};

void apply_state(const GpuState& st) {
    const DrawCfg& c = st.cfg;
    sc_cap(CLIP_DISTANCE0, true); sc_cap(CLIP_DISTANCE1, true); sc_cap(GLenum(CLIP_DISTANCE0 + 2), true);
    // blending / logic op
    uint32_t bm = c.blend;
    if (bm & 1) {
        sc_cap(COLOR_LOGIC_OP, false);
        sc_cap(BLEND, true);
        if (bm & (1 << 11)) {
            // dst - src (alpha too, unless constant alpha replaces it)
            sc_blend_eq(FUNC_REVERSE_SUBTRACT, c.const_alpha_en ? FUNC_ADD : FUNC_REVERSE_SUBTRACT);
            if (c.const_alpha_en) sc_blend_func(ONE, ONE, ONE, ZERO);
            else sc_blend_func(ONE, ONE, ONE, ONE);
        } else {
            uint32_t sf = (bm >> 8) & 7, df = (bm >> 5) & 7;
            GLenum s = blend_factor(sf, true, c.dst_has_alpha), d = blend_factor(df, false, c.dst_has_alpha);
            sc_blend_eq(FUNC_ADD, FUNC_ADD);
            if (c.const_alpha_en) sc_blend_func(s, d, ONE, ZERO);
            else sc_blend_func(s, d, s, d);
        }
    } else {
        sc_cap(BLEND, false);
        if (bm & 2) { sc_cap(COLOR_LOGIC_OP, true); sc_logic(kLogic[(bm >> 12) & 15]); }
        else sc_cap(COLOR_LOGIC_OP, false);
    }
    bool cu = bm & (1 << 3), au = (bm & (1 << 4)) && c.dst_has_alpha;
    sc_color_mask(cu, cu, cu, au);
    // depth
    if (c.z_enable) {
        sc_cap(DEPTH_TEST, true);
        sc_depth_func(NEVER + c.z_func);
        sc_depth_mask(c.z_update);
    } else {
        sc_cap(DEPTH_TEST, false);
        sc_depth_mask(0);
    }
    // culling: GX front faces are clockwise with y down == positive area in our (row-preserving) window coords
    if (st.cull == 0) sc_cap(CULL_FACE, false);
    else {
        sc_cap(CULL_FACE, true);
        sc_front(CCW);
        sc_cull(st.cull == 1 ? BACK : st.cull == 2 ? FRONT : FRONT_AND_BACK);
    }
    // scissor
    sc_cap(SCISSOR_TEST, true);
    int x0 = std::max(0, c.scissor_x0), y0 = std::max(0, c.scissor_y0);
    int x1 = std::min(EFB_W - 1, c.scissor_x1), y1 = std::min(EFB_H - 1, c.scissor_y1);
    if (x1 < x0 || y1 < y0) sc_scissor(0, 0, 0, 0);
    else sc_scissor(SX(x0), SY(y0), SX(x1 + 1) - SX(x0), SY(y1 + 1) - SY(y0));
}

void run_draw(const GpuFrame& f, const GpuCmd& cmd) {
    const GpuState& st = f.states[cmd.state];
    const DrawCfg& c = st.cfg;
    int ntex = int(cmd.ntex);
    // SSNE_GPU_ALWAYS_FRAGDEPTH=1: every draw writes gl_FragDepth like before the early-Z change (for A/B testing
    // if a driver's fixed-function depth ever differs from gl_FragCoord.z on coplanar geometry).
    static const bool always_fragdepth = getenv("SSNE_GPU_ALWAYS_FRAGDEPTH") != nullptr;
    Program* p = program_for(c, ntex, cmd.zsnap || always_fragdepth);
    if (!p) return;
    // Textures first (reinterpretation passes change GL state).
    GLuint tex[8];
    for (int m = 0; m < 8; m++) {
        bool used = false;
        for (int s = 0; s < c.stages; s++) used |= c.st[s].tex_enabled && c.st[s].texmap == m;
        for (int i = 0; i < c.nind; i++) used |= c.ind_map[i] == m;
        tex[m] = used ? texture_for(c.tex[m]) : g_white;
    }
    sc_program(p->prog);
    apply_state(st);
    for (int m = 0; m < 8; m++) {
        sc_texture(m, tex[m]);
        uint32_t ws = std::min<uint32_t>(c.wrap_s[m], 2), wt = std::min<uint32_t>(c.wrap_t[m], 2);
        const Texture* tx = c.tex[m];
        bool aniso_ok = g_aniso > 1 && tx && tx->efb_copy < 0 && !tx->mips.empty() && c.linear[m] &&
                        (c.min_filter[m] == 5 || c.min_filter[m] == 6);
        sc_sampler(m, sampler_for(c.min_filter[m], c.linear[m], ws, wt, aniso_ok ? g_aniso : 1));
    }
    bool first = !p->shadow;
    if (first) p->shadow = std::make_shared<UniformShadow>();
    UniformShadow& sh = *p->shadow;
    float vp0[4] = {st.vp[0], st.vp[1], st.vp[2], 0}, vp1[4] = {st.vp[3] - 342.f - c.xoff, st.vp[4] - 342.f - c.yoff, st.vp[5], 0};
    if (changed(sh.vp0, vp0, first)) Uniform4fv(p->u_vp0, 1, vp0);
    if (changed(sh.vp1, vp1, first)) Uniform4fv(p->u_vp1, 1, vp1);
    float efb[2] = {float(EFB_W), float(EFB_H)};
    if (changed(sh.efb, efb, first)) Uniform2fv(p->u_efb, 1, efb);
    GLint reg[16], konst[16];
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) { reg[i * 4 + k] = c.tev_reg[i][k]; konst[i * 4 + k] = c.konst[i][k]; }
    if (changed(sh.reg, reg, first)) Uniform4iv(p->u_reg, 4, reg);
    if (changed(sh.konst, konst, first)) Uniform4iv(p->u_konst, 4, konst);
    GLint aref[4] = {GLint(c.alpha_cmp & 0xFF), GLint((c.alpha_cmp >> 8) & 0xFF), 0, 0};
    if (changed(sh.aref, aref, first)) Uniform4iv(p->u_aref, 1, aref);
    float sts[32];
    for (int s = 0; s < 16; s++) {
        float tw = 1.f, th = 1.f;
        if (s < c.stages) {
            const Texture* t = c.tex[c.st[s].texmap];
            if (t && t->w) { tw = float(t->w); th = float(t->h); }
            int tc = c.st[s].texcoord & 7;
            sts[s * 2] = c.su_s[tc] / tw;
            sts[s * 2 + 1] = c.su_t[tc] / th;
        } else {
            sts[s * 2] = sts[s * 2 + 1] = 0;
        }
    }
    if (changed(sh.sts, sts, first)) Uniform2fv(p->u_stscale, 16, sts);
    if (p->u_calpha >= 0) {
        float ca = float(c.const_alpha) / 255.f;
        if (first || ca != sh.calpha) { Uniform1f(p->u_calpha, ca); sh.calpha = ca; }
    }
    if (p->u_indmtx >= 0) {
        GLint im[24];
        for (int m = 0; m < 3; m++) {
            const int16_t* x = c.ind_mtx[m];
            GLint* o = im + m * 8;
            o[0] = x[0]; o[1] = x[2]; o[2] = x[4]; o[3] = c.ind_shift[m];
            o[4] = x[1]; o[5] = x[3]; o[6] = x[5]; o[7] = 0;
        }
        if (changed(sh.indmtx, im, first)) Uniform4iv(p->u_indmtx, 6, im);
    }
    if (p->u_texsz >= 0) {
        float ts[16];
        for (int m = 0; m < 8; m++) {
            const Texture* t = c.tex[m];
            ts[m * 2] = t && t->w ? float(t->w) : 1.f;
            ts[m * 2 + 1] = t && t->h ? float(t->h) : 1.f;
        }
        if (changed(sh.texsz, ts, first)) Uniform2fv(p->u_texsz, 8, ts);
    }
    if (p->u_su >= 0) {
        float su[16];
        for (int t = 0; t < 8; t++) { su[t * 2] = c.su_s[t]; su[t * 2 + 1] = c.su_t[t]; }
        if (changed(sh.su, su, first)) Uniform2fv(p->u_su, 8, su);
    }
    if (p->u_fog >= 0) {
        float fog[4] = {c.fog_a, c.fog_c, float(c.fog_bmag), float(c.fog_bshift)};
        if (changed(sh.fog, fog, first)) Uniform4fv(p->u_fog, 1, fog);
        GLint fc[4] = {c.fog_color[0], c.fog_color[1], c.fog_color[2], 0};
        if (changed(sh.fogc, fc, first)) Uniform4iv(p->u_fogcolor, 1, fc);
    }
    {   // depth of the near (z = -w) and far (z = 0) planes for this viewport, quantized like the 24-bit z buffer
        auto q = [](double d) { return float(std::floor(std::clamp(d, 0.0, 1.0) * 16777215.0 + 0.5) / 16777215.0); };
        float zr[2] = {q((double(st.vp[5]) - double(st.vp[2])) / 16777216.0), q(double(st.vp[5]) / 16777216.0)};
        if (changed(sh.zr, zr, first)) Uniform2fv(p->u_zrange, 1, zr);
    }
    // vertex attributes
    const uint8_t* base = reinterpret_cast<const uint8_t*>(uintptr_t(cmd.vtx_off));
    GLsizei stride = GLsizei(cmd.stride);
    VertexAttribPointer(0, 4, FLOAT, 0, stride, base);
    VertexAttribPointer(1, 4, UNSIGNED_BYTE, 1, stride, base + 16);
    VertexAttribPointer(2, 4, UNSIGNED_BYTE, 1, stride, base + 20);
    for (int i = 0; i < 8; i++) {
        if (i < ntex) { sc_attrib(3 + i, true); VertexAttribPointer(GLuint(3 + i), 3, FLOAT, 0, stride, base + 24 + 12 * i); }
        else sc_attrib(3 + i, false);
    }
    DrawElements(TRIANGLES, GLsizei(cmd.idx_count), UNSIGNED_INT,
                 reinterpret_cast<const void*>(uintptr_t(cmd.idx_first) * 4));
    g_stats.draws++;
    g_stats.triangles += cmd.idx_count / 3;
}

void run_clear(const GpuCmd& cmd) {
    sc_cap(SCISSOR_TEST, true);
    sc_scissor(SX(cmd.x), SY(cmd.y), SX(cmd.x + cmd.w) - SX(cmd.x), SY(cmd.y + cmd.h) - SY(cmd.y));
    sc_cap(COLOR_LOGIC_OP, false);
    GLbitfield mask = 0;
    if (cmd.cu || cmd.au) {
        sc_color_mask(cmd.cu, cmd.cu, cmd.cu, cmd.au);
        ClearColor(float(cmd.color & 255) / 255.f, float((cmd.color >> 8) & 255) / 255.f,
                   float((cmd.color >> 16) & 255) / 255.f, float(cmd.color >> 24) / 255.f);
        mask |= COLOR_BUFFER_BIT;
    }
    if (cmd.zu) {
        sc_depth_mask(1);
        ClearDepth(double(cmd.z) / 16777215.0);
        mask |= DEPTH_BUFFER_BIT;
    }
    if (mask) Clear(mask);
}

GLint g_copy_u_src = -1;

void run_copy_tex(const GpuCmd& cmd) {
    // A copy into part of an earlier copy (see GpuCmd::base_copy_id): start from the earlier copy's texels and write
    // this copy's rows (dst_row..) into them.
    const bool comp = cmd.base_copy_id != 0;
    int w = std::max(1, SX(std::max(1, comp ? cmd.base_w : cmd.out_w)));
    int h = std::max(1, SY(std::max(1, comp ? cmd.base_h : cmd.out_h)));
    int yoff = comp ? SY(cmd.dst_row) : 0, bh = comp ? std::min(h - yoff, std::max(1, SY(std::max(1, cmd.out_h)))) : h;
    RT t = rt_acquire(w, h);
    PooledTex entry{t, g_frame_no};
    // Source origin in internal pixels; the shader reads it at gl_FragCoord (x2 for half-size copies), which starts at
    // yoff when writing into the composite's rows.
    GLint rect[4] = {SX(cmd.x), SY(cmd.y) - (cmd.half ? 2 * yoff : yoff), cmd.half ? 1 : 0, 0};
    int nat = cmd.copy_fmt_known ? natural_tex_fmt(cmd.copy_fmt) : -1;
    // Full-size copies (texel exact) also get their natural-format conversion in the same pass. Half-size copies are
    // box filtered: their stored value is rounded by the framebuffer, so they keep the separate pass on the result.
    // Composites keep only the raw copy (reads reinterpret it as needed).
    bool mrt = g_copy_mrt_prog && !comp && !cmd.half && nat >= 0 && !(cmd.copy_fmt == 6 && nat == 6);
    pass_state();
    if (comp) {
        auto base = g_copies.find(cmd.base_copy_id);
        if (base != g_copies.end()) {
            const RT& b = base->second.rt;
            BindFramebuffer(READ_FRAMEBUFFER, b.fbo);
            BindFramebuffer(DRAW_FRAMEBUFFER, t.fbo);
            BlitFramebuffer(0, 0, b.w, b.h, 0, 0, w, h, COLOR_BUFFER_BIT, NEAREST);
        } else {
            BindFramebuffer(FRAMEBUFFER, t.fbo);
            ClearColor(0, 0, 0, 0);
            Clear(COLOR_BUFFER_BIT);
        }
    }
    if (mrt) {
        RT conv = rt_acquire(w, h);
        // Blend factors left over from a dual-source (SRC1) blend make a 2-target draw invalid on some drivers
        // (Mesa) even with blending disabled: reset them to plain ones.
        sc_blend_func(ONE, ZERO, ONE, ZERO);
        BindFramebuffer(FRAMEBUFFER, g_mrt_fbo);
        FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, t.tex, 0);
        FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT1, TEXTURE_2D, conv.tex, 0);
        Viewport(0, 0, w, h);
        sc_program(g_copy_mrt_prog);
        Uniform4iv(g_copy_mrt_u_rect, 1, rect);
        GLint mode[4] = {cmd.copy_fmt, nat, cmd.intensity ? 1 : 0, 0};
        Uniform4iv(g_copy_mrt_u_mode, 1, mode);
        entry.conv = conv;
        entry.conv_fmt = nat;
    } else {
        BindFramebuffer(FRAMEBUFFER, t.fbo);
        Viewport(0, yoff, w, bh);
        sc_program(g_copy_prog);
        Uniform4iv(g_copy_u_rect, 1, rect);
    }
    if (g_msaa > 1) {
        GLuint fbo_now = mrt ? g_mrt_fbo : t.fbo;
        int sx = SX(cmd.x), sy = SY(cmd.y);  // source rectangle (rect[1] is shifted for composites)
        int sw = cmd.half ? 2 * w + 2 : w, sh = cmd.half ? 2 * bh + 2 : bh;
        efb_color_for_read(sx, sy, std::min(sx + sw, SX(EFB_W)), std::min(sy + sh, SY(EFB_H)));
        BindFramebuffer(FRAMEBUFFER, fbo_now);
    }
    sc_texture(0, g_msaa > 1 ? g_resolve_tex : g_efb_color);
    sc_sampler(0, 0);
    DrawArrays(TRIANGLES, 0, 3);
    auto old = g_copies.find(cmd.copy_id);
    if (old != g_copies.end()) release_copy(old->second);
    g_copies[cmd.copy_id] = entry;
    BindFramebuffer(FRAMEBUFFER, g_efb_fbo);
    Viewport(0, 0, SX(EFB_W), SY(EFB_H));
    g_stats.tex_copies++;
}

void run_copy_xfb(const GpuCmd& cmd) {
    int w = SX(cmd.x + cmd.w) - SX(cmd.x), h = SY(cmd.y + cmd.h) - SY(cmd.y);
    if (w != g_out_w || h != g_out_h) {
        if (g_out_tex) { sc_delete_texture(g_out_tex); DeleteFramebuffers(1, &g_out_fbo); }
        g_out_tex = make_tex(w, h, RGBA8, RGBA, UNSIGNED_BYTE, nullptr, true);
        g_out_fbo = fbo_for(g_out_tex);
        g_out_w = w; g_out_h = h;
    }
    pass_state();
    BindFramebuffer(READ_FRAMEBUFFER, g_efb_fbo);
    BindFramebuffer(DRAW_FRAMEBUFFER, g_out_fbo);
    BlitFramebuffer(SX(cmd.x), SY(cmd.y), SX(cmd.x) + w, SY(cmd.y) + h, 0, 0, w, h,
                    COLOR_BUFFER_BIT, NEAREST);
    // The TV shows RGB only: make the output opaque (EFB alpha is often 0, and the port draws the image with
    // alpha blending, which would make it invisible).
    BindFramebuffer(FRAMEBUFFER, g_out_fbo);
    sc_color_mask(0, 0, 0, 1);
    ClearColor(0, 0, 0, 1);
    Clear(COLOR_BUFFER_BIT);
    sc_color_mask(1, 1, 1, 1);
    g_final_tex = g_out_tex;
    g_final_fbo = g_out_fbo;
    if (g_fxaa && g_fxaa_prog) {
        if (g_fxaa_w != w || g_fxaa_h != h) {
            if (g_fxaa_tex) { sc_delete_texture(g_fxaa_tex); DeleteFramebuffers(1, &g_fxaa_fbo); }
            g_fxaa_tex = make_tex(w, h, RGBA8, RGBA, UNSIGNED_BYTE, nullptr, true);
            g_fxaa_fbo = fbo_for(g_fxaa_tex);
            g_fxaa_w = w; g_fxaa_h = h;
        }
        BindFramebuffer(FRAMEBUFFER, g_fxaa_fbo);
        Viewport(0, 0, w, h);
        sc_program(g_fxaa_prog);
        float rcp[2] = {1.f / float(w), 1.f / float(h)};
        Uniform2fv(g_fxaa_u_rcp, 1, rcp);
        sc_texture(0, g_out_tex);
        sc_sampler(0, sampler_for(4, true, 0, 0));  // bilinear, clamp
        DrawArrays(TRIANGLES, 0, 3);
        sc_sampler(0, 0);
        Viewport(0, 0, SX(EFB_W), SY(EFB_H));
        g_final_tex = g_fxaa_tex;
        g_final_fbo = g_fxaa_fbo;
    }
    {   // Mipmaps on the presented image: smooth downscaling when the display is smaller than it (supersampling).
        sc_texture(0, g_final_tex);
        if (g_smooth) {
            GenerateMipmap(TEXTURE_2D);
            TexParameteri(TEXTURE_2D, TEXTURE_MAX_LEVEL, 16);
            TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR_MIPMAP_LINEAR);
        } else {
            TexParameteri(TEXTURE_2D, TEXTURE_MAX_LEVEL, 0);
            TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
        }
    }
    BindFramebuffer(FRAMEBUFFER, g_efb_fbo);
    g_out_wide = g_wide;
    g_out_seq++;
}

void execute(GpuFrame& f) {
    auto t0 = std::chrono::steady_clock::now();
    sc_invalidate();  // the port's UI used the context since the last frame
    poll_timer_queries();
    bool timed = g_tq_ok && g_tq_pending < kTimerQueries;
    if (timed) BeginQuery(TIME_ELAPSED, g_tq[(g_tq_first + g_tq_pending) % kTimerQueries]);
    if (f.wide != g_wide) { g_wide = f.wide; create_efb(); }  // gameplay <-> front end: anamorphic EFB width
    g_frame_no++;
    g_phase_frame = g_frame_no;
    g_phase_ncmds = uint32_t(f.cmds.size());
    g_phase = "upload vertices";
    BindFramebuffer(FRAMEBUFFER, g_efb_fbo);
    Viewport(0, 0, SX(EFB_W), SY(EFB_H));
    sc_cap(DEPTH_CLAMP, true);
    sc_cap(CLIP_DISTANCE0, true); sc_cap(CLIP_DISTANCE1, true); sc_cap(GLenum(CLIP_DISTANCE0 + 2), true);
    BindVertexArray(g_vao);
    BindBuffer(ARRAY_BUFFER, g_vbo);
    BufferData(ARRAY_BUFFER, GLsizeiptr(f.vtx.size()), f.vtx.data(), STREAM_DRAW);
    BindBuffer(ELEMENT_ARRAY_BUFFER, g_ibo);
    BufferData(ELEMENT_ARRAY_BUFFER, GLsizeiptr(f.idx.size() * 4), f.idx.data(), STREAM_DRAW);
    sc_attrib(0, true); sc_attrib(1, true); sc_attrib(2, true);
    static const long dump_frame = getenv("SSNE_GPU_DUMP_FRAME") ? atol(getenv("SSNE_GPU_DUMP_FRAME")) : -1;
    bool has_copy = false;
    for (const GpuCmd& c : f.cmds) has_copy |= c.type == GpuCmd::COPY_TEX;
    g_dumping = long(g_frame_no) == dump_frame || (g_dump_req && (has_copy || g_frame_no % 300 == 0));
    if (g_dumping) {
        if (g_dump_dir.empty()) g_dump_dir = "/tmp/claude-0";
        g_dump_req = false;
        g_dump_n = 0;
        g_dump_log = fopen((g_dump_dir + "/frame.txt").c_str(), "w");
        if (g_dump_log) fprintf(g_dump_log, "frame %llu, scale %d, %zu commands, renderer %s\n", (unsigned long long)g_frame_no, g_scale,
                                f.cmds.size(), g_info.c_str());
    }
    for (const GpuCmd& c : f.cmds) {
        if (g_dumping && g_dump_log) {
            if (c.type == GpuCmd::DRAW) {
                const GpuState& gs = f.states[c.state];
                const DrawCfg& d = gs.cfg;
                fprintf(g_dump_log, "draw tris=%u ntex=%u stages=%d blend=%06X z=%d/%u/%d acmp=%06X cmask=%08X dsta=%d calpha=%d/%d scissor=%d,%d-%d,%d off=%.0f,%.0f vp=%.1f,%.1f,%.1f,%.1f,%.1f,%.1f cull=%u\n",
                        c.idx_count / 3, c.ntex, d.stages, d.blend, d.z_enable, d.z_func, d.z_update, d.alpha_cmp, d.write_mask,
                        d.dst_has_alpha, d.const_alpha_en, d.const_alpha, d.scissor_x0, d.scissor_y0, d.scissor_x1, d.scissor_y1,
                        d.xoff, d.yoff, gs.vp[0], gs.vp[1], gs.vp[2], gs.vp[3], gs.vp[4], gs.vp[5], gs.cull);
                for (int si = 0; si < d.stages; si++) {
                    const StageCfg& st = d.st[si];
                    fprintf(g_dump_log, "   s%d c=%06X a=%06X tex=%d map=%d tc=%d ras=%d", si, st.cenv, st.aenv, st.tex_enabled, st.texmap,
                            st.texcoord, st.ras_chan);
                    if (st.tex_enabled) {
                        const Texture* t = d.tex[st.texmap];
                        if (t) fprintf(g_dump_log, " [%dx%d id %llu copy %lld fmt %u->%u minf %u mag %d wrap %u/%u mips %zu su %.0f/%.0f]", t->w, t->h,
                                       (unsigned long long)t->id, (long long)t->efb_copy, t->copy_fmt, t->tex_fmt, d.min_filter[st.texmap],
                                       d.linear[st.texmap], d.wrap_s[st.texmap], d.wrap_t[st.texmap], t->mips.size(),
                                       d.su_s[st.texcoord & 7], d.su_t[st.texcoord & 7]);
                        else fprintf(g_dump_log, " [no texture]");
                    }
                    fprintf(g_dump_log, "\n");
                }
                if (c.idx_count <= 6)
                    for (uint32_t vi = 0; vi < c.vcount; vi++) {
                        float pp[4], tc[3] = {0, 0, 0};
                        memcpy(pp, f.vtx.data() + c.vtx_off + vi * c.stride, 16);
                        const uint8_t* col = f.vtx.data() + c.vtx_off + vi * c.stride + 16;
                        if (c.ntex) memcpy(tc, f.vtx.data() + c.vtx_off + vi * c.stride + 24, 12);
                        fprintf(g_dump_log, "   v%u clip=(%.3f %.3f %.6f %.6f) depth=%.7f col0=%u,%u,%u,%u tex0=(%.3f %.3f %.3f)\n", vi, pp[0],
                                pp[1], pp[2], pp[3], (pp[2] / pp[3] * gs.vp[2] + gs.vp[5]) / 16777216.f, col[0], col[1], col[2], col[3],
                                tc[0], tc[1], tc[2]);
                    }
            } else {
                fprintf(g_dump_log, "%s rect=%d,%d %dx%d color=%08X z=%06X cu/au/zu=%d%d%d half=%d copy_id=%llu out=%dx%d\n",
                        c.type == GpuCmd::CLEAR ? "CLEAR" : c.type == GpuCmd::COPY_TEX ? "COPY_TEX" : "COPY_XFB", c.x, c.y, c.w, c.h,
                        c.color, c.z, c.cu, c.au, c.zu, c.half, (unsigned long long)c.copy_id, c.out_w, c.out_h);
                efb_color_for_read(0, 0, SX(EFB_W), SY(EFB_H));
                dump_fbo(g_msaa > 1 ? g_resolve_fbo : g_efb_fbo, SX(EFB_W), SY(EFB_H), c.type == GpuCmd::CLEAR ? "efb_before_clear" : "efb_before_copy");
                if (g_msaa <= 1) dump_depth("efb_depth");
                BindFramebuffer(FRAMEBUFFER, g_efb_fbo);
            }
            fflush(g_dump_log);
        }
        g_phase_cmd = uint32_t(&c - f.cmds.data());
        g_phase = c.type == GpuCmd::DRAW ? "draw" : c.type == GpuCmd::CLEAR ? "clear" : c.type == GpuCmd::COPY_TEX ? "copy" : "display copy";
        double pt0 = 0;
        if (g_prof) { Finish(); pt0 = prof_now(); }
        switch (c.type) {
        case GpuCmd::DRAW: run_draw(f, c); break;
        case GpuCmd::CLEAR: run_clear(c); break;
        case GpuCmd::COPY_TEX:
            run_copy_tex(c);
            if (g_dumping) { auto& ct = g_copies[c.copy_id].rt; dump_fbo(ct.fbo, ct.w, ct.h, "copy_texture"); BindFramebuffer(FRAMEBUFFER, g_efb_fbo); }
            break;
        case GpuCmd::COPY_XFB:
            run_copy_xfb(c);
            if (g_dumping) { dump_fbo(g_out_fbo, g_out_w, g_out_h, "output"); BindFramebuffer(FRAMEBUFFER, g_efb_fbo); }
            break;
        }
        if (g_prof) {
            Finish();
            int k = c.type == GpuCmd::DRAW ? (c.zsnap ? 5 : 0) : c.type == GpuCmd::CLEAR ? 1 : c.type == GpuCmd::COPY_TEX ? 2 : 3;
            g_prof_ms[k] += prof_now() - pt0;
            g_prof_n[k]++;
            static const bool verbose = getenv("SSNE_GPU_PROFILE")[0] == '2';
            if (verbose && c.type == GpuCmd::DRAW) {
                const DrawCfg& d = f.states[c.state].cfg;
                fprintf(stdout, "  draw %5.2f ms tris %5u stages %d nind %d ntex %u zsnap %d blend %06X z %d/%d atest %d scissor %d,%d-%d,%d\n",
                        prof_now() - pt0, c.idx_count / 3, d.stages, d.nind, c.ntex, c.zsnap, d.blend, d.z_enable, d.z_update,
                        !d.atest_always, d.scissor_x0, d.scissor_y0, d.scissor_x1, d.scissor_y1);
            } else if (verbose) fprintf(stdout, "  %s %5.2f ms\n", k == 1 ? "clear" : k == 2 ? "copy" : "xfb", prof_now() - pt0);
        }
    }
    if (g_dumping) {
        if (g_dump_log) { fprintf(g_dump_log, "end of frame\n"); fclose(g_dump_log); g_dump_log = nullptr; }
        g_dumping = false;
        log_message("[gpu] frame " + std::to_string(g_frame_no) + " captured to " + g_dump_dir);
    }
    // Leave a neutral state for whoever draws next (the port's UI).
    for (int m = 0; m < 8; m++) BindSampler(GLuint(m), 0);
    ActiveTexture(TEXTURE0);
    Disable(DEPTH_CLAMP);
    Disable(CLIP_DISTANCE0); Disable(CLIP_DISTANCE1); Disable(GLenum(CLIP_DISTANCE0 + 2));
    Disable(COLOR_LOGIC_OP); Disable(SCISSOR_TEST); Disable(DEPTH_TEST); Disable(CULL_FACE); Disable(BLEND);
    ColorMask(1, 1, 1, 1);
    DepthMask(1);
    BindVertexArray(0);
    BindFramebuffer(FRAMEBUFFER, 0);
    UseProgram(0);
    evict();
    if (timed) { EndQuery(TIME_ELAPSED); g_tq_pending++; }
    GLenum e = (g_frame_no % 120 == 1) ? GetError() : 0;  // glGetError can force a driver sync: check rarely
    if (e) {
        static int reported = 0;
        if (reported++ < 10) {
            char buf[96];
            snprintf(buf, sizeof(buf), "[gpu] OpenGL error 0x%04X in frame %llu", e, (unsigned long long)g_frame_no);
            log_message(buf);
        }
        while (GetError()) {}
    }
    g_stats.frames++;
    g_stats.frame_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_max_frame_ms = std::max(g_max_frame_ms, g_stats.frame_ms);
    if (g_stats.frame_ms > 250) {
        char buf[160];
        snprintf(buf, sizeof(buf), "[gpu] slow frame %llu: %.0f ms (%zu commands, %zu vertices bytes)",
                 (unsigned long long)g_frame_no, g_stats.frame_ms, f.cmds.size(), f.vtx.size());
        log_message(buf);
    }
    g_phase = "idle";
}

} // namespace

void gx_use_gpu(bool on) {
    g_gpu_mode = on;
    gx::gpu_set_enabled(on);
}
bool gx_gpu_active() { return g_gpu_mode; }

bool gpu_renderer_init(void* (*getproc)(const char*), int scale, std::string& err) {
    const char* missing = nullptr;
    if (!gl::load(getproc, &missing)) { err = std::string("OpenGL function missing: ") + (missing ? missing : "?"); return false; }
    g_scale = std::clamp(scale, 1, 8);
    g_info = std::string(reinterpret_cast<const char*>(GetString(RENDERER))) + " | " +
             reinterpret_cast<const char*>(GetString(VERSION));
    GenVertexArrays(1, &g_vao);
    GenBuffers(1, &g_vbo);
    GenBuffers(1, &g_ibo);
    uint32_t white = 0xFFFFFFFFu;
    g_white = make_tex(1, 1, RGBA8, RGBA, UNSIGNED_BYTE, &white);
    g_copy_prog = link(kFullscreenVS, kCopyFS);
    g_fxaa_prog = link(kFullscreenVS, kFxaaFS);
    if (g_fxaa_prog) {
        UseProgram(g_fxaa_prog);
        Uniform1i(GetUniformLocation(g_fxaa_prog, "u_src"), 0);
        g_fxaa_u_rcp = GetUniformLocation(g_fxaa_prog, "u_rcp");
    }
    if (SamplerParameterf) {  // anisotropic filtering available? (the query fails harmlessly without the extension)
        while (GetError()) {}
        GLint a = 1;
        GetIntegerv(MAX_TEXTURE_MAX_ANISOTROPY, &a);
        g_aniso_max = GetError() ? 1 : std::max(1, int(a));
        g_aniso = std::min(g_aniso, g_aniso_max);
    }
    if (TexImage2DMultisample) {
        GLint ms = 1;
        GetIntegerv(MAX_SAMPLES, &ms);
        g_msaa_max = std::max(1, int(ms));
    }
    g_msaa = std::min(g_msaa, g_msaa_max);
    g_copy_mrt_prog = link(kFullscreenVS, kCopyMrtFS);  // optional: without it every copy is reinterpreted separately
    if (g_copy_mrt_prog) {
        UseProgram(g_copy_mrt_prog);
        Uniform1i(GetUniformLocation(g_copy_mrt_prog, "u_src"), 0);
        g_copy_mrt_u_rect = GetUniformLocation(g_copy_mrt_prog, "u_rect");
        g_copy_mrt_u_mode = GetUniformLocation(g_copy_mrt_prog, "u_mode");
        GenFramebuffers(1, &g_mrt_fbo);
        BindFramebuffer(FRAMEBUFFER, g_mrt_fbo);
        const GLenum bufs[2] = {COLOR_ATTACHMENT0, COLOR_ATTACHMENT1};
        DrawBuffers(2, bufs);
        BindFramebuffer(FRAMEBUFFER, 0);
    }
    g_reint_prog = link(kFullscreenVS, kReinterpretFS);
    if (!g_copy_prog || !g_reint_prog) { err = "failed to build GPU renderer shaders"; return false; }
    UseProgram(g_copy_prog);
    Uniform1i(GetUniformLocation(g_copy_prog, "u_src"), 0);
    g_copy_u_rect = GetUniformLocation(g_copy_prog, "u_rect");
    UseProgram(g_reint_prog);
    Uniform1i(GetUniformLocation(g_reint_prog, "u_src"), 0);
    g_reint_u_mode = GetUniformLocation(g_reint_prog, "u_mode");
    UseProgram(0);
    GenFramebuffers(1, &g_tmp_fbo);
    g_tq_ok = GenQueries && BeginQuery && EndQuery && GetQueryObjectiv && GetQueryObjectui64v;
    init_shader_cache();
    if (g_tq_ok) GenQueries(kTimerQueries, g_tq);
    sc_invalidate();
    create_efb();
    BindFramebuffer(FRAMEBUFFER, 0);
    g_ready = true;
    fprintf(stdout, "[gpu] renderer: %s, internal resolution %dx (%dx%d)\n", g_info.c_str(), g_scale, SX(EFB_W), SY(EFB_H));
    return true;
}

bool gpu_renderer_run(int max_frames) {
    if (!g_ready) return false;
    uint64_t before = g_out_seq;
    for (int i = 0; i < max_frames; i++) {
        g_phase = "take frame";
        auto f = gx::gpu_take_frame();
        g_phase = "idle";
        if (!f) break;
        execute(*f);
        gx::gpu_recycle_frame(std::move(f));
    }
    return g_out_seq != before;
}

bool gpu_renderer_wait(int timeout_ms) { return gx::gpu_wait_frame(timeout_ms); }

void gpu_renderer_set_scale(int scale) {
    scale = std::clamp(scale, 1, 8);
    if (scale == g_scale || !g_ready) { g_scale = scale; return; }
    g_scale = scale;
    sc_invalidate();
    create_efb();
    BindFramebuffer(FRAMEBUFFER, 0);
}
int gpu_renderer_scale() { return g_scale; }

void gpu_renderer_set_widescreen(float) {}  // per frame now (GpuFrame::wide)

GpuOutput gpu_renderer_output() { return {g_final_tex ? g_final_tex : g_out_tex, g_out_w, g_out_h, g_out_seq, g_out_wide}; }

void gpu_renderer_set_antialiasing(int msaa_samples, bool fxaa, bool smooth_downscale) {
    int want = 1;
    while (want * 2 <= std::clamp(msaa_samples, 1, 16) && want * 2 <= g_msaa_max) want *= 2;
    g_fxaa = fxaa;
    g_smooth = smooth_downscale;
    if (want == g_msaa) return;
    g_msaa = want;
    if (g_ready) { sc_invalidate(); create_efb(); BindFramebuffer(FRAMEBUFFER, 0); }
}
int gpu_renderer_msaa() { return g_msaa; }

void gpu_renderer_set_anisotropy(int level) {
    int a = 1;
    while (a * 2 <= std::clamp(level, 1, 16) && (!g_ready || a * 2 <= g_aniso_max)) a *= 2;
    g_aniso = a;
}
int gpu_renderer_anisotropy() { return g_aniso; }

bool gpu_renderer_read_output(std::vector<uint32_t>& rgba, int& w, int& h) {
    if (!g_out_tex) return false;
    w = g_out_w; h = g_out_h;
    rgba.resize(size_t(w) * h);
    BindFramebuffer(FRAMEBUFFER, g_final_fbo ? g_final_fbo : g_out_fbo);
    PixelStorei(PACK_ALIGNMENT, 4);
    ReadPixels(0, 0, w, h, RGBA, UNSIGNED_BYTE, rgba.data());
    BindFramebuffer(FRAMEBUFFER, 0);
    return true;
}

std::string gpu_renderer_info() { return g_info; }
std::string gpu_renderer_profile() {
    static const char* names[6] = {"draws", "clears", "texture copies", "display copies", "reinterpretations", "draws (depth write)"};
    std::string r;
    for (int i = 0; i < 6; i++) {
        char b[128];
        snprintf(b, sizeof(b), "%-20s %6llu  %9.1f ms\n", names[i], (unsigned long long)g_prof_n[i], g_prof_ms[i]);
        r += b;
    }
    return r;
}

void gpu_renderer_request_dump(const std::string& dir) { g_dump_dir = dir; g_dump_req = true; }
void gpu_renderer_set_shader_cache(const std::string& dir) { g_cache_dir = dir; }

std::string gpu_renderer_phase() {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s (frame %llu, command %u of %u)", g_phase.load(), (unsigned long long)g_phase_frame.load(),
             g_phase_cmd.load(), g_phase_ncmds.load());
    return buf;
}

double gpu_renderer_take_max_frame_ms() { double m = g_max_frame_ms; g_max_frame_ms = 0; return m; }
bool gpu_renderer_take_gpu_time(double& total_ms, int& frames) {
    poll_timer_queries();
    total_ms = g_gpu_ms_total;
    frames = g_gpu_frames;
    g_gpu_ms_total = 0;
    g_gpu_frames = 0;
    return g_tq_ok;
}
GpuStats gpu_renderer_stats() { return g_stats; }

} // namespace ssne
