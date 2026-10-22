// AX audio microcode HLE + host audio FIFO.
//
// The game's AX library (Dolphin SDK, ucode CRC 0x07F88145: the standard GameCube AX with a low-pass filter in
// the parameter block) sends the DSP a command list every 5 ms. The DSP walks the linked list of voice parameter
// blocks (AXPB) in main RAM, decodes each running voice from ARAM (DSP-ADPCM / PCM8 / PCM16 through the DSP's
// sample accelerator), resamples it, applies the volume envelope and low-pass filter, and mixes it into
// main/auxA/auxB buses. The final main L/R mix is written back to RAM as 160 stereo frames, which the AX library
// then plays with AI DMA. This file does the DSP's part natively.
//
// Behavior follows the reverse-engineered description of the ucode (Dolphin's AX HLE was used as the reference).
#include "ssne/audio.h"
#include "state_io.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "ssne/hw.h"
#include "ssne/ppc_context.h"

namespace ssne {

namespace {

// ------------------------------------------------------------------ guest memory helpers
inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, g_ram + (a & (kRamSize - 1)), 2); return __builtin_bswap16(v); }
inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, g_ram + (a & (kRamSize - 1)), 4); return __builtin_bswap32(v); }
inline void wr16(uint32_t a, uint16_t x) { x = __builtin_bswap16(x); memcpy(g_ram + (a & (kRamSize - 1)), &x, 2); }
inline void wr32(uint32_t a, uint32_t x) { x = __builtin_bswap32(x); memcpy(g_ram + (a & (kRamSize - 1)), &x, 4); }
inline uint8_t aram8(uint32_t a) { return aram()[a & 0x00FFFFFF]; }

inline int16_t clamp16(int64_t v) { return int16_t(std::clamp<int64_t>(v, -32768, 32767)); }

// ------------------------------------------------------------------ AXPB (u16 word offsets)
enum : int {
    PB_NEXT_HI = 0, PB_NEXT_LO = 1, PB_SRC_TYPE = 4, PB_COEF_SEL = 5, PB_MIXER_CTRL = 6, PB_RUNNING = 7, PB_IS_STREAM = 8,
    PB_MIXER = 9,        // 9 x (volume, delta): mainL mainR auxAL auxAR auxBL auxBR auxBS mainS auxAS
    PB_ITD = 27,
    PB_NUM_UPDATES = 34, PB_UPD_HI = 39, PB_UPD_LO = 40,
    PB_DPOP = 41,        // mainL auxAL auxBL mainR auxAR auxBR mainS auxAS auxBS
    PB_VE_CUR = 50, PB_VE_DELTA = 51,
    PB_LOOPING = 55, PB_FORMAT = 56, PB_LOOP_HI = 57, PB_LOOP_LO = 58, PB_END_HI = 59, PB_END_LO = 60,
    PB_CUR_HI = 61, PB_CUR_LO = 62,
    PB_COEFS = 63, PB_GAIN = 79, PB_PRED_SCALE = 80, PB_YN1 = 81, PB_YN2 = 82,
    PB_RATIO_HI = 83, PB_RATIO_LO = 84, PB_FRAC = 85, PB_LAST = 86,
    PB_LOOP_PS = 90, PB_LOOP_YN1 = 91, PB_LOOP_YN2 = 92,
    PB_LPF_ON = 93, PB_LPF_YN1 = 94, PB_LPF_A0 = 95, PB_LPF_B0 = 96,
    PB_LOOP_COUNTER = 97,
    PB_WORDS = 122,      // including padding (read), 98 words written back
    PB_WRITE_WORDS = 98,
};

constexpr int kSpms = 32;           // samples per millisecond (AX always mixes at 32 kHz)
constexpr int kFrame = 5 * kSpms;   // 160 samples per 5 ms frame

// Mix buses.
enum Bus { MAIN_L, MAIN_R, MAIN_S, AUXA_L, AUXA_R, AUXA_S, AUXB_L, AUXB_R, AUXB_S, NUM_BUSES };
int g_bus[NUM_BUSES][kFrame];

