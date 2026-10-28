// High-level emulation: loading the game and replacing Dolphin SDK functions with native code.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <vector>
#include <set>
#include <mutex>
#include <thread>
#include <fstream>
#include <iterator>
#include <string>

#include "ssne/gx.h"
#include "gc/dol.h"
#include "ssne/hw.h"
#include "ssne/input.h"
#include "ssne/recomp_hooks.h"
#include "ssne/scheduler.h"
#include "ssne/runtime.h"

namespace ssne {

namespace {
LogSink g_log_sink = nullptr;
}

void set_log_sink(LogSink sink) { g_log_sink = sink; }
void log_message(const std::string& line) {
    fprintf(stdout, "%s\n", line.c_str());
    fflush(stdout);
    if (g_log_sink) g_log_sink(line.c_str());
}

static std::vector<uint8_t> slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

uint32_t load_game(const std::string& data_dir, std::string& error) {
    auto data = slurp(data_dir + "/sys/main.dol");
    if (data.empty()) { error = "main.dol not found in " + data_dir + "/sys (run the launcher to install your disc)"; return 0; }
    gc::Dol dol;
    if (!gc::Dol::parse(data, dol, error)) return 0;
    memset(g_ram, 0, kRamSize);
    dol.load_into(g_ram, kRamSize);
    boot_globals();

    // What the IPL + apploader leave in memory (YAGCD 4.2 "OS globals"):
    auto boot = slurp(data_dir + "/sys/boot.bin");
    auto bi2 = slurp(data_dir + "/sys/bi2.bin");
    auto fst = slurp(data_dir + "/sys/fst.bin");
    if (boot.size() >= 0x20) memcpy(g_ram, boot.data(), 0x20);              // disc ID @ 0x80000000
    if (!fst.empty()) {
        uint32_t fst_addr = (0x81800000u - uint32_t(fst.size())) & ~31u;
        memcpy(g_ram + (fst_addr & 0x01FFFFFF), fst.data(), fst.size());
        mem_write32(0x80000038, fst_addr);                                   // FST location
        mem_write32(0x8000003C, uint32_t(fst.size()));                       // FST max size
        if (bi2.size() >= 0x2000) {
            uint32_t bi2_addr = (fst_addr - 0x2000) & ~31u;
            memcpy(g_ram + (bi2_addr & 0x01FFFFFF), bi2.data(), 0x2000);
            mem_write32(0x800000F4, bi2_addr);                               // BI2 location
        }
    }
    mem_write32(0x800000CC, 0);                                              // TV mode: NTSC
    mem_write32(0x800030D8, 0);                                              // system time (low)

    if (!disc_open(data_dir, error)) return 0;
    return dol.entry;
}

uint32_t boot(PPCContext& ctx, const std::string& data_dir, std::string& error) {
    uint32_t entry = load_game(data_dir, error);
    if (!entry) return 0;
    reset_cpu(ctx);
    install_sdk_hooks();
    install_thread_hooks();
    scheduler_init(ctx);
    return entry;
}

namespace {

std::string guest_string(uint32_t addr, size_t max = 1024) {
    std::string s;
    if (!addr) return "(null)";
    for (uint32_t a = addr; s.size() < max; a++) { char c = char(mem_read8(a)); if (!c) break; s += c; }
    return s;
}

// printf for guest varargs (PowerPC EABI): integer/pointer args in r3..r10, doubles in f1..f8.
std::string guest_format(PPCContext& ctx, uint32_t fmt_addr, int first_gpr) {
    std::string fmt = guest_string(fmt_addr), out;
    int gpr = first_gpr, fpr = 1;
    auto next_int = [&]() -> uint32_t { return gpr <= 10 ? ctx.r[gpr++] : 0; };
    auto next_dbl = [&]() -> double { return fpr <= 8 ? ctx.ps0[fpr++] : 0.0; };
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        size_t j = i + 1;
        while (j < fmt.size() && strchr("-+ #0", fmt[j])) j++;
        if (j < fmt.size() && fmt[j] == '*') j++;
        while (j < fmt.size() && isdigit(uint8_t(fmt[j]))) j++;
        if (j < fmt.size() && fmt[j] == '.') { j++; while (j < fmt.size() && isdigit(uint8_t(fmt[j]))) j++; }
        while (j < fmt.size() && strchr("hlLqjzt", fmt[j])) j++;
        if (j >= fmt.size()) break;
        std::string spec = fmt.substr(i, j - i);        // flags/width/precision, no length modifiers
        spec.erase(std::remove_if(spec.begin(), spec.end(), [](char c) { return strchr("hlLqjzt", c); }), spec.end());
        char conv = fmt[j], buf[512];
        buf[0] = 0;
        switch (conv) {
        case '%': strcpy(buf, "%"); break;
        case 'd': case 'i': snprintf(buf, sizeof(buf), (spec + "d").c_str(), int32_t(next_int())); break;
        case 'u': case 'x': case 'X': case 'o': case 'c':
            snprintf(buf, sizeof(buf), (spec + conv).c_str(), next_int()); break;
        case 'p': snprintf(buf, sizeof(buf), "0x%08X", next_int()); break;
        case 's': snprintf(buf, sizeof(buf), (spec + "s").c_str(), guest_string(next_int(), 400).c_str()); break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            snprintf(buf, sizeof(buf), (spec + conv).c_str(), next_dbl()); break;
        default: out += spec + conv; i = j; continue;
        }
        out += buf;
        i = j;
    }
    return out;
}

// --- Native replacements. Each one fully replaces the game's copy of the SDK function.
void hle_OSReport(PPCContext& ctx) {
    std::string s = guest_format(ctx, ctx.r[3], 4);
    // Split into lines for the log sink; keep stdout output for headless runs.
    static std::string pending;
    pending += s;
    size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, nl);
        pending.erase(0, nl + 1);
        fprintf(stdout, "[game] %s\n", line.c_str());
        if (g_log_sink) g_log_sink(line.c_str());
    }
    fflush(stdout);
}

