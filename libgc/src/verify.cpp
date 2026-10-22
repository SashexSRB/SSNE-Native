#include "gc/verify.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "gc/dol.h"

namespace gc {

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::vector<KnownDump> load_known_dumps(const std::string& path) {
    std::vector<KnownDump> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::stringstream ss(line);
        KnownDump k;
        std::getline(ss, k.game_id, '|');
        std::getline(ss, k.sha1, '|');
        std::getline(ss, k.label);
        k.game_id = trim(k.game_id);
        k.sha1 = lower(trim(k.sha1));
        k.label = trim(k.label);
        if (k.sha1.size() == 40) out.push_back(k);
    }
    return out;
}

bool is_target_game(const DiscHeader& h) {
    // Confirmed IDs: G3BE9G = USA, G3BP9G = Europe (English/French/German); boot title "Serious Sam". Other
    // releases share the "G3B" prefix and title; they are accepted by title until confirmed and added here.
    static const char* kConfirmedIds[] = {"G3BE9G", "G3BP9G"};
    for (auto* id : kConfirmedIds)
        if (h.game_id == id) return true;
    std::string t = lower(h.title);
    return h.game_id.size() == 6 && h.game_id[0] == 'G' && t.find("serious sam") != std::string::npos;
}

VerifyResult verify_disc(const std::string& path, const std::vector<KnownDump>& known,
                         const Disc::Progress& progress) {
    VerifyResult r;
    std::string err;
    auto disc = Disc::open(path, err);
    if (!disc) { r.status = VerifyStatus::NotADisc; r.message = err; return r; }
    r.header = disc->header();
    r.format = disc->describe();

    bool id_known = std::any_of(known.begin(), known.end(),
                                [&](const KnownDump& k) { return k.game_id == r.header.game_id; });
    if (!is_target_game(r.header) && !id_known) {
        r.status = VerifyStatus::WrongGame;
        r.message = "This disc is \"" + r.header.title + "\" (" + r.header.game_id +
                    "), not Serious Sam: Next Encounter.";
        return r;
    }

    Dol dol;
    std::string dol_err;
    r.main_dol_ok = Dol::parse(disc->read_dol(), dol, dol_err);
    if (!r.main_dol_ok) {
        r.status = VerifyStatus::NotADisc;
        r.message = "main.dol is damaged: " + dol_err;
        return r;
    }

    r.sha1 = disc->sha1(progress);
    if (r.sha1.empty()) {
        r.status = VerifyStatus::NotADisc;
        r.message = disc->read_error().empty() ? "Cancelled while hashing." : "Read error while hashing: " + disc->read_error();
        return r;
    }

    for (const auto& k : known) {
        if (k.sha1 == r.sha1) {
            r.status = VerifyStatus::Verified;
            r.matched_label = k.label;
            r.message = "Verified: " + k.label;
            return r;
        }
    }
    r.status = VerifyStatus::UnknownDump;
    r.message = "Serious Sam: Next Encounter detected, but this dump's checksum is not in the known list. "
                "It may be a different revision, trimmed, or modified.";
    if (disc->format() == ImageFormat::Ciso || disc->format() == ImageFormat::Gcz)
        r.message += " (CISO/GCZ files are often scrubbed, which changes the checksum but not the game.)";
    return r;
}

const char* to_string(VerifyStatus s) {
    switch (s) {
    case VerifyStatus::NotADisc: return "Not a valid disc";
    case VerifyStatus::WrongGame: return "Wrong game";
    case VerifyStatus::UnknownDump: return "Recognized (unverified dump)";
    case VerifyStatus::Verified: return "Verified";
    }
    return "?";
}

} // namespace gc