std::atomic<uint64_t> s_cmdlists{0}, s_voices{0};
std::atomic<uint32_t> s_active{0};
bool g_ax_trace = getenv("SSNE_TRACE_AX") != nullptr;

// ------------------------------------------------------------------ sample accelerator
struct Accel {
    uint32_t start, end, cur;
    uint16_t format;
    int16_t yn1, yn2, gain;
    uint16_t ps;
    bool stopped = false;
    uint16_t* pb;   // PB words (for loop handling)

    int16_t read() {
        if (stopped) return 0;
        int size = format & 3, decode = (format >> 2) & 3, gscale = (format >> 4) & 3;
        int ci = (ps >> 4) & 7;
        int32_t c1 = int16_t(pb[PB_COEFS + ci * 2]), c2 = int16_t(pb[PB_COEFS + ci * 2 + 1]);

        uint16_t raw = 0;
        switch (size) {
        case 0: raw = aram8(cur >> 1); raw = (cur & 1) ? (raw & 0xF) : (raw >> 4); break;
        case 1: raw = aram8(cur); break;
        case 2: raw = uint16_t(aram8(cur * 2) << 8 | aram8(cur * 2 + 1)); break;
        default: break;
        }
        int16_t val = 0;
        int step = 2;
        if (decode == 0) {  // DSP-ADPCM
            int s = raw & 0xF;
            if (s >= 8) s -= 16;
            int32_t v = (int32_t(1 << (ps & 0xF)) * s) + ((0x400 + c1 * yn1 + c2 * yn2) >> 11);
            val = clamp16(v);
            yn2 = yn1; yn1 = val;
            cur += 1;
            if ((end & 0xF) == 0 && cur == end) cur = start + 1;
            else if ((end & 0xF) == 1 && cur == end - 1) cur = start;
            else if ((cur & 15) == 0) {
                ps = aram8((cur & ~15u) >> 1) & 0x7F;
                cur += 2;
                step += 2;
            }
        } else {  // PCM (decode 2) / MMIO modes (treated as PCM from ARAM)
            int shift = gscale == 1 ? 0 : gscale == 2 ? 16 : 11;
            int32_t v = ((int32_t(gain) * int16_t(raw)) >> shift) + ((c1 * yn1) >> shift) + ((c2 * yn2) >> shift);
            val = int16_t(v);
            yn2 = yn1; yn1 = val;
            if (decode != 1) cur += 1;
        }
        if (cur == end + step - 1) {  // end of sample reached
            cur = start;
            stopped = true;
            if (pb[PB_LOOPING]) {
                ps = pb[PB_LOOP_PS] & 0x7F;
                if (pb[PB_IS_STREAM] != 1) { yn1 = int16_t(pb[PB_LOOP_YN1]); yn2 = int16_t(pb[PB_LOOP_YN2]); }
                else pb[PB_LOOP_COUNTER]++;
                stopped = false;
            } else {
                pb[PB_RUNNING] = 0;
            }
        }
        cur &= 0xBFFFFFFF;
        return val;
    }
};

inline uint32_t hilo(const uint16_t* w, int i) { return uint32_t(w[i]) << 16 | w[i + 1]; }

// ------------------------------------------------------------------ resampling
constexpr double kPi = 3.14159265358979323846;
std::atomic<bool> g_hq{getenv("SSNE_AUDIO_ORIGINAL") == nullptr};