void hle_OSPanic(PPCContext& ctx) {
    // OSPanic(const char* file, int line, const char* fmt, ...)
    std::string msg = guest_format(ctx, ctx.r[5], 6);
    fatal(("OSPanic at " + guest_string(ctx.r[3], 128) + ":" + std::to_string(ctx.r[4]) + ": " + msg).c_str());
}

void hle_nop(PPCContext&) {}

// The IPL's SRAM isn't emulated (EXI reads return zeros), which reads as "mono" and would make the game mix
// every voice to the center. Report the console's sound setting as stereo.
void hle_OSGetSoundMode(PPCContext& ctx) { ctx.r[3] = 1; }  // OS_SOUND_MODE_STEREO

// ---- Widescreen: the game's camera projections all go through the SDK's C_MTXPerspective (and projected
// textures through C_MTXLightPerspective). The replacements below compute the same matrices (same SDK constants,
// the game's own tanf) with the aspect ratio multiplied by the widescreen factor, which widens the horizontal
// field of view; the renderer and display then show the image at the wider aspect.
std::atomic<float> g_widescreen{1.0f};

// Addresses of the game functions the port hooks, per version of main.dol (the disc ID is at 0x80000000). The PAL
// main.dol has the same functions as the USA one, shifted (see data/symbols/G3BP9G_r0.sym); every hook still checks
// the code it patches before installing.
struct GameAddresses {
    const char* id;
    uint32_t look_update;    // player look update (mouse aim)
    uint32_t flip_bl;        // CcEngine::Flip: bl VIWaitForRetrace
    uint32_t vi_wait;        // VIWaitForRetrace
    uint32_t tanf;           // the game's tanf (used by the projection replacements)
    uint32_t mtx_persp;      // C_MTXPerspective
    uint32_t mtx_light;      // C_MTXLightPerspective
    uint32_t weapon_lr;      // return address of the first-person weapon's projection call (gameplay detector)
    uint32_t cam_lr[4];      // return addresses of the scene camera's projection calls (FOV slider)
    uint32_t pad_clamp;      // PADClamp (called after PADRead in the VI retrace callback)
    uint32_t pad_process;    // the game's controller processing (rescales the sticks in place)
    uint32_t flip;           // CcEngine::Flip (save states are taken at its start)
    uint32_t player_update;  // the player's per-frame update (r3 = player), on foot and in vehicles
};
const GameAddresses kVersions[] = {
    {"G3BE9G", 0x800BFCA4, 0x8011855C, 0x801BF964, 0x80181590, 0x801BA874, 0x801BA5EC, 0x8014CDF0,
     {0x8014C690, 0x8014C6B8, 0x8014C6D8, 0x8014C6FC}, 0x801C0BAC, 0x8014F83C, 0x801183F0, 0x800BEEC8},  // USA Rev 1 (FINAL.107)
    {"G3BP9G", 0x800BFCA4, 0x80118514, 0x801BF904, 0x80181530, 0x801BA814, 0x801BA58C, 0x8014CDA8,
     {0x8014C648, 0x8014C670, 0x8014C690, 0x8014C6B4}, 0x801C0B4C, 0x8014F7F4, 0x801183A8, 0x800BEEC8},  // Europe (FINAL.113)
};
const GameAddresses* g_ver = &kVersions[0];
void select_version() {
    char id[7] = {};
    for (int i = 0; i < 6; i++) id[i] = char(mem_read8(0x80000000u + uint32_t(i)));
    for (const auto& v : kVersions)
        if (!strcmp(v.id, id)) { g_ver = &v; fprintf(stdout, "[ssne] game version %s\n", id); return; }
    fprintf(stdout, "[ssne] unknown game version %s: using USA addresses (hooks verify their code first)\n", id);
}
// ---- player control. The player's update (800BEEC8, hooked below) runs once in each frame the player controls Sam,
// on foot or driving, between the frame's drawing and its flip; it doesn't run in the pause menu, NETRICSA, briefings
// or cutscenes. So the frame after a flip is a player-control frame if the player update ran before that flip (the
// first frame of a menu still is: the game opens it in that frame's update).
// Without both hooks (another game build), gameplay is detected by the weapon alone.
std::atomic<bool> g_player_seen{false}, g_player_control{false};
std::atomic<int> g_control_hooks{0};  // player update and flip hooks installed (2: player control is known)
void end_player_frame() { g_player_control = g_player_seen.exchange(false); }

