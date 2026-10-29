// Glue between generated code and the runtime: function table + per-function override hooks.
// Hooks let us replace game/SDK functions (OS, GX, DVD, PAD, AX...) with native implementations.
#pragma once
#include <cstddef>
#include <cstdint>

#include "ssne/ppc_context.h"
#include "ssne/scheduler.h"

struct SSNEFuncEntry {
    uint32_t address;
    PPCFunc func;
    const char* name;
};

// Provided by generated func_table.cpp (or port/src/no_game_code.cpp).
extern const SSNEFuncEntry g_ssne_funcs[];
extern const size_t g_ssne_func_count;
extern const uint32_t g_ssne_entry;
extern const bool g_ssne_has_game_code;

// Returns a stable slot for `addr`; non-null contents replace the recompiled function.
PPCFunc* ssne_hook_slot(uint32_t addr);
void ssne_install_hook(uint32_t addr, PPCFunc fn);
const SSNEFuncEntry* ssne_find_func(uint32_t addr);

// Safe point: deliver pending interrupts (if the thread has MSR[EE] set).
#define SSNE_POLL()                                                                     \
    do {                                                                                \
        if (ssne::g_irq_pending.load(std::memory_order_relaxed)) ssne::safepoint(ctx);  \
    } while (0)

#ifdef SSNE_COUNT_CALLS  // diagnostics build: calls per function (headless --count-calls)
extern uint32_t g_ssne_call_counts[];
#define SSNE_COUNT_CALL(addr) (g_ssne_call_counts[((addr) - 0x80000000u) >> 2 & 0x7FFFF]++)
#else
#define SSNE_COUNT_CALL(addr) ((void)0)
#endif

#define SSNE_HOOK(addr)                                              \
    do {                                                             \
        SSNE_COUNT_CALL(addr);                                       \
        static PPCFunc* hook_slot_ = ssne_hook_slot(addr);           \
        if (*hook_slot_) { (*hook_slot_)(ctx); return; }             \
        SSNE_POLL();                                                 \
    } while (0)