// 4-tap polyphase (src type 0). The DSP takes 128-phase x 4-tap coefficient tables from its ROM, selected by
// PB_COEF_SEL (the SDK's AX_SRC_TYPE_4TAP_8K / 12K / 16K). The ROM is not available, so equivalent windowed-sinc
// low-pass tables are generated: cutoffs of 1/2, 3/4 and all of the input band (the 16K table is exact at frac 0).
struct Poly4 { int16_t c[3][128][4]; };
const Poly4& poly4() {
    static const Poly4 tab = [] {
        Poly4 t{};
        const double cut[3] = {0.25, 0.375, 0.5};  // cycles per input sample
        for (int sel = 0; sel < 3; sel++)
            for (int ph = 0; ph < 128; ph++) {
                double f = ph / 128.0, h[4], sum = 0;
                for (int k = 0; k < 4; k++) {
                    double x = (k - 1) - f;  // taps at offsets -1, 0, +1, +2 around the interpolation point
                    double a = 2 * cut[sel] * x;
                    double sinc = std::abs(a) < 1e-9 ? 1.0 : std::sin(kPi * a) / (kPi * a);
                    double w = std::abs(x) >= 2 ? 0.0 : 0.5 * (1 + std::cos(kPi * x / 2));
                    h[k] = sinc * w;
                    sum += h[k];
                }
                for (int k = 0; k < 4; k++) t.c[sel][ph][k] = int16_t(std::lround(h[k] / sum * 32767.0));
            }
        return t;
    }();
    return tab;
}

// High quality: 64-tap Kaiser-windowed sinc (about 70 dB stopband). The cutoff follows the voice's rate ratio so that downsampling
// (ratio > 1) is band-limited to the 16 kHz output band. Tables are built per cutoff bucket on first use.
constexpr int kTaps = 64, kHistory = 128, kHalf = kTaps / 2, kPhases = 256;
double bessel_i0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
    return s;
}
struct SincTable { float c[kPhases + 1][kTaps]; };
const SincTable& sinc_table(int bucket) {  // bucket: cutoff scale min(1, 1/ratio) in 1/64 steps (1..64)
    static std::unique_ptr<SincTable> tabs[65];
    auto& t = tabs[bucket];
    if (!t) {
        t = std::make_unique<SincTable>();
        // Cutoff in cycles per input sample: the transition band (~0.067) ends at the output band edge.
        const double fc = 0.5 * bucket / 64.0 - 0.034;
        const double beta = 7.0, i0b = bessel_i0(beta);
        for (int ph = 0; ph <= kPhases; ph++) {
            double f = double(ph) / kPhases, sum = 0, h[kTaps];
            for (int k = 0; k < kTaps; k++) {
                double x = (k - (kHalf - 1)) - f;  // taps at offsets -31 .. +32
                double a = 2 * fc * x;
                double sinc = std::abs(a) < 1e-9 ? 1.0 : std::sin(kPi * a) / (kPi * a);
                double r = x / kHalf;
                double w = std::abs(r) >= 1 ? 0.0 : bessel_i0(beta * std::sqrt(1 - r * r)) / i0b;
                h[k] = sinc * w;
                sum += h[k];
            }
            for (int k = 0; k < kTaps; k++) t->c[ph][k] = float(h[k] / sum);
        }
    }
    return *t;
}

// Input history for the high-quality filter, kept on the host per parameter block. PB_LAST still holds the four
// newest samples; when they don't match (voice restarted or switched modes) the history is reset from them.
struct VoiceHist { int16_t h[kHistory]; uint32_t n; };
std::unordered_map<uint32_t, VoiceHist> g_hist;