// Only gameplay is widened: the front end, cutscenes and the in-game menus (pause, NETRICSA, briefings) place 3D
// models in 4:3 2D layouts, so they stay 4:3 (the display fills the sides). Gameplay is detected by the first-person
// weapon's projection (called from 8014CDF0), which only exists while playing, in player-control frames.
std::atomic<int64_t> g_last_gameplay_ms{-1000000};
std::atomic<float> g_frame_factor{1.0f};  // factor used by the most recent camera projection
int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
float effective_widescreen(uint32_t caller) {
    if (caller == g_ver->weapon_lr) g_last_gameplay_ms = now_ms();
    bool gameplay = now_ms() - g_last_gameplay_ms.load() < 400 && (g_control_hooks.load() < 2 || g_player_control.load());
    float f = gameplay ? g_widescreen.load() : 1.0f;
    // The GX thread reads the factor when it records the frame's display copy: let it finish the frames rendered
    // with the old factor first (the factor changes only between gameplay and the menus).
    if (f != g_frame_factor.load()) gx_sync();
    g_frame_factor = f;
    return f;
}

// Gameplay field of view (FOV slider). The scene camera (8014C090) builds its projections from these call
// sites; the first-person weapon (8014CDF0) keeps its own. 0 = the game's own FOV (63 degrees vertical).
std::atomic<float> g_fov{0.0f};
constexpr float kGameFov = 63.0f;
bool scene_camera_caller(uint32_t lr) {
    return lr == g_ver->cam_lr[0] || lr == g_ver->cam_lr[1] || lr == g_ver->cam_lr[2] || lr == g_ver->cam_lr[3];
}
// Widens (or narrows) the game's vertical FOV by the ratio of the chosen FOV to the default, applied to the
// tangent so the game's own zooms keep their proportions.
float adjust_fov(uint32_t lr, float fovy) {
    float target = g_fov.load();
    if (target <= 0 || !scene_camera_caller(lr) || now_ms() - g_last_gameplay_ms.load() >= 400) return fovy;
    constexpr float d2r = 0.017453292f;
    float k = std::tan(0.5f * target * d2r) / std::tan(0.5f * kGameFov * d2r);
    float out = 2.0f * std::atan(std::tan(0.5f * fovy * d2r) * k) / d2r;
    return std::clamp(out, 1.0f, 170.0f);
}

inline float sda2_float(PPCContext& ctx, int32_t off) { return u2f(mem_read32(ctx.r[13] + uint32_t(off))); }
inline void put_f(uint32_t a, float v) { mem_write32(a, f2u(v)); }

float game_tanf(PPCContext& ctx, float x) {
    static PPCFunc tanf_fn = [] { const SSNEFuncEntry* e = ssne_find_func(g_ver->tanf); return e ? e->func : nullptr; }();
    if (!tanf_fn) return std::tan(x);
    ctx.ps0[1] = ctx.ps1[1] = double(x);
    tanf_fn(ctx);
    return float(ctx.ps0[1]);
}

// C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f)  @ 801BA874
void hle_MTXPerspective(PPCContext& ctx) {
    if (static const char* tp = getenv("SSNE_TRACE_PROJ"); tp) {
        static const int every = std::max(1, atoi(tp));
        static std::map<uint32_t, int> seen;
        if (seen[ctx.lr]++ % every == 0)
            fprintf(stdout, "[proj] persp from %08X aspect %.3f fovy %.2f (x%d)\n", ctx.lr, ctx.ps0[2], ctx.ps0[1], seen[ctx.lr]);
    }
    uint32_t m = ctx.r[3];
    float aspect = float(ctx.ps0[2]) * effective_widescreen(ctx.lr), n = float(ctx.ps0[3]), f = float(ctx.ps0[4]);
    float fovy = adjust_fov(ctx.lr, float(ctx.ps0[1]));
    float half = sda2_float(ctx, -0x44B0), d2r = sda2_float(ctx, -0x44AC);  // 0.5, pi/180
    float one = sda2_float(ctx, -0x44C0), zero = sda2_float(ctx, -0x44B8), mone = sda2_float(ctx, -0x44B4);
    float angle = d2r * (half * fovy);
    float cot = one / game_tanf(ctx, angle);
    float tmp = one / (f - n);
    float r[16] = {cot / aspect, zero, zero, zero,  zero, cot, zero, zero,
                   zero, zero, -n * tmp, -(f * n) * tmp,  zero, zero, mone, zero};
    for (int i = 0; i < 16; i++) put_f(m + i * 4, r[i]);
}

// C_MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT, f32 transS, f32 transT)  @ 801BA5EC
void hle_MTXLightPerspective(PPCContext& ctx) {
    if (getenv("SSNE_TRACE_PROJ")) { static std::map<uint32_t, int> seen; if (seen[ctx.lr]++ % 600 == 0) fprintf(stdout, "[proj] light from %08X aspect %.3f fovy %.1f (x%d)\n", ctx.lr, ctx.ps0[2], ctx.ps0[1], seen[ctx.lr]); }
    uint32_t m = ctx.r[3];
    float aspect = float(ctx.ps0[2]) * effective_widescreen(ctx.lr);
    float fovy = adjust_fov(ctx.lr, float(ctx.ps0[1]));
    float sS = float(ctx.ps0[3]), sT = float(ctx.ps0[4]), tS = float(ctx.ps0[5]), tT = float(ctx.ps0[6]);
    float half = sda2_float(ctx, -0x4518), d2r = sda2_float(ctx, -0x4504);
    float one = sda2_float(ctx, -0x4520), zero = sda2_float(ctx, -0x451C), mone = sda2_float(ctx, -0x4508);
    float angle = d2r * (half * fovy);
    float cot = one / game_tanf(ctx, angle);
    float r[12] = {sS * (cot / aspect), zero, -tS, zero,  zero, cot * sT, -tT, zero,  zero, zero, mone, zero};
    for (int i = 0; i < 12; i++) put_f(m + i * 4, r[i]);
}

