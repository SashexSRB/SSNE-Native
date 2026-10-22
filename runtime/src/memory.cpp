#include <cmath>
#include <cstdio>
#include <vector>

#include "ssne/ppc_context.h"

namespace ssne {

static std::vector<uint8_t> s_ram(kRamSize);
uint8_t* g_ram = s_ram.data();
uint8_t g_locked_cache[kLockedCacheSize];

} // namespace ssne

// 2^e for the GQR scale range (a 6-bit signed field, so e = -32..32): the same exact values std::ldexp(1.0, e) gives,
// without a libm call on every paired-single load/store.
static const struct Pow2 {
    double v[65];
    Pow2() { for (int e = -32; e <= 32; e++) v[e + 32] = std::ldexp(1.0, e); }
} s_pow2;
static inline double pow2(int e) { return e >= -32 && e <= 32 ? s_pow2.v[e + 32] : std::ldexp(1.0, e); }

// Paired-single (de)quantization, GQR types: 0=f32 4=u8 5=u16 6=s8 7=s16.
double ssne_dequantize(uint32_t ea, uint32_t type, int scale, int* bytes) {
    double m = pow2(-scale);
    switch (type) {
    case 4: *bytes = 1; return mem_read8(ea) * m;
    case 5: *bytes = 2; return mem_read16(ea) * m;
    case 6: *bytes = 1; return int8_t(mem_read8(ea)) * m;
    case 7: *bytes = 2; return int16_t(mem_read16(ea)) * m;
    default: *bytes = 4; return double(u2f(mem_read32(ea)));
    }
}

void ssne_quantize(uint32_t ea, double v, uint32_t type, int scale, int* bytes) {
    double s = v * pow2(scale);
    auto clamp = [](double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); };
    switch (type) {
    case 4: *bytes = 1; mem_write8(ea, uint8_t(clamp(s, 0, 255))); return;
    case 5: *bytes = 2; mem_write16(ea, uint16_t(clamp(s, 0, 65535))); return;
    case 6: *bytes = 1; mem_write8(ea, uint8_t(int8_t(clamp(s, -128, 127)))); return;
    case 7: *bytes = 2; mem_write16(ea, uint16_t(int16_t(clamp(s, -32768, 32767)))); return;
    default: *bytes = 4; mem_write32(ea, f2u(float(v))); return;
    }
}