void get_input_samples(uint16_t* pb, uint32_t pb_addr, int16_t* out, int count) {
    Accel a;
    a.pb = pb;
    a.start = hilo(pb, PB_LOOP_HI) & 0x3FFFFFFF;
    a.end = hilo(pb, PB_END_HI) & 0x3FFFFFFF;
    a.cur = hilo(pb, PB_CUR_HI) & 0xBFFFFFFF;
    a.format = pb[PB_FORMAT];
    a.yn1 = int16_t(pb[PB_YN1]);
    a.yn2 = int16_t(pb[PB_YN2]);
    a.gain = int16_t(pb[PB_GAIN]);
    a.ps = pb[PB_PRED_SCALE] & 0x7F;

    uint32_t ratio = hilo(pb, PB_RATIO_HI);
    uint32_t pos = pb[PB_FRAC];
    int16_t* last = reinterpret_cast<int16_t*>(pb + PB_LAST);
    int src_type = pb[PB_SRC_TYPE];

    if ((src_type == 0 || src_type == 1) && g_hq.load(std::memory_order_relaxed)) {
        VoiceHist& vh = g_hist[pb_addr];
        bool match = vh.n >= 4;
        for (int k = 0; k < 4 && match; k++) match = vh.h[(vh.n - 4 + k) % kHistory] == last[k];
        if (!match) {
            memset(vh.h, 0, sizeof(vh.h));
            vh.n = kHistory;
            for (int k = 0; k < 4; k++) vh.h[kHistory - 4 + k] = last[k];
        }
        double scale = ratio > 0x10000 ? double(0x10000) / ratio : 1.0;
        int bucket = std::clamp(int(std::ceil(scale * 64 - 1e-6)), 1, 64);
        const SincTable& tab = sinc_table(bucket);
        uint32_t n = vh.n;
        for (int i = 0; i < count; i++) {
            pos += ratio;
            while (pos >= 0x10000) { vh.h[n++ % kHistory] = a.read(); pos -= 0x10000; }
            uint16_t frac = uint16_t(pos);
            // Interpolate between samples n-1-kHalf and n-kHalf (a fixed kHalf-sample delay gives the lookahead).
            uint32_t base = n - kTaps;  // oldest tap
            if (!frac) { out[i] = vh.h[(base + kHalf - 1) % kHistory]; continue; }
            uint32_t pf = uint32_t(frac) * kPhases;
            int ph = int(pf >> 16);
            float w = float(pf & 0xFFFF) * (1.0f / 65536.0f);
            const float* c0 = tab.c[ph];
            const float* c1 = tab.c[ph + 1];
            float acc = 0;
            for (int k = 0; k < kTaps; k++) {
                float c = c0[k] + (c1[k] - c0[k]) * w;
                acc += c * vh.h[(base + k) % kHistory];
            }
            out[i] = clamp16(std::lround(acc));
        }
        vh.n = n;
        for (int k = 0; k < 4; k++) last[k] = vh.h[(n - 4 + k) % kHistory];
    } else if (src_type == 0) {
        // DSP 4-tap polyphase: interpolate between t[n-3] and t[n-2] using t[n-4 .. n-1].
        const auto& c = poly4().c[std::min<int>(pb[PB_COEF_SEL], 2)];
        int16_t t[4] = {last[0], last[1], last[2], last[3]};
        uint32_t idx = 4;
        for (int i = 0; i < count; i++) {
            pos += ratio;
            while (pos >= 0x10000) { t[idx++ & 3] = a.read(); pos -= 0x10000; }
            const int16_t* cc = c[uint16_t(pos) >> 9];
            int32_t s = 0;
            for (int k = 0; k < 4; k++) s += int32_t(t[(idx + k) & 3]) * cc[k];
            out[i] = clamp16((s + 0x4000) >> 15);
        }
        for (int k = 0; k < 4; k++) last[k] = t[(idx + k) & 3];
    } else if (src_type == 1) {
        int16_t t[4] = {last[0], last[1], last[2], last[3]};
        uint32_t idx = 4;
        for (int i = 0; i < count; i++) {
            pos += ratio;
            while (pos >= 0x10000) { t[idx++ & 3] = a.read(); pos -= 0x10000; }
            uint16_t frac = uint16_t(pos);
            int16_t s;
            if (frac) {
                // Interpolate between the two most recent input samples.
                int32_t s0 = t[(idx - 2) & 3], s1 = t[(idx - 1) & 3];
                s = int16_t((s0 * int32_t(uint16_t(-frac)) + s1 * int32_t(frac)) >> 16);
            } else {
                s = t[(idx - 2) & 3];
            }
            out[i] = s;
        }
        for (int k = 0; k < 4; k++) last[k] = t[(idx + k) & 3];
    } else {  // nearest / no conversion
        for (int i = 0; i < count; i++) out[i] = a.read();
        memcpy(last, out + count - 4, 8);
    }
    pb[PB_FRAC] = uint16_t(pos);
    pb[PB_CUR_HI] = uint16_t(a.cur >> 16);
    pb[PB_CUR_LO] = uint16_t(a.cur);
    pb[PB_YN1] = uint16_t(a.yn1);
    pb[PB_YN2] = uint16_t(a.yn2);
    pb[PB_PRED_SCALE] = a.ps;
}

