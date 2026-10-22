// CPU state and memory primitives used by recompiled code. Everything here is inline so the
// C++ compiler can optimize the generated functions aggressively.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#if defined(_MSC_VER)
#include <stdlib.h>
#define SSNE_BSWAP16 _byteswap_ushort
#define SSNE_BSWAP32 _byteswap_ulong
#define SSNE_BSWAP64 _byteswap_uint64
#define SSNE_NOINLINE __declspec(noinline)
#else
#define SSNE_BSWAP16 __builtin_bswap16
#define SSNE_BSWAP32 __builtin_bswap32
#define SSNE_BSWAP64 __builtin_bswap64
#define SSNE_NOINLINE __attribute__((noinline))
#endif

struct PPCContext {
    uint32_t r[32] = {};
    double ps0[32] = {};  // FPR / paired-single slot 0
    double ps1[32] = {};  // paired-single slot 1
    uint8_t cr[8] = {};   // each field: bit3=LT bit2=GT bit1=EQ bit0=SO
    uint32_t lr = 0, ctr = 0;
    uint32_t xer_so = 0, xer_ov = 0, xer_ca = 0;
    uint32_t fpscr = 0;
    uint32_t gqr[8] = {};
    uint32_t hid2 = 0xA0000000; // paired singles + locked cache enabled
    uint32_t msr = 0x00002032, srr0 = 0, srr1 = 0;
    uint32_t dec = 0;
    uint32_t sprg[4] = {};
};

// ------------------------------------------------------------------ memory
namespace ssne {
constexpr uint32_t kRamSize = 0x02000000;       // 32 MiB backing (24 MiB MEM1 is used)
constexpr uint32_t kLockedCacheSize = 0x4000;   // 16 KiB L1 locked cache @ 0xE0000000
extern uint8_t* g_ram;
extern uint8_t g_locked_cache[kLockedCacheSize];

uint32_t mmio_read(uint32_t addr, int size);
void mmio_write(uint32_t addr, uint32_t value, int size);

inline uint8_t* host_ptr(uint32_t a) {
    if ((a >> 28) == 0xE) return g_locked_cache + (a & (kLockedCacheSize - 1));
    return g_ram + (a & (kRamSize - 1));
}
inline bool is_mmio(uint32_t a) { return (a >> 24) == 0xCC; }
} // namespace ssne

inline uint8_t mem_read8(uint32_t a) {
    if (ssne::is_mmio(a)) return uint8_t(ssne::mmio_read(a, 1));
    return *ssne::host_ptr(a);
}
inline uint16_t mem_read16(uint32_t a) {
    if (ssne::is_mmio(a)) return uint16_t(ssne::mmio_read(a, 2));
    uint16_t v; memcpy(&v, ssne::host_ptr(a), 2); return SSNE_BSWAP16(v);
}
inline uint32_t mem_read32(uint32_t a) {
    if (ssne::is_mmio(a)) return ssne::mmio_read(a, 4);
    uint32_t v; memcpy(&v, ssne::host_ptr(a), 4); return SSNE_BSWAP32(v);
}
inline uint64_t mem_read64(uint32_t a) {
    if (ssne::is_mmio(a)) return uint64_t(ssne::mmio_read(a, 4)) << 32 | ssne::mmio_read(a + 4, 4);
    uint64_t v; memcpy(&v, ssne::host_ptr(a), 8); return SSNE_BSWAP64(v);
}
inline void mem_write8(uint32_t a, uint8_t v) {
    if (ssne::is_mmio(a)) { ssne::mmio_write(a, v, 1); return; }
    *ssne::host_ptr(a) = v;
}
inline void mem_write16(uint32_t a, uint16_t v) {
    if (ssne::is_mmio(a)) { ssne::mmio_write(a, v, 2); return; }
    v = SSNE_BSWAP16(v); memcpy(ssne::host_ptr(a), &v, 2);
}
inline void mem_write32(uint32_t a, uint32_t v) {
    if (ssne::is_mmio(a)) { ssne::mmio_write(a, v, 4); return; }
    v = SSNE_BSWAP32(v); memcpy(ssne::host_ptr(a), &v, 4);
}
inline void mem_write64(uint32_t a, uint64_t v) {
    if (ssne::is_mmio(a)) { ssne::mmio_write(a, uint32_t(v >> 32), 4); ssne::mmio_write(a + 4, uint32_t(v), 4); return; }
    v = SSNE_BSWAP64(v); memcpy(ssne::host_ptr(a), &v, 8);
}