// Installed by address, only if the code at that address is the expected SDK function (USA Rev 1 main.dol).
struct AddressHook { uint32_t addr; uint32_t sig[6]; PPCFunc fn; const char* name; };
std::vector<AddressHook> address_hooks() {
    return {
        {g_ver->mtx_persp, {0x7C0802A6, 0x90010004, 0x9421FFC0, 0xDBE10038, 0xFFE02090, 0xDBC10030}, hle_MTXPerspective,
         "C_MTXPerspective"},
        {g_ver->mtx_light, {0x7C0802A6, 0x90010004, 0x9421FFA8, 0xDBE10050, 0xDBC10048, 0xDBA10040}, hle_MTXLightPerspective,
         "C_MTXLightPerspective"},
    };
}

struct NamedHook { const char* name; PPCFunc fn; };
const NamedHook kHooks[] = {
    {"OSReport", hle_OSReport},
    {"OSPanic", hle_OSPanic},
    {"OSGetSoundMode", hle_OSGetSoundMode},
    // Hardware-poking init routines that are meaningless on PC:
    {"__OSInitAudioSystem", hle_nop},
    {"OSInitCache", hle_nop},
    {"ICFlashInvalidate", hle_nop},
    {"DCFlushRange", hle_nop},
    {"DCStoreRange", hle_nop},
    {"DCInvalidateRange", hle_nop},
    {"ICInvalidateRange", hle_nop},
    // TODO(roadmap): OSThread*, OSAlarm*, DVD*, PAD*, VI*, GX*, AX*/AI*, CARD*, ARQ*
};

} // namespace

// ---- debug: pass-through call tracer (SSNE_TRACE_CALLS=addr[,addr...], hex). Counts calls per second and prints
// a guest backtrace (LR chain) for the first calls of each function. Used to find the game's frame pacing.
namespace {
struct CallTrace { uint32_t addr = 0; PPCFunc orig = nullptr; std::atomic<uint64_t> calls{0}; int printed = 0; };
CallTrace g_ct[8];
int g_ct_n = 0;
template <int N>
void trace_tramp(PPCContext& ctx) {
    CallTrace& t = g_ct[N];
    t.calls++;
    static std::mutex seen_mu;
    static std::set<uint64_t> seen;  // (lr, r3) pairs already reported
    bool fresh;
    {
        std::lock_guard<std::mutex> lk(seen_mu);
        fresh = t.printed < 40 && seen.insert(uint64_t(t.addr) << 32 ^ uint64_t(ctx.lr) << 16 ^ ctx.r[3]).second;
        static const bool all = getenv("SSNE_TRACE_CALLS_ALL") != nullptr;  // every call, up to 5 per second
        if (all) {
            static int64_t sec = 0; static int n = 0;
            int64_t now = now_ms() / 1000;
            if (now != sec) { sec = now; n = 0; }
            if (n++ < 5) fprintf(stdout, "[call] %lld %08X lr=%08X r3=%08X r4=%08X\n", (long long)now, t.addr, ctx.lr, ctx.r[3], ctx.r[4]);
        }
    }
    if (fresh) {
        t.printed++;
        fprintf(stdout, "[calls] t=%.2f ", double(now_ms() % 1000000) / 1000.0);
        fprintf(stdout, "[calls] %08X from %08X", t.addr, ctx.lr);
        uint32_t sp = ctx.r[1];
        for (int i = 0; i < 10 && sp >= 0x80000000u && sp < 0x81800000u; i++) {
            uint32_t next = mem_read32(sp);
            if (next < 0x80000000u || next >= 0x81800000u) break;
            fprintf(stdout, " <- %08X", mem_read32(next + 4));
            sp = next;
        }
        fprintf(stdout, "  r3=%08X r4=%08X f1=%g\n", ctx.r[3], ctx.r[4], ctx.ps0[1]);
    }
    PPCFunc* slot = ssne_hook_slot(t.addr);
    *slot = nullptr;
    t.orig(ctx);
    *slot = &trace_tramp<N>;
}
PPCFunc kTramps[8] = {trace_tramp<0>, trace_tramp<1>, trace_tramp<2>, trace_tramp<3>,
                      trace_tramp<4>, trace_tramp<5>, trace_tramp<6>, trace_tramp<7>};
void install_call_tracer() {
    const char* e = getenv("SSNE_TRACE_CALLS");
    if (!e) return;
    std::string list = e;
    size_t pos = 0;
    while (pos < list.size() && g_ct_n < 8) {
        size_t c = list.find(',', pos);
        uint32_t a = uint32_t(strtoul(list.substr(pos, c - pos).c_str(), nullptr, 16));
        const SSNEFuncEntry* f = ssne_find_func(a);
        if (f && !*ssne_hook_slot(a)) {
            g_ct[g_ct_n].addr = a;
            g_ct[g_ct_n].orig = f->func;
            ssne_install_hook(a, kTramps[g_ct_n]);
            g_ct_n++;
        } else fprintf(stdout, "[calls] can't trace %08X\n", a);
        if (c == std::string::npos) break;
        pos = c + 1;
    }
    std::thread([] {
        std::vector<uint64_t> last(8, 0);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            fprintf(stdout, "[calls/s]");
            for (int i = 0; i < g_ct_n; i++) { uint64_t c = g_ct[i].calls; fprintf(stdout, " %08X:%llu", g_ct[i].addr, (unsigned long long)(c - last[i])); last[i] = c; }
            fprintf(stdout, "\n");
            fflush(stdout);
        }
    }).detach();
}
} // namespace