void mix_add(int* out, const int16_t* in, int count, uint16_t* vol, int16_t* dpop, bool ramp) {
    uint16_t v = vol[0];
    uint16_t d = ramp ? vol[1] : 0;
    for (int i = 0; i < count; i++) {
        int16_t s = clamp16((int64_t(in[i]) * v) >> 15);
        out[i] += s;
        v = uint16_t(v + d);
        *dpop = s;
    }
    vol[0] = v;
}

// Separate volumes: music and video sound are the streamed voices (DSP-ADPCM ring buffers refilled from
// StreamData.dat) and the PCM16 voices (Bink video audio, streamed music); everything else (DSP-ADPCM one-shots
// from the sound bank) is sound effects. Applied per voice before the game's own mixer volumes.
std::atomic<int> g_music_pct{100}, g_sfx_pct{100};
bool is_music_voice(const uint16_t* pb) { return pb[PB_IS_STREAM] == 1 || pb[PB_FORMAT] == 0x0A; }

void process_voice(uint16_t* pb, uint32_t pb_addr, int* const bus[NUM_BUSES], int count) {
    if (pb[PB_RUNNING] != 1) return;
    int16_t smp[kSpms];
    get_input_samples(pb, pb_addr, smp, count);
    if (int pct = is_music_voice(pb) ? g_music_pct.load() : g_sfx_pct.load(); pct != 100)
        for (int i = 0; i < count; i++) smp[i] = clamp16(int32_t(smp[i]) * pct / 100);

    for (int i = 0; i < count; i++) {
        int32_t vol = int16_t(pb[PB_VE_CUR]);
        smp[i] = clamp16((int32_t(smp[i]) * vol) >> 15);
        pb[PB_VE_CUR] = uint16_t(pb[PB_VE_CUR] + pb[PB_VE_DELTA]);
    }
    if (pb[PB_LPF_ON]) {
        int32_t a0 = pb[PB_LPF_A0], b0 = int16_t(pb[PB_LPF_B0]);
        int16_t y = int16_t(pb[PB_LPF_YN1]);
        for (int i = 0; i < count; i++) y = smp[i] = clamp16((a0 * smp[i] + b0 * y) >> 15);
        pb[PB_LPF_YN1] = uint16_t(y);
    }

    uint16_t mc = pb[PB_MIXER_CTRL];
    auto dpop = [&](int k) { return reinterpret_cast<int16_t*>(pb + PB_DPOP + k); };
    auto vol = [&](int k) { return pb + PB_MIXER + 2 * k; };
    // mixer slots: 0 mainL 1 mainR 2 auxAL 3 auxAR 4 auxBL 5 auxBR 6 auxBS 7 mainS 8 auxAS
    // dpop slots:  0 mainL 1 auxAL 2 auxBL 3 mainR 4 auxAR 5 auxBR 6 mainS 7 auxAS 8 auxBS
    if (mc & 0x0001) mix_add(bus[MAIN_L], smp, count, vol(0), dpop(0), mc & 0x0008);
    if (mc & 0x0002) mix_add(bus[MAIN_R], smp, count, vol(1), dpop(3), mc & 0x0008);
    if (mc & 0x0004) mix_add(bus[MAIN_S], smp, count, vol(7), dpop(6), mc & 0x0008);
    if (mc & 0x0010) mix_add(bus[AUXA_L], smp, count, vol(2), dpop(1), mc & 0x0040);
    if (mc & 0x0020) mix_add(bus[AUXA_R], smp, count, vol(3), dpop(4), mc & 0x0040);
    if (mc & 0x0080) mix_add(bus[AUXA_S], smp, count, vol(8), dpop(7), mc & 0x0100);
    if (mc & 0x0200) mix_add(bus[AUXB_L], smp, count, vol(4), dpop(2), mc & 0x0800);
    if (mc & 0x0400) mix_add(bus[AUXB_R], smp, count, vol(5), dpop(5), mc & 0x0800);
    if (mc & 0x1000) mix_add(bus[AUXB_S], smp, count, vol(6), dpop(8), mc & 0x2000);
}

