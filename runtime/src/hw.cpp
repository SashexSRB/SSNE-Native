// MMIO at 0xCC000000. Registers are kept as 16-bit halfwords; 32-bit accesses are split into two halves
// (high half at the lower address), matching the hardware's 16-bit register buses.
#include "ssne/hw.h"
#include "state_io.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "gc/bytes.h"
#include "ssne/audio.h"
#include "ssne/exi.h"
#include "ssne/gx.h"
#include "ssne/input.h"
#include "ssne/ppc_context.h"
#include "ssne/scheduler.h"

namespace fs = std::filesystem;

namespace ssne {

namespace {

uint16_t R[0x10000 / 2];
std::vector<uint8_t> g_aram(16 << 20);
std::atomic<uint64_t> s_aram_dmas{0}, s_disc_reads{0}, s_disc_bytes{0};

inline uint16_t& reg(uint32_t off) { return R[(off & 0xFFFF) >> 1]; }
inline uint32_t reg32(uint32_t off) { return uint32_t(reg(off)) << 16 | reg(off + 2); }

// ------------------------------------------------------------------ disc (served from game_data)
struct Extent { uint64_t offset; uint32_t size; fs::path file; };
std::vector<Extent> g_extents;  // sorted by offset
std::mutex g_disc_mutex;

// ------------------------------------------------------------------ DSP / ARAM (0xCC005000)
constexpr uint16_t DSPCR_RES = 0x0001, DSPCR_AIINT = 0x0008, DSPCR_ARINT = 0x0020, DSPCR_ARINTMSK = 0x0040,
                   DSPCR_DSPINT = 0x0080, DSPCR_DMAINT_BUSY = 0x0200, DSPCR_INIT = 0x0800;

std::mutex g_dspcr_mutex;  // DSPCR interrupt bits are set by the AI DMA thread too

void dspcr_raise(uint16_t bit, uint16_t mask, int irq) {
    bool fire;
    {
        std::lock_guard<std::mutex> lk(g_dspcr_mutex);
        reg(0x500A) |= bit;
        fire = reg(0x500A) & mask;
    }
    if (fire) raise_interrupt(irq);
}

void aram_dma() {
    uint32_t mm = reg32(0x5020) & 0x03FFFFE0;
    uint32_t ar = reg32(0x5024) & 0x00FFFFE0;
    uint32_t cnt = reg32(0x5028);
    bool to_main = cnt & 0x80000000u;
    uint32_t len = cnt & 0x7FFFFFE0;
    for (uint32_t i = 0; i < len; i += 32) {
        uint8_t* m = g_ram + ((mm + i) & (kRamSize - 1));
        uint8_t* a = g_aram.data() + ((ar + i) & 0x00FFFFFF);
        if (to_main) memcpy(m, a, 32); else memcpy(a, m, 32);
    }
    s_aram_dmas++;
    reg(0x5028) = 0; reg(0x502A) = 0;  // count reads back as 0 when done
    dspcr_raise(DSPCR_ARINT, DSPCR_ARINTMSK, IRQ_DSP_ARAM);
}

// ---- DSP stand-in: speaks the mailbox protocol of the DSP ROM boot loader and the AX microcode, without
// mixing any audio yet (silent). Mail from the DSP raises the DSP interrupt (if unmasked), like hardware.
constexpr uint32_t DSP_INIT = 0xDCD10000, DSP_RESUME = 0xDCD10001, DSP_YIELD = 0xDCD10002;
enum class DspState { Boot, BootMails, Running, CmdlistAddr };
DspState g_dsp_state = DspState::Boot;
int g_dsp_boot_mails = 0;
uint16_t g_cmdlist_words = 0;
std::deque<std::pair<uint32_t, bool>> g_dsp_out;   // mail, raise interrupt
std::mutex g_dsp_mutex;
bool g_dsp_trace = getenv("SSNE_TRACE_DSP") != nullptr;

void dsp_push(uint32_t mail, bool irq) {
    if (g_dsp_trace) fprintf(stdout, "[dsp] -> cpu %08X\n", mail);
    g_dsp_out.push_back({mail, irq});
}

void dsp_signal_if_needed() {  // requires g_dsp_mutex
    if (g_dsp_out.empty() || !g_dsp_out.front().second) return;
    g_dsp_out.front().second = false;
    dspcr_raise(DSPCR_DSPINT, 0x0100, IRQ_DSP_DSP);
}

void dsp_mail_from_cpu(uint32_t mail) {  // requires g_dsp_mutex
    if (g_dsp_trace) fprintf(stdout, "[dsp] <- cpu %08X\n", mail);
    switch (g_dsp_state) {
    case DspState::Boot:
    case DspState::BootMails:
        // __DSP_boot_task sends 5 command/value pairs (IRAM addr/len, DRAM, init vector); the last one
        // (0x80F3D001 + vector) starts the microcode, which announces itself with DSP_INIT.
        g_dsp_state = DspState::BootMails;
        if (++g_dsp_boot_mails >= 10) {
            g_dsp_boot_mails = 0;
            g_dsp_state = DspState::Running;
            dsp_push(DSP_INIT, true);
        }
        break;
    case DspState::Running:
        if ((mail & 0xFFFF0000) == 0xBABE0000) {  // AX cmdlist size
            g_cmdlist_words = uint16_t(mail);
            g_dsp_state = DspState::CmdlistAddr;
            break;
        }
        if (mail == 0xCDD10000) { dsp_push(DSP_RESUME, true); break; }                          // MAIL_RESUME
        if (mail == 0xCDD10001 || mail == 0xCDD10002) {                                         // new ucode / reset
            g_dsp_state = DspState::Boot;
            dsp_push(0x8071FEED, false);
        }
        break;
    case DspState::CmdlistAddr:
        ax_process_cmdlist(mail & 0x7FFFFFFF, g_cmdlist_words);
        // Command list address. Mixing isn't implemented: acknowledge the frame immediately.
        g_dsp_state = DspState::Running;
        dsp_push(DSP_YIELD, true);
        break;
    }
    dsp_signal_if_needed();
}

// ------------------------------------------------------------------ AI DMA (0xCC005030)
// The AX library double-buffers 5 ms of 32 kHz stereo (640 bytes). The DMA engine plays 32-byte blocks (8
// frames) in real time; when a buffer is used up it reloads the start/length registers (which the game has
// already pointed at the next buffer) and raises AIDINT. The played samples go to the host audio FIFO.
struct AiDma { uint32_t cur = 0; uint32_t left = 0; bool running = false; } g_ai_dma;
std::mutex g_ai_dma_mutex;
std::once_flag g_ai_thread_once;
std::atomic<bool> g_ai_stop{false};

uint32_t ai_dsp_rate() { return (reg(0x6C02) & 0x40) ? 32000 : 48000; }

// Bumped whenever the game reprograms the DMA (AIInitDMA/AIStartDMA from its AIDINT handler).
std::condition_variable g_ai_prog_cv;
uint64_t g_ai_prog_seq = 0;

void ai_dma_thread() {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    uint64_t blocks_done = 0;
    uint32_t rate = ai_dsp_rate();
    std::vector<uint8_t> out;
    static const int sleep_ms = getenv("SSNE_AI_SLEEP_MS") ? atoi(getenv("SSNE_AI_SLEEP_MS")) : 1;
    bool caught_up = true;
    while (!g_ai_stop) {
        if (caught_up) std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        uint32_t r = ai_dsp_rate();
        auto now = clock::now();
        if (r != rate) { rate = r; t0 = now; blocks_done = 0; }
        uint64_t due = uint64_t(std::chrono::duration<double>(now - t0).count() * rate / 8.0);
        if (due > blocks_done + rate / 8 / 2) blocks_done = due - rate / 8 / 50;  // >0.5 s behind: skip ahead
        out.clear();
        bool buffer_end = false;
        uint64_t seq = 0;
        {
            std::lock_guard<std::mutex> lk(g_ai_dma_mutex);
            // Play blocks up to "now", but stop at the end of each buffer: the game queues the next buffer from
            // the interrupt, so running past it would replay a stale buffer (audible crackle). This matters when
            // the host timer is coarse (Windows sleeps in ~15.6 ms steps by default).
            for (; blocks_done < due && !buffer_end; blocks_done++) {
                if (!(reg(0x5036) & 0x8000)) { out.insert(out.end(), 32, 0); continue; }
                const uint8_t* src = g_ram + (g_ai_dma.cur & (kRamSize - 1) & ~31u);
                out.insert(out.end(), src, src + 32);
                if (g_ai_dma.left) { g_ai_dma.left--; g_ai_dma.cur += 32; }
                if (g_ai_dma.left == 0) {
                    g_ai_dma.cur = reg32(0x5030) & 0x03FFFFE0;
                    g_ai_dma.left = reg(0x5036) & 0x7FFF;
                    buffer_end = true;
                }
            }
            seq = g_ai_prog_seq;
        }
        caught_up = blocks_done >= due;
        if (!out.empty()) audio_push_ai(out.data(), uint32_t(out.size() / 4), rate);
        if (buffer_end) {
            dspcr_raise(DSPCR_AIINT, 0x0010, IRQ_DSP_AI);
            // The buffer that starts now was latched above; wait for the game's handler to queue the one after it
            // (AIInitDMA writes the length register last) so the next buffer end latches fresh audio. If the game
            // is late (e.g. its thread is busy flushing a frame), the audio clock waits for it rather than
            // replaying a stale buffer; the host-side FIFO absorbs the jitter and catch-up is paced by handoffs.
            std::unique_lock<std::mutex> lk(g_ai_dma_mutex);
            g_ai_prog_cv.wait_for(lk, std::chrono::milliseconds(100), [&] { return g_ai_prog_seq != seq || g_ai_stop; });
        }
    }
}

void ai_dma_control(uint16_t v) {
    bool start;
    {
        std::lock_guard<std::mutex> lk(g_ai_dma_mutex);
        bool was = reg(0x5036) & 0x8000;
        reg(0x5036) = v;
        start = !was && (v & 0x8000);
        g_ai_prog_seq++;
        g_ai_prog_cv.notify_all();
        if (start) {
            g_ai_dma.cur = reg32(0x5030) & 0x03FFFFE0;
            g_ai_dma.left = v & 0x7FFF;
        }
    }
    if (start) {
        std::call_once(g_ai_thread_once, [] { std::thread(ai_dma_thread).detach(); });
        dspcr_raise(DSPCR_AIINT, 0x0010, IRQ_DSP_AI);
    }
}

void dsp_write16(uint32_t off, uint16_t v) {
    switch (off) {
    case 0x500A: {
        // Interrupt status bits are write-1-to-clear; reset/halt bits latch.
        std::lock_guard<std::mutex> lk(g_dspcr_mutex);
        uint16_t cur = reg(off);
        uint16_t ack = v & (DSPCR_AIINT | DSPCR_ARINT | DSPCR_DSPINT);
        cur = uint16_t((cur & (DSPCR_AIINT | DSPCR_ARINT | DSPCR_DSPINT)) & ~ack);
        cur |= v & ~(DSPCR_AIINT | DSPCR_ARINT | DSPCR_DSPINT | DSPCR_RES | DSPCR_DMAINT_BUSY | DSPCR_INIT);
        reg(off) = cur;
        return;
    }
    case 0x502A: reg(off) = v; aram_dma(); return;
    case 0x5030: case 0x5032: {
        std::lock_guard<std::mutex> lk(g_ai_dma_mutex);
        reg(off) = off == 0x5030 ? uint16_t(v & 0x03FF) : uint16_t(v & 0xFFE0);
        return;
    }
    case 0x5036: ai_dma_control(v); return;
    case 0x5002: {  // CPU->DSP mailbox low half: completes a mail
        std::lock_guard<std::mutex> lk(g_dsp_mutex);
        reg(off) = v;
        dsp_mail_from_cpu(uint32_t(reg(0x5000) & 0x7FFF) << 16 | v | 0x80000000u);
        return;
    }
    default: reg(off) = v; return;
    }
}

uint16_t dsp_read16(uint32_t off) {
    switch (off) {
    case 0x5000: return reg(off) & 0x7FFF;  // CPU->DSP mailbox: DSP has always read the mail
    case 0x5004: {  // DSP->CPU mailbox high: bit 15 = mail present
        std::lock_guard<std::mutex> lk(g_dsp_mutex);
        if (g_dsp_out.empty() && g_dsp_state == DspState::Boot) dsp_push(0x8071FEED, false);  // DSP ROM is ready
        return g_dsp_out.empty() ? 0 : uint16_t((g_dsp_out.front().first >> 16) | 0x8000);
    }
    case 0x5006: {  // low half: reading it consumes the mail
        std::lock_guard<std::mutex> lk(g_dsp_mutex);
        if (g_dsp_out.empty()) return 0;
        uint16_t lo = uint16_t(g_dsp_out.front().first);
        g_dsp_out.pop_front();
        dsp_signal_if_needed();
        return lo;
    }
    case 0x500A: return reg(off) & ~(DSPCR_RES | DSPCR_DMAINT_BUSY | DSPCR_INIT);
    case 0x5016: return 1;                  // AR_MODE: ARAM ready
    case 0x503A: {                          // AI DMA blocks left (zero-based)
        std::lock_guard<std::mutex> lk(g_ai_dma_mutex);
        return uint16_t(g_ai_dma.left > 0 ? g_ai_dma.left - 1 : 0);
    }
    default: return reg(off);
    }
}

// ------------------------------------------------------------------ DI (0xCC006000)
constexpr uint32_t DISR = 0x6000, DICVR = 0x6004, DICMD0 = 0x6008, DICMD1 = 0x600C, DICMD2 = 0x6010,
                   DIMAR = 0x6014, DILENGTH = 0x6018, DICR = 0x601C, DIIMMBUF = 0x6020;

void set32(uint32_t off, uint32_t v) { reg(off) = uint16_t(v >> 16); reg(off + 2) = uint16_t(v); }

void di_execute() {
    uint32_t cmd = reg32(DICMD0), a1 = reg32(DICMD1), a2 = reg32(DICMD2);
    uint32_t mar = reg32(DIMAR) & 0x03FFFFE0, len = reg32(DILENGTH);
    switch (cmd >> 24) {
    case 0xA8: {  // read (sub 0x40 = read disc ID)
        uint64_t off = (cmd & 0xFF) == 0x40 ? 0 : uint64_t(a1) << 2;
        uint32_t n = (cmd & 0xFF) == 0x40 ? 0x20 : std::min(len, a2 ? a2 : len);
        std::vector<uint8_t> buf(n);
        disc_read(off, buf.data(), n);
        for (uint32_t i = 0; i < n; i++) g_ram[(mar + i) & (kRamSize - 1)] = buf[i];
        set32(DIMAR, (mar + n) | 0x80000000u);
        set32(DILENGTH, len - std::min(len, n));
        s_disc_reads++;
        s_disc_bytes += n;
        break;
    }
    case 0x12: {  // inquiry: drive revision / date (values of a retail drive)
        uint8_t inq[0x20] = {0x00, 0x00, 0x00, 0x02, 0x20, 0x02, 0x04, 0x02, 0x61};
        for (int i = 0; i < 0x20; i++) g_ram[(mar + i) & (kRamSize - 1)] = inq[i];
        set32(DILENGTH, 0);
        break;
    }
    case 0xE0: set32(DIIMMBUF, 0); break;   // request error: none
    case 0xE2: set32(DIIMMBUF, 0); break;   // request audio status: not playing
    case 0xAB: case 0xE1: case 0xE3: case 0xE4: default: break;  // seek, audio stream, stop motor, buffer cfg
    }
    reg(DICR + 2) &= ~1;                    // TSTART clear
    reg(DISR + 2) |= 0x10;                  // TCINT
    if (reg(DISR + 2) & 0x08) raise_interrupt(IRQ_DI);
}

void di_write16(uint32_t off, uint16_t v) {
    if (off == DISR + 2) {  // interrupt bits (0x04 DE, 0x10 TC, 0x40 BRK) are write-1-to-clear
        uint16_t ints = reg(off) & 0x54;
        reg(off) = uint16_t((v & 0x2A) | (ints & ~(v & 0x54)));
        return;
    }
    if (off == DICVR + 2) { reg(off) = uint16_t((v & 0x2) | (reg(off) & 0x4 & ~(v & 0x4))); return; }
    reg(off) = v;
    if (off == DICR + 2 && (v & 1)) di_execute();
}

// ------------------------------------------------------------------ AI (0xCC006C00)
// AICR: bit0 PSTAT (streaming clock running), bit1 AFR (1 = 48 kHz), bit2 AIINTMSK, bit3 AIINT (w1c),
//       bit4 AIINTVLD, bit5 SCRESET (w: reset sample counter), bit6 DSP rate
// AISCNT counts streaming samples while PSTAT = 1. It's derived from the timebase on read.
uint64_t g_ai_base_tb = 0;
uint32_t g_ai_base_count = 0;

uint32_t ai_rate() { return (reg(0x6C02) & 0x2) ? 48000 : 32000; }
uint32_t ai_count() {
    if (!(reg(0x6C02) & 0x1)) return g_ai_base_count;
    uint64_t dt = ssne_timebase() - g_ai_base_tb;           // 40.5 MHz ticks
    return g_ai_base_count + uint32_t(dt * ai_rate() / 40500000ull);
}
void ai_rebase() { g_ai_base_count = ai_count(); g_ai_base_tb = ssne_timebase(); }

void ai_write16(uint32_t off, uint16_t v) {
    if (off == 0x6C02) {
        ai_rebase();
        uint16_t cur = reg(off);
        if (v & 0x20) { g_ai_base_count = 0; g_ai_base_tb = ssne_timebase(); }
        uint16_t aiint = uint16_t(cur & 0x8 & ~(v & 0x8));
        reg(off) = uint16_t((v & ~0x28) | aiint);
        return;
    }
    if (off == 0x6C08 || off == 0x6C0A) { ai_rebase(); g_ai_base_count = 0; return; }
    reg(off) = v;
}

// ------------------------------------------------------------------ VI (0xCC002000)
std::mutex g_vi_mutex;

} // namespace

// ------------------------------------------------------------------ public

bool disc_open(const std::string& data_dir, std::string& error) {
    std::lock_guard<std::mutex> lk(g_disc_mutex);
    g_extents.clear();
    fs::path root = fs::u8path(data_dir);
    auto slurp = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
    };
    auto boot = slurp(root / "sys/boot.bin");
    auto fst = slurp(root / "sys/fst.bin");
    if (boot.size() < 0x440 || fst.size() < 12) { error = "sys/boot.bin or sys/fst.bin missing in " + data_dir; return false; }
    uint32_t dol_off = gc::be32(&boot[0x420]), fst_off = gc::be32(&boot[0x424]);
    auto add = [&](uint64_t off, const fs::path& p) {
        std::error_code ec;
        auto sz = fs::file_size(p, ec);
        if (!ec) g_extents.push_back({off, uint32_t(sz), p});
    };
    add(0, root / "sys/boot.bin");
    add(0x440, root / "sys/bi2.bin");
    add(0x2440, root / "sys/apploader.img");
    add(dol_off, root / "sys/main.dol");
    add(fst_off, root / "sys/fst.bin");