// ---- vehicles. The player's per-frame update (800BEEC8, r3 = player) runs on foot and while driving; the
// player's vehicle (CcBaseVehicle::Enter / Exit) is at +0x6C4, 0 on foot. The jeep's physics only work at the
// original time step: above ~120 fps it jolts, and once stopped it no longer drives or steers (it still does at
// 60 fps from the same state). So frames are paced at 60 fps while the player is in a vehicle.
namespace {
const uint32_t kPlayerUpdateSig[6] = {0x9421FF18u, 0x7C0802A6u, 0xDB6100C0u, 0xDB8100C8u, 0xDBA100D0u, 0xDBC100D8u};
PPCFunc g_orig_player_update = nullptr;
std::atomic<int64_t> g_vehicle_last_ms{-100000};

// ---- weapon number keys. The player's weapons by id: 0 chainsaw, 1 Desert Hawk, 2 Uzis, 3 double shotgun,
// 4 minigun, 5 grenade launcher, 6 rocket launcher, 7 flamethrower (gas gun), 8 sniper rifle, 9 Sirian power gun,
// 10 cannon, 11 Serious Bomb; player+0x5DC is the weapon in hand, +0x5F8 the one highlighted in the weapon bar /
// wheel, +0x5E0 the previous one. Next weapon (800C59CC, X) steps the highlight to the next id that is owned
// (800C6170) and has ammo (800C61A0) and marks a switch: the weapon in hand lowers, and the weapon update (800C4EF8,
// part of the player update) then commits the highlight (800C5C18) and raises it. A number key picks its group's
// first available weapon (the group's next one if one of them is already in hand or highlighted), taps X so the game
// starts a normal switch, and once the game has taken the tap (highlight != weapon in hand) puts the chosen id into
// the highlight: the game finishes the switch with it. Same addresses on PAL (unchanged below 800E270C).
const int kWeaponGroups[9][2] = {{0, -1}, {1, -1}, {3, -1}, {2, 4}, {6, 5}, {9, 7}, {8, -1}, {10, -1}, {11, -1}};
const uint32_t kWeaponOwned = 0x800C6170, kWeaponHasAmmo = 0x800C61A0;
const uint32_t kWeaponOwnedSig[6] = {0x1C840018u, 0x386304CCu, 0x39200000u, 0x7C03202Eu, 0x2C000000u, 0x40820010u};
const uint32_t kWeaponHasAmmoSig[6] = {0x9421FFE8u, 0x7C0802A6u, 0xBF810008u, 0x9001001Cu, 0x1FA40018u, 0x7C7C1B78u};
PPCFunc g_weapon_owned = nullptr, g_weapon_has_ammo = nullptr;
std::atomic<int> g_weapon_req{-1};
std::atomic<int64_t> g_weapon_req_ms{0};
int g_weapon_target = -1;      // chosen weapon waiting for the X tap to be taken
int64_t g_weapon_deadline = 0;

bool weapon_available(PPCContext& ctx, uint32_t player, int id) {
    PPCContext saved = ctx;  // the game's functions run on the caller's registers; give them back untouched
    ctx.r[3] = player; ctx.r[4] = uint32_t(id);
    g_weapon_owned(ctx);
    bool ok = ctx.r[3] != 0;
    if (ok) {
        ctx.r[3] = player; ctx.r[4] = uint32_t(id);
        g_weapon_has_ammo(ctx);
        ok = ctx.r[3] != 0;
    }
    ctx = saved;
    return ok;
}

void weapon_keys(PPCContext& ctx, uint32_t player) {
    int cur = int(mem_read32(player + 0x5DC)), sel = int(mem_read32(player + 0x5F8));
    if (cur < 0 || cur > 11 || sel < 0 || sel > 11) return;
    int group = g_weapon_req.exchange(-1);
    if (group >= 0 && group < 9 && now_ms() - g_weapon_req_ms.load() < 500) {
        const int* g = kWeaponGroups[group];
        int n = g[1] < 0 ? 1 : 2, from = 0, base = -1;
        for (int i = 0; i < n; i++) if (g[i] == sel || (base < 0 && g[i] == cur)) { base = g[i]; from = i + 1; }
        int target = -1;
        for (int k = 0; k < n && target < 0; k++) {
            int id = g[(from + k) % n];
            if (id != base && weapon_available(ctx, player, id)) target = id;
        }
        if (target >= 0 && target != cur) {
            if (sel != cur) mem_write32(player + 0x5F8, uint32_t(target));  // a switch is under way: retarget it
            else { inject_buttons(PAD_X, 60); g_weapon_target = target; g_weapon_deadline = now_ms() + 500; }
        }
    }
    if (g_weapon_target >= 0) {
        if (sel != cur) { mem_write32(player + 0x5F8, uint32_t(g_weapon_target)); g_weapon_target = -1; }
        else if (now_ms() > g_weapon_deadline) g_weapon_target = -1;
    }
}

void hle_player_update(PPCContext& ctx) {
    uint32_t player = ctx.r[3];
    PPCFunc* slot = ssne_hook_slot(g_ver->player_update);
    *slot = nullptr;
    g_orig_player_update(ctx);
    *slot = &hle_player_update;
    if (player < 0x80000000u || player >= 0x81800000u) return;
    g_player_seen = true;
    uint32_t vehicle = mem_read32(player + 0x6C4);
    if (vehicle >= 0x80000000u && vehicle < 0x81800000u) g_vehicle_last_ms = now_ms();
    else if (g_weapon_owned && g_weapon_has_ammo) weapon_keys(ctx, player);
}

void install_vehicle_detector() {
    const SSNEFuncEntry* f = ssne_find_func(g_ver->player_update);
    bool ok = f;
    for (int k = 0; k < 6 && ok; k++) ok = mem_read32(g_ver->player_update + uint32_t(k) * 4) == kPlayerUpdateSig[k];
    if (!ok) { fprintf(stdout, "[ssne] vehicles: player update not found (different game build?)\n"); return; }
    g_orig_player_update = f->func;
    ssne_install_hook(g_ver->player_update, hle_player_update);
    g_control_hooks++;
    const SSNEFuncEntry* own = ssne_find_func(kWeaponOwned);
    const SSNEFuncEntry* ammo = ssne_find_func(kWeaponHasAmmo);
    bool wk = own && ammo;
    for (int k = 0; k < 6 && wk; k++)
        wk = mem_read32(kWeaponOwned + uint32_t(k) * 4) == kWeaponOwnedSig[k] &&
             mem_read32(kWeaponHasAmmo + uint32_t(k) * 4) == kWeaponHasAmmoSig[k];
    if (wk) { g_weapon_owned = own->func; g_weapon_has_ammo = ammo->func; }
    else fprintf(stdout, "[ssne] weapon keys: weapon functions not found (different game build?)\n");
}

bool in_vehicle() { return now_ms() - g_vehicle_last_ms.load() < 200; }
} // namespace