void process_pb_list(uint32_t pb_addr) {
    uint16_t pb[PB_WORDS];
    uint32_t active = 0;
    int guard = 0;
    while (pb_addr && guard++ < 256) {
        pb_addr &= kRamSize - 1;
        for (int i = 0; i < PB_WORDS; i++) pb[i] = rd16(pb_addr + i * 2);

        uint32_t upd_addr = hilo(pb, PB_UPD_HI);
        uint16_t nupd[5];
        memcpy(nupd, pb + PB_NUM_UPDATES, sizeof(nupd));
        uint32_t upd_idx = 0;
        if (pb[PB_RUNNING] == 1) {
            active++;
            if (g_ax_trace && s_cmdlists % 200 == 0)
                fprintf(stdout, "[ax] pb %08X fmt %04X src %u coef %u ratio %08X mc %04X ve %04X/%04X lpf %u loop %u stream %u "
                        "cur %08X end %08X loopa %08X gain %04X mixL %04X mixR %04X\n", pb_addr, pb[PB_FORMAT],
                        pb[PB_SRC_TYPE], pb[PB_COEF_SEL], hilo(pb, PB_RATIO_HI), pb[PB_MIXER_CTRL], pb[PB_VE_CUR], pb[PB_VE_DELTA],
                        pb[PB_LPF_ON], pb[PB_LOOPING], pb[PB_IS_STREAM], hilo(pb, PB_CUR_HI), hilo(pb, PB_END_HI),
                        hilo(pb, PB_LOOP_HI), pb[PB_GAIN], pb[PB_MIXER], pb[PB_MIXER + 2]);
        }

        int* bus[NUM_BUSES];
        for (int b = 0; b < NUM_BUSES; b++) bus[b] = g_bus[b];
        for (int ms = 0; ms < 5; ms++) {
            for (uint32_t u = 0; u < nupd[ms] && upd_idx < 1024; u++, upd_idx++) {
                uint16_t off = rd16(upd_addr + upd_idx * 4), val = rd16(upd_addr + upd_idx * 4 + 2);
                if (off < PB_WORDS) pb[off] = val;
            }
            process_voice(pb, pb_addr, bus, kSpms);
            for (auto& p : bus) p += kSpms;
        }
        for (int i = 0; i < PB_WRITE_WORDS; i++) wr16(pb_addr + i * 2, pb[i]);
        s_voices++;
        pb_addr = hilo(pb, PB_NEXT_HI);
    }
    s_active = active;
}

void upload(uint32_t addr, const int* buf) {
    for (int i = 0; i < kFrame; i++) wr32(addr + i * 4, uint32_t(buf[i]));
}

void mix_aux(int aux, uint32_t write_addr, uint32_t read_addr) {
    int base = aux == 0 ? AUXA_L : AUXB_L;
    if (write_addr)
        for (int k = 0; k < 3; k++) upload(write_addr + k * kFrame * 4, g_bus[base + k]);
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < kFrame; i++) g_bus[MAIN_L + k][i] += int32_t(rd32(read_addr + (k * kFrame + i) * 4));
}

} // namespace

