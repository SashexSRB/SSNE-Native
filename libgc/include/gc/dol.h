// DOL executable (GameCube main program) parser.
// Header: 7 text + 11 data sections. offsets[18] @0x00, addresses[18] @0x48, sizes[18] @0x90,
// bss address @0xD8, bss size @0xDC, entry point @0xE0. Header size 0x100.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gc {

struct DolSection {
    bool is_text = false;
    int index = 0;           // T0..T6 / D0..D10
    uint32_t file_offset = 0;
    uint32_t address = 0;
    uint32_t size = 0;
};

struct Dol {
    std::vector<DolSection> sections; // only non-empty ones
    uint32_t bss_address = 0;
    uint32_t bss_size = 0;
    uint32_t entry = 0;
    std::vector<uint8_t> image;       // the raw file

    static bool parse(const std::vector<uint8_t>& data, Dol& out, std::string& error);
    static uint32_t file_size_from_header(const uint8_t* header0x100);

    const DolSection* section_for(uint32_t addr) const;
    // Read big-endian u32 at virtual address (0 if unmapped).
    bool read32(uint32_t addr, uint32_t& value) const;
    // Copy all sections into a flat buffer that represents RAM starting at 0x80000000.
    void load_into(uint8_t* ram, size_t ram_size) const;
};

} // namespace gc
