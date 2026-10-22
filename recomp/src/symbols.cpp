#include "symbols.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

#include "ppc_decode.h"

namespace recomp {

using namespace ppc;

bool dolphin_function_end(const gc::Dol& dol, uint32_t start, uint32_t& last) {
    // Mirrors Dolphin's PPCAnalyst::AnalyzeFunction (max_size = 0): the function ends at the first
    // blr/rfi that is not jumped over by an earlier *conditional* branch.
    uint32_t farthest = start;
    for (uint32_t a = start; a < start + 0x40000; a += 4) {
        uint32_t i;
        if (!dol.read32(a, i) || i == 0) return false;
        if (i == 0x4E800020 || i == 0x4C000064) {
            if (farthest > a) continue;
            last = a;
            return true;
        }
        if (OPCD(i) == 16 && !LK(i)) {
            uint32_t t = branch_target(a, i);
            if (t >= start && t > farthest) farthest = t;
        }
    }
    return false;
}

uint32_t dolphin_checksum(const gc::Dol& dol, uint32_t start, uint32_t last) {
    uint32_t sum = 0;
    for (uint32_t a = start; a <= last; a += 4) {
        uint32_t opcode = 0;
        dol.read32(a, opcode);
        uint32_t op = opcode & 0xFC000000, op2 = 0, op3 = 0, aux = op >> 26;
        switch (aux) {
        case 4:
            op2 = opcode & 0x3F;
            if (op2 == 0 || op2 == 8 || op2 == 16 || op2 == 21 || op2 == 22) op3 = opcode & 0x7C0;
            break;
        case 7: case 8: case 10: case 11: case 12: case 13: case 14: case 15:
            op2 = opcode & 0x03FF0000; break;
        case 19: case 31: case 63:
            op2 = opcode & 0x7FF; break;
        case 59:
            op2 = opcode & 0x3F;
            if (op2 < 16) op3 = opcode & 0x7C0;
            break;
        default:
            if (aux >= 32 && aux < 56) op2 = opcode & 0x03FF0000;
        }
        sum = ((sum << 17) & 0xFFFE0000) | ((sum >> 15) & 0x0001FFFF);
        sum ^= (op | op2 | op3);
    }
    return sum;
}

namespace {

struct DsyEntry { uint32_t size; std::string name; };

bool load_dsy(const std::string& path, std::unordered_map<uint32_t, DsyEntry>& db) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    uint32_t count = 0;
    in.read(reinterpret_cast<char*>(&count), 4);   // little-endian host format
    struct Raw { uint32_t checksum, size; char name[128]; } raw;
    for (uint32_t k = 0; k < count && in.read(reinterpret_cast<char*>(&raw), sizeof(raw)); k++) {
        raw.name[127] = 0;
        std::string n = raw.name;
        n = n.substr(0, n.find('\t'));             // strip "\tlibrary.a object.o"
        while (!n.empty() && n.back() == ' ') n.pop_back();
        for (char& c : n) if (c == ' ' || c == '"' || c == '\\') c = '_';
        db[raw.checksum] = {raw.size, n};
    }
    return true;
}

// Registers r2/r13 (small-data bases) as set by __init_registers: lis rN,hi ; ori rN,rN,lo.
void find_sda_bases(const gc::Dol& dol, uint32_t& r2, uint32_t& r13) {
    r2 = r13 = 0;
    for (auto& s : dol.sections) {
        if (!s.is_text) continue;
        for (uint32_t a = s.address; a + 8 <= s.address + s.size && a < s.address + 0x2000; a += 4) {
            uint32_t i, j;
            dol.read32(a, i); dol.read32(a + 4, j);
            if (OPCD(i) == 15 && RA(i) == 0 && OPCD(j) == 24 && RD(j) == RD(i) && RA(j) == RD(i)) {
                uint32_t v = (UIMM(i) << 16) | UIMM(j);
                if (RD(i) == 2 && !r2) r2 = v;
                if (RD(i) == 13 && !r13) r13 = v;
            }
        }
    }
}

bool read_cstring(const gc::Dol& dol, uint32_t addr, std::string& out) {
    const gc::DolSection* s = dol.section_for(addr);
    if (!s) return false;
    out.clear();
    for (uint32_t a = addr; a < s->address + s->size && out.size() < 256; a++) {
        uint8_t c = dol.image[s->file_offset + (a - s->address)];
        if (!c) return out.size() >= 3;
        if (c < 9 || c > 126) return false;
        out += char(c);
    }
    return false;
}

// ------------------------------------------------------------------ reference layouts

struct RefFunc { uint32_t addr, size; std::string name; };

std::vector<RefFunc> load_ref(const std::string& path) {
    std::vector<RefFunc> out;
    std::ifstream in(path);
    std::regex re(R"(^(\S+) = \.text:0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+))");
    std::string line;
    std::smatch m;
    while (std::getline(in, line))
        if (std::regex_search(line, m, re))
            out.push_back({uint32_t(std::stoul(m[2], nullptr, 16)), uint32_t(std::stoul(m[3], nullptr, 16)), m[1]});
    std::sort(out.begin(), out.end(), [](const RefFunc& a, const RefFunc& b) { return a.addr < b.addr; });
    return out;
}

bool is_terminator(uint32_t i) {
    return i == 0x4E800020 || i == 0x4C000064 || i == 0x4E800420 || (OPCD(i) == 18 && !LK(i));
}

// Library family from an SDK-style name ("__OSInitAudioSystem" -> "OS", "ARQInit" -> "AR").
std::string family(const std::string& name) {
    static const char* fams[] = {"CARD", "DEMO", "DSP", "DVD", "EXI", "ARQ", "PAD", "PPC", "MTX", "THP",
                                 "AX", "AI", "AR", "DB", "GD", "GX", "OS", "PS", "SI", "VI"};
    size_t p = name.find_first_not_of('_');
    if (p == std::string::npos) return {};
    std::string n = name.substr(p);
    for (const char* f : fams) {
        size_t L = strlen(f);
        if (n.size() > L && n.compare(0, L, f) == 0 && (isupper(uint8_t(n[L])) || n[L] == '_'))
            return std::string(f) == "ARQ" ? "AR" : f;
    }
    return {};
}

class RefAligner {
public:
    RefAligner(const gc::Dol& dol, const std::set<uint32_t>& starts) : dol_(dol), starts_(starts) {}