// ------------------------------------------------------------------ float bit helpers
inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
inline double round_single(double d) { return double(float(d)); }

// ------------------------------------------------------------------ condition register
inline void cr_set_cmp(PPCContext& c, int field, bool lt, bool gt, bool eq) {
    c.cr[field] = uint8_t((lt << 3) | (gt << 2) | (eq << 1) | (c.xer_so & 1));
}
inline void cr0_update(PPCContext& c, uint32_t v) {
    int32_t s = int32_t(v);
    cr_set_cmp(c, 0, s < 0, s > 0, s == 0);
}
inline uint32_t cr_bit(const PPCContext& c, uint32_t bi) { return (c.cr[bi >> 2] >> (3 - (bi & 3))) & 1; }
inline void cr_set_bit(PPCContext& c, uint32_t bi, uint32_t v) {
    uint8_t m = uint8_t(1u << (3 - (bi & 3)));
    c.cr[bi >> 2] = uint8_t(v ? (c.cr[bi >> 2] | m) : (c.cr[bi >> 2] & ~m));
}
inline uint32_t cr_get(const PPCContext& c) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) v |= uint32_t(c.cr[i] & 15) << (28 - 4 * i);
    return v;
}
inline void cr_put(PPCContext& c, uint32_t v, uint32_t crm) {
    for (int i = 0; i < 8; i++)
        if (crm & (0x80 >> i)) c.cr[i] = uint8_t((v >> (28 - 4 * i)) & 15);
}
inline void fcmp(PPCContext& c, int field, double a, double b) {
    bool un = std::isnan(a) || std::isnan(b);
    c.cr[field] = uint8_t(un ? 1 : ((a < b) << 3 | (a > b) << 2 | (a == b) << 1));
}

// ------------------------------------------------------------------ integer helpers
inline uint32_t rotl32(uint32_t v, uint32_t n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
inline uint32_t cntlzw(uint32_t v) {
    if (!v) return 32;
    uint32_t n = 0; while (!(v & 0x80000000u)) { v <<= 1; n++; } return n;
}
inline int32_t fctiw_sat(double d) {
    if (std::isnan(d)) return std::numeric_limits<int32_t>::min();
    if (d >= 2147483647.0) return std::numeric_limits<int32_t>::max();
    if (d <= -2147483648.0) return std::numeric_limits<int32_t>::min();
    return int32_t(d);
}
inline void set_fpr_int(PPCContext& c, int n, int32_t v) {
    c.ps0[n] = u2d(0xFFF8000000000000ull | uint32_t(v));
}

// ------------------------------------------------------------------ paired-single quantization
double ssne_dequantize(uint32_t ea, uint32_t type, int scale, int* bytes_out);
void ssne_quantize(uint32_t ea, double v, uint32_t type, int scale, int* bytes_out);

inline void psq_load(PPCContext& c, int fd, uint32_t ea, uint32_t w, uint32_t gqr_index) {
    uint32_t g = c.gqr[gqr_index];
    uint32_t type = (g >> 16) & 7;
    int scale = int32_t(g << 2) >> 26; // bits 24-29, sign-extended
    int n = 0;
    c.ps0[fd] = ssne_dequantize(ea, type, scale, &n);
    c.ps1[fd] = w ? 1.0 : ssne_dequantize(ea + uint32_t(n), type, scale, &n);
}
inline void psq_store(PPCContext& c, int fs, uint32_t ea, uint32_t w, uint32_t gqr_index) {
    uint32_t g = c.gqr[gqr_index];
    uint32_t type = g & 7;
    int scale = int32_t(g << 18) >> 26; // bits 8-13
    int n = 0;
    ssne_quantize(ea, c.ps0[fs], type, scale, &n);
    if (!w) ssne_quantize(ea + uint32_t(n), c.ps1[fs], type, scale, &n);
}

// ------------------------------------------------------------------ runtime entry points
using PPCFunc = void (*)(PPCContext&);
void ssne_call(PPCContext& ctx, uint32_t addr);                         // indirect call / tail call
[[noreturn]] void ssne_unimplemented(PPCContext& ctx, uint32_t addr, uint32_t insn);
void ssne_syscall(PPCContext& ctx, uint32_t addr);
uint32_t ssne_mfspr(PPCContext& ctx, uint32_t spr);
void ssne_mtspr(PPCContext& ctx, uint32_t spr, uint32_t value);
uint64_t ssne_timebase();
