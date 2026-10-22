// Placeholder function table used when the port is built without recompiled game code.
#include "ssne/recomp_hooks.h"

static void ssne_placeholder(PPCContext&) {}
const SSNEFuncEntry g_ssne_funcs[] = {{0, ssne_placeholder, ""}};
const size_t g_ssne_func_count = 0;
const uint32_t g_ssne_entry = 0;
const bool g_ssne_has_game_code = false;
