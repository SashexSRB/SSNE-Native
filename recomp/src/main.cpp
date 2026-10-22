// ssne-recomp: translates main.dol (from your own disc) into C++ source for the PC port.
//   ssne-recomp <main.dol | game.iso> <out_dir> [--map symbols.map] [--no-comments] [--no-data-scan]
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "analysis.h"
#include "codegen.h"
#include "gc/disc.h"
#include "gc/dol.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: ssne-recomp <main.dol|game.iso> <out_dir> [--map file] [--no-comments] [--no-data-scan]\n");
        return 2;
    }
    std::string input = argv[1];
    recomp::AnalysisOptions aopt;
    recomp::CodegenOptions copt;
    copt.out_dir = argv[2];
    for (int k = 3; k < argc; k++) {
        std::string a = argv[k];
        if (a == "--map" && k + 1 < argc) aopt.symbol_map = argv[++k];
        else if (a == "--no-comments") copt.emit_comments = false;
        else if (a == "--no-data-scan") aopt.scan_data_pointers = false;
    }

    std::vector<uint8_t> data;
    std::string err;
    if (gc::Disc::sniff(input) == gc::ImageFormat::RawGcm) {
        auto disc = gc::Disc::open(input, err);
        if (!disc) { fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        data = disc->read_dol();
        printf("disc %s \"%s\"\n", disc->header().game_id.c_str(), disc->header().title.c_str());
    } else {
        std::ifstream in(input, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(in), {});
    }

    gc::Dol dol;
    if (!gc::Dol::parse(data, dol, err)) { fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    printf("entry %08X, %zu sections\n", dol.entry, dol.sections.size());

    std::string log;
    auto an = recomp::analyze(dol, aopt, log);
    fputs(log.c_str(), stdout);

    recomp::CodegenStats st;
    if (!recomp::generate(dol, an, copt, st, err)) { fprintf(stderr, "error: %s\n", err.c_str()); return 1; }

    printf("translated %zu instructions, %zu unimplemented (%.2f%%)\n", st.instructions, st.unimplemented,
           st.instructions ? 100.0 * st.unimplemented / st.instructions : 0.0);
    std::vector<std::pair<size_t, std::string>> top;
    for (auto& [k, v] : st.unimplemented_by_op) top.push_back({v, k});
    std::sort(top.rbegin(), top.rend());
    for (size_t k = 0; k < top.size() && k < 15; k++) printf("  %6zu  %s\n", top[k].first, top[k].second.c_str());
    printf("output: %s\n", copt.out_dir.c_str());
    return 0;
}