    uint32_t count = gc::be32(&fst[8]);
    const char* strtab = reinterpret_cast<const char*>(&fst[count * 12]);
    std::vector<std::pair<uint32_t, fs::path>> dirs{{count, root / "files"}};
    for (uint32_t i = 1; i < count && (i + 1) * 12 <= fst.size(); i++) {
        while (dirs.size() > 1 && i >= dirs.back().first) dirs.pop_back();
        const uint8_t* e = &fst[i * 12];
        fs::path p = dirs.back().second / fs::u8path(strtab + (gc::be32(e) & 0xFFFFFF));
        if (e[0]) dirs.push_back({gc::be32(e + 8), p});
        else add(gc::be32(e + 4), p);
    }
    std::sort(g_extents.begin(), g_extents.end(), [](auto& a, auto& b) { return a.offset < b.offset; });
    fprintf(stdout, "[ssne] disc: %zu extents mapped from %s\n", g_extents.size(), data_dir.c_str());
    return true;
}

bool disc_read(uint64_t offset, uint8_t* dst, uint32_t len) {
    std::lock_guard<std::mutex> lk(g_disc_mutex);
    memset(dst, 0, len);
    uint64_t end = offset + len;
    auto it = std::upper_bound(g_extents.begin(), g_extents.end(), offset,
                               [](uint64_t o, const Extent& e) { return o < e.offset; });
    if (it != g_extents.begin()) --it;
    bool any = false;
    for (; it != g_extents.end() && it->offset < end; ++it) {
        uint64_t a = std::max(offset, it->offset), b = std::min(end, it->offset + it->size);
        if (a >= b) continue;
        std::ifstream in(it->file, std::ios::binary);
        in.seekg(std::streamoff(a - it->offset));
        in.read(reinterpret_cast<char*>(dst + (a - offset)), std::streamsize(b - a));
        any = true;
        intro_skip_file_read(reinterpret_cast<const char*>(it->file.filename().u8string().c_str()));
    }
    return any;
}

