// Minimal PNG writer (8-bit RGB) for screenshots.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace port {
// rgba: w*h pixels, R in the low byte (alpha ignored), row 0 = top.
bool write_png(const std::string& path, const uint32_t* rgba, int w, int h);
}