    bool plausible(uint32_t a) const {
        if (starts_.count(a)) return true;
        uint32_t p;
        return dol_.read32(a - 4, p) && (p == 0 || is_terminator(p));
    }
    bool ends_at(uint32_t end) const { uint32_t i; return dol_.read32(end - 4, i) && is_terminator(i); }
    bool exact_size(uint32_t a, uint32_t size) const {
        uint32_t last;
        return dolphin_function_end(dol_, a, last) && last + 4 - a == size;
    }

    // Proposals from one reference, given currently known names.
    std::map<uint32_t, std::string> run(const std::vector<RefFunc>& ref, std::map<uint32_t, std::string> named) {
        std::unordered_map<std::string, int> cnt;
        for (auto& f : ref) cnt[f.name]++;
        std::unordered_map<std::string, size_t> idx;
        for (size_t k = 0; k < ref.size(); k++) if (cnt[ref[k].name] == 1) idx[ref[k].name] = k;
        std::set<std::string> used;
        for (auto& [a, n] : named) used.insert(n);
        std::map<uint32_t, std::string> added;
        auto propose = [&](uint32_t a, size_t k) {
            const std::string& n = ref[k].name;
            if (cnt[n] != 1 || used.count(n) || named.count(a)) return false;
            named[a] = n; used.insert(n); added[a] = n;
            return true;
        };
        for (int round = 0; round < 50; round++) {
            size_t before = added.size();
            std::vector<std::pair<uint32_t, std::string>> items(named.begin(), named.end());
            // A: equal byte distance between consecutive anchors -> identical layout in between
            for (size_t j = 0; j + 1 < items.size(); j++) {
                auto i1 = idx.find(items[j].second), i2 = idx.find(items[j + 1].second);
                if (i1 == idx.end() || i2 == idx.end()) continue;
                uint32_t r1 = ref[i1->second].addr, r2 = ref[i2->second].addr;
                uint32_t a1 = items[j].first, a2 = items[j + 1].first;
                if (r2 <= r1 || r2 - r1 != a2 - a1 || r2 - r1 > 0x10000) continue;
                for (size_t k = i1->second + 1; k < i2->second; k++) {
                    uint32_t t = a1 + (ref[k].addr - r1);
                    if (plausible(t) && exact_size(t, ref[k].size)) propose(t, k);
                }
            }
            // B: walk outward while sizes keep matching (both boundaries verified)
            for (auto& [a, n] : items) {
                auto it = idx.find(n);
                if (it == idx.end()) continue;
                uint32_t cur = a;
                for (size_t k = it->second; k + 1 < ref.size() && ref[k + 1].addr - ref[k].addr == ref[k].size; k++) {
                    uint32_t nxt = cur + ref[k].size, e2 = nxt + ref[k + 1].size;
                    if (!ends_at(nxt) || !plausible(nxt) || !ends_at(e2) || !plausible(e2)) break;
                    auto kn = named.find(nxt);
                    if (kn == named.end() && !exact_size(nxt, ref[k + 1].size)) break;
                    if (kn != named.end() ? kn->second != ref[k + 1].name : !propose(nxt, k + 1)) break;
                    cur = nxt;
                }
                cur = a;
                for (size_t k = it->second; k > 0 && ref[k].addr - ref[k - 1].addr == ref[k - 1].size; k--) {
                    uint32_t prv = cur - ref[k - 1].size;
                    if (!ends_at(cur) || !plausible(prv)) break;
                    auto kn = named.find(prv);
                    if (kn == named.end() && !exact_size(prv, ref[k - 1].size)) break;
                    if (kn != named.end() ? kn->second != ref[k - 1].name : !propose(prv, k - 1)) break;
                    cur = prv;
                }
            }
            // C: sandwich — exactly one reference function between two known ones (size may differ ±25%)
            items.assign(named.begin(), named.end());
            for (size_t j = 0; j + 1 < items.size(); j++) {
                auto i1 = idx.find(items[j].second), i2 = idx.find(items[j + 1].second);
                if (i1 == idx.end() || i2 == idx.end() || i2->second != i1->second + 2) continue;
                uint32_t c = items[j].first + ref[i1->second].size, a2 = items[j + 1].first;
                uint32_t rs = ref[i1->second + 1].size, os = a2 - c;
                if (c < a2 && ends_at(c) && plausible(c) && ends_at(a2) && os >= 16 && os * 4 >= rs * 3 && os * 3 <= rs * 4)
                    propose(c, i1->second + 1);
            }
            if (added.size() == before) break;
        }
        return added;
    }

private:
    const gc::Dol& dol_;
    const std::set<uint32_t>& starts_;
};

} // namespace

