// Static analysis over the DOL: find function starts, their extents, branch labels and jump tables.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "gc/dol.h"

namespace recomp {

struct Function {
    uint32_t start = 0;
    uint32_t end = 0;                    // exclusive
    std::string name;                    // from symbol map, if any
    std::set<uint32_t> labels;           // intra-function branch targets
    std::map<uint32_t, std::vector<uint32_t>> jump_tables; // bctr address -> targets
};

struct Analysis {
    std::vector<Function> functions;     // sorted by start
    std::map<uint32_t, std::string> names;

    const Function* find(uint32_t addr) const; // function containing addr
    bool is_start(uint32_t addr) const;
};

struct AnalysisOptions {
    std::string symbol_map;   // CodeWarrior .map or "ADDR NAME" lines
    bool scan_data_pointers = true;
};

Analysis analyze(const gc::Dol& dol, const AnalysisOptions& opt, std::string& log);

} // namespace recomp
