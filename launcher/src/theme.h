// Launcher look: styled after the game's front end (deep blue backdrop with light streaks and a targeting
// reticle, chunky yellow-to-orange lettering with a dark red outline, grey button-prompt bar at the bottom).
// Everything is drawn procedurally; no game assets are used.
#pragma once
#include <imgui.h>

namespace theme {

struct Fonts {
    ImFont* display = nullptr;  // Erica One, baked large; drawn scaled
    ImFont* body = nullptr;     // Outfit Regular
    ImFont* bold = nullptr;     // Outfit Bold
    ImFont* small = nullptr;    // Outfit Regular, smaller
};
extern Fonts fonts;

// Palette
constexpr ImU32 kYellow = IM_COL32(255, 238, 70, 255);
constexpr ImU32 kOrange = IM_COL32(255, 128, 0, 255);
constexpr ImU32 kOutline = IM_COL32(120, 8, 0, 255);
constexpr ImU32 kShadow = IM_COL32(0, 0, 20, 170);
const ImVec4 kText(0.92f, 0.93f, 1.00f, 1), kDim(0.62f, 0.66f, 0.86f, 1), kGood(0.55f, 0.95f, 0.45f, 1),
    kWarn(1.00f, 0.80f, 0.25f, 1), kBad(1.00f, 0.42f, 0.32f, 1), kGold(1.00f, 0.80f, 0.20f, 1);

void load_fonts(float scale);  // call once after ImGui::CreateContext
void apply_style();

// Full-window animated backdrop (drawn into the background draw list).
void draw_background(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float t);

enum class Look { Normal, Selected, Disabled };
// Chunky title lettering: every word's first letter full size, the rest as smaller capitals; each glyph gets its
// own vertical gradient, a thick outline, a drop shadow and a slight tilt. Selected text bounces.
// Returns the size of the text; draws nothing when dl is null.
ImVec2 chunky_text(ImDrawList* dl, ImVec2 pos, float size, const char* text, Look look, float t, float small = 0.78f);

// Letter-spaced plain text (the "NEXT ENCOUNTER" style subtitle). Returns the width.
float spaced_text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* text, float spacing);

// Translucent navy panel with an orange edge.
void panel(ImDrawList* dl, ImVec2 p0, ImVec2 p1);

// Progress bar in the menu colours.
void progress_bar(float fraction, ImVec2 size, const char* overlay);

// Rounded key-cap icon followed by a label, as in the game's prompt bar. Returns the total width.
float key_prompt(ImDrawList* dl, ImVec2 pos, float h, const char* key, const char* label, ImU32 cap_col);

} // namespace theme