void vi_retrace() {
    si_poll();
    {
        std::lock_guard<std::mutex> lk(g_vi_mutex);
        // Display interrupt 0 fires at the line programmed by the SDK: set its status bit (bit 15 of the
        // high half) if it's enabled (bit 12).
        uint16_t& di0 = reg(0x2030);
        if (di0 & 0x1000) di0 |= 0x8000;
        uint16_t& di1 = reg(0x2034);
        if (di1 & 0x1000) di1 |= 0x8000;
    }
    if ((reg(0x2030) & 0x8000) || (reg(0x2034) & 0x8000)) raise_interrupt(IRQ_VI);
}

uint32_t vi_framebuffer_address() {
    uint32_t tfbl = reg32(0x201C);
    uint32_t addr = tfbl & 0x00FFFFFF;
    if (tfbl & 0x10000000) addr <<= 5;  // page offset bit: address in 32-byte units
    return addr;
}

uint8_t* aram() { return g_aram.data(); }

void hw_state_save(StateWriter& w) {
    w.tag("HW01");
    { std::lock_guard<std::mutex> lk(g_vi_mutex); w.bytes(R, sizeof(R)); }
    w.block(g_aram.data(), g_aram.size());
    {
        std::lock_guard<std::mutex> lk(g_dsp_mutex);
        w.pod(int32_t(g_dsp_state)); w.pod(int32_t(g_dsp_boot_mails)); w.pod(g_cmdlist_words);
        w.u32(uint32_t(g_dsp_out.size()));
        for (auto& m : g_dsp_out) { w.u32(m.first); w.pod(uint8_t(m.second)); }
    }
    { std::lock_guard<std::mutex> lk(g_ai_dma_mutex); w.pod(g_ai_dma); }
    w.u64(g_ai_base_tb); w.u32(g_ai_base_count);
}

