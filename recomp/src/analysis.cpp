#include "analysis.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "ppc_decode.h"

namespace recomp {

using namespace ppc;

const Function* Analysis::find(uint32_t addr) const {
    auto it = std::upper_bound(functions.begin(), functions.end(), addr,
                               [](uint32_t a, const Function& f) { return a < f.start; });
    if (it == functions.begin()) return nullptr;
    --it;
    return addr < it->end ? &*it : nullptr;
}

bool Analysis::is_start(uint32_t addr) const {
    auto it = std::lower_bound(functions.begin(), functions.end(), addr,
                               [](const Function& f, uint32_t a) { return f.start < a; });
    return it != functions.end() && it->start == addr;
}

namespace {

bool in_text(const gc::Dol& dol, uint32_t addr) {
    auto* s = dol.section_for(addr);
    return s && s->is_text && (addr & 3) == 0;
}

// A plausible function start for a *pointer* found in data / materialized in code: it must be
// preceded by an unconditional control transfer (or be the first word of a section).
bool plausible_start(const gc::Dol& dol, uint32_t addr) {
    if (!in_text(dol, addr)) return false;
    auto* s = dol.section_for(addr);
    if (addr == s->address) return true;
    uint32_t prev = 0;
    if (!dol.read32(addr - 4, prev)) return false;
    if (is_bclr(prev) && bo_always(prev)) return true;
    if (is_bcctr(prev) && bo_always(prev) && !LK(prev)) return true;
    if (is_b(prev) && !LK(prev)) return true;
    if (prev == 0) return true; // alignment padding
    return false;
}

void load_symbols(const std::string& path, std::map<uint32_t, std::string>& names,
                  std::set<uint32_t>& starts, const gc::Dol& dol) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::vector<std::string> tok;
        std::string t;
        while (ss >> t) tok.push_back(t);
        // Formats handled:
        //   "80003100 __start"                                  (simple)
        //   "00000000 000024 80003100  4 __start \tGlobal"       (CodeWarrior map)
        //   "00000000 000024 80003100 00000000  4 __start ..."   (CodeWarrior map, newer)
        if (tok.size() >= 2 && tok.size() <= 3 && tok[0].size() == 8 && tok[0][0] != '#') {
            // Simple format: "ADDR NAME [SOURCE]"
            uint32_t a = uint32_t(strtoul(tok[0].c_str(), nullptr, 16));
            if (in_text(dol, a)) { names[a] = tok[1]; starts.insert(a); }
            continue;
        }
        for (size_t i = 0; i + 1 < tok.size(); i++) {
            if (tok[i].size() != 8) continue;
            uint32_t a = uint32_t(strtoul(tok[i].c_str(), nullptr, 16));
            if (!in_text(dol, a)) continue;
            // The name is the first token after the address that isn't hex-ish/alignment.
            for (size_t j = i + 1; j < tok.size(); j++) {
                bool numeric = tok[j].find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
                if (!numeric) { names[a] = tok[j]; starts.insert(a); break; }
            }
            break;
        }
    }
}

struct JumpTable { uint32_t table = 0; std::vector<uint32_t> targets; };