// ---- frame rate limit. The game measures real frame time (median of the last 5 frames, 8015544C) and
// CcEngine::Flip waits for the next VI retrace when no retrace happened since the previous flip, capping it at
// 59.94 fps. With a different limit that wait is skipped and frames are paced on the host clock instead
// (frame_limiter_wait, called at each display copy). Limits above 60 (and unlimited) only apply while playing on
// foot (the look update runs every gameplay frame): everywhere else the game uses its own 60 fps pacing. The front
// end, the pause screen and cutscenes animate per frame (menu lines scrolled, the player model spun and the title's
// bomb pulsed twice as fast at 120 fps), and vehicles need the original time step (see in_vehicle; the look update
// doesn't run while driving).
namespace {
std::atomic<int> g_frame_limit{60};
int effective_frame_limit() {
    int lim = g_frame_limit.load();
    if (lim > 0 && lim <= 60) return lim;
    if (!mouse_aim_available()) return in_vehicle() ? 60 : lim;  // look update not found: only vehicles are known
    return mouse_aim_active() ? lim : 60;
}
PPCFunc g_orig_vi_wait = nullptr;
// CcEngine::Flip's call to VIWaitForRetrace returns here (USA 80118560)
uint32_t flip_wait_return() { return g_ver->flip_bl + 4; }
bool g_flip_wait_ok = false;

void hle_VIWaitForRetrace(PPCContext& ctx) {
    if (g_flip_wait_ok && ctx.lr == flip_wait_return() && effective_frame_limit() != 60) return;
    PPCFunc* slot = ssne_hook_slot(g_ver->vi_wait);
    *slot = nullptr;
    g_orig_vi_wait(ctx);
    *slot = &hle_VIWaitForRetrace;
}

void install_frame_limiter() {
    const SSNEFuncEntry* f = ssne_find_func(g_ver->vi_wait);
    if (!f || !f->name || strcmp(f->name, "VIWaitForRetrace") != 0) return;
    // Check Flip's call site: bl VIWaitForRetrace.
    uint32_t insn = mem_read32(g_ver->flip_bl);
    int32_t off = int32_t(insn << 6) >> 6 & ~3;
    g_flip_wait_ok = (insn & 0xFC000003u) == 0x48000001u && uint32_t(g_ver->flip_bl + off) == g_ver->vi_wait;
    if (!g_flip_wait_ok) { fprintf(stdout, "[ssne] frame limiter: Flip not found (different game build?)\n"); return; }
    g_orig_vi_wait = f->func;
    ssne_install_hook(g_ver->vi_wait, hle_VIWaitForRetrace);
}
} // namespace

