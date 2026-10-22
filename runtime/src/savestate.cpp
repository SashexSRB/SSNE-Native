// Save states. The recompiled game runs as native code on host threads, so a state can only be taken or restored
// where the host side is in a known shape: at the start of the game's frame flip (CcEngine::Flip), called from
// its main loop with only the main thread alive. There the host call stack holds nothing but the recompiled
// functions of that call chain (which keep all guest state in the PPCContext and guest memory), so swapping guest
// memory, CPU registers and the emulated hardware is enough. The chain of guest return addresses is stored and
// must match when loading (it is the same in menus and gameplay; only the boot screens differ).
//
// File: "SSNESTAT", version, game ID, function count, call chain, timebase, CRC32 of the payload, then the payload:
// CPU registers, RAM (zero pages elided), locked cache, scheduler (interrupts, alarms), MMIO/ARAM/DSP/AI, SI,
// EXI (memory card contents included), GX.
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include "state_io.h"

void ssne_timebase_shift(int64_t ticks);

namespace ssne {
namespace {

constexpr char kMagic[8] = {'S', 'S', 'N', 'E', 'S', 'T', 'A', 'T'};
constexpr uint32_t kVersion = 1;

std::mutex g_mx;
bool g_req = false, g_req_save = false;
std::string g_req_path;
int g_req_tries = 0;
bool g_have_result = false;
SavestateResult g_result;

uint32_t crc32(const uint8_t* p, size_t n) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::vector<uint32_t> call_chain(const PPCContext& ctx) {
    std::vector<uint32_t> chain{ctx.lr};
    uint32_t sp = ctx.r[1];
    for (int i = 0; i < 32 && sp >= 0x80000000u && sp < 0x81800000u; i++) {
        uint32_t back = mem_read32(sp);
        if (back < 0x80000000u || back >= 0x81800000u || back <= sp) break;
        chain.push_back(mem_read32(back + 4));
        sp = back;
    }
    return chain;
}

std::string game_id() {
    char id[7] = {};
    for (int i = 0; i < 6; i++) id[i] = char(mem_read8(0x80000000u + uint32_t(i)));
    return id;
}

void finish(bool save, bool ok, const std::string& path, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_result = {save, ok, path, msg};
    g_have_result = true;
    fprintf(stdout, "[state] %s\n", msg.c_str());
}

bool write_file(const std::string& path, const std::vector<uint8_t>& data, std::string& err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = fs::u8path(path), tmp = fs::u8path(path + ".tmp");
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { err = "cannot write " + path; return false; }
        f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!f) { err = "cannot write " + path + " (disk full?)"; return false; }
    }
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(p, ec); fs::rename(tmp, p, ec); }
    if (ec) { err = "cannot write " + path; return false; }
    return true;
}

