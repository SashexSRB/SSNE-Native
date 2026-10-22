// ssne-headless: runs the recompiled game with no window, for bring-up and CI.
//   ssne-headless --data <extracted_game_dir> [--seconds N]
// Prints OSReport output and the reason execution stopped. A watchdog reports the last
// function entered if the guest spins (typically waiting on hardware that isn't emulated yet).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>

#include "ssne/recomp_hooks.h"
#include "ssne/audio.h"
#include "ssne/exi.h"
#include "ssne/gx_gpu.h"
#ifdef __linux__
#include <dlfcn.h>
#endif
#include "ssne/gx.h"
#include "ssne/hw.h"
#include "ssne/input.h"
#include "ssne/runtime.h"
#include "ssne/scheduler.h"
#include "ssne/thread_util.h"

[[noreturn]] static void throw_fatal(const char* m) { throw std::runtime_error(m); }

#ifdef __linux__
// Headless OpenGL for testing the GPU renderer: surfaceless EGL (Mesa), loaded at runtime.
static void* (*s_egl_getproc)(const char*) = nullptr;
static void* egl_getproc(const char* name) { return s_egl_getproc(name); }
static bool egl_init(std::string& err) {
    void* lib = dlopen("libEGL.so.1", RTLD_NOW);
    if (!lib) { err = "libEGL.so.1 not found"; return false; }
    using P = void*;
    s_egl_getproc = reinterpret_cast<void* (*)(const char*)>(dlsym(lib, "eglGetProcAddress"));
    auto get_platform = reinterpret_cast<P (*)(unsigned, void*, const int*)>(s_egl_getproc("eglGetPlatformDisplayEXT"));
    auto init = reinterpret_cast<unsigned (*)(P, int*, int*)>(dlsym(lib, "eglInitialize"));
    auto bind = reinterpret_cast<unsigned (*)(unsigned)>(dlsym(lib, "eglBindAPI"));
    auto create = reinterpret_cast<P (*)(P, P, P, const int*)>(dlsym(lib, "eglCreateContext"));
    auto make = reinterpret_cast<unsigned (*)(P, P, P, P)>(dlsym(lib, "eglMakeCurrent"));
    if (!get_platform || !init || !bind || !create || !make) { err = "EGL entry points missing"; return false; }
    P d = get_platform(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */, nullptr, nullptr);
    int ma, mi;
    if (!init(d, &ma, &mi)) { err = "eglInitialize failed"; return false; }
    bind(0x30A2 /* EGL_OPENGL_API */);
    const int attrs[] = {0x3098, 3, 0x30FB, 3, 0x30FD, 1, 0x3038};  // GL 3.3 core
    P ctx = create(d, nullptr, nullptr, attrs);
    if (!ctx) { err = "eglCreateContext failed"; return false; }
    make(d, nullptr, nullptr, ctx);
    return true;
}
#endif

