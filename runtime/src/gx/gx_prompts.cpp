// Button prompts: the game draws its controller buttons (A, B, X, Y, Z, L, R) as glyphs of its two text fonts
// (512x256 texture atlases). When the player uses another controller (or the keyboard and mouse), the port asks for
// those glyphs to be replaced with the matching Xbox / PlayStation buttons, keycaps or mouse buttons: the atlas is recognized when it is decoded and the glyph
// rectangles are repainted with the icons from prompt_icons.inc (tools/make_prompt_icons.py).
//
// GameCube A can stand for two pad buttons (with the Xbox layout, the jump button also selects in menus and X is
// "use" in gameplay), so there is one map for menus/cutscenes and one for player control; the atlas is decoded
// again (texture cache key variant) whenever the map in effect changes.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include "gx_internal.h"
#include "ssne/runtime.h"

namespace ssne::gx {
namespace {

struct PromptIcon { int w, h; const uint8_t* rgba; };
#include "prompt_icons.inc"
constexpr int kIconCount = int(sizeof(kPromptIcons) / sizeof(kPromptIcons[0]));

// Glyph rectangles in the two font atlases, in GameCube button order A, B, X, Y, Z, L, R.
struct Rect { int x, y, w, h; };
struct Layout { int tw, th; Rect slot[7]; };  // texture size; slots with w = 0 are absent
const Layout kLayouts[3] = {
    {512, 256, {{480, 2, 30, 30}, {2, 2, 30, 30}, {87, 2, 22, 32}, {203, 7, 32, 22}, {35, 4, 46, 17}, {160, 4, 40, 25},
      {117, 4, 40, 25}}},  // menu / HUD font
    {512, 256, {{242, 121, 28, 29}, {88, 122, 29, 29}, {32, 136, 22, 32}, {134, 186, 30, 21}, {1, 175, 44, 15},
      {210, 188, 38, 23}, {67, 190, 38, 23}}},  // large font
    {256, 128, {{48, 18, 28, 28}, {80, 17, 28, 28}, {6, 46, 20, 31}, {69, 51, 30, 20}, {1, 20, 44, 17}, {}, {}}},
    // front-end sprite sheet (menu button bar)
};

std::mutex g_mutex;
uint8_t g_map[2][7] = {};        // [0] menus / cutscenes, [1] player control; 0 = GameCube glyph
std::atomic<uint32_t> g_generation{0};
std::atomic<bool> g_active{false};
std::atomic<bool> g_maps_differ{false};
std::unordered_set<uint32_t> g_atlas_samples;  // fingerprints of atlases seen (game thread only)

bool is_green(uint32_t c) { int r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = c >> 24; return a > 200 && g > r + 60 && g > b + 30; }
bool is_red(uint32_t c) { int r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = c >> 24; return a > 200 && r > g + 60 && r > b + 60; }

int count_in(const Texture& t, const Rect& r, bool (*pred)(uint32_t)) {
    int n = 0;
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) n += pred(t.rgba[size_t(y) * t.w + x]);
    return n;
}

// The atlas is recognized by its green A and red B glyphs (at least a third of each rectangle in that color).
int find_layout(const Texture& t) {
    if (t.rgba.size() < size_t(t.w) * t.h) return -1;
    for (int i = 0; i < 3; i++) {
        if (t.w != kLayouts[i].tw || t.h != kLayouts[i].th) continue;
        const Rect& a = kLayouts[i].slot[0];
        const Rect& b = kLayouts[i].slot[1];
        if (count_in(t, a, is_green) * 3 > a.w * a.h && count_in(t, b, is_red) * 3 > b.w * b.h) return i;
    }
    return -1;
}

// Icons are painted 24 pixels high whichever GameCube button they replace, so they look alike: fitted to each
// button's rectangle instead, a keycap in A's 30 x 30 was half again as big as one in X's 22 x 32 or Y's 32 x 22.
// Where the rectangle is too narrow the icon is narrowed by up to 15% (X: 20-22 wide); where it is too low (Y: 20-22,
// Z: 15-17) the icon is as high as the rectangle allows.
constexpr int kIconHeight = 24;

// Paints icon `id` into rectangle r of a 512-wide RGBA atlas (box-filtered resample).
void paint(std::vector<uint32_t>& px, int stride, const Rect& r, int id) {
    // clear the GameCube glyph (and the faint antialiasing pixels around it)
    for (int y = r.y - 1; y <= r.y + r.h; y++)
        for (int x = r.x - 1; x <= r.x + r.w; x++) {
            if (x < 0 || y < 0 || x >= stride || size_t(y) * stride + x >= px.size()) continue;
            bool inside = x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
            uint32_t& c = px[size_t(y) * stride + x];
            if (inside || (c >> 24) < 64) c = 0;
        }
    if (id <= 0 || id >= kIconCount) return;
    const PromptIcon& ic = kPromptIcons[id];
    double s = std::min(double(r.h), double(kIconHeight)) / ic.h, sw = s;  // vertical / horizontal scale
    if (ic.w * sw > r.w) {
        sw = double(r.w) / ic.w;
        if (sw < s * 0.85) s = sw / 0.85;
    }
    int dw = std::max(1, int(std::lround(ic.w * sw))), dh = std::max(1, int(std::lround(ic.h * s)));
    int ox = r.x + (r.w - dw) / 2, oy = r.y + (r.h - dh) / 2;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            // source area covered by this destination pixel
            double sx0 = x / sw, sx1 = (x + 1) / sw, sy0 = y / s, sy1 = (y + 1) / s;
            double acc[4] = {}, wsum = 0;
            for (int sy = int(sy0); sy < std::min(ic.h, int(std::ceil(sy1))); sy++)
                for (int sx = int(sx0); sx < std::min(ic.w, int(std::ceil(sx1))); sx++) {
                    double wx = std::min(sx1, sx + 1.0) - std::max(sx0, double(sx));
                    double wy = std::min(sy1, sy + 1.0) - std::max(sy0, double(sy));
                    double w = wx * wy;
                    if (w <= 0) continue;
                    const uint8_t* p = ic.rgba + (size_t(sy) * ic.w + sx) * 4;
                    double a = p[3] / 255.0;
                    acc[0] += p[0] * a * w; acc[1] += p[1] * a * w; acc[2] += p[2] * a * w; acc[3] += a * w;
                    wsum += w;
                }
            if (wsum <= 0 || acc[3] <= 0) continue;
            double a = acc[3] / wsum;
            uint32_t cr = uint32_t(std::clamp(acc[0] / acc[3], 0.0, 255.0)), cg = uint32_t(std::clamp(acc[1] / acc[3], 0.0, 255.0));
            uint32_t cb = uint32_t(std::clamp(acc[2] / acc[3], 0.0, 255.0)), ca = uint32_t(std::lround(a * 255));
            int X = ox + x, Y = oy + y;
            px[size_t(Y) * stride + X] = cr | cg << 8 | cb << 16 | ca << 24;
        }
}

