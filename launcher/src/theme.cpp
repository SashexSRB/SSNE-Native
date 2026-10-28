#include "theme.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "font_EricaOne_Regular.h"
#include "font_Outfit_Bold.h"
#include "font_Outfit_Regular.h"

namespace theme {

Fonts fonts;

namespace {
ImU32 lerp_col(ImU32 a, ImU32 b, float t) {
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImLerp(x, y, ImClamp(t, 0.0f, 1.0f)));
}
ImU32 with_alpha(ImU32 c, int a) { return (c & ~IM_COL32_A_MASK) | (ImU32(ImClamp(a, 0, 255)) << IM_COL32_A_SHIFT); }

// Cheap deterministic noise for the backdrop.
float hash01(int i) {
    uint32_t x = uint32_t(i) * 2654435761u;
    x ^= x >> 13; x *= 0x5bd1e995u; x ^= x >> 15;
    return float(x & 0xFFFFFF) / float(0x1000000);
}

// Boink LET, the lettering of the game's front end, when the player has it installed (a commercial font: it isn't
// shipped). Looked for by file name in the system's and the user's font folders, then (Linux) by family through
// fontconfig. Only TrueType / OpenType files are taken (what the font loader reads).
std::vector<unsigned char> g_boink;
bool font_file(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e == ".ttf" || e == ".otf";
}
bool load_font_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 1024) return false;
    uint32_t tag = uint32_t(d[0]) << 24 | uint32_t(d[1]) << 16 | uint32_t(d[2]) << 8 | d[3];
    if (tag != 0x00010000u && tag != 0x74727565u /* true */ && tag != 0x4F54544Fu /* OTTO */) return false;
    g_boink = std::move(d);
    return true;
}
bool find_boink() {
    std::vector<std::filesystem::path> dirs;
    auto env = [](const char* n) { const char* v = getenv(n); return std::string(v ? v : ""); };
#ifdef _WIN32
    if (!env("WINDIR").empty()) dirs.push_back(std::filesystem::path(env("WINDIR")) / "Fonts");
    if (!env("LOCALAPPDATA").empty()) dirs.push_back(std::filesystem::path(env("LOCALAPPDATA")) / "Microsoft" / "Windows" / "Fonts");
#else
    if (!env("XDG_DATA_HOME").empty()) dirs.push_back(std::filesystem::path(env("XDG_DATA_HOME")) / "fonts");
    if (!env("HOME").empty()) {
        dirs.push_back(std::filesystem::path(env("HOME")) / ".local" / "share" / "fonts");
        dirs.push_back(std::filesystem::path(env("HOME")) / ".fonts");
    }
    dirs.push_back("/usr/local/share/fonts");
    dirs.push_back("/usr/share/fonts");
#endif
    for (const auto& dir : dirs) {
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::string name = it->path().filename().string();
            for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (name.find("boink") != std::string::npos && font_file(it->path()) && load_font_file(it->path())) return true;
        }
    }
#ifndef _WIN32
    if (FILE* p = popen("fc-list ':family=Boink LET' file 2>/dev/null", "r")) {
        char line[1024];
        std::string path;
        while (path.empty() && fgets(line, sizeof(line), p)) {
            std::string s = line;
            size_t c = s.find(':');  // "path: "
            if (c != std::string::npos) path = s.substr(0, c);
        }
        pclose(p);
        if (!path.empty() && font_file(path) && load_font_file(path)) return true;
    }
#endif
    return false;
}

void rotate_verts(ImDrawList* dl, int v0, ImVec2 c, float ang) {
    float cs = std::cos(ang), sn = std::sin(ang);
    for (int v = v0; v < dl->VtxBuffer.Size; v++) {
        ImVec2 d = dl->VtxBuffer[v].pos - c;
        dl->VtxBuffer[v].pos = ImVec2(c.x + d.x * cs - d.y * sn, c.y + d.x * sn + d.y * cs);
    }
}
} // namespace

void load_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // static arrays
    cfg.OversampleH = 2;
    auto add = [&](const unsigned char* data, size_t size, float px) {
        return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), int(size), px * scale, &cfg);
    };
    fonts.body = add(font_Outfit_Regular, sizeof(font_Outfit_Regular), 18);  // first = default font
    fonts.bold = add(font_Outfit_Bold, sizeof(font_Outfit_Bold), 18);
    fonts.small = add(font_Outfit_Regular, sizeof(font_Outfit_Regular), 15);
    if (find_boink()) fonts.display = add(g_boink.data(), g_boink.size(), 80);
    else fonts.display = add(font_EricaOne_Regular, sizeof(font_EricaOne_Regular), 80);
}