// CodeWarrior switch pattern:
//   cmplwi rI,N ; bgt default ; lis rT,hi ; addi rT,rT,lo ; slwi rI,rI,2 ; lwzx rX,rT,rI ; mtctr rX ; bctr
std::map<uint32_t, JumpTable> find_jump_tables(const gc::Dol& dol) {
    std::map<uint32_t, JumpTable> out;
    for (auto& s : dol.sections) {
        if (!s.is_text) continue;
        for (uint32_t a = s.address + 8; a < s.address + s.size; a += 4) {
            uint32_t i, mt, lw;
            dol.read32(a, i);
            if (!is_bcctr(i) || LK(i) || !bo_always(i)) continue;
            dol.read32(a - 4, mt);
            if (!(OPCD(mt) == 31 && XO10(mt) == 467 && SPR(mt) == 9)) continue;
            uint32_t xreg = RD(mt), table = 0, count = 0;
            bool found = false;
            for (uint32_t b = a - 8; b > s.address && b + 40 > a; b -= 4) {
                dol.read32(b, lw);
                if (!(OPCD(lw) == 31 && XO10(lw) == 23 && RD(lw) == xreg)) continue;
                uint32_t base = RA(lw), lo = 0;
                bool lv = false;
                for (uint32_t c = b - 4; c > s.address && c + 80 > b; c -= 4) {
                    uint32_t k; dol.read32(c, k);
                    if (!lv && OPCD(k) == 14 && RD(k) == base) { lo = uint32_t(SIMM(k)); lv = true; base = RA(k); }
                    else if (lv && OPCD(k) == 15 && RD(k) == base && RA(k) == 0) {
                        table = (UIMM(k) << 16) + lo; found = true;
                    }
                    else if (OPCD(k) == 10 && !count) count = UIMM(k) + 1; // cmplwi bound
                    if (found && count) break;
                }
                break;
            }
            if (!found) continue;
            JumpTable jt; jt.table = table;
            uint32_t limit = count ? count : 1024;
            for (uint32_t e = 0; e < limit; e++) {
                uint32_t t;
                if (!dol.read32(table + e * 4, t) || !in_text(dol, t)) break;
                if (t < a - 0x20000 || t > a + 0x20000) break; // must be near the dispatch
                jt.targets.push_back(t);
            }
            if (!jt.targets.empty()) out[a] = std::move(jt);
        }
    }
    return out;
}

} // namespace

