// gctool: command-line companion to the launcher.
//   gctool info    <image>          (.iso/.gcm, .ciso, .gcz, .wia, .rvz)
//   gctool verify  <image> [known_dumps.txt]
//   gctool extract <image> <out_dir>
//   gctool dol     <main.dol>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "gc/disc.h"
#include "gc/dol.h"
#include "gc/verify.h"

using namespace gc;

static int usage() {
    fprintf(stderr, "usage: gctool info|verify|extract|dol <file> [args]\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1], path = argv[2], err;

    if (cmd == "dol") {
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
        Dol dol;
        if (!Dol::parse(data, dol, err)) { fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        printf("entry %08X  bss %08X+%X\n", dol.entry, dol.bss_address, dol.bss_size);
        for (auto& s : dol.sections)
            printf("  %c%-2d  file %08X  addr %08X  size %08X\n", s.is_text ? 'T' : 'D', s.index,
                   s.file_offset, s.address, s.size);
        return 0;
    }

    if (cmd == "verify") {
        auto known = load_known_dumps(argc > 3 ? argv[3] : "known_dumps.txt");
        auto r = verify_disc(path, known, [](uint64_t d, uint64_t t) {
            fprintf(stderr, "\rhashing %3d%%", int(d * 100 / t));
            return true;
        });
        fprintf(stderr, "\n");
        printf("status : %s\nid     : %s\ntitle  : %s\nsha1   : %s\n%s\n", to_string(r.status),
               r.header.game_id.c_str(), r.header.title.c_str(), r.sha1.c_str(), r.message.c_str());
        return r.status == VerifyStatus::Verified || r.status == VerifyStatus::UnknownDump ? 0 : 1;
    }

    auto disc = Disc::open(path, err);
    if (!disc) { fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    const auto& h = disc->header();

    if (cmd == "info") {
        printf("id %s  disc %d  rev %d  title \"%s\"\n", h.game_id.c_str(), h.disc_number, h.version,
               h.title.c_str());
        printf("format %s\n", disc->describe().c_str());
        printf("dol @%08X  fst @%08X (%u bytes)  image %llu bytes\n", h.dol_offset, h.fst_offset, h.fst_size,
               (unsigned long long)disc->size());
        size_t n = 0; uint64_t bytes = 0;
        for (auto& f : disc->files()) if (!f.is_dir) { n++; bytes += f.size; }
        printf("%zu files, %llu bytes\n", n, (unsigned long long)bytes);
        for (auto& f : disc->files())
            printf("  %s %10u  %s\n", f.is_dir ? "d" : "f", f.size, f.path.c_str());
        return 0;
    }

    if (cmd == "extract") {
        if (argc < 4) return usage();
        bool ok = disc->extract(argv[3], err, [](uint64_t d, uint64_t t) {
            if (t) fprintf(stderr, "\rextracting %3d%%", int(d * 100 / t));
            return true;
        });
        fprintf(stderr, "\n");
        if (!ok) { fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        return 0;
    }
    return usage();
}