void do_save(PPCContext& ctx, const std::string& path) {
    StateWriter pay;
    pay.tag("CPU1");
    pay.pod(ctx);
    pay.tag("RAM1");
    pay.block(g_ram, kRamSize);
    pay.bytes(g_locked_cache, kLockedCacheSize);
    sched_state_save(pay);
    hw_state_save(pay);
    si_state_save(pay);
    exi_state_save(pay);
    gx_state_save(pay);
    StateWriter w;
    w.bytes(kMagic, 8);
    w.u32(kVersion);
    std::string id = game_id();
    w.bytes(id.data(), 6);
    w.u64(g_ssne_func_count);
    auto chain = call_chain(ctx);
    w.u32(uint32_t(chain.size()));
    for (uint32_t a : chain) w.u32(a);
    w.u64(ssne_timebase());
    w.u64(uint64_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
    w.u32(crc32(pay.buf.data(), pay.buf.size()));
    w.u64(pay.buf.size());
    w.bytes(pay.buf.data(), pay.buf.size());
    std::string err;
    if (!write_file(path, w.buf, err)) { finish(true, false, path, "Save state failed: " + err); return; }
    char msg[96];
    snprintf(msg, sizeof(msg), "State saved (%.1f MB)", w.buf.size() / 1048576.0);
    finish(true, true, path, msg);
}

void do_load(PPCContext& ctx, const std::string& path) {
    std::vector<uint8_t> data;
    {
        std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
        if (!f) { finish(false, false, path, "No state saved in this slot"); return; }
        data.assign(std::istreambuf_iterator<char>(f), {});
    }
    StateReader r{data.data(), data.size()};
    char magic[8] = {};
    r.bytes(magic, 8);
    if (!r.ok || memcmp(magic, kMagic, 8)) { finish(false, false, path, "Not a save state file"); return; }
    if (r.u32() != kVersion) { finish(false, false, path, "This state was made by another version of the port"); return; }
    char id[7] = {};
    r.bytes(id, 6);
    if (game_id() != id) { finish(false, false, path, std::string("This state is from another release of the game (") + id + ")"); return; }
    if (r.u64() != g_ssne_func_count) { finish(false, false, path, "This state was made with a different game build"); return; }
    std::vector<uint32_t> chain(r.u32());
    for (auto& a : chain) a = r.u32();
    uint64_t tb = r.u64();
    r.u64();  // wall-clock time of the save (shown by the port)
    uint32_t crc = r.u32();
    uint64_t len = r.u64();
    if (!r.ok || len != data.size() - r.pos || crc32(data.data() + r.pos, len) != crc) {
        finish(false, false, path, "The state file is damaged");
        return;
    }
    if (chain != call_chain(ctx)) {
        finish(false, false, path, "Can't load this state from here: get past the boot screens (to the menus or a level) first");
        return;
    }
    StateReader p{data.data() + r.pos, size_t(len)};
    PPCContext saved;
    if (!p.tag("CPU1") || !p.pod(saved)) { finish(false, false, path, "The state file is damaged"); return; }
    // From here on the state is applied (the CRC was checked, so the layout is what this build wrote).
    ssne_timebase_shift(int64_t(tb) - int64_t(ssne_timebase()));
    bool ok = p.tag("RAM1") && p.block(g_ram, kRamSize) && p.bytes(g_locked_cache, kLockedCacheSize);
    ok = ok && sched_state_load(p) && hw_state_load(p) && si_state_load(p) && exi_state_load(p) && gx_state_load(p);
    ctx = saved;
    audio_state_reset();
    finish(false, ok, path, ok ? "State loaded" : "Loading the state failed: the game may be unstable, restart it");
}

} // namespace

void savestate_request(bool save, const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_req = true;
    g_req_save = save;
    g_req_path = path;
    g_req_tries = 0;
}

bool savestate_pending() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_req;
}

bool savestate_take_result(SavestateResult& out) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_have_result) return false;
    out = g_result;
    g_have_result = false;
    return true;
}

bool savestate_read_info(const std::string& path, uint64_t& unix_time) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> head(4096);
    f.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
    head.resize(size_t(f.gcount()));
    StateReader r{head.data(), head.size()};
    char magic[8] = {};
    r.bytes(magic, 8);
    if (!r.ok || memcmp(magic, kMagic, 8) || r.u32() != kVersion) return false;
    char id[6];
    r.bytes(id, 6);
    r.u64();
    uint32_t n = r.u32();
    for (uint32_t i = 0; i < n && r.ok; i++) r.u32();
    r.u64();
    unix_time = r.u64();
    return r.ok;
}

// Called at the start of every CcEngine::Flip (hle.cpp), on the game's main thread.
void savestate_safe_point(PPCContext& ctx) {
    bool save;
    std::string path;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_req) return;
        save = g_req_save;
        path = g_req_path;
    }
    std::string why;
    if (!sched_state_check(why)) {
        std::lock_guard<std::mutex> lk(g_mx);
        if (++g_req_tries < 180) return;  // try again on the next frames (~3 s)
        g_req = false;
        g_result = {save, false, path, std::string(save ? "Can't save now: " : "Can't load now: ") + why};
        g_have_result = true;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_req = false;
    }
    if (save) do_save(ctx, path);
    else do_load(ctx, path);
}

} // namespace ssne