void apply_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(0, 0);
    s.WindowBorderSize = 0;
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    s.FrameRounding = 5;
    s.ChildRounding = 6;
    s.GrabRounding = 5;
    s.FrameBorderSize = 1;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.48f, 0.65f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.03f, 0.04f, 0.18f, 0.97f);
    c[ImGuiCol_Border] = ImVec4(1.0f, 0.55f, 0.0f, 0.45f);
    c[ImGuiCol_FrameBg] = ImVec4(0.03f, 0.05f, 0.22f, 0.9f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.07f, 0.10f, 0.36f, 0.9f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.10f, 0.13f, 0.45f, 0.9f);
    c[ImGuiCol_Button] = ImVec4(0.82f, 0.42f, 0.00f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.97f, 0.60f, 0.08f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(1.00f, 0.75f, 0.20f, 1);
    c[ImGuiCol_CheckMark] = kGold;
    c[ImGuiCol_TextSelectedBg] = ImVec4(1.0f, 0.55f, 0.0f, 0.35f);
    c[ImGuiCol_NavHighlight] = kGold;
    c[ImGuiCol_Separator] = ImVec4(1.0f, 0.55f, 0.0f, 0.35f);
}

void draw_background(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float t) {
    float w = p1.x - p0.x, h = p1.y - p0.y;
    // Deep blue, lighter towards the middle.
    ImVec2 mid(p1.x, p0.y + h * 0.55f);
    ImU32 top = IM_COL32(2, 4, 26, 255), center = IM_COL32(16, 17, 88, 255), bottom = IM_COL32(4, 6, 36, 255);
    dl->AddRectFilledMultiColor(p0, mid, top, top, center, center);
    dl->AddRectFilledMultiColor(ImVec2(p0.x, mid.y), p1, center, center, bottom, bottom);

    // Soft glow behind the menu.
    ImVec2 gc(p0.x + w * 0.62f, p0.y + h * 0.55f);
    for (int i = 20; i > 0; i--) dl->AddCircleFilled(gc, h * 0.05f * i, IM_COL32(60, 60, 220, 9), 64);

    // Light streaks drifting sideways (the game's "data bars").
    for (int i = 0; i < 64; i++) {
        float y = p0.y + h * (0.08f + 0.9f * hash01(i * 7 + 1));
        float len = w * (0.04f + 0.30f * hash01(i * 7 + 2));
        float speed = 8 + 40 * hash01(i * 7 + 3);
        float thick = 1 + std::floor(4 * hash01(i * 7 + 4));
        float x = std::fmod(hash01(i * 7 + 5) * (w + len) + (i & 1 ? t : -t) * speed, w + len);
        if (x < 0) x += w + len;
        x = p0.x + x - len;
        int a = 18 + int(60 * hash01(i * 7 + 6));
        if (i % 6 == 0) {
            // Dashed run of small blocks.
            float bw = 6 + thick * 2, gap = 3;
            for (float bx = x; bx < x + len; bx += bw + gap)
                dl->AddRect(ImVec2(bx, y), ImVec2(bx + bw, y + thick * 2 + 3), IM_COL32(120, 150, 255, a), 0, 0, 1);
        } else {
            dl->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + len, y + thick), IM_COL32(70, 100, 255, 0),
                                        IM_COL32(110, 140, 255, a), IM_COL32(110, 140, 255, a), IM_COL32(70, 100, 255, 0));
        }
    }

    // Targeting reticle.
    ImVec2 rc(p0.x + w * 0.30f, p0.y + h * 0.66f);
    ImU32 rcol = IM_COL32(120, 140, 255, 34);
    float r = h * 0.40f;
    dl->AddCircle(rc, r, rcol, 96, 2.5f);
    dl->AddCircle(rc, r * 0.68f, rcol, 96, 1.5f);
    dl->AddCircle(rc, r * 0.22f, rcol, 48, 2.0f);
    dl->AddLine(ImVec2(rc.x - r * 1.25f, rc.y), ImVec2(rc.x + r * 1.25f, rc.y), rcol, 1.5f);
    dl->AddLine(ImVec2(rc.x, rc.y - r * 1.25f), ImVec2(rc.x, rc.y + r * 1.25f), rcol, 1.5f);
    float sweep = t * 0.4f;
    dl->PathArcTo(rc, r * 0.84f, sweep, sweep + 0.9f, 24);
    dl->PathStroke(IM_COL32(140, 170, 255, 40), 0, 6.0f);
    for (int k = 0; k < 24; k++) {
        float a = k * IM_PI / 12;
        ImVec2 d(std::cos(a), std::sin(a));
        dl->AddLine(rc + d * (r * 0.95f), rc + d * (r * (k % 6 == 0 ? 1.08f : 1.02f)), rcol, 1.5f);
    }

    // Scanlines and vignette.
    for (float y = p0.y; y < p1.y; y += 3) dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(0, 0, 10, 26));
    ImU32 clear = IM_COL32(0, 0, 12, 0), dark = IM_COL32(0, 0, 12, 170);
    dl->AddRectFilledMultiColor(p0, ImVec2(p1.x, p0.y + h * 0.12f), dark, dark, clear, clear);
    dl->AddRectFilledMultiColor(ImVec2(p0.x, p1.y - h * 0.15f), p1, clear, clear, dark, dark);
    dl->AddRectFilledMultiColor(p0, ImVec2(p0.x + w * 0.10f, p1.y), dark, clear, clear, dark);
    dl->AddRectFilledMultiColor(ImVec2(p1.x - w * 0.10f, p0.y), p1, clear, dark, dark, clear);
}