bool hw_state_load(StateReader& r) {
    if (!r.tag("HW01")) return false;
    {
        std::lock_guard<std::mutex> lk1(g_vi_mutex);
        std::lock_guard<std::mutex> lk2(g_dspcr_mutex);
        std::lock_guard<std::mutex> lk3(g_ai_dma_mutex);
        r.bytes(R, sizeof(R));
    }
    if (!r.block(g_aram.data(), g_aram.size())) return false;
    {
        std::lock_guard<std::mutex> lk(g_dsp_mutex);
        int32_t st = 0, mails = 0;
        r.pod(st); r.pod(mails); r.pod(g_cmdlist_words);
        g_dsp_state = DspState(st); g_dsp_boot_mails = mails;
        g_dsp_out.clear();
        uint32_t n = r.u32();
        for (uint32_t i = 0; i < n && r.ok; i++) { uint32_t m = r.u32(); uint8_t irq = 0; r.pod(irq); g_dsp_out.push_back({m, irq != 0}); }
    }
    {
        std::lock_guard<std::mutex> lk(g_ai_dma_mutex);
        r.pod(g_ai_dma);
        g_ai_prog_seq++;
        g_ai_prog_cv.notify_all();
    }
    g_ai_base_tb = r.u64(); g_ai_base_count = r.u32();
    if (reg(0x5036) & 0x8000) std::call_once(g_ai_thread_once, [] { std::thread(ai_dma_thread).detach(); });
    return r.ok;
}

