// GPU (OpenGL 3.3) renderer for the GX pipeline. The game's command stream is recorded on the guest thread and
// executed on the thread that owns the GL context.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ssne {

void gx_use_gpu(bool on);    // choose the renderer before boot (default: software)
void gx_set_gpu_vertex(bool on);  // GPU renderer: texgen + lighting in the vertex shader (default on; before boot)
bool gx_gpu_active();

// Render thread (GL context current):
void gpu_renderer_set_shader_cache(const std::string& dir);  // before init: where compiled shaders are kept (empty: off)
bool gpu_renderer_init(void* (*getproc)(const char*), int scale, std::string& err);
bool gpu_renderer_run(int max_frames = 2);   // executes recorded frames; true if a new display frame is ready
bool gpu_renderer_wait(int timeout_ms);      // sleeps until the game has queued a frame (or the timeout)
void gpu_renderer_set_scale(int scale);      // internal resolution: 1 = 640x528 ... 8
// Native resolution: the 448-line game image rendered at display_h lines (and the matching width: h / 480 internal
// pixels per EFB pixel horizontally, the GameCube's pixel aspect). 1920x1080 for 16:9 gameplay at display_h 1080.
void gpu_renderer_set_native(int display_h);
int gpu_renderer_native();  // display height of native resolution, 0 = integer scale
// Anti-aliasing: MSAA sample count for the EFB (1 = off; capped by the driver), FXAA post pass on the display image,
// smooth (mipmapped) downscaling of the display image (for supersampling). Callable before or after init.
void gpu_renderer_set_antialiasing(int msaa_samples, bool fxaa, bool smooth_downscale);
int gpu_renderer_msaa();
void gpu_renderer_set_anisotropy(int level);  // anisotropic filtering 1 (off), 2, 4, 8, 16 (capped by the driver)
int gpu_renderer_anisotropy();
int gpu_renderer_scale();
void gpu_renderer_set_widescreen(float factor);  // EFB rendered this much wider (matches set_widescreen)
struct GpuOutput { uint32_t texture; int w, h; uint64_t seq; float wide; };  // wide: display at 4:3 * wide  // GL texture; row 0 = top of the image
GpuOutput gpu_renderer_output();
bool gpu_renderer_read_output(std::vector<uint32_t>& rgba, int& w, int& h);  // RGBA8, row 0 = top
std::string gpu_renderer_info();
std::string gpu_renderer_profile();  // SSNE_GPU_PROFILE=1: GPU time by command kind
std::string gpu_renderer_phase();
void gpu_renderer_request_dump(const std::string& dir);  // capture the next frame with an EFB copy (step by step)            // what the render thread is doing (for hang reports)
double gpu_renderer_take_max_frame_ms();     // slowest frame since the last call
// GPU time spent executing frames (timer queries) since the last call: false if the driver can't measure it.
bool gpu_renderer_take_gpu_time(double& total_ms, int& frames);
uint64_t gpu_dropped_frames();               // frames the game dropped because the renderer fell behind
struct GpuStats {
    uint64_t frames, draws, triangles, tex_copies, uploads, reinterpretations, programs, render_targets;
    uint64_t cached_programs;  // programs loaded from the shader cache instead of compiled
    double shader_ms, frame_ms;
};
GpuStats gpu_renderer_stats();

} // namespace ssne
