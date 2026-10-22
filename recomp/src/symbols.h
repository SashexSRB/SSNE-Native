// Automatic function naming for main.dol:
//  1. Dolphin signature databases (.dsy, e.g. Dolphin's Sys/totaldb.dsy): opcode-only hash of a
//     function body, same algorithm as Dolphin's HashSignatureDB.
//  2. Reference layouts: symbol tables from decompilation projects of other games built with a
//     similar SDK (decomp-toolkit symbols.txt). SDK objects link in a fixed order, so between two
//     already-identified functions the layout often matches byte-for-byte.
//  3. Self-describing strings: a function that references "Name(): ..." or "Class::Method ..."
//     strings (debug/warning messages) is named after them.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "analysis.h"
#include "gc/dol.h"

namespace recomp {

struct NamedSymbol {
    std::string name;
    std::string source;   // "manual", "dsy", "ref", "string"
    uint32_t size = 0;
};

struct SymbolOptions {
    std::vector<std::string> dsy_files;
    std::vector<std::string> ref_files;   // decomp-toolkit symbols.txt of other games
    uint32_t sdk_region_start = 0;    // below this, signature hits are treated as "game code" and filtered harder
    uint32_t min_size = 16;           // smaller functions hash-collide too often
};

// Dolphin-compatible function extent (last instruction address) — false if it runs into invalid code.
bool dolphin_function_end(const gc::Dol& dol, uint32_t start, uint32_t& last);
uint32_t dolphin_checksum(const gc::Dol& dol, uint32_t start, uint32_t last);

std::map<uint32_t, NamedSymbol> identify_symbols(const gc::Dol& dol, const Analysis& an, const SymbolOptions& opt,
                                                 std::string& log);

} // namespace recomp
