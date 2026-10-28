#include "ssne/scheduler.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "ssne/hw.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include "ssne/thread_util.h"
#include "state_io.h"

namespace ssne {

std::atomic<bool> g_irq_pending{false};

namespace {

// ---- guest memory layout (see load_game): reserved area above the game's arena
constexpr uint32_t OS_CURRENT_THREAD = 0x800000E4;
constexpr uint32_t OS_INTERRUPT_TABLE = 0x80003040;
constexpr uint32_t OS_INTERRUPT_MASK = 0x800000C4;    // SDK: masked sources (1 = masked)
constexpr uint32_t OS_INTERRUPT_MASK_USER = 0x800000C8;
constexpr uint32_t IRQ_STACK_TOP = 0x817EFF00;
constexpr uint32_t IRQ_CONTEXT = 0x817E0000;           // scratch OSContext handed to handlers
constexpr uint32_t MSR_EE = 0x8000;

// OSThread field offsets (SDK 0x2301)
constexpr uint32_t THR_STATE = 0x2C8, THR_SUSPEND = 0x2CC, THR_PRIORITY = 0x2D0, THR_BASE = 0x2D4, THR_VAL = 0x2D8;

enum class TState { Ready, Running, Waiting, Suspended, Dead };

struct ThreadExit {};  // unwinds a host thread when its guest thread exits

struct GThread {
    uint32_t addr = 0;
    PPCContext own_ctx;
    PPCContext* ctx = &own_ctx;
    int prio = 16;
    int suspend = 1;
    TState st = TState::Suspended;
    uint32_t wait_queue = 0;
    uint32_t entry = 0, arg = 0, stack = 0;
    bool started = false, is_main = false, detached_exit = false;
    uint64_t seq = 0;
    std::condition_variable cv;
};

std::mutex M;
std::vector<std::unique_ptr<GThread>> g_threads;
GThread* g_cur = nullptr;          // thread holding the baton (nullptr = idle)
GThread* g_main = nullptr;
std::condition_variable g_idle_cv;
int g_sched_disabled = 0;
uint64_t g_seq = 0;
bool g_shutdown = false;
thread_local GThread* t_self = nullptr;

// interrupts
std::atomic<uint32_t> g_pending_bits{0};
std::mutex IM;
std::deque<std::function<void(PPCContext&)>> g_jobs;
bool g_in_irq = false;
bool g_need_resched = false;
PPCContext g_irq_ctx, g_idle_ctx;
std::atomic<uint64_t> g_irq_count{0}, g_retraces{0};

// timers
struct Alarm { uint64_t fire, period; uint32_t handler; };
std::map<uint32_t, Alarm> g_alarms;   // guarded by TM
std::mutex TM;
std::condition_variable g_timer_cv;
std::thread g_timer_thread, g_idle_thread;

GThread* find(uint32_t addr) {
    for (auto& t : g_threads) if (t->addr == addr && t->st != TState::Dead) return t.get();
    return nullptr;
}

void write_state(GThread* t) {
    if (!t->addr) return;
    uint16_t s = t->st == TState::Ready ? 1 : t->st == TState::Running ? 2 : t->st == TState::Dead ? 8 : 4;
    mem_write16(t->addr + THR_STATE, s);
    mem_write32(t->addr + THR_SUSPEND, uint32_t(t->suspend));
    mem_write32(t->addr + THR_PRIORITY, uint32_t(t->prio));
}

bool runnable(GThread* t) { return t->st == TState::Ready || t->st == TState::Running; }

GThread* pick(GThread* self, bool yield) {
    GThread* best = nullptr;
    for (auto& up : g_threads) {
        GThread* t = up.get();
        if (!runnable(t)) continue;
        if (!best || t->prio < best->prio) { best = t; continue; }
        if (t->prio == best->prio) {
            // Ties: keep running the current thread unless yielding; otherwise oldest-ready first.
            if (!yield && t == self) best = t;
            else if (best == self && yield) best = t;
            else if (best != self && t != self && t->seq < best->seq) best = t;
        }
    }
    return best;
}

void hand_to(GThread* next) {  // requires M
    g_cur = next;
    if (next) {
        next->st = TState::Running;
        write_state(next);
        mem_write32(OS_CURRENT_THREAD, next->addr);
        next->cv.notify_all();
    } else {
        g_idle_cv.notify_all();
    }
}

void wait_for_baton(std::unique_lock<std::mutex>& lk, GThread* self) {
    self->cv.wait(lk, [&] { return g_cur == self || self->st == TState::Dead || g_shutdown; });
    if (self->st == TState::Dead || g_shutdown) throw ThreadExit{};
}

// Core scheduling decision, called by the running thread.
void reschedule(bool yield) {
    GThread* self = t_self;
    std::unique_lock<std::mutex> lk(M);
    if (!self || g_cur != self) return;
    if (g_in_irq) { g_need_resched = true; return; }
    if (g_sched_disabled > 0 && runnable(self)) return;
    GThread* best = pick(self, yield);
    if (best == self) return;
    if (runnable(self)) { self->st = TState::Ready; self->seq = ++g_seq; write_state(self); }
    hand_to(best);
    wait_for_baton(lk, self);
}

void host_thread_main(GThread* g) {
    t_self = g;
    try {
        {
            std::unique_lock<std::mutex> lk(M);
            wait_for_baton(lk, g);
        }
        PPCContext& c = *g->ctx;
        ssne_call(c, g->entry);
        // Returning from the thread function == OSExitThread(r3).
        std::unique_lock<std::mutex> lk(M);
        g->st = TState::Dead;
        write_state(g);
        mem_write32(g->addr + THR_VAL, c.r[3]);
        hand_to(pick(nullptr, false));
    } catch (const ThreadExit&) {
    } catch (const std::exception& e) {
        fprintf(stderr, "[ssne] guest thread %08X stopped: %s\n", g->addr, e.what());
        std::unique_lock<std::mutex> lk(M);
        g->st = TState::Dead;
        if (g_cur == g) hand_to(pick(nullptr, false));
    }
}

// ---- interrupt delivery

bool deliverable_now(uint32_t bits) {
    uint32_t masked = mem_read32(OS_INTERRUPT_MASK) | mem_read32(OS_INTERRUPT_MASK_USER);
    return (bits & ~masked) != 0;
}

void run_in_irq_ctx(const PPCContext& from, const std::function<void(PPCContext&)>& fn) {
    PPCContext& c = g_irq_ctx;
    c.r[2] = from.r[2];
    c.r[13] = from.r[13];
    memcpy(c.gqr, from.gqr, sizeof(c.gqr));
    c.hid2 = from.hid2;
    c.msr = from.msr & ~MSR_EE;
    c.r[1] = IRQ_STACK_TOP;
    c.lr = 0;
    fn(c);
}

void service(PPCContext& from) {
    g_in_irq = true;
    for (int guard = 0; guard < 64; guard++) {
        std::function<void(PPCContext&)> job;
        {
            std::lock_guard<std::mutex> lk(IM);
            if (!g_jobs.empty()) { job = std::move(g_jobs.front()); g_jobs.pop_front(); }
        }
        if (job) { run_in_irq_ctx(from, job); g_irq_count++; continue; }
        uint32_t bits = g_pending_bits.load();
        uint32_t masked = mem_read32(OS_INTERRUPT_MASK) | mem_read32(OS_INTERRUPT_MASK_USER);
        uint32_t ready = bits & ~masked;
        if (!ready) break;
        int irq = std::countl_zero(ready);       // bit (0x80000000 >> irq)
        g_pending_bits.fetch_and(~(0x80000000u >> irq));
        uint32_t handler = mem_read32(OS_INTERRUPT_TABLE + 4 * irq);
        if (handler) {
            run_in_irq_ctx(from, [&](PPCContext& c) {
                c.r[3] = uint32_t(irq);
                c.r[4] = IRQ_CONTEXT;
                ssne_call(c, handler);
            });
            g_irq_count++;
        }
    }
    g_in_irq = false;
    std::lock_guard<std::mutex> lk(IM);
    g_irq_pending = !g_jobs.empty() || deliverable_now(g_pending_bits.load());
}

void idle_loop() {
    std::unique_lock<std::mutex> lk(M);
    while (!g_shutdown) {
        g_idle_cv.wait_for(lk, std::chrono::milliseconds(5), [] { return g_shutdown || (!g_cur && g_irq_pending); });
        if (g_shutdown) break;
        if (g_cur || !g_irq_pending) continue;
        // Nobody is running: take the baton to run interrupt handlers, then pick a thread.
        g_cur = reinterpret_cast<GThread*>(uintptr_t(1));
        lk.unlock();
        service(g_idle_ctx);
        lk.lock();
        g_need_resched = false;
        hand_to(pick(nullptr, false));
    }
}

std::atomic<bool> g_vi_host_sync{false};
bool g_host_vblank = false;  // guarded by TM

void timer_loop() {
    using clock = std::chrono::steady_clock;
    const auto vi_period = std::chrono::nanoseconds(16683350); // 59.94 Hz
    auto next_vi = clock::now() + vi_period;
    std::unique_lock<std::mutex> lk(TM);
    while (!g_shutdown) {
        // Next deadline: VI or earliest alarm.
        auto deadline = next_vi;
        uint64_t now_tb = ssne_timebase();
        for (auto& [a, al] : g_alarms) {
            auto d = clock::now() + std::chrono::nanoseconds(int64_t((double(al.fire) - double(now_tb)) / 0.0405));
            if (d < deadline) deadline = d;
        }
        if (!g_host_vblank) g_timer_cv.wait_until(lk, deadline);
        if (g_shutdown) break;
        bool fire_vi = false;
        if (g_host_vblank) {
            g_host_vblank = false;
            if (g_vi_host_sync) {
                // Retrace now; the timer only fires if the next host vblank is late (window dragged, sync lost...).
                fire_vi = true;
                next_vi = clock::now() + vi_period + std::chrono::milliseconds(4);
            }
        }
        if (!fire_vi && clock::now() >= next_vi) {
            fire_vi = true;
            next_vi += vi_period;
            if (clock::now() > next_vi) next_vi = clock::now() + vi_period;
        }
        if (fire_vi) {
            g_retraces++;
            lk.unlock();
            vi_retrace();
            lk.lock();
        }
        now_tb = ssne_timebase();
        std::vector<std::pair<uint32_t, uint32_t>> fire;
        for (auto it = g_alarms.begin(); it != g_alarms.end();) {
            if (it->second.fire <= now_tb) {
                fire.push_back({it->first, it->second.handler});
                if (it->second.period) { it->second.fire += it->second.period; ++it; }
                else it = g_alarms.erase(it);
            } else ++it;
        }
        lk.unlock();
        for (const auto& f : fire) {
            // (plain copies: capturing structured bindings needs clang 16, and releases build with older ones)
            uint32_t alarm = f.first, handler = f.second;
            queue_interrupt_work([alarm, handler](PPCContext& c) {
                c.r[3] = alarm;
                c.r[4] = IRQ_CONTEXT;
                ssne_call(c, handler);
            });
        }
        lk.lock();
    }
}

// ---- guest API replacements (Dolphin OS thread/alarm functions)

void bind_main() {
    if (g_main && !g_main->addr) {
        uint32_t a = mem_read32(OS_CURRENT_THREAD);
        if (a) {
            g_main->addr = a;
            g_main->prio = int(mem_read32(a + THR_PRIORITY));
            g_idle_ctx.r[2] = g_main->ctx->r[2];
            g_idle_ctx.r[13] = g_main->ctx->r[13];
        }
    }
}

GThread* self_or_main() {
    if (!t_self && g_main) t_self = g_main;
    bind_main();
    return t_self;
}

void hle_OSCreateThread(PPCContext& ctx) {
    uint32_t addr = ctx.r[3];
    std::unique_lock<std::mutex> lk(M);
    if (auto* old = find(addr)) old->st = TState::Dead;
    auto t = std::make_unique<GThread>();
    t->addr = addr;
    t->entry = ctx.r[4];
    t->arg = ctx.r[5];
    t->stack = ctx.r[6];
    t->prio = int(ctx.r[8]);
    t->suspend = 1;
    t->st = TState::Suspended;
    PPCContext& c = t->own_ctx;
    c = PPCContext{};
    c.r[2] = ctx.r[2];
    c.r[13] = ctx.r[13];
    memcpy(c.gqr, ctx.gqr, sizeof(c.gqr));
    c.hid2 = ctx.hid2;
    c.fpscr = ctx.fpscr;
    c.msr = 0x9032;  // EE | ME | IR | DR (+FP)
    uint32_t sp = (t->stack & ~7u) - 8;
    mem_write32(sp, 0);
    mem_write32(sp + 4, 0);
    c.r[1] = sp;
    c.r[3] = t->arg;
    mem_write32(addr + THR_BASE, uint32_t(t->prio));
    write_state(t.get());
    g_threads.push_back(std::move(t));
    ctx.r[3] = 1;
}

void start_if_needed(GThread* t) {  // requires M
    if (!t->started) {
        t->started = true;
        spawn_guest_thread([t] { host_thread_main(t); });
    }
}

void hle_OSResumeThread(PPCContext& ctx) {
    {
        std::unique_lock<std::mutex> lk(M);
        GThread* t = find(ctx.r[3]);
        if (!t) { ctx.r[3] = 0; return; }
        int prev = t->suspend;
        if (t->suspend > 0 && --t->suspend == 0 && t->st == TState::Suspended) {
            t->st = TState::Ready;
            t->seq = ++g_seq;
            start_if_needed(t);
        }
        write_state(t);
        ctx.r[3] = uint32_t(prev);
    }
    reschedule(false);
}

void hle_OSSuspendThread(PPCContext& ctx) {
    GThread* self = self_or_main();
    std::unique_lock<std::mutex> lk(M);
    GThread* t = find(ctx.r[3]);
    if (!t) { ctx.r[3] = 0; return; }
    int prev = t->suspend++;
    if (prev == 0 && runnable(t)) t->st = TState::Suspended;
    write_state(t);
    ctx.r[3] = uint32_t(prev);
    if (t == self && g_cur == self && !g_in_irq) {
        hand_to(pick(self, false));
        wait_for_baton(lk, self);
    }
}

void hle_OSSleepThread(PPCContext& ctx) {
    GThread* self = self_or_main();
    std::unique_lock<std::mutex> lk(M);
    if (!self || g_in_irq) return;
    self->st = TState::Waiting;
    self->wait_queue = ctx.r[3];
    write_state(self);
    hand_to(pick(self, false));
    wait_for_baton(lk, self);
}

void hle_OSWakeupThread(PPCContext& ctx) {
    {
        std::unique_lock<std::mutex> lk(M);
        for (auto& t : g_threads)
            if (t->st == TState::Waiting && t->wait_queue == ctx.r[3]) {
                t->st = TState::Ready;
                t->wait_queue = 0;
                t->seq = ++g_seq;
                write_state(t.get());
            }
    }
    reschedule(false);
}

void hle_OSYieldThread(PPCContext&) { reschedule(true); }
void hle_OSReschedule(PPCContext&) { reschedule(false); }
void hle_SelectThread(PPCContext& ctx) { reschedule(ctx.r[3] != 0); }

void hle_OSExitThread(PPCContext& ctx) {
    GThread* self = self_or_main();
    std::unique_lock<std::mutex> lk(M);
    self->st = TState::Dead;
    write_state(self);
    mem_write32(self->addr + THR_VAL, ctx.r[3]);
    hand_to(pick(nullptr, false));
    throw ThreadExit{};
}

void hle_OSCancelThread(PPCContext& ctx) {
    GThread* self = self_or_main();
    std::unique_lock<std::mutex> lk(M);
    GThread* t = find(ctx.r[3]);
    if (!t) return;
    t->st = TState::Dead;
    write_state(t);
    t->cv.notify_all();
    if (t == self) { hand_to(pick(nullptr, false)); throw ThreadExit{}; }
}

void hle_OSGetCurrentThread(PPCContext& ctx) {
    GThread* self = self_or_main();
    ctx.r[3] = self ? self->addr : mem_read32(OS_CURRENT_THREAD);
}

void hle_OSDisableScheduler(PPCContext& ctx) { std::lock_guard<std::mutex> lk(M); ctx.r[3] = uint32_t(g_sched_disabled++); }
void hle_OSEnableScheduler(PPCContext& ctx) { std::lock_guard<std::mutex> lk(M); ctx.r[3] = uint32_t(g_sched_disabled--); }

void hle_OSSetAlarm(PPCContext& ctx) {
    uint32_t alarm = ctx.r[3];
    uint64_t ticks = (uint64_t(ctx.r[5]) << 32) | ctx.r[6];
    Alarm a{ssne_timebase() + ticks, 0, ctx.r[7]};
    mem_write32(alarm + 0, a.handler);
    mem_write32(alarm + 8, uint32_t(a.fire >> 32));
    mem_write32(alarm + 12, uint32_t(a.fire));
    std::lock_guard<std::mutex> lk(TM);
    g_alarms[alarm] = a;
    g_timer_cv.notify_all();
}

void hle_OSSetPeriodicAlarm(PPCContext& ctx) {
    uint32_t alarm = ctx.r[3];
    uint64_t start = (uint64_t(ctx.r[5]) << 32) | ctx.r[6];
    uint64_t period = (uint64_t(ctx.r[7]) << 32) | ctx.r[8];
    uint64_t now = ssne_timebase();
    uint64_t fire = start;
    if (period && start <= now) fire = start + period * ((now - start) / period + 1);
    Alarm a{fire, period, ctx.r[9]};
    mem_write32(alarm + 0, a.handler);
    std::lock_guard<std::mutex> lk(TM);
    g_alarms[alarm] = a;
    g_timer_cv.notify_all();
}

void hle_OSCancelAlarm(PPCContext& ctx) {
    std::lock_guard<std::mutex> lk(TM);
    g_alarms.erase(ctx.r[3]);
    mem_write32(ctx.r[3], 0);
}

struct Named { const char* name; PPCFunc fn; };
const Named kThreadHooks[] = {
    {"OSCreateThread", hle_OSCreateThread},   {"OSResumeThread", hle_OSResumeThread},
    {"OSSuspendThread", hle_OSSuspendThread}, {"OSSleepThread", hle_OSSleepThread},
    {"OSWakeupThread", hle_OSWakeupThread},   {"OSYieldThread", hle_OSYieldThread},
    {"OSExitThread", hle_OSExitThread},       {"OSCancelThread", hle_OSCancelThread},
    {"OSGetCurrentThread", hle_OSGetCurrentThread},
    {"OSDisableScheduler", hle_OSDisableScheduler}, {"OSEnableScheduler", hle_OSEnableScheduler},
    {"__OSReschedule", hle_OSReschedule},     {"SelectThread", hle_SelectThread},
    {"OSSetAlarm", hle_OSSetAlarm},           {"OSSetPeriodicAlarm", hle_OSSetPeriodicAlarm},
    {"OSCancelAlarm", hle_OSCancelAlarm},
};

} // namespace

void call_guest(PPCContext& ctx, uint32_t addr) { ssne_call(ctx, addr); }

void raise_interrupt(int irq) {
    g_pending_bits.fetch_or(0x80000000u >> irq);
    g_irq_pending = true;
    g_idle_cv.notify_all();
}

void queue_interrupt_work(std::function<void(PPCContext&)> fn) {
    {
        std::lock_guard<std::mutex> lk(IM);
        g_jobs.push_back(std::move(fn));
    }
    g_irq_pending = true;
    g_idle_cv.notify_all();
}

void safepoint(PPCContext& ctx) {
    if (!(ctx.msr & MSR_EE) || g_in_irq) return;
    GThread* self = self_or_main();
    if (self && g_cur != self) return;
    service(ctx);
    bool resched;
    {
        std::lock_guard<std::mutex> lk(M);
        resched = g_need_resched;
        g_need_resched = false;
    }
    // Handlers may have woken a higher-priority thread: preempt.
    if (self) reschedule(false);
    (void)resched;
}

void scheduler_init(PPCContext& main_ctx) {
    std::lock_guard<std::mutex> lk(M);
    auto t = std::make_unique<GThread>();
    t->ctx = &main_ctx;
    t->is_main = true;
    t->started = true;
    t->suspend = 0;
    t->st = TState::Running;
    t->prio = 16;
    g_main = t.get();
    g_cur = g_main;
    t_self = g_main;
    g_threads.push_back(std::move(t));
    g_idle_ctx = main_ctx;
    g_timer_thread = std::thread(timer_loop);
    g_idle_thread = std::thread(idle_loop);
    g_timer_thread.detach();
    g_idle_thread.detach();
}

void scheduler_shutdown() {
    std::lock_guard<std::mutex> lk(M);
    g_shutdown = true;
    for (auto& t : g_threads) t->cv.notify_all();
    g_idle_cv.notify_all();
    g_timer_cv.notify_all();
}

void install_thread_hooks() {
    int n = 0;
    for (size_t i = 0; i < g_ssne_func_count; i++) {
        const auto& f = g_ssne_funcs[i];
        if (!f.name || !*f.name) continue;
        for (auto& h : kThreadHooks)
            if (!strcmp(f.name, h.name)) { ssne_install_hook(f.address, h.fn); n++; }
    }
    fprintf(stdout, "[ssne] installed %d thread/alarm replacements\n", n);
}

void set_vi_host_sync(bool on) { g_vi_host_sync = on; }
bool vi_host_sync() { return g_vi_host_sync.load(); }
void vi_host_vblank() {
    if (!g_vi_host_sync) return;
    {
        std::lock_guard<std::mutex> lk(TM);
        g_host_vblank = true;
    }
    g_timer_cv.notify_all();
}

SchedulerStats scheduler_stats() {
    std::lock_guard<std::mutex> lk(M);
    SchedulerStats s{};
    for (auto& t : g_threads) { if (t->st != TState::Dead) s.threads++; if (runnable(t.get())) s.ready++; }
    s.interrupts = g_irq_count;
    s.retraces = g_retraces;
    return s;
}

bool sched_state_check(std::string& why) {
    std::lock_guard<std::mutex> lk(M);
    int alive = 0;
    for (auto& t : g_threads) alive += t->st != TState::Dead;
    if (alive != 1 || !g_main || g_main->st == TState::Dead) { why = "the game is busy (loading)"; return false; }
    if (g_in_irq) { why = "the game is busy"; return false; }
    std::lock_guard<std::mutex> lk2(IM);
    if (!g_jobs.empty()) { why = "the game is busy"; return false; }
    return true;
}

void sched_state_save(StateWriter& w) {
    w.tag("SC01");
    { std::lock_guard<std::mutex> lk(M); w.u32(g_main ? g_main->addr : 0); }
    w.u32(g_pending_bits.load());
    std::lock_guard<std::mutex> lk(TM);
    w.u32(uint32_t(g_alarms.size()));
    for (auto& [addr, al] : g_alarms) { w.u32(addr); w.u64(al.fire); w.u64(al.period); w.u32(al.handler); }
}

bool sched_state_load(StateReader& r) {
    if (!r.tag("SC01")) return false;
    uint32_t main_addr = r.u32();
    {
        std::lock_guard<std::mutex> lk(M);
        if (!g_main || g_main->addr != main_addr) { r.ok = false; return false; }
    }
    uint32_t bits = r.u32();
    std::map<uint32_t, Alarm> alarms;
    uint32_t n = r.u32();
    for (uint32_t i = 0; i < n && r.ok; i++) {
        uint32_t a = r.u32();
        Alarm al;
        al.fire = r.u64(); al.period = r.u64(); al.handler = r.u32();
        alarms[a] = al;
    }
    if (!r.ok) return false;
    {
        std::lock_guard<std::mutex> lk(TM);
        g_alarms.swap(alarms);
    }
    g_timer_cv.notify_all();
    {
        std::lock_guard<std::mutex> lk(IM);
        g_jobs.clear();
        g_pending_bits = bits;
        g_irq_pending = bits != 0;
    }
    return true;
}

} // namespace ssne
