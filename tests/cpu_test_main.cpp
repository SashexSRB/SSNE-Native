// Runs the recompiled synthetic test program and checks its results in guest memory.
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"

[[noreturn]] static void throw_fatal(const char* m) { throw std::runtime_error(m); }

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: cpu_test <extracted_dir>\n"); return 2; }
    ssne::set_fatal_handler(throw_fatal);
    std::string err;
    uint32_t entry = ssne::load_game(argv[1], err);
    if (!entry) { fprintf(stderr, "load: %s\n", err.c_str()); return 1; }
    static PPCContext ctx;
    ssne::reset_cpu(ctx);
    try { ssne_call(ctx, entry); } catch (const std::exception& e) { fprintf(stderr, "guest: %s\n", e.what()); return 1; }

    const uint32_t R = 0x80100000;
    struct Check { const char* what; uint32_t got, want; } checks[] = {
        {"fib(10) via bl", mem_read32(R + 0), 55},
        {"bdnz loop sum", mem_read32(R + 4), 5050},
        {"rlwinm extract", mem_read32(R + 8), 0x12},
        {"rlwinm wrap mask", mem_read32(R + 12), 0x10000008},
        {"cmpwi/ble", mem_read32(R + 16), 1},
        {"fmuls 1.5*4", mem_read32(R + 20), f2u(6.0f)},
        {"bctrl indirect", mem_read32(R + 24), 42},
        {"switch jump table", mem_read32(R + 28), 300},
        {"paired singles", mem_read32(R + 32), f2u(8.0f)},
    };
    int fails = 0;
    for (auto& c : checks) {
        bool ok = c.got == c.want;
        fails += !ok;
        printf("%-20s %s (got %08X want %08X)\n", c.what, ok ? "ok  " : "FAIL", c.got, c.want);
    }
    return fails ? 1 : 0;
}
