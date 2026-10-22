// Checks that a disc image is Serious Sam: Next Encounter and whether it is a known-good dump.
#pragma once
#include <string>
#include <vector>

#include "gc/disc.h"

namespace gc {

enum class VerifyStatus {
    NotADisc,        // unreadable / wrong format
    WrongGame,       // a GameCube disc, but not this game
    UnknownDump,     // correct game, but hash isn't in the known-dump list (modified or new revision)
    Verified,        // hash matches a known-good dump
};

struct KnownDump {
    std::string game_id;   // e.g. from boot.bin
    std::string sha1;      // lowercase hex of the full image
    std::string label;     // "USA 1.00" etc.
};

struct VerifyResult {
    VerifyStatus status = VerifyStatus::NotADisc;
    std::string message;
    DiscHeader header;
    std::string sha1;
    std::string matched_label;
    std::string format;    // container description, e.g. "RVZ, zstd level 5, 128 KiB chunks"
    bool main_dol_ok = false;
};

// Loads "GAMEID|sha1|label" lines ('#' comments allowed). Missing file = empty list.
std::vector<KnownDump> load_known_dumps(const std::string& path);

// Game identification without hashing (fast). Returns true if this looks like the right game.
bool is_target_game(const DiscHeader& h);

VerifyResult verify_disc(const std::string& path, const std::vector<KnownDump>& known,
                         const Disc::Progress& progress = {});

const char* to_string(VerifyStatus s);

} // namespace gc