HwStats hw_stats() { return {s_aram_dmas.load(), s_disc_reads.load(), s_disc_bytes.load()}; }

// ------------------------------------------------------------------ bus

static uint16_t read16(uint32_t off) {
    if (off >= 0x6400 && off < 0x6500) return si_read16(off);
    if (off < 0x0080) return gx_cp_read16(off);
    if (off >= 0x1000 && off < 0x1080) return gx_pe_read16(off);
    if (off == 0x6C08) return uint16_t(ai_count() >> 16);
    if (off == 0x6C0A) return uint16_t(ai_count());
    if (off >= 0x5000 && off < 0x5200) return dsp_read16(off);
    if (off >= 0x6000 && off < 0x6040) {
        if (off == DICVR + 2) return reg(off) & ~1;  // cover closed
        return reg(off);
    }
    if (off >= 0x6800 && off < 0x6840) return exi_read16(off);
    return reg(off);
}

static void write16(uint32_t off, uint16_t v) {
    if (off >= 0x6400 && off < 0x6500) { si_write16(off, v); return; }
    if (off < 0x0080) { gx_cp_write16(off, v); return; }
    if (off >= 0x1000 && off < 0x1080) { gx_pe_write16(off, v); return; }
    if (off >= 0x5000 && off < 0x5200) { dsp_write16(off, v); return; }
    if (off >= 0x6000 && off < 0x6040) { di_write16(off, v); return; }
    if (off >= 0x6C00 && off < 0x6C20) { ai_write16(off, v); return; }
    if (off >= 0x6800 && off < 0x6840) { exi_write16(off, v); return; }
    if (off >= 0x2000 && off < 0x2100) {
        std::lock_guard<std::mutex> lk(g_vi_mutex);
        reg(off) = v;
        return;
    }
    reg(off) = v;
}

uint32_t mmio_read(uint32_t addr, int size) {
    uint32_t off = addr & 0xFFFF;
    if (size == 4) return uint32_t(read16(off)) << 16 | read16(off + 2);
    if (size == 2) return read16(off & ~1u);
    uint16_t h = read16(off & ~1u);
    return (off & 1) ? (h & 0xFF) : (h >> 8);
}

void mmio_write(uint32_t addr, uint32_t value, int size) {
    uint32_t off = addr & 0xFFFF;
    if (off >= 0x8000) { gx_fifo_write(value, size); return; }  // GX write-gather pipe
    if (size == 4) {
        // DMA-trigger registers must see the high half first; the hardware order is high then low.
        write16(off, uint16_t(value >> 16));
        write16(off + 2, uint16_t(value));
        return;
    }
    if (size == 2) { write16(off & ~1u, uint16_t(value)); return; }
    uint16_t h = reg(off & ~1u);
    h = (off & 1) ? uint16_t((h & 0xFF00) | (value & 0xFF)) : uint16_t((h & 0x00FF) | (value << 8));
    write16(off & ~1u, h);
}

} // namespace ssne