ImVec2 chunky_text(ImDrawList* dl, ImVec2 pos, float size, const char* text, Look look, float t, float small) {
    ImFont* f = fonts.display;
    const float fs = f->FontSize;
    ImU32 c_top = kYellow, c_bot = kOrange, c_out = kOutline;
    if (look == Look::Selected) { c_top = IM_COL32(255, 255, 190, 255); c_bot = IM_COL32(255, 160, 10, 255); }
    if (look == Look::Disabled) {
        c_top = IM_COL32(150, 160, 215, 255); c_bot = IM_COL32(80, 90, 160, 255); c_out = IM_COL32(14, 16, 60, 255);
    }
    const float outline = std::max(2.0f, size * 0.065f);
    float x = pos.x;
    bool word_start = true;
    int idx = 0;
    for (const char* p = text; *p; p++) {
        if (*p == ' ') { x += f->GetCharAdvance(' ') * size / fs * 0.7f; word_start = true; continue; }
        char u = char(std::toupper(uint8_t(*p)));
        float gs = (word_start || std::isupper(uint8_t(*p))) ? size : size * small;
        word_start = false;
        const ImFontGlyph* g = f->FindGlyph(ImWchar(uint8_t(u)));
        if (!g) continue;
        float k = gs / fs;
        float adv = g->AdvanceX * k;
        float y = pos.y + f->Ascent * (size - gs) / fs;  // shared baseline
        float ang = (float((idx * 37 + 11) % 7) - 3.0f) * 0.022f;
        if (look == Look::Selected) {
            y -= std::fabs(std::sin(t * 5.5f - idx * 0.55f)) * size * 0.12f;
            ang += std::sin(t * 4.0f + idx) * 0.03f;
        }
        idx++;
        if (dl) {
            ImVec2 gp(x, y);
            ImVec2 center(x + adv * 0.5f, y + (g->Y0 + g->Y1) * 0.5f * k);
            char s[2] = {u, 0};
            // Shadow
            int v0 = dl->VtxBuffer.Size;
            dl->AddText(f, gs, gp + ImVec2(size * 0.05f, size * 0.08f), kShadow, s);
            // Outline ring
            for (int d = 0; d < 16; d++) {
                float a = d * IM_PI / 8;
                dl->AddText(f, gs, gp + ImVec2(std::cos(a), std::sin(a)) * outline, c_out, s);
            }
            // Fill with a vertical gradient over the glyph
            int vf = dl->VtxBuffer.Size;
            dl->AddText(f, gs, gp, c_top, s);
            float gy0 = y + g->Y0 * k, gy1 = y + g->Y1 * k;
            for (int v = vf; v < dl->VtxBuffer.Size; v++) {
                float tt = (dl->VtxBuffer[v].pos.y - gy0) / std::max(1.0f, gy1 - gy0);
                dl->VtxBuffer[v].col = lerp_col(c_top, c_bot, tt * 1.1f - 0.05f);
            }
            rotate_verts(dl, v0, center, ang);
        }
        x += adv + size * 0.01f;
    }
    return ImVec2(x - pos.x, size * f->Ascent / fs + outline);
}