// ---- mouse aim. The player's look update (800BFCA4, once per gameplay frame, r3 = player) turns the C-stick
// into rates: yaw (+0x290, radians, wrapped with fmod 2 pi, + = right) and pitch (+0x294, radians, + = down, clamped to
// [player+0x194]->+0x24 .. [player+0x198]->+0x24 degrees). Mouse aim runs the original update (so the pads and
// the game's own logic still apply) and then adds the host's accumulated mouse rotation directly: 1:1, no
// acceleration, independent of frame rate.
namespace {
#define kLookUpdate (g_ver->look_update)
const uint32_t kLookUpdateSig[6] = {0x9421FFD0u, 0x7C0802A6u, 0xDBC10020u, 0xDBE10028u, 0x93E1001Cu, 0x90010034u};
PPCFunc g_orig_look = nullptr;
std::mutex g_aim_mutex;
double g_aim_yaw = 0, g_aim_pitch = 0;
std::atomic<int64_t> g_aim_last_ms{-100000};

void hle_look_update(PPCContext& ctx) {
    uint32_t player = ctx.r[3];
    PPCFunc* slot = ssne_hook_slot(kLookUpdate);
    *slot = nullptr;
    g_orig_look(ctx);
    *slot = &hle_look_update;
    double dyaw, dpitch;
    {
        std::lock_guard<std::mutex> lk(g_aim_mutex);
        dyaw = g_aim_yaw; dpitch = g_aim_pitch;
        g_aim_yaw = g_aim_pitch = 0;
    }
    g_aim_last_ms = now_ms();
    if (dyaw == 0 && dpitch == 0) return;
    if (player < 0x80000000u || player >= 0x81800000u) return;
    constexpr double kTwoPi = 6.283185307179586;
    double yaw = std::fmod(double(u2f(mem_read32(player + 0x290))) + dyaw, kTwoPi);
    mem_write32(player + 0x290, f2u(float(yaw)));
    uint32_t lo_obj = mem_read32(player + 0x194), hi_obj = mem_read32(player + 0x198);
    if (lo_obj < 0x80000000u || lo_obj >= 0x81800000u || hi_obj < 0x80000000u || hi_obj >= 0x81800000u) return;
    constexpr float kDeg = 0.017453292f;
    float lo = u2f(mem_read32(lo_obj + 0x24)) * kDeg, hi = u2f(mem_read32(hi_obj + 0x24)) * kDeg;
    if (lo > hi) std::swap(lo, hi);
    // The game's pitch grows downward (C-stick up -> look down unless the game's invert option is set).
    float pitch = std::clamp(u2f(mem_read32(player + 0x294)) - float(dpitch), lo, hi);
    mem_write32(player + 0x294, f2u(pitch));
}

void install_mouse_aim() {
    const SSNEFuncEntry* f = ssne_find_func(kLookUpdate);
    if (!f) return;
    for (int k = 0; k < 6; k++)
        if (mem_read32(kLookUpdate + uint32_t(k) * 4) != kLookUpdateSig[k]) {
            fprintf(stdout, "[ssne] mouse aim: look update not found (different game build?)\n");
            return;
        }
    g_orig_look = f->func;
    ssne_install_hook(kLookUpdate, hle_look_update);
}
} // namespace

// ---- analog stick fix. The game reads the controllers once per video retrace (VI callback: PADRead + PADClamp
// into a PADStatus array) and its controller processing (8014F83C, once per game frame) rescales the clamped
// octagon back to a square *in place* in that array: x *= 72 / ((1 - |y|/40) * 32 + 40) for y <= 40 (other formula
// above 40; the C-stick uses 59/31/28). On the GameCube the game ticks exactly once per retrace, so every status is
// processed once. When a status is processed twice or more (game frames faster than retraces: frame limit above
// 60, or a frame landing just before a retrace), the already-scaled values are scaled again, and with both axes
// negative the denominator goes negative: down-left goes (-40,-40) -> (-72,-72) -> (-128,-128) -> (127,127), i.e.
// the stick flips to up-right and the player stops/jitters (the other diagonals use the formula that stays
// positive). Fix: keep the stick bytes as PADClamp left them and put them back before each processing pass, so
// every pass sees the raw input, as on hardware.
namespace {
const uint32_t kPadClampSig[6] = {0x7C0802A6u, 0x3C808023u, 0x90010004u, 0x9421FFE0u, 0x93E1001Cu, 0x93C10018u};
const uint32_t kPadProcessSig[6] = {0x9421FF28u, 0x7C0802A6u, 0xDA010058u, 0xDA210060u, 0xDA410068u, 0xDA610070u};
PPCFunc g_orig_pad_clamp = nullptr, g_orig_pad_process = nullptr;
uint32_t g_pad_status = 0;    // the PADStatus array (4 x 12 bytes) PADClamp last worked on
uint8_t g_pad_sticks[4][4];   // its stick bytes (x, y, sub x, sub y) right after PADClamp
bool g_pad_fix_trace = false;
int g_pad_passes = 0;         // processing passes since the last PADClamp (trace)

void hle_pad_clamp(PPCContext& ctx) {
    uint32_t status = ctx.r[3];
    PPCFunc* slot = ssne_hook_slot(g_ver->pad_clamp);
    *slot = nullptr;
    g_orig_pad_clamp(ctx);
    *slot = &hle_pad_clamp;
    if (status < 0x80000000u || status >= 0x817FFFD0u) { g_pad_status = 0; return; }
    for (uint32_t ch = 0; ch < 4; ch++)
        for (uint32_t k = 0; k < 4; k++) g_pad_sticks[ch][k] = mem_read8(status + 12 * ch + 2 + k);
    if (g_pad_fix_trace && g_pad_passes != 1)
        fprintf(stdout, "[ssne] pad status processed %d times since the previous read\n", g_pad_passes);
    g_pad_passes = 0;
    g_pad_status = status;
}

void hle_pad_process(PPCContext& ctx) {
    if (g_pad_status) {
        for (uint32_t ch = 0; ch < 4; ch++)
            for (uint32_t k = 0; k < 4; k++) mem_write8(g_pad_status + 12 * ch + 2 + k, g_pad_sticks[ch][k]);
        g_pad_passes++;
    }
    PPCFunc* slot = ssne_hook_slot(g_ver->pad_process);
    *slot = nullptr;
    g_orig_pad_process(ctx);
    *slot = &hle_pad_process;
}

bool code_matches(uint32_t addr, const uint32_t sig[6]) {
    for (int k = 0; k < 6; k++)
        if (mem_read32(addr + uint32_t(k) * 4) != sig[k]) return false;
    return true;
}

// ---- save states: the request is carried out at the start of the game's frame flip (savestate.cpp).
const uint32_t kFlipSig[6] = {0x9421FFE8u, 0x7C0802A6u, 0xBFA1000Cu, 0x9001001Cu, 0x7C7F1B78u, 0x7C9D2378u};
PPCFunc g_orig_flip = nullptr;
void hle_flip(PPCContext& ctx) {
    end_player_frame();
    savestate_safe_point(ctx);
    PPCFunc* slot = ssne_hook_slot(g_ver->flip);
    *slot = nullptr;
    g_orig_flip(ctx);
    *slot = &hle_flip;
}
void install_savestates() {
    const SSNEFuncEntry* f = ssne_find_func(g_ver->flip);
    bool ok = f;
    for (int k = 0; k < 6 && ok; k++) ok = mem_read32(g_ver->flip + uint32_t(k) * 4) == kFlipSig[k];
    if (!ok) { fprintf(stdout, "[ssne] save states: frame flip not found (different game build?)\n"); return; }
    g_orig_flip = f->func;
    ssne_install_hook(g_ver->flip, hle_flip);
    g_control_hooks++;
}

void install_pad_fix() {
    if (getenv("SSNE_NO_PAD_FIX")) return;  // A/B testing: the game's original behavior
    g_pad_fix_trace = getenv("SSNE_TRACE_PAD_FIX") != nullptr;
    const SSNEFuncEntry* c = ssne_find_func(g_ver->pad_clamp);
    const SSNEFuncEntry* p = ssne_find_func(g_ver->pad_process);
    if (!c || !p || !code_matches(g_ver->pad_clamp, kPadClampSig) || !code_matches(g_ver->pad_process, kPadProcessSig)) {
        fprintf(stdout, "[ssne] stick fix: controller code not found (different game build?)\n");
        return;
    }
    g_orig_pad_clamp = c->func;
    g_orig_pad_process = p->func;
    ssne_install_hook(g_ver->pad_clamp, hle_pad_clamp);
    ssne_install_hook(g_ver->pad_process, hle_pad_process);
}
} // namespace