int current_map() { return g_maps_differ.load() && ssne::player_control_active() ? 1 : 0; }

} // namespace

uint32_t prompt_variant(uint32_t sample) {
    if (!g_active.load() || !g_atlas_samples.count(sample)) return 0;
    return (g_generation.load() << 1 | uint32_t(current_map())) + 1;
}

uint32_t prompt_apply(Texture& t, uint32_t sample) {
    int layout = find_layout(t);
    if (layout < 0) return 0;
    t.font = layout <= 1;
    g_atlas_samples.insert(sample);
    if (!g_active.load()) return 0;
    int m = current_map();
    uint8_t map[7];
    uint32_t gen;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        memcpy(map, g_map[m], 7);
        gen = g_generation.load();
    }
    for (int s = 0; s < 7; s++)
        if (map[s] && kLayouts[layout].slot[s].w) paint(t.rgba, t.w, kLayouts[layout].slot[s], map[s]);
    return (gen << 1 | uint32_t(m)) + 1;
}

} // namespace ssne::gx

namespace ssne {
void set_button_prompts(const uint8_t menu[7], const uint8_t control[7]) {
    using namespace gx;
    std::lock_guard<std::mutex> lk(g_mutex);
    bool active = false, differ = false;
    for (int s = 0; s < 7; s++) { active |= menu[s] || control[s]; differ |= menu[s] != control[s]; }
    if (!memcmp(g_map[0], menu, 7) && !memcmp(g_map[1], control, 7)) return;
    memcpy(g_map[0], menu, 7);
    memcpy(g_map[1], control, 7);
    g_maps_differ = differ;
    g_active = active;
    g_generation++;
}
} // namespace ssne

namespace ssne::gx {
bool prompt_candidate_size(int w, int h) { return (w == 512 && h == 256) || (w == 256 && h == 128); }
}

namespace ssne {
uint8_t prompt_key_glyph(const char* name) {
    if (!name) return 0;
    for (const auto& k : gx::kKeyGlyphs)
        if (!strcmp(k.name, name)) return k.glyph;
    return 0;
}
} // namespace ssne