float spaced_text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* text, float spacing) {
    float x = pos.x;
    for (const char* p = text; *p; p++) {
        char s[2] = {*p, 0};
        if (dl) dl->AddText(font, size, ImVec2(x, pos.y), col, s);
        x += font->GetCharAdvance(ImWchar(uint8_t(*p))) * size / font->FontSize + spacing;
    }
    return x - pos.x - spacing;
}

void panel(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    dl->AddRectFilled(p0 + ImVec2(5, 7), p1 + ImVec2(5, 7), IM_COL32(0, 0, 10, 110), 10);
    dl->AddRectFilled(p0, p1, IM_COL32(5, 8, 46, 228), 8);
    dl->AddRect(p0, p1, IM_COL32(255, 140, 0, 170), 8, 0, 2.0f);
    dl->AddLine(p0 + ImVec2(10, 3), ImVec2(p1.x - 10, p0.y + 3), IM_COL32(255, 220, 120, 60), 1.0f);
}

void progress_bar(float fraction, ImVec2 size, const char* overlay) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (size.x <= 0) size.x = ImGui::GetContentRegionAvail().x + size.x;
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q = p + size;
    dl->AddRectFilled(p, q, IM_COL32(4, 6, 36, 240), 6);
    float fx = p.x + (size.x - 4) * ImClamp(fraction, 0.0f, 1.0f);
    if (fx > p.x + 4) {
        dl->AddRectFilledMultiColor(p + ImVec2(2, 2), ImVec2(fx + 2, q.y - 2), IM_COL32(230, 90, 0, 255),
                                    IM_COL32(255, 220, 60, 255), IM_COL32(255, 180, 20, 255), IM_COL32(200, 70, 0, 255));
    }
    dl->AddRect(p, q, IM_COL32(255, 140, 0, 180), 6, 0, 1.5f);
    if (overlay) {
        ImVec2 ts = ImGui::CalcTextSize(overlay);
        ImVec2 tp(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f);
        dl->AddText(tp + ImVec2(1, 1), IM_COL32(40, 10, 0, 220), overlay);
        dl->AddText(tp, IM_COL32(255, 255, 255, 255), overlay);
    }
}

float key_prompt(ImDrawList* dl, ImVec2 pos, float h, const char* key, const char* label, ImU32 cap_col) {
    ImFont* fb = fonts.bold;
    float ks = h * 0.52f;
    ImVec2 kt = fb->CalcTextSizeA(ks, FLT_MAX, 0, key);
    float cw = std::max(h, kt.x + h * 0.6f);
    ImVec2 c0 = pos, c1 = pos + ImVec2(cw, h);
    dl->AddRectFilled(c0 + ImVec2(0, 2), c1 + ImVec2(0, 2), IM_COL32(0, 0, 0, 120), h * 0.5f);
    dl->AddRectFilled(c0, c1, lerp_col(cap_col, IM_COL32(0, 0, 0, 255), 0.25f), h * 0.5f);
    dl->AddRectFilled(c0 + ImVec2(1, 1), ImVec2(c1.x - 1, c0.y + h * 0.5f), with_alpha(lerp_col(cap_col, IM_COL32(255, 255, 255, 255), 0.25f), 200),
                      h * 0.5f, ImDrawFlags_RoundCornersTop);
    dl->AddRect(c0, c1, IM_COL32(255, 255, 255, 110), h * 0.5f, 0, 1.2f);
    dl->AddText(fb, ks, ImVec2(c0.x + (cw - kt.x) * 0.5f, c0.y + (h - kt.y) * 0.5f), IM_COL32(255, 255, 255, 255), key);
    float ls = h * 0.62f;
    ImVec2 lt = fonts.body->CalcTextSizeA(ls, FLT_MAX, 0, label);
    dl->AddText(fonts.body, ls, ImVec2(c1.x + h * 0.25f, pos.y + (h - lt.y) * 0.5f), IM_COL32(240, 240, 250, 255), label);
    return cw + h * 0.25f + lt.x;
}

} // namespace theme