void mouse_aim_add(double yaw, double pitch) {
    if (!g_orig_look) return;
    std::lock_guard<std::mutex> lk(g_aim_mutex);
    g_aim_yaw += yaw;
    g_aim_pitch += pitch;
    // Don't let rotation pile up while the game isn't consuming it (menus, loading, pause).
    if (!mouse_aim_active()) g_aim_yaw = g_aim_pitch = 0;
}
bool mouse_aim_available() { return g_orig_look != nullptr; }
bool mouse_aim_active() { return g_orig_look && now_ms() - g_aim_last_ms.load() < 200; }
bool player_control_active() { return mouse_aim_active() || in_vehicle(); }
bool player_in_vehicle() { return in_vehicle(); }
void weapon_select_group(int group) { g_weapon_req_ms = now_ms(); g_weapon_req = group; }

void set_frame_limit(int fps) { g_frame_limit = fps < 0 ? 0 : fps; }
int frame_limit() { return g_frame_limit.load(); }

namespace { std::atomic<bool> g_paused{false}; }
void set_paused(bool p) { g_paused = p; }
bool paused() { return g_paused.load(); }

void frame_limiter_wait() {
    while (g_paused.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    int lim = effective_frame_limit();
    if (lim == 60 || lim <= 0 || !g_flip_wait_ok) return;
    using clk = std::chrono::steady_clock;
    static clk::time_point next = clk::now();
    auto period = std::chrono::nanoseconds(1000000000LL / lim);
    auto now = clk::now();
    if (next < now - period * 2) next = now;  // fell behind (slow frame / loading): don't try to catch up
    if (now < next) {
        auto spin_from = next - std::chrono::microseconds(1500);
        if (now < spin_from) std::this_thread::sleep_until(spin_from);
        while (clk::now() < next) std::this_thread::yield();
    }
    next += period;
}

void install_sdk_hooks() {
    select_version();
    int n = 0;
    for (size_t i = 0; i < g_ssne_func_count; i++) {
        const auto& f = g_ssne_funcs[i];
        if (!f.name || !*f.name) continue;
        for (auto& h : kHooks)
            if (!strcmp(f.name, h.name)) { ssne_install_hook(f.address, h.fn); n++; }
    }
    static const bool no_game_hooks = getenv("SSNE_NO_GAME_HOOKS") != nullptr;  // A/B testing: SDK-only hooks
    if (!no_game_hooks)
    for (const auto& h : address_hooks()) {
        bool ok = true;
        for (int k = 0; k < 6; k++) ok &= mem_read32(h.addr + uint32_t(k) * 4) == h.sig[k];
        if (ok) { ssne_install_hook(h.addr, h.fn); n++; }
        else fprintf(stdout, "[ssne] %s not found at %08X (different game build?): widescreen unavailable\n", h.name, h.addr);
    }
    fprintf(stdout, "[ssne] installed %d native SDK replacements\n", n);
    if (!no_game_hooks) {
        install_frame_limiter();
        install_vehicle_detector();
        install_mouse_aim();
        install_pad_fix();
        install_savestates();
    }
    install_call_tracer();
}

void set_fov(float degrees) { g_fov = degrees <= 0 ? 0.0f : std::clamp(degrees, 30.0f, 120.0f); }
float fov() { return g_fov.load(); }

void set_widescreen(float factor) { g_widescreen = factor < 1.0f ? 1.0f : factor; }
float widescreen() { return g_widescreen.load(); }
float widescreen_frame_factor() { return g_frame_factor.load(); }

} // namespace ssne
