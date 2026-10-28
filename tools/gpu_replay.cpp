// gpu_replay: executes GX frames captured with SSNE_GPU_CAPTURE=dir:first:count through the GPU renderer
// (headless EGL, Linux) and writes the output images. Comparing the images of two builds checks renderer
// changes independently of the game's timing; --time reports the render time per frame (glFinish'ed; with --repeat,
// also the median over every repetition after the first).
//   gpu_replay <capture dir> <out dir> [--scale N | --native H] [--skip K] [--time] [--repeat R] [--shader-cache dir]
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../runtime/src/gx/gx_gpu_frame.h"
#include "../runtime/src/gx/gx_gl.h"
#include "ssne/gx_gpu.h"

static void* (*s_getproc)(const char*) = nullptr;
static void* getproc(const char* n) { return s_getproc(n); }

static bool egl_init(std::string& err) {
    void* lib = dlopen("libEGL.so.1", RTLD_NOW);
    if (!lib) { err = "libEGL.so.1 not found"; return false; }
    using P = void*;
    s_getproc = reinterpret_cast<void* (*)(const char*)>(dlsym(lib, "eglGetProcAddress"));
    auto get_platform = reinterpret_cast<P (*)(unsigned, void*, const int*)>(s_getproc("eglGetPlatformDisplayEXT"));
    auto init = reinterpret_cast<unsigned (*)(P, int*, int*)>(dlsym(lib, "eglInitialize"));
    auto bind = reinterpret_cast<unsigned (*)(unsigned)>(dlsym(lib, "eglBindAPI"));
    auto create = reinterpret_cast<P (*)(P, P, P, const int*)>(dlsym(lib, "eglCreateContext"));
    auto make = reinterpret_cast<unsigned (*)(P, P, P, P)>(dlsym(lib, "eglMakeCurrent"));
    if (!get_platform || !init || !bind || !create || !make) { err = "EGL entry points missing"; return false; }
    P d = get_platform(0x31DD, nullptr, nullptr);
    int ma, mi;
    if (!init(d, &ma, &mi)) { err = "eglInitialize failed"; return false; }
    bind(0x30A2);
    const int attrs[] = {0x3098, 3, 0x30FB, 3, 0x30FD, 1, 0x3038};
    P ctx = create(d, nullptr, nullptr, attrs);
    if (!ctx) { err = "eglCreateContext failed"; return false; }
    make(d, nullptr, nullptr, ctx);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) { puts("usage: gpu_replay <capture dir> <out dir> [--scale N] [--skip K] [--time] [--repeat R]"); return 1; }
    std::string in = argv[1], out = argv[2], tex_dir;
    int scale = 1, skip = 0, repeat = 1, native = 0;  // native: display height (fractional internal resolution)
    bool timing = false, fxaa = false;
    int msaa = 1, aniso = 1;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--scale" && i + 1 < argc) scale = atoi(argv[++i]);
        else if (a == "--native" && i + 1 < argc) native = atoi(argv[++i]);
        else if (a == "--skip" && i + 1 < argc) skip = atoi(argv[++i]);
        else if (a == "--repeat" && i + 1 < argc) repeat = atoi(argv[++i]);
        else if (a == "--time") timing = true;
        else if (a == "--msaa" && i + 1 < argc) msaa = atoi(argv[++i]);
        else if (a == "--fxaa") fxaa = true;
        else if (a == "--aniso" && i + 1 < argc) aniso = atoi(argv[++i]);
        else if (a == "--shader-cache" && i + 1 < argc) ssne::gpu_renderer_set_shader_cache(argv[++i]);
        else if (a == "--dump-textures" && i + 1 < argc) tex_dir = argv[++i];  // every decoded texture as PAM
    }
    std::string err;
    if (!egl_init(err) || !ssne::gpu_renderer_init(getproc, scale, err)) { printf("GPU init failed: %s\n", err.c_str()); return 1; }
    if (native > 0) ssne::gpu_renderer_set_native(native);
    ssne::gpu_renderer_set_antialiasing(msaa, fxaa, false);
    ssne::gpu_renderer_set_anisotropy(aniso);
    if (aniso > 1) printf("anisotropic filtering %dx\n", ssne::gpu_renderer_anisotropy());
    if (msaa > 1) printf("MSAA %dx\n", ssne::gpu_renderer_msaa());
    std::vector<std::string> files;
    for (auto& e : std::filesystem::directory_iterator(in))
        if (e.path().extension() == ".bin") files.push_back(e.path().string());
    std::sort(files.begin(), files.end());
    std::filesystem::create_directories(out);
    auto finish = reinterpret_cast<void (*)()>(getproc("glFinish"));
    double total = 0;
    int timed = 0;
    unsigned long long draws[2] = {0, 0}, tris[2] = {0, 0};
    // Captures don't store the copy format in COPY_TEX commands; recover it from the textures that sample each copy.
    std::unordered_map<uint64_t, std::pair<uint8_t, bool>> copy_fmt;
    for (auto& fn : files) {
        auto f = ssne::gx::gpu_load_captured_frame(fn, err);
        if (!f) { printf("%s\n", err.c_str()); return 1; }
        for (auto& t : f->textures)
            if (t && t->efb_copy >= 0) copy_fmt[uint64_t(t->efb_copy)] = {t->copy_fmt, t->intensity};
        if (!tex_dir.empty())
            for (auto& t : f->textures) {
                if (!t || t->efb_copy >= 0 || t->rgba.size() < size_t(t->w) * t->h) continue;
                std::filesystem::create_directories(tex_dir);
                char name[96];
                snprintf(name, sizeof(name), "/tex_%016llx_%dx%d_f%d.pam", (unsigned long long)t->id, t->w, t->h, int(t->tex_fmt));
                if (FILE* o = fopen((tex_dir + name).c_str(), "wb")) {
                    fprintf(o, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", t->w, t->h);
                    fwrite(t->rgba.data(), 4, size_t(t->w) * t->h, o);
                    fclose(o);
                }
            }
    }
    // Frames are loaded once (loading a ~40 MB capture between timed frames let the GPU clock down).
    std::vector<std::unique_ptr<ssne::gx::GpuFrame>> loaded;
    for (size_t i = 0; i < files.size(); i++) {
        auto f = ssne::gx::gpu_load_captured_frame(files[i], err);
        if (!f) { printf("%s\n", err.c_str()); return 1; }
#ifdef SSNE_GPU_CMD_HAS_ZSNAP
        for (auto& c : f->cmds) {
            auto it = c.type == ssne::gx::GpuCmd::COPY_TEX ? copy_fmt.find(c.copy_id) : copy_fmt.end();
            if (it != copy_fmt.end()) { c.copy_fmt = it->second.first; c.intensity = it->second.second; c.copy_fmt_known = true; }
        }
#endif
        loaded.push_back(std::move(f));
    }
    std::vector<double> times;  // per frame, every repetition after the first (warm-up: uploads, shader compiles)
    for (int r = 0; r < repeat; r++)
        for (size_t i = 0; i < files.size(); i++) {
            auto f = std::make_unique<ssne::gx::GpuFrame>(*loaded[i]);
#ifdef SSNE_GPU_CMD_HAS_ZSNAP
            if (r == 0)
                for (const auto& c : f->cmds)
                    if (c.type == ssne::gx::GpuCmd::DRAW) { draws[c.zsnap]++; tris[c.zsnap] += c.idx_count / 3; }
#endif
            finish();
            auto t0 = std::chrono::steady_clock::now();
            ssne::gx::gpu_push_frame(std::move(f));
            ssne::gpu_renderer_run(1);
            finish();
            static auto get_error = reinterpret_cast<unsigned (*)()>(getproc("glGetError"));
            if (unsigned e = get_error()) printf("OpenGL error 0x%04X after frame %zu\n", e, i);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (int(i) >= skip && r == repeat - 1) { total += ms; timed++; }
            if (int(i) >= skip && (r > 0 || repeat == 1)) times.push_back(ms);
            if (r == repeat - 1 && int(i) >= skip) {
                std::vector<uint32_t> px;
                int w, h;
                if (ssne::gpu_renderer_read_output(px, w, h)) {
                    char name[64];
                    snprintf(name, sizeof(name), "/out_%05zu.ppm", i);
                    if (FILE* o = fopen((out + name).c_str(), "wb")) {
                        fprintf(o, "P6\n%d %d\n255\n", w, h);
                        for (uint32_t c : px) { uint8_t rgb[3] = {uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16)}; fwrite(rgb, 1, 3, o); }
                        fclose(o);
                    }
                }
            }
        }
    auto st = ssne::gpu_renderer_stats();
    printf("replayed %zu frames x %d, %llu draws, %llu programs (%llu from the shader cache, %.0f ms creating them)\n",
           files.size(), repeat, (unsigned long long)st.draws, (unsigned long long)st.programs,
           (unsigned long long)st.cached_programs, st.shader_ms);
    if (draws[0] + draws[1])
        printf("depth snapping: %llu of %llu draws (%llu of %llu triangles)\n", draws[1], draws[0] + draws[1], tris[1],
               tris[0] + tris[1]);
    if (getenv("SSNE_GPU_PROFILE")) printf("%s", ssne::gpu_renderer_profile().c_str());
    if (timing && timed) printf("average %.2f ms per frame (%d frames timed)\n", total / timed, timed);
    if (timing && times.size() > size_t(timed)) {
        std::sort(times.begin(), times.end());
        double sum = 0;
        for (double t : times) sum += t;
        printf("all repetitions after the first: median %.2f ms, mean %.2f ms per frame (%zu frames)\n", times[times.size() / 2],
               sum / double(times.size()), times.size());
    }
    return 0;
}
