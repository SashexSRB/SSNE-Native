// Guest threads, interrupts and alarms.
//
// Model: every guest OSThread runs on its own host thread, but only one guest thread executes at a
// time (the Gekko is single-core). The scheduler hands a "baton" to the highest-priority ready thread,
// exactly like the Dolphin OS scheduler would.
//
// Interrupts (ARAM DMA done, disc transfer done, VI retrace, alarms) are queued by the hardware layer and
// delivered at safe points (every function entry and loop back-edge in recompiled code) when the running
// thread has MSR[EE] set. Delivery calls the handler the game registered in the SDK's interrupt table, so
// the SDK's own drivers (__ARHandler, __DVDInterruptHandler, __VIRetraceHandler, ...) run unmodified.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>

#include "ssne/ppc_context.h"

namespace ssne {

extern std::atomic<bool> g_irq_pending;

// __OSInterrupt numbers used by the SDK.
enum Interrupt : int {
    IRQ_DSP_AI = 5, IRQ_DSP_ARAM = 6, IRQ_DSP_DSP = 7, IRQ_AI = 8,
    IRQ_EXI0 = 9, IRQ_EXI0_TC = 10, IRQ_PI_CP = 17, IRQ_PE_TOKEN = 18, IRQ_PE_FINISH = 19,
    IRQ_SI = 20, IRQ_DI = 21, IRQ_RSW = 22, IRQ_VI = 24,
};

// Raise a hardware interrupt: it is dispatched through the SDK interrupt table.
void raise_interrupt(int irq);
// Queue arbitrary work to run in interrupt context (used for alarm handlers).
void queue_interrupt_work(std::function<void(PPCContext&)> fn);
// Called from recompiled code (SSNE_POLL) when g_irq_pending is set.
void safepoint(PPCContext& ctx);

// Starts timers (VI 60 Hz, alarms). Call once before running the guest.
void scheduler_init(PPCContext& main_ctx);
void scheduler_shutdown();
void install_thread_hooks();

// Display sync: when on, VI retraces are driven by the host's vertical blank (vi_host_vblank(), called by the port
// right after each vsynced buffer swap) instead of a free-running 59.94 Hz timer. A 60 Hz display and the game then
// tick together, so no frame is ever shown twice or skipped (the free-running clock drifts against the display and
// periodically repeats a frame: visible stutter). The timer takes over again whenever host vblanks stop coming.
void set_vi_host_sync(bool on);
bool vi_host_sync();
void vi_host_vblank();

// Call a guest function from native code (arguments already placed in ctx.r[3..]).
void call_guest(PPCContext& ctx, uint32_t addr);

// For diagnostics.
struct SchedulerStats { int threads; int ready; uint64_t interrupts; uint64_t retraces; };
SchedulerStats scheduler_stats();

} // namespace ssne
