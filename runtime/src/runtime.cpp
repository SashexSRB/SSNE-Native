#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "ssne/ppc_context.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"

namespace {
std::unordered_map<uint32_t, PPCFunc> g_hooks; // node-based: slot pointers stay valid
std::mutex g_hook_mutex;
ssne::FatalHandler g_fatal = nullptr;
} // namespace

PPCFunc* ssne_hook_slot(uint32_t addr) {
    std::lock_guard<std::mutex> lk(g_hook_mutex);
    return &g_hooks[addr];
}

void ssne_install_hook(uint32_t addr, PPCFunc fn) { *ssne_hook_slot(addr) = fn; }

const SSNEFuncEntry* ssne_find_func(uint32_t addr) {
    const SSNEFuncEntry* b = g_ssne_funcs;
    const SSNEFuncEntry* e = g_ssne_funcs + g_ssne_func_count;
    auto it = std::lower_bound(b, e, addr, [](const SSNEFuncEntry& f, uint32_t a) { return f.address < a; });
    return (it != e && it->address == addr) ? it : nullptr;
}

void ssne_call(PPCContext& ctx, uint32_t addr) {
    if (addr < 0x01800000u) addr |= 0x80000000u; // physical -> cached virtual
    PPCFunc hook = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_hook_mutex);
        auto h = g_hooks.find(addr);
        if (h != g_hooks.end()) hook = h->second;
    }
    if (hook) { hook(ctx); return; }
    if (auto* f = ssne_find_func(addr)) { f->func(ctx); return; }
    char msg[160];
    snprintf(msg, sizeof(msg), "Jump to %08X, which is not a known function start (LR=%08X).\n"
             "Add it with --map or improve function discovery.", addr, ctx.lr);
    ssne::fatal(msg);
}

void ssne_unimplemented(PPCContext& ctx, uint32_t addr, uint32_t insn) {
    char msg[128];
    snprintf(msg, sizeof(msg), "Unimplemented instruction %08X at %08X.", insn, addr);
    ssne::fatal(msg);
}

void ssne_syscall(PPCContext& ctx, uint32_t addr) {
    // Dolphin-SDK games only use `sc` inside PPCHalt/OSResetSystem paths; ignore.
    (void)ctx; (void)addr;
}

uint32_t ssne_mfspr(PPCContext& ctx, uint32_t spr) {
    switch (spr) {
    case 920: return ctx.hid2;
    case 22: return ctx.dec;
    case 26: return ctx.srr0;
    case 27: return ctx.srr1;
    case 272: case 273: case 274: case 275: return ctx.sprg[spr - 272];
    default: return 0; // HID0/1, L2CR, WPAR, DMA regs, etc.
    }
}

void ssne_mtspr(PPCContext& ctx, uint32_t spr, uint32_t v) {
    switch (spr) {
    case 920: ctx.hid2 = v; break;
    case 22: ctx.dec = v; break;
    case 26: ctx.srr0 = v; break;
    case 27: ctx.srr1 = v; break;
    case 272: case 273: case 274: case 275: ctx.sprg[spr - 272] = v; break;
    default: break; // Locked-cache DMA (DMAU/DMAL 922/923) handled later.
    }
}

static std::atomic<int64_t> g_tb_shift{0};  // save states: the guest clock continues from the saved time
void ssne_timebase_shift(int64_t ticks) { g_tb_shift += ticks; }

uint64_t ssne_timebase() {
    // Gekko timebase runs at bus clock / 4 = 162 MHz / 4 = 40.5 MHz.
    using namespace std::chrono;
    // Like the console's boot ROM, start it at the RTC time (local seconds since 2000-01-01), so OSGetTime /
    // OSTicksToCalendarTime give the real date (memory card saves get correct timestamps).
    static const auto t0 = steady_clock::now();
    static const uint64_t base = [] {
        std::time_t t = std::time(nullptr);
        std::tm lt{}, gt{};
#ifdef _WIN32
        localtime_s(&lt, &t); gmtime_s(&gt, &t);
#else
        localtime_r(&t, &lt); gmtime_r(&t, &gt);
#endif
        gt.tm_isdst = -1;  // let mktime apply DST for the local interpretation
        int64_t local = int64_t(t) + int64_t(std::mktime(&lt) - std::mktime(&gt));
        return uint64_t(local - 946684800) * 40500000ull;
    }();
    auto ns = duration_cast<nanoseconds>(steady_clock::now() - t0).count();
    return uint64_t(int64_t(base + uint64_t(double(ns) * 0.0405)) + g_tb_shift.load());
}

namespace ssne {

void set_fatal_handler(FatalHandler h) { g_fatal = h; }

void fatal(const char* message) {
    fprintf(stderr, "[ssne] FATAL: %s\n", message);
    if (g_fatal) g_fatal(message);
    std::exit(1);
}

void reset_cpu(PPCContext& ctx) {
    ctx = PPCContext{};
    // Apploader/IPL leave the stack near the top of MEM1; the game's __start sets r1/r2/r13 itself.
    ctx.r[1] = 0x816FFFF0;
    ctx.lr = 0;
}

void boot_globals(uint32_t mem_size) {
    // "OS globals" low memory that the IPL normally fills (see YAGCD, section 4.2).
    mem_write32(0x80000020, 0x0D15EA5E);  // boot magic (normal boot)
    mem_write32(0x80000024, 1);           // version
    mem_write32(0x80000028, mem_size);    // physical memory size
    mem_write32(0x8000002C, 1);           // console type: retail
    mem_write32(0x800000F0, mem_size);    // simulated memory size
    mem_write32(0x800000F8, 162000000);   // bus clock
    mem_write32(0x800000FC, 486000000);   // CPU clock
}

} // namespace ssne
