// Serial interface: one standard GameCube controller on channel 0, nothing on 1-3.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "ssne/input.h"
#include "ssne/scheduler.h"
#include "state_io.h"

namespace ssne {

namespace {

std::mutex M;
PadState g_pads[4];
int g_motor[4] = {};
uint16_t R[0x80];  // halfword registers 0x6400..0x64FF

inline uint16_t& reg(uint32_t off) { return R[((off - 0x6400) & 0xFF) >> 1]; }
inline uint32_t reg32(uint32_t off) { return uint32_t(reg(off)) << 16 | reg(off + 2); }
inline void set32(uint32_t off, uint32_t v) { reg(off) = uint16_t(v >> 16); reg(off + 2) = uint16_t(v); }

uint8_t buf_byte(int i) { uint16_t h = reg(0x6480 + (i & ~1)); return (i & 1) ? uint8_t(h) : uint8_t(h >> 8); }
void set_buf_byte(int i, uint8_t b) {
    uint16_t& h = reg(0x6480 + (i & ~1));
    h = (i & 1) ? uint16_t((h & 0xFF00) | b) : uint16_t((h & 0x00FF) | (b << 8));
}

constexpr uint32_t COMCSR = 0x6434, SISR = 0x6438, SIPOLL = 0x6430;
constexpr uint32_t TCINT = 1u << 31, TCINTMSK = 1u << 30, COMERR = 1u << 29, RDSTINT = 1u << 28, RDSTINTMSK = 1u << 27;

// SISR per-channel bits (shift = 24 - 8 * chan): RDST 0x20, WRST 0x10, NOREP 0x08
inline uint32_t sisr_bit(int chan, uint32_t bit) { return bit << (24 - 8 * chan); }

// ---- skip intros. Boot: "Licensed by Nintendo", memory card check (and the language choice on PAL), then the
// legal screen (Legal.sst), publisher logo, Climax / Croteam / Serious Sam videos and the title screen, ~40 s in
// all. Start pressed on the legal screen jumps straight to the main menu (FrontEnd.ssg), so the port presses it
// once the legal screen is up, and again (up to 3 more times) if the main menu hasn't started loading. Nothing is
// pressed before the legal screen, so memory card and language prompts are never answered for the player.
std::atomic<bool> g_skip_intros{false};
std::atomic<int> g_skip_state{0};      // 0 waiting for Legal.sst, 1 skipping, 2 done (main menu loaded)
std::atomic<int64_t> g_skip_next_ms{0};
int g_skip_presses = 0;
int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
uint16_t intro_skip_buttons() {
    if (!g_skip_intros || g_skip_state != 1) return 0;
    int64_t t = now_ms(), next = g_skip_next_ms;
    if (t < next) return 0;
    if (t < next + 200) return 0x1000;  // Start held for 200 ms
    if (++g_skip_presses >= 4) { g_skip_state = 2; return 0; }
    g_skip_next_ms = t + 1500;          // try again if the menu doesn't load
    return 0;
}

void report(int chan, uint32_t out[2]) {
    const PadState& p = g_pads[chan];
    uint16_t b = uint16_t(p.buttons | 0x0080);  // "use origin"
    if (chan == 0) b |= intro_skip_buttons();
    out[0] = uint32_t(b) << 16 | uint32_t(p.stick_x) << 8 | p.stick_y;
    out[1] = uint32_t(p.cstick_x) << 24 | uint32_t(p.cstick_y) << 16 | uint32_t(p.trigger_l) << 8 | p.trigger_r;
}

void transfer(uint32_t csr) {
    int chan = int((csr >> 1) & 3);
    int outlen = int((csr >> 16) & 0x7F), inlen = int((csr >> 8) & 0x7F);
    if (!outlen) outlen = 128;
    if (!inlen) inlen = 128;
    uint8_t cmd = buf_byte(0);
    bool present = chan == 0;
    uint32_t status = csr & ~(1u | COMERR);
    if (!present) {
        status |= COMERR;
        set32(SISR, reg32(SISR) | sisr_bit(chan, 0x08));  // NOREP
    } else {
        uint8_t resp[10] = {};
        int n = 0;
        switch (cmd) {
        case 0x00: case 0xFF: resp[0] = 0x09; resp[1] = 0x00; resp[2] = 0x00; n = 3; break;  // standard controller
        case 0x40: {
            uint32_t r[2];
            report(chan, r);
            for (int k = 0; k < 4; k++) { resp[k] = uint8_t(r[0] >> (24 - 8 * k)); resp[4 + k] = uint8_t(r[1] >> (24 - 8 * k)); }
            n = 8;
            break;
        }
        case 0x41: case 0x42: {  // origin / recalibrate: neutral sticks, triggers released
            const uint8_t origin[10] = {0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x1F, 0x1F, 0x00, 0x00};
            for (int k = 0; k < 10; k++) resp[k] = origin[k];
            n = 10;
            break;
        }
        default: n = 0; break;
        }
        for (int k = 0; k < inlen && k < 128; k++) set_buf_byte(k, k < n ? resp[k] : 0);
    }
    status |= TCINT;
    set32(COMCSR, status);
    if (status & TCINTMSK) raise_interrupt(IRQ_SI);
}

} // namespace

void si_state_save(StateWriter& w) {
    std::lock_guard<std::mutex> lk(M);
    w.tag("SI01");
    w.bytes(R, sizeof(R));
    w.bytes(g_motor, sizeof(g_motor));
}
bool si_state_load(StateReader& r) {
    std::lock_guard<std::mutex> lk(M);
    if (!r.tag("SI01")) return false;
    r.bytes(R, sizeof(R));
    r.bytes(g_motor, sizeof(g_motor));
    return r.ok;
}

void intro_skip_file_read(const char* name) {
    int st = g_skip_state;
    if (st == 0 && !strcmp(name, "Legal.sst")) { g_skip_next_ms = now_ms() + 800; g_skip_state = 1; }
    else if (st != 2 && !strcmp(name, "FrontEnd.ssg")) g_skip_state = 2;
}
void set_skip_intros(bool on) { g_skip_intros = on; }

void set_pad(int port, const PadState& s) { std::lock_guard<std::mutex> lk(M); g_pads[port & 3] = s; }
PadState get_pad(int port) { std::lock_guard<std::mutex> lk(M); return g_pads[port & 3]; }
int pad_motor(int port) { std::lock_guard<std::mutex> lk(M); return g_motor[port & 3]; }

uint16_t si_read16(uint32_t off) {
    std::lock_guard<std::mutex> lk(M);
    // Reading INBUFH of a channel clears its RDST status.
    for (int ch = 0; ch < 4; ch++)
        if (off == 0x6404u + 12u * uint32_t(ch)) set32(SISR, reg32(SISR) & ~sisr_bit(ch, 0x20));
    return reg(off);
}

void si_write16(uint32_t off, uint16_t v) {
    std::lock_guard<std::mutex> lk(M);
    if (off == COMCSR) {  // high half: TCINT / RDSTINT are write-1-to-clear
        uint16_t cur = reg(off);
        uint16_t w1c = uint16_t((TCINT | RDSTINT) >> 16);
        reg(off) = uint16_t((v & ~w1c) | (cur & w1c & ~(v & w1c)));
        return;
    }
    if (off == COMCSR + 2) {
        reg(off) = v;
        if (v & 1) transfer(reg32(COMCSR));
        return;
    }
    if (off == SISR) {  // error bits are write-1-to-clear; WR (bit 31) latches the command buffers
        uint16_t cur = reg(off);
        reg(off) = uint16_t(cur & ~(v & 0x0F0F));
        return;
    }
    if (off == SISR + 2) { reg(off) = uint16_t(reg(off) & ~(v & 0x0F0F)); return; }
    // Channel output buffer (SIC0OUTBUF + 12 * chan) = 0x0040030M: the poll command (0x40, 0x03) carries the rumble
    // motor command in its last byte (PADControlMotor: 0 stop, 1 rumble, 2 hard stop).
    if (off < 0x6430 && (off - 0x6400) % 12 == 2 && (reg(off - 2) & 0xFF) == 0x40) {
        int ch = int((off - 0x6400) / 12);
        int m = v & 3;
        if (m != g_motor[ch]) {
            static const bool trace = getenv("SSNE_TRACE_RUMBLE") != nullptr;
            if (trace) fprintf(stdout, "[si] port %d motor %d\n", ch + 1, m);
            g_motor[ch] = m;
        }
    }
    reg(off) = v;
}

void si_poll() {
    bool irq = false;
    {
        std::lock_guard<std::mutex> lk(M);
        uint32_t poll = reg32(SIPOLL);
        for (int ch = 0; ch < 4; ch++) {
            if (!((poll >> (7 - ch)) & 1)) continue;
            if (ch != 0) { set32(SISR, reg32(SISR) | sisr_bit(ch, 0x08)); continue; }
            uint32_t r[2];
            report(ch, r);
            set32(0x6404 + 12 * ch, r[0]);
            set32(0x6408 + 12 * ch, r[1]);
            set32(SISR, reg32(SISR) | sisr_bit(ch, 0x20));
            uint32_t csr = reg32(COMCSR);
            if (csr & RDSTINTMSK) { set32(COMCSR, csr | RDSTINT); irq = true; }
        }
    }
    if (irq) raise_interrupt(IRQ_SI);
}

} // namespace ssne
