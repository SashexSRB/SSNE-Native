// EXI bus + memory card (slot A) emulation.
#pragma once
#include <cstdint>
#include <string>

namespace ssne {

// Before boot: memory card image for slot A (created blank if missing; empty path = no card inserted).
// Size is in Mbit for new cards: 4 (59 blocks), 8 (123), 16 (251), 32 (507), 64 (1019), 128 (2043).
void exi_configure_memcard(const std::string& path_a, uint32_t size_mbit = 16);
std::string exi_memcard_path();   // image in use ("" if none)
uint64_t exi_memcard_writes();    // number of write-throughs to the image

uint16_t exi_read16(uint32_t off);
void exi_write16(uint32_t off, uint16_t v);

} // namespace ssne