void ax_process_cmdlist(uint32_t addr, uint16_t size_words) {
    (void)size_words;
    s_cmdlists++;
    uint32_t p = addr & (kRamSize - 1);
    uint32_t pb_addr = 0;
    auto next = [&]() { uint16_t v = rd16(p); p += 2; return v; };
    auto next32 = [&]() { uint32_t hi = next(); return hi << 16 | next(); };
    for (int n = 0; n < 4096; n++) {
        uint16_t cmd = next();
        if (g_ax_trace && s_cmdlists < 4) fprintf(stdout, "[ax] cmd %02X\n", cmd);
        switch (cmd) {
        case 0x00: {  // SETUP: initial values (hi, lo, delta) for the 9 buses
            uint32_t a = next32();
            for (int b = 0; b < NUM_BUSES; b++) {
                int32_t v = int32_t(uint32_t(rd16(a + b * 6)) << 16 | rd16(a + b * 6 + 2));
                int16_t d = int16_t(rd16(a + b * 6 + 4));
                for (int i = 0; i < kFrame; i++) g_bus[b][i] = v ? v + i * d : 0;
            }
            break;
        }
        case 0x01: {  // DL_AND_VOL_MIX
            uint32_t a = next32();
            uint16_t vols[3] = {next(), next(), next()};
            for (int g = 0; g < 3; g++)
                for (int k = 0; k < 3; k++)
                    for (int i = 0; i < kFrame; i++) {
                        int64_t s = int32_t(rd32(a + (k * kFrame + i) * 4));
                        g_bus[g * 3 + k][i] += int32_t((s * vols[g]) >> 15);
                    }
            break;
        }
        case 0x02: pb_addr = next32(); break;
        case 0x03: process_pb_list(pb_addr); break;
        case 0x04:
        case 0x05: { uint32_t w = next32(), r = next32(); mix_aux(cmd - 4, w, r); break; }
        case 0x06: {  // UPLOAD_LRS
            uint32_t a = next32();
            for (int k = 0; k < 3; k++) upload(a + k * kFrame * 4, g_bus[MAIN_L + k]);
            break;
        }
        case 0x07: {  // SET_LR
            uint32_t a = next32();
            for (int i = 0; i < kFrame; i++) {
                int32_t s = int32_t(rd32(a + i * 4));
                g_bus[MAIN_L][i] = s; g_bus[MAIN_R][i] = s; g_bus[MAIN_S][i] = 0;
            }
            break;
        }
        case 0x08: p += 20; break;
        case 0x09: { uint32_t r = next32(); mix_aux(1, 0, r); break; }
        case 0x0A: case 0x0B: case 0x0C: break;
        case 0x0D: {  // MORE: continue with another command list
            uint32_t a = next32();
            next();
            p = a & (kRamSize - 1);
            break;
        }
        case 0x0E: {  // OUTPUT: surround upload + interleaved R,L s16
            uint32_t surround = next32(), lr = next32();
            upload(surround, g_bus[MAIN_S]);
            for (int i = 0; i < kFrame; i++) {
                wr16(lr + i * 4, uint16_t(clamp16(g_bus[MAIN_R][i])));
                wr16(lr + i * 4 + 2, uint16_t(clamp16(g_bus[MAIN_L][i])));
            }
            break;
        }
        case 0x0F: return;  // END
        case 0x10: {  // MIX_AUXB_LR
            uint32_t ul = next32(), dl = next32();
            upload(ul, g_bus[AUXB_L]);
            upload(ul + kFrame * 4, g_bus[AUXB_R]);
            for (int k = 0; k < 2; k++)
                for (int i = 0; i < kFrame; i++) {
                    int32_t s = int32_t(rd32(dl + (k * kFrame + i) * 4));
                    g_bus[AUXB_L + k][i] = s;
                    g_bus[MAIN_L + k][i] += s;
                }
            break;
        }
        case 0x11: {  // SET_OPPOSITE_LR
            uint32_t a = next32();
            for (int i = 0; i < kFrame; i++) {
                int32_t s = int32_t(rd32(a + i * 4));
                g_bus[MAIN_L][i] = -s; g_bus[MAIN_R][i] = s; g_bus[MAIN_S][i] = 0;
            }
            break;
        }
        case 0x12: {  // COMPRESSOR
            static uint32_t pos = 0;
            uint16_t threshold = next(), frames = next();
            uint32_t table = next32();
            bool trig = false;
            for (int i = 0; i < kFrame && !trig; i++)
                trig = std::abs(g_bus[MAIN_L][i]) > int(threshold) || std::abs(g_bus[MAIN_R][i]) > int(threshold);
            uint32_t off;
            if (trig) { off = pos * kFrame * 2; pos = frames; }
            else if (pos) { --pos; off = (11 + pos) * kFrame * 2; }
            else break;
            for (int i = 0; i < kFrame; i++) {
                uint16_t c = rd16(table + off + i * 2);
                g_bus[MAIN_L][i] = int32_t((int64_t(g_bus[MAIN_L][i]) * c) >> 15);
                g_bus[MAIN_R][i] = int32_t((int64_t(g_bus[MAIN_R][i]) * c) >> 15);
            }
            break;
        }
        case 0x13: {  // SEND_AUX_AND_MIX
            uint32_t auxa_up = next32(), auxb_s_up = next32();
            uint32_t dl[4] = {next32(), next32(), next32(), next32()};
            for (int k = 0; k < 3; k++) upload(auxa_up + k * kFrame * 4, g_bus[AUXA_L + k]);
            upload(auxb_s_up, g_bus[AUXB_S]);
            const int dst[4] = {MAIN_L, MAIN_R, AUXB_L, AUXB_R};
            for (int k = 0; k < 4; k++)
                for (int i = 0; i < kFrame; i++) g_bus[dst[k]][i] += int32_t(rd32(dl[k] + i * 4));
            break;
        }
        default:
            if (g_ax_trace) fprintf(stdout, "[ax] unknown command %04X\n", cmd);
            return;
        }
    }
}

