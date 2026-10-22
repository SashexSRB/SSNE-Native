// Gekko (PowerPC 750CXe + paired singles) instruction field helpers.
// Bit numbering in comments follows IBM convention (bit 0 = MSB).
#pragma once
#include <cstdint>
#include <string>

namespace ppc {

inline uint32_t OPCD(uint32_t i) { return i >> 26; }
inline uint32_t RD(uint32_t i) { return (i >> 21) & 31; }   // also RS, FRD, FRS, BO, crfD<<2
inline uint32_t RA(uint32_t i) { return (i >> 16) & 31; }   // also BI
inline uint32_t RB(uint32_t i) { return (i >> 11) & 31; }
inline uint32_t RC(uint32_t i) { return (i >> 6) & 31; }    // FRC (A-form), MB (M-form)
inline uint32_t ME(uint32_t i) { return (i >> 1) & 31; }
inline uint32_t SH(uint32_t i) { return (i >> 11) & 31; }
inline uint32_t MB(uint32_t i) { return (i >> 6) & 31; }
inline uint32_t XO10(uint32_t i) { return (i >> 1) & 0x3FF; }
inline uint32_t XO9(uint32_t i) { return (i >> 1) & 0x1FF; }  // XO-form (OE at bit 21)
inline uint32_t XO6(uint32_t i) { return (i >> 1) & 0x3F; }
inline uint32_t XO5(uint32_t i) { return (i >> 1) & 0x1F; }
inline bool Rc(uint32_t i) { return i & 1; }
inline bool OE(uint32_t i) { return (i >> 10) & 1; }
inline bool LK(uint32_t i) { return i & 1; }
inline bool AA(uint32_t i) { return (i >> 1) & 1; }
inline int32_t SIMM(uint32_t i) { return int16_t(i & 0xFFFF); }
inline uint32_t UIMM(uint32_t i) { return i & 0xFFFF; }
inline uint32_t CRFD(uint32_t i) { return (i >> 23) & 7; }
inline uint32_t CRFS(uint32_t i) { return (i >> 18) & 7; }
inline uint32_t SPR(uint32_t i) { return ((i >> 16) & 31) | (((i >> 11) & 31) << 5); }
inline uint32_t CRM(uint32_t i) { return (i >> 12) & 0xFF; }
inline int32_t PSQ_D(uint32_t i) { return int32_t(i << 20) >> 20; } // 12-bit signed
inline uint32_t PSQ_W(uint32_t i) { return (i >> 15) & 1; }
inline uint32_t PSQ_I(uint32_t i) { return (i >> 12) & 7; }
inline uint32_t PSQX_W(uint32_t i) { return (i >> 10) & 1; }
inline uint32_t PSQX_I(uint32_t i) { return (i >> 7) & 7; }

inline uint32_t branch_target(uint32_t addr, uint32_t i) {
    if (OPCD(i) == 18) {
        int32_t li = int32_t((i & 0x03FFFFFC) << 6) >> 6;
        return AA(i) ? uint32_t(li) : addr + uint32_t(li);
    }
    int32_t bd = int16_t(i & 0xFFFC);
    return AA(i) ? uint32_t(bd) : addr + uint32_t(bd);
}

// PowerPC rotate mask: ones from bit mb through me (wrapping).
inline uint32_t rot_mask(uint32_t mb, uint32_t me) {
    uint32_t begin = 0xFFFFFFFFu >> mb;
    uint32_t end = me >= 31 ? 0xFFFFFFFFu : ~(0x7FFFFFFFu >> me);
    return (mb <= me) ? (begin & end) : (begin | end); // mb > me wraps around
}

inline bool is_b(uint32_t i) { return OPCD(i) == 18; }
inline bool is_bc(uint32_t i) { return OPCD(i) == 16; }
inline bool is_bclr(uint32_t i) { return OPCD(i) == 19 && XO10(i) == 16; }
inline bool is_bcctr(uint32_t i) { return OPCD(i) == 19 && XO10(i) == 528; }
inline bool bo_always(uint32_t i) { return (RD(i) & 0x14) == 0x14; }

std::string mnemonic(uint32_t insn); // best-effort, for comments and statistics

} // namespace ppc