std::map<uint32_t, NamedSymbol> identify_symbols(const gc::Dol& dol, const Analysis& an, const SymbolOptions& opt,
                                                 std::string& log) {
    std::ostringstream L;
    std::map<uint32_t, NamedSymbol> out;

    // ---- 1. signatures
    std::unordered_map<uint32_t, DsyEntry> db;
    for (auto& f : opt.dsy_files) {
        size_t before = db.size();
        if (load_dsy(f, db)) L << "signatures: " << db.size() - before << " from " << f << "\n";
        else L << "signatures: could not read " << f << "\n";
    }
    if (!db.empty()) {
        std::unordered_map<uint32_t, int> hash_count;
        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> fns; // start, size, hash
        for (auto& f : an.functions) {
            uint32_t last;
            if (!dolphin_function_end(dol, f.start, last)) continue;
            uint32_t h = dolphin_checksum(dol, f.start, last);
            fns.emplace_back(f.start, last + 4 - f.start, h);
            hash_count[h]++;
        }
        size_t accepted = 0, rejected_game = 0;
        std::unordered_map<std::string, int> name_count;
        std::vector<std::pair<uint32_t, NamedSymbol>> hits;
        for (auto& [start, size, h] : fns) {
            auto it = db.find(h);
            if (it == db.end() || it->second.size != size) continue;
            if (hash_count[h] != 1 || size < opt.min_size || it->second.name.empty()) continue;
            bool game_code = opt.sdk_region_start && start < opt.sdk_region_start;
            // In game code, only trust longer, non-C++ (library) names: other games' class methods
            // in the database collide with this game's code.
            if (game_code && (size < 64 || it->second.name.find("::") != std::string::npos)) { rejected_game++; continue; }
            hits.push_back({start, {it->second.name, "dsy", size}});
            name_count[it->second.name]++;
        }
        for (auto& [a, s] : hits) {
            if (name_count[s.name] > 1) continue; // same name matched twice -> ambiguous
            out[a] = s;
            accepted++;
        }
        L << "signatures: " << accepted << " functions named (" << rejected_game << " low-confidence game-code hits skipped)\n";
    }

    // ---- 2. reference layouts (anchored on signature names; every reference must agree)
    if (!opt.ref_files.empty() && !out.empty()) {
        std::set<uint32_t> starts;
        for (auto& f : an.functions) starts.insert(f.start);
        std::map<uint32_t, std::string> anchors;
        for (auto& [a, s] : out) anchors[a] = s.name;
        for (auto& [a, n] : an.names) anchors[a] = n;   // manual names are anchors too
        std::set<std::string> anchor_names;
        for (auto& [a, n] : anchors) anchor_names.insert(n);

        RefAligner aligner(dol, starts);
        std::map<uint32_t, std::map<std::string, int>> votes;
        for (auto& path : opt.ref_files) {
            auto ref = load_ref(path);
            if (ref.empty()) { L << "reference: could not read " << path << "\n"; continue; }
            auto added = aligner.run(ref, anchors);
            L << "reference: " << added.size() << " proposals from " << path << "\n";
            for (auto& [a, n] : added) votes[a][n]++;
        }
        // Keep proposals where all references agree, the name is used once, and it isn't already known.
        std::unordered_map<std::string, int> name_uses;
        std::map<uint32_t, std::string> cand;
        size_t conflicts = 0;
        for (auto& [a, v] : votes) {
            if (v.size() != 1) { conflicts++; continue; }
            if (anchor_names.count(v.begin()->first)) continue;
            cand[a] = v.begin()->first;
            name_uses[v.begin()->first]++;
        }
        // Library-family check against the nearest known neighbors (catches CARD code named "AX...").
        std::map<uint32_t, std::string> all = anchors;
        for (auto& [a, n] : cand) all[a] = n;
        size_t accepted = 0, fam_rejected = 0;
        for (auto& [a, n] : cand) {
            if (name_uses[n] != 1) continue;
            std::string f = family(n);
            if (!f.empty()) {
                auto it = all.find(a);
                std::vector<std::string> nb;
                if (it != all.begin()) nb.push_back(family(std::prev(it)->second));
                if (std::next(it) != all.end()) nb.push_back(family(std::next(it)->second));
                bool any = false, match = false;
                for (auto& x : nb) if (!x.empty()) { any = true; match |= x == f; }
                if (any && !match) { fam_rejected++; continue; }
            }
            out[a] = {n, "ref", 0};
            accepted++;
        }
        L << "reference: " << accepted << " functions named (" << conflicts << " conflicting, " << fam_rejected
          << " rejected by library-family check)\n";
    }

    // ---- final pass: small signature hits must fit their neighborhood's library family too
    {
        size_t dropped = 0;
        for (auto it = out.begin(); it != out.end();) {
            std::string f = family(it->second.name);
            if (it->second.source != "dsy" || f.empty() || it->second.size >= 64) { ++it; continue; }
            std::vector<std::string> nb;
            if (it != out.begin()) nb.push_back(family(std::prev(it)->second.name));
            if (std::next(it) != out.end()) nb.push_back(family(std::next(it)->second.name));
            bool any = false, match = false;
            for (auto& x : nb) if (!x.empty()) { any = true; match |= x == f; }
            if (any && !match) { it = out.erase(it); dropped++; } else ++it;
        }
        L << "family check: dropped " << dropped << " small signature hits\n";
    }

    // ---- 3. self-describing strings
    uint32_t r2, r13;
    find_sda_bases(dol, r2, r13);
    std::regex pat_c(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\(\))");
    std::regex pat_cpp(R"(^([A-Za-z_]\w*::~?[A-Za-z_]\w*)\b)");
    std::map<uint32_t, std::string> by_string;
    std::unordered_map<std::string, int> sname_count;
    for (auto& f : an.functions) {
        std::set<std::string> names;
        uint32_t hi[32]; bool hv[32] = {};
        for (uint32_t a = f.start; a < f.end; a += 4) {
            uint32_t i;
            if (!dol.read32(a, i)) break;
            if (OPCD(i) == 15 && RA(i) == 0) { hi[RD(i)] = UIMM(i) << 16; hv[RD(i)] = true; continue; }
            if (OPCD(i) != 14) continue;
            uint32_t base = 0; bool ok = false;
            if (RA(i) == 13 && r13) { base = r13; ok = true; }
            else if (RA(i) == 2 && r2) { base = r2; ok = true; }
            else if (RA(i) && hv[RA(i)]) { base = hi[RA(i)]; ok = true; }
            if (!ok) continue;
            std::string s;
            if (!read_cstring(dol, base + uint32_t(SIMM(i)), s)) continue;
            std::smatch m;
            if (std::regex_search(s, m, pat_c) || std::regex_search(s, m, pat_cpp)) names.insert(m[1]);
        }
        if (names.size() == 1) { by_string[f.start] = *names.begin(); sname_count[*names.begin()]++; }
    }
    size_t n_str = 0;
    for (auto& [a, n] : by_string) {
        if (sname_count[n] != 1 || out.count(a)) continue;
        out[a] = {n, "string", 0};
        n_str++;
    }
    L << "strings: " << n_str << " functions named (r2=" << std::hex << r2 << " r13=" << r13 << std::dec << ")\n";
    log = L.str();
    return out;
}

} // namespace recomp
