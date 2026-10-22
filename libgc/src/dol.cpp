#include "gc/dol.h"

#include <cstring>

#include "gc/bytes.h"

namespace gc {

uint32_t Dol::file_size_from_header(const uint8_t* h) {
    uint32_t end = 0x100;
    for (int i = 0; i < 18; i++) {
        uint32_t off = be32(h + i * 4), sz = be32(h + 0x90 + i * 4);
        if (sz && off + sz > end) end = off + sz;
    }
    return end;
}

bool Dol::parse(const std::vector<uint8_t>& data, Dol& out, std::string& error) {
    if (data.size() < 0x100) { error = "DOL is smaller than its header"; return false; }
    const uint8_t* h = data.data();
    out = Dol{};
    for (int i = 0; i < 18; i++) {
        DolSection s;
        s.is_text = i < 7;
        s.index = s.is_text ? i : i - 7;
        s.file_offset = be32(h + i * 4);
        s.address = be32(h + 0x48 + i * 4);
        s.size = be32(h + 0x90 + i * 4);
        if (!s.size) continue;
        if (uint64_t(s.file_offset) + s.size > data.size()) {
            error = "DOL section " + std::to_string(i) + " runs past end of file";
            return false;
        }
        if (s.address < 0x80000000u || s.address + s.size > 0x81800000u) {
            error = "DOL section " + std::to_string(i) + " is outside GameCube RAM";
            return false;
        }
        out.sections.push_back(s);
    }
    out.bss_address = be32(h + 0xD8);
    out.bss_size = be32(h + 0xDC);
    out.entry = be32(h + 0xE0);
    out.image = data;
    if (!out.section_for(out.entry)) { error = "DOL entry point is not inside a section"; return false; }
    return true;
}

const DolSection* Dol::section_for(uint32_t addr) const {
    for (const auto& s : sections)
        if (addr >= s.address && addr < s.address + s.size) return &s;
    return nullptr;
}

bool Dol::read32(uint32_t addr, uint32_t& value) const {
    const DolSection* s = section_for(addr);
    if (!s || addr + 4 > s->address + s->size) return false;
    value = be32(image.data() + s->file_offset + (addr - s->address));
    return true;
}

void Dol::load_into(uint8_t* ram, size_t ram_size) const {
    for (const auto& s : sections) {
        uint32_t phys = s.address & 0x01FFFFFF;
        if (phys + s.size <= ram_size) memcpy(ram + phys, image.data() + s.file_offset, s.size);
    }
    uint32_t bss = bss_address & 0x01FFFFFF;
    // BSS overlaps initialized sections on real DOLs (sbss/sdata interleave); crt0 clears it itself,
    // so we don't zero it here.
    (void)bss;
}

} // namespace gc