// ------------------------------------------------------------------ host FIFO
namespace {
std::mutex g_fifo_mutex;
std::vector<int16_t> g_fifo(48000 * 2);   // 1 s of stereo at 48 kHz (ring)
size_t g_rd = 0, g_wr = 0, g_count = 0;  // in frames
std::atomic<uint32_t> g_rate{32000};
std::atomic<bool> g_sink{false};
std::atomic<uint64_t> s_ai_frames{0}, s_underruns{0}, s_overruns{0};
} // namespace

void audio_set_sink_active(bool active) { g_sink = active; }

void audio_push_ai(const uint8_t* src, uint32_t frames, uint32_t rate) {
    g_rate = rate;
    s_ai_frames += frames;
    if (!g_sink) return;
    std::lock_guard<std::mutex> lk(g_fifo_mutex);
    size_t cap = g_fifo.size() / 2;
    for (uint32_t i = 0; i < frames; i++) {
        int16_t r = int16_t(src[i * 4] << 8 | src[i * 4 + 1]);
        int16_t l = int16_t(src[i * 4 + 2] << 8 | src[i * 4 + 3]);
        if (g_count == cap) { g_rd = (g_rd + 1) % cap; g_count--; s_overruns++; }
        g_fifo[g_wr * 2] = l;
        g_fifo[g_wr * 2 + 1] = r;
        g_wr = (g_wr + 1) % cap;
        g_count++;
    }
}

uint32_t audio_pull(int16_t* out, uint32_t frames) {
    std::lock_guard<std::mutex> lk(g_fifo_mutex);
    size_t cap = g_fifo.size() / 2;
    uint32_t n = uint32_t(std::min<size_t>(frames, g_count));
    for (uint32_t i = 0; i < n; i++) {
        out[i * 2] = g_fifo[g_rd * 2];
        out[i * 2 + 1] = g_fifo[g_rd * 2 + 1];
        g_rd = (g_rd + 1) % cap;
    }
    g_count -= n;
    if (n < frames) {
        s_underruns++;
        memset(out + n * 2, 0, (frames - n) * 4);
    }
    return n;
}

void audio_set_hq(bool hq) { g_hq = hq; }
void audio_set_mix(int music_pct, int sfx_pct) { g_music_pct = std::clamp(music_pct, 0, 100); g_sfx_pct = std::clamp(sfx_pct, 0, 100); }
bool audio_hq() { return g_hq; }

uint32_t audio_rate() { return g_rate; }
uint32_t audio_buffered() { std::lock_guard<std::mutex> lk(g_fifo_mutex); return uint32_t(g_count); }

AudioStats audio_stats() {
    return {s_cmdlists.load(), s_voices.load(), s_ai_frames.load(), s_underruns.load(), s_overruns.load(), s_active.load()};
}

void audio_state_reset() {
    g_hist.clear();  // guest thread (AX runs there too)
    std::lock_guard<std::mutex> lk(g_fifo_mutex);
    g_count = 0;
    g_rd = g_wr;
}

} // namespace ssne