int main(int argc, char** argv) {
    std::string data = "game_data";
    int seconds = 10;
    std::string dump_dir;
    int dump_ms = 500;
    std::string wav_path;
    bool gpu = false;
    float wide = 1.0f;
    int gpu_scale = 1;
    struct Press { uint16_t button; double at; double dur = 0.2; uint8_t sx = 128, sy = 128, cx = 128, cy = 128; };
    std::vector<Press> presses;
    double pause_at = -1, pause_len = 0;
    std::vector<double> ram_dumps;
    struct Aim { double t, yaw, pitch; };
    std::vector<Aim> aims;
    struct StateOp { double t; bool save; std::string path; };
    std::vector<StateOp> state_ops;  // --dump-ram 95,98,101: raw MEM1 snapshots to /tmp/ram_<t>.bin  // --press A@5.0,START@8,R@90:5 (hold 5 s)
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--data" && i + 1 < argc) data = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (a == "--dump-frames" && i + 1 < argc) dump_dir = argv[++i];
        else if (a == "--dump-ms" && i + 1 < argc) dump_ms = atoi(argv[++i]);
        else if (a == "--wav" && i + 1 < argc) wav_path = argv[++i];
        else if (a == "--memcard" && i + 1 < argc) ssne::exi_configure_memcard(argv[++i]);
        else if (a == "--no-memcard") ssne::exi_configure_memcard("");
        else if (a == "--audio-original") ssne::audio_set_hq(false);
        else if (a == "--mix" && i + 1 < argc) { int m = 100, x = 100; sscanf(argv[++i], "%d:%d", &m, &x); ssne::audio_set_mix(m, x); }
        else if (a == "--gpu") gpu = true;
        else if (a == "--skip-intros") ssne::set_skip_intros(true);
        else if ((a == "--save-state" || a == "--load-state") && i + 1 < argc) {  // --save-state T:path
            std::string v = argv[++i];
            size_t c = v.find(':');
            state_ops.push_back({atof(v.substr(0, c).c_str()), a == "--save-state", v.substr(c + 1)});
        }
        else if (a == "--ps-prompts") {  // PlayStation glyphs, Xbox-style layout
            const uint8_t menu[7] = {ssne::PS_CROSS, ssne::PS_CIRCLE, ssne::PS_R1, ssne::PS_L1, ssne::PS_TRIANGLE, ssne::PS_CROSS, ssne::PS_R2};
            const uint8_t ctl[7] = {ssne::PS_SQUARE, ssne::PS_CIRCLE, ssne::PS_R1, ssne::PS_L1, ssne::PS_TRIANGLE, ssne::PS_CROSS, ssne::PS_R2};
            ssne::set_button_prompts(menu, ctl);
        }
        else if (a == "--xbox-prompts") {  // Xbox layout button prompts (jump on A, use on X)
            const uint8_t menu[7] = {ssne::XB_A, ssne::XB_B, ssne::XB_RB, ssne::XB_LB, ssne::XB_Y, ssne::XB_A, ssne::XB_RT};
            const uint8_t ctl[7] = {ssne::XB_X, ssne::XB_B, ssne::XB_RB, ssne::XB_LB, ssne::XB_Y, ssne::XB_A, ssne::XB_RT};
            ssne::set_button_prompts(menu, ctl);
        }
        else if (a == "--fps" && i + 1 < argc) ssne::set_frame_limit(atoi(argv[++i]));
        else if (a == "--fov" && i + 1 < argc) ssne::set_fov(float(atof(argv[++i])));
        else if (a == "--aim" && i + 1 < argc) {  // --aim T:YAW:PITCH  one mouse-aim rotation (radians) at T seconds
            double t = 0, y = 0, p = 0;
            sscanf(argv[++i], "%lf:%lf:%lf", &t, &y, &p);
            aims.push_back({t, y, p});
        }
        else if (a == "--dump-ram" && i + 1 < argc) {
            std::string v = argv[++i];
            for (size_t pos = 0; pos < v.size();) {
                size_t c = v.find(',', pos);
                ram_dumps.push_back(atof(v.substr(pos, c - pos).c_str()));
                if (c == std::string::npos) break;
                pos = c + 1;
            }
        }
        else if (a == "--pause" && i + 1 < argc) {  // --pause T:D  pause the game at T seconds for D seconds
            std::string v = argv[++i];
            pause_at = atof(v.c_str());
            size_t c = v.find(':');
            pause_len = c == std::string::npos ? 1.0 : atof(v.substr(c + 1).c_str());
        }
        else if (a == "--norender" && i + 1 < argc) {
#ifdef _WIN32
            _putenv_s("SSNE_NORENDER", argv[++i]);
#else
            setenv("SSNE_NORENDER", argv[++i], 1);
#endif
        }  // seconds (1 = always)
        else if (a == "--widescreen" && i + 1 < argc) wide = float(atof(argv[++i]));
        else if (a == "--scale" && i + 1 < argc) gpu_scale = atoi(argv[++i]);
        else if (a == "--press" && i + 1 < argc) {
            std::string spec = argv[++i];
            size_t pos = 0;
            while (pos < spec.size()) {
                size_t comma = spec.find(',', pos);
                std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                size_t at = item.find('@');
                std::string name = item.substr(0, at);
                double t = atof(item.substr(at + 1).c_str());
                size_t colon = item.find(':');
                double dur = colon == std::string::npos ? 0.2 : atof(item.substr(colon + 1).c_str());
                size_t first = presses.size();
                static const struct { const char* n; uint16_t b; } kNames[] = {
                    {"A", ssne::PAD_A}, {"B", ssne::PAD_B}, {"X", ssne::PAD_X}, {"Y", ssne::PAD_Y},
                    {"START", ssne::PAD_START}, {"Z", ssne::PAD_Z}, {"L", ssne::PAD_L}, {"R", ssne::PAD_R},
                    {"UP", ssne::PAD_UP}, {"DOWN", ssne::PAD_DOWN}, {"LEFT", ssne::PAD_LEFT}, {"RIGHT", ssne::PAD_RIGHT}};
                for (auto& k : kNames) if (name == k.n) presses.push_back({k.b, t});
                // Analog stick directions use pseudo-button codes 0x8000+dir
                if (name == "SUP") presses.push_back({0x8001, t});
                if (name == "SDOWN") presses.push_back({0x8002, t});
                if (name == "SLEFT") presses.push_back({0x8003, t});
                if (name == "SRIGHT") presses.push_back({0x8004, t});
                if (name == "CLEFT") presses.push_back({0x8005, t});
                if (name == "CRIGHT") presses.push_back({0x8006, t});
                if (name == "CUP") presses.push_back({0x8007, t});
                if (name == "CDOWN") presses.push_back({0x8008, t});
                // RAW<x>_<y>: main stick at raw bytes x, y (128 = center), e.g. RAW1_1@60:4 = full down-left
                if (name.rfind("RAW", 0) == 0) {
                    int x = 128, y = 128, cx = 128, cy = 128;  // RAW<x>_<y>_<cx>_<cy>: C-stick too
                    sscanf(name.c_str() + 3, "%d_%d_%d_%d", &x, &y, &cx, &cy);
                    Press pr{0x8009, t};
                    pr.sx = uint8_t(x); pr.sy = uint8_t(y); pr.cx = uint8_t(cx); pr.cy = uint8_t(cy);
                    presses.push_back(pr);
                }
                for (size_t k = first; k < presses.size(); k++) presses[k].dur = dur;
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
    }
    if (!g_ssne_has_game_code) { puts("no game code linked"); return 1; }
    ssne::set_fatal_handler(throw_fatal);
    if (gpu) {
#ifdef __linux__
        std::string err;
        if (!egl_init(err) || !ssne::gpu_renderer_init(egl_getproc, gpu_scale, err)) { printf("GPU init failed: %s\n", err.c_str()); return 1; }
        ssne::gx_use_gpu(true);
        ssne::gpu_renderer_set_widescreen(wide);
#else
        puts("--gpu is only supported on Linux (EGL) in the headless runner"); return 1;
#endif
    }
    ssne::set_widescreen(wide);
    std::atomic<bool> done{false};
    ssne::spawn_guest_thread([&] {
        static PPCContext ctx;
        std::string err;
        uint32_t entry = ssne::boot(ctx, data, err);
        if (!entry) { printf("load failed: %s\n", err.c_str()); done = true; return; }
        printf("[headless] %zu functions, entry %08X\n", g_ssne_func_count, entry);
        try {
            ssne_call(ctx, entry);
            puts("[headless] entry returned");
        } catch (const std::exception& e) {
            printf("[headless] stopped: %s\n", e.what());
        }
        done = true;
    });
    auto t0 = std::chrono::steady_clock::now();
    // Optionally record the audio output (AI DMA) to a WAV file.
    FILE* wav = nullptr;
    uint32_t wav_frames = 0, wav_rate = 0;
    std::vector<int16_t> abuf;
    if (!wav_path.empty()) {
        wav = fopen(wav_path.c_str(), "wb");
        if (wav) { uint8_t hdr[44] = {}; fwrite(hdr, 1, 44, wav); ssne::audio_set_sink_active(true); }
    }
    auto drain_audio = [&] {
        if (!wav) return;
        uint32_t n = ssne::audio_buffered();
        if (!n) return;
        abuf.resize(n * 2);
        ssne::audio_pull(abuf.data(), n);
        if (!wav_rate) wav_rate = ssne::audio_rate();
        fwrite(abuf.data(), 4, n, wav);
        wav_frames += n;
    };
    // Optionally save the latest displayed frame periodically (PPM) for inspection.
    auto next_dump = t0;
    int dumped = 0;
    uint64_t seq = 0;
    std::vector<uint32_t> px;
    while (!done && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(seconds)) {
        if (gpu) {
            // Act as the render thread: execute recorded frames as they arrive.
            auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            while (std::chrono::steady_clock::now() < until) {
                if (!ssne::gpu_renderer_run(4)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        drain_audio();
        {   // scripted controller input: each press is held for 200 ms
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            ssne::PadState ps;
            for (auto& p : presses) {
                if (!(el >= p.at && el < p.at + p.dur)) continue;
                if (p.button == 0x8001) ps.stick_y = 230;
                else if (p.button == 0x8002) ps.stick_y = 26;
                else if (p.button == 0x8003) ps.stick_x = 26;
                else if (p.button == 0x8004) ps.stick_x = 230;
                else if (p.button == 0x8005) ps.cstick_x = 26;
                else if (p.button == 0x8006) ps.cstick_x = 230;
                else if (p.button == 0x8007) ps.cstick_y = 230;
                else if (p.button == 0x8008) ps.cstick_y = 26;
                else if (p.button == 0x8009) { ps.stick_x = p.sx; ps.stick_y = p.sy; ps.cstick_x = p.cx; ps.cstick_y = p.cy; }
                else ps.buttons |= p.button;
            }
            for (auto& op : state_ops)
                if (op.t >= 0 && el >= op.t) {
                    printf("[headless] %s state %s at %.2fs\n", op.save ? "saving" : "loading", op.path.c_str(), el);
                    ssne::savestate_request(op.save, op.path);
                    op.t = -1;
                }
            { ssne::SavestateResult res; if (ssne::savestate_take_result(res)) printf("[headless] state result: %s (%s)\n", res.message.c_str(), res.ok ? "ok" : "failed"); }
            if (pause_at >= 0) ssne::set_paused(el >= pause_at && el < pause_at + pause_len);
            for (Aim& a : aims)
                if (a.t >= 0 && el >= a.t) {
                    printf("[headless] mouse aim %+.3f %+.3f (active %d)\n", a.yaw, a.pitch, int(ssne::mouse_aim_active()));
                    ssne::mouse_aim_add(a.yaw, a.pitch);
                    a.t = -1;
                }
            for (double& t : ram_dumps)
                if (t >= 0 && el >= t) {
                    char fn[64];
                    snprintf(fn, sizeof(fn), "/tmp/ram_%g.bin", t);
                    if (FILE* f = fopen(fn, "wb")) { fwrite(ssne::g_ram, 1, 24u << 20, f); fclose(f); }
                    printf("[headless] RAM snapshot %s at %.2fs\n", fn, el);
                    t = -1;
                }
            if (ps.buttons & ssne::PAD_R) ps.trigger_r = 255;
            if (ps.buttons & ssne::PAD_L) ps.trigger_l = 255;
            ssne::set_pad(0, ps);
        }
        {   // every 5 s: frames per second (display copies)
            static auto last = t0;
            static uint64_t last_copies = 0;
            auto now = std::chrono::steady_clock::now();
            if (now - last >= std::chrono::seconds(5)) {
                uint64_t copies = ssne::gx_render_stats().display_copies;
                double dt = std::chrono::duration<double>(now - last).count();
                auto au = ssne::audio_stats();
                if (gpu) { auto gs = ssne::gpu_renderer_stats(); printf("[gpu] frames %llu draws %llu tris %llu copies %llu uploads %llu reinterp %llu programs %llu (%.0f ms compiling) last frame %.1f ms\n", (unsigned long long)gs.frames, (unsigned long long)gs.draws, (unsigned long long)gs.triangles, (unsigned long long)gs.tex_copies, (unsigned long long)gs.uploads, (unsigned long long)gs.reinterpretations, (unsigned long long)gs.programs, gs.shader_ms, gs.frame_ms); }
                printf("[perf] t=%.0fs  %.1f fps  | audio: %llu cmdlists, %u voices playing, %llu AI frames\n",
                       std::chrono::duration<double>(now - t0).count(), double(copies - last_copies) / dt,
                       (unsigned long long)au.cmdlists, au.active_voices, (unsigned long long)au.ai_frames);
                fflush(stdout);
                last = now;
                last_copies = copies;
            }
        }
        if (!dump_dir.empty() && std::chrono::steady_clock::now() >= next_dump) {
            next_dump += std::chrono::milliseconds(dump_ms);
            int w, h;
            bool have = false;
            if (gpu) { static uint64_t gseq = 0; auto o = ssne::gpu_renderer_output(); if (o.seq != gseq && ssne::gpu_renderer_read_output(px, w, h)) { gseq = o.seq; have = true; } }
            else have = ssne::gx_get_frame(px, w, h, seq);
            if (have) {
                char name[512];
                long ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
                snprintf(name, sizeof(name), "%s/t%06ld.ppm", dump_dir.c_str(), ms);
                dumped++;
                if (FILE* f = fopen(name, "wb")) {
                    fprintf(f, "P6\n%d %d\n255\n", w, h);
                    for (uint32_t c : px) { uint8_t rgb[3] = {uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16)}; fwrite(rgb, 1, 3, f); }
                    fclose(f);
                }
            }
        }
    }
    if (wav) {
        drain_audio();
        auto le32 = [](uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i)); };
        uint8_t h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0};
        if (!wav_rate) wav_rate = 32000;
        le32(h + 4, 36 + wav_frames * 4);
        le32(h + 24, wav_rate);
        le32(h + 28, wav_rate * 4);
        h[32] = 4; h[34] = 16;
        memcpy(h + 36, "data", 4);
        le32(h + 40, wav_frames * 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, 44, wav);
        fclose(wav);
        printf("[headless] wrote %s (%u frames @ %u Hz)\n", wav_path.c_str(), wav_frames, wav_rate);
    }
    if (!done) {
        auto st = ssne::scheduler_stats();
        auto hw = ssne::hw_stats();
        printf("[headless] still running after %ds: %d guest threads (%d ready), %llu interrupts, %llu retraces, "
               "%llu ARAM DMAs, %llu disc reads (%llu KB)\n", seconds, st.threads, st.ready,
               (unsigned long long)st.interrupts, (unsigned long long)st.retraces, (unsigned long long)hw.aram_dmas,
               (unsigned long long)hw.disc_reads, (unsigned long long)(hw.disc_bytes / 1024));
        auto gx = ssne::gx_stats();
        printf("[headless] GX: %llu KB stream, %llu commands, %llu primitives (%llu vertices), %llu display lists, "
               "%llu EFB copies, %llu draw-done, %llu tokens (last %u)%s\n",
               (unsigned long long)(gx.bytes / 1024), (unsigned long long)gx.commands, (unsigned long long)gx.primitives,
               (unsigned long long)gx.vertices, (unsigned long long)gx.display_lists, (unsigned long long)gx.efb_copies,
               (unsigned long long)gx.draw_done, (unsigned long long)gx.tokens, gx.last_token,
               gx.unknown_opcode ? " [stream desync seen]" : "");
        auto rs = ssne::gx_render_stats();
        printf("[headless] render: %llu triangles, %llu pixels, %llu display copies, %llu texture copies, "
               "%llu textures decoded, %llu line/point prims skipped\n",
               (unsigned long long)rs.triangles, (unsigned long long)rs.pixels, (unsigned long long)rs.display_copies,
               (unsigned long long)rs.tex_copies, (unsigned long long)rs.textures_decoded, (unsigned long long)rs.lines_skipped);
        fflush(stdout);
        std::_Exit(3);
    }
    std::_Exit(0);
    return 0;
}