Analysis analyze(const gc::Dol& dol, const AnalysisOptions& opt, std::string& log) {
    Analysis an;
    std::set<uint32_t> starts{dol.entry};
    std::ostringstream L;

    if (!opt.symbol_map.empty()) {
        load_symbols(opt.symbol_map, an.names, starts, dol);
        L << "symbols: " << an.names.size() << " from " << opt.symbol_map << "\n";
    }
    for (auto& s : dol.sections)
        if (s.is_text) starts.insert(s.address);

    // Pass 1: bl targets and materialized code addresses (lis rX,hi ; addi/ori rX,rX,lo).
    size_t n_bl = 0, n_const = 0, n_data = 0;
    for (auto& s : dol.sections) {
        if (!s.is_text) continue;
        uint32_t hi_reg[32]; bool hi_valid[32] = {};
        for (uint32_t a = s.address; a < s.address + s.size; a += 4) {
            uint32_t i; dol.read32(a, i);
            if ((is_b(i) || is_bc(i)) && LK(i)) {
                uint32_t t = branch_target(a, i);
                if (in_text(dol, t) && starts.insert(t).second) n_bl++;
            }
            // Track "lis rX,hi" per register to recover addresses built as hi + lo.
            uint32_t op = OPCD(i);
            if (op == 15 && RA(i) == 0) { hi_reg[RD(i)] = UIMM(i) << 16; hi_valid[RD(i)] = true; continue; }
            if ((op == 14 || op == 24) && hi_valid[RA(i)]) {
                uint32_t v = op == 14 ? hi_reg[RA(i)] + uint32_t(SIMM(i)) : hi_reg[RA(i)] | UIMM(i);
                if (v < 0x01800000u) v |= 0x80000000u; // physical address (real-mode code, e.g. rfi targets)
                if (plausible_start(dol, v) && starts.insert(v).second) n_const++;
            }
            if ((is_b(i) || is_bc(i) || is_bcctr(i) || is_bclr(i)) && LK(i)) {
                // A call clobbers volatile registers only; r14-r31 keep a base loaded long before.
                hi_valid[0] = false;
                for (int r = 3; r <= 12; r++) hi_valid[r] = false;
            } else if (is_b(i) || is_bclr(i) || is_bcctr(i)) {
                std::fill(hi_valid, hi_valid + 32, false);          // end of straight-line flow
            } else if (op == 14 || op == 15 || (op >= 32 && op <= 35) || (op >= 40 && op <= 43) || op == 46 ||
                       op == 7 || op == 8 || op == 12 || op == 13) {
                hi_valid[RD(i)] = false;                            // rD written (addi result, loads, ...)
                if (op == 33 || op == 35 || op == 41 || op == 43) hi_valid[RA(i)] = false;  // update forms
            } else if (op == 24 || op == 25 || op == 26 || op == 27 || op == 28 || op == 29 || op == 20 ||
                       op == 21 || op == 23) {
                hi_valid[RA(i)] = false;                            // logical/rotate write rA
            } else if (op == 31) {
                hi_valid[RD(i)] = false;                            // conservative: X-form may write rD or rA
                hi_valid[RA(i)] = false;
            }
        }
    }
    // Jump tables are found first so their entries aren't mistaken for function pointers.
    auto tables = find_jump_tables(dol);
    std::set<uint32_t> table_words;
    for (auto& [at, jt] : tables)
        for (size_t k = 0; k < jt.targets.size(); k++) table_words.insert(jt.table + uint32_t(k) * 4);

    // Pass 2: pointers stored in data sections (vtables, callback tables).
    if (opt.scan_data_pointers) {
        for (auto& s : dol.sections) {
            if (s.is_text) continue;
            for (uint32_t a = s.address; a + 4 <= s.address + s.size; a += 4) {
                if (table_words.count(a)) continue;
                uint32_t v; dol.read32(a, v);
                if (plausible_start(dol, v) && starts.insert(v).second) n_data++;
            }
        }
    }

    // Pass 3: iterate — non-linking branches that leave their function become tail calls, and
    // their targets become function starts, which may split other functions.
    auto build = [&]() {
        an.functions.clear();
        std::vector<uint32_t> v(starts.begin(), starts.end());
        for (size_t k = 0; k < v.size(); k++) {
            auto* s = dol.section_for(v[k]);
            uint32_t sec_end = s->address + s->size;
            uint32_t end = (k + 1 < v.size() && v[k + 1] < sec_end) ? v[k + 1] : sec_end;
            Function f; f.start = v[k]; f.end = end;
            auto nm = an.names.find(v[k]);
            if (nm != an.names.end()) f.name = nm->second;
            an.functions.push_back(std::move(f));
        }
    };
    for (int iter = 0; iter < 16; iter++) {
        build();
        size_t added = 0;
        for (auto& f : an.functions) {
            for (uint32_t a = f.start; a < f.end; a += 4) {
                uint32_t i; dol.read32(a, i);
                if ((is_b(i) || is_bc(i)) && !LK(i)) {
                    uint32_t t = branch_target(a, i);
                    if ((t < f.start || t >= f.end) && in_text(dol, t) && starts.insert(t).second) added++;
                }
            }
        }
        if (!added) break;
    }

    // Pass 4: labels and jump tables.
    size_t n_tables = 0;
    for (auto& f : an.functions) {
        for (uint32_t a = f.start; a < f.end; a += 4) {
            uint32_t i; dol.read32(a, i);
            if ((is_b(i) || is_bc(i)) && !LK(i)) {
                uint32_t t = branch_target(a, i);
                if (t >= f.start && t < f.end) f.labels.insert(t);
            }
            if (is_bcctr(i)) {
                auto jt = tables.find(a);
                if (jt == tables.end()) continue;
                std::vector<uint32_t> inside;
                for (uint32_t t : jt->second.targets)
                    if (t >= f.start && t < f.end) { inside.push_back(t); f.labels.insert(t); }
                if (!inside.empty()) { f.jump_tables[a] = inside; n_tables++; }
            }
        }
    }

    L << "function starts: entry + " << n_bl << " call targets + " << n_const << " code constants + "
      << n_data << " data pointers\n";
    L << "functions: " << an.functions.size() << ", jump tables: " << n_tables << "\n";
    log = L.str();
    return an;
}

} // namespace recomp
