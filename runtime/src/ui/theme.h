// The Animal Crossing look shared by the settings screen (ui.cpp, settings.cpp) and the launcher (launcher/):
// ImGui colours and rounded shapes, the menu fonts and the small vector icons.
//
// Sizes are "design pixels" for a 1280x720 window times a scale (theme_scale), so a screen reads the same on a 13"
// laptop, a 4K monitor or a portrait phone. docs/UI_DESIGN.md is the design every menu follows: its colours, type,
// parts, states and sizes; a new menu or control is built from it.
#pragma once

#include <string>

#include "imgui.h"

namespace nx::ui {

struct ThemeFonts {
    ImFont* body = nullptr;   // 20 design pixels
    ImFont* title = nullptr;  // 30 design pixels
    ImFont* icons = nullptr;  // the embedded icon font (theme_set_embedded_icons), 56 design pixels; else nullptr
};

// Fonts compiled into the program (the launcher's Fredoka and Nunito), used in place of the system's for the body or the
// title; the system's cover and CJK fonts are still merged under them. Call before theme_build_fonts.
void theme_set_embedded_fonts(const void* body, int body_size, const void* title, int title_size);
// An icon font compiled into the program (the launcher's Lucide), built as ThemeFonts::icons over the private-use
// area. Call before theme_build_fonts.
void theme_set_embedded_icons(const void* data, int size);

// Design 1280x720: the shorter side decides (a portrait window is laid out on its width).
float theme_scale(float width, float height);

// Rebuilds ImGui's font atlas at `scale` for ui_language() (a CJK font merged in for ja/ko/zh). `user_font` is a
// .ttf tried before the system's ("" = none). The renderer backend must recreate its font texture afterwards.
ThemeFonts theme_build_fonts(float scale, const std::string& user_font);

// ACNHRecomp-Importer's island notice board (and the launcher's): sand, a paper board, bark ink; every size times
// `scale`.
void theme_apply_style(float scale);

// Its colours: bark ink (never black), sand and paper, leaf green for what is done, lagoon teal for water and focus,
// sun yellow for the one action that matters, bell orange and coral for warnings and errors.
constexpr ImU32 kThemeInk = IM_COL32(107, 84, 68, 255), kThemeInkSoft = IM_COL32(154, 133, 113, 255);
constexpr ImU32 kThemeSand = IM_COL32(251, 244, 220, 255), kThemeSandDeep = IM_COL32(241, 230, 194, 255);
constexpr ImU32 kThemeBoard = IM_COL32(255, 252, 240, 255);
constexpr ImU32 kThemeLeaf = IM_COL32(120, 195, 106, 255), kThemeLeafDeep = IM_COL32(78, 154, 69, 255);
constexpr ImU32 kThemeLeafPale = IM_COL32(227, 243, 220, 255);
constexpr ImU32 kThemeLagoon = IM_COL32(78, 195, 179, 255), kThemeLagoonDeep = IM_COL32(46, 156, 144, 255);
constexpr ImU32 kThemeSun = IM_COL32(246, 211, 74, 255), kThemeSunDeep = IM_COL32(217, 174, 34, 255);
constexpr ImU32 kThemeBell = IM_COL32(240, 164, 58, 255), kThemeCoral = IM_COL32(236, 122, 107, 255);

ImU32 theme_shade(ImU32 c, float k);         // k < 1 darker, k > 1 lighter (toward white), alpha kept
ImU32 theme_mix(ImU32 a, ImU32 b, float t);  // a..b
ImU32 theme_ink_on(ImU32 face);              // bark on a light face, paper on a dark one

// The two-lobed leaf, the Importer's emblem, centred at c and `size` tall, turned by `turn` radians.
void theme_leaf(ImDrawList* dl, ImVec2 c, float size, float turn);
// The board's stitched edge: a dashed rounded rectangle (dashes and gaps of 6 design pixels).
void theme_stitch(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float scale);
// A row of short dashes `w` wide at p: the Importer's divider.
void theme_dashes(ImDrawList* dl, ImVec2 p, float w, float scale);
// A face with a lip under it; `lift` (hovered) raises it off the lip, `down` (pressed) sinks it onto it.
void theme_lip(ImDrawList* dl, ImVec2 p, ImVec2 size, float lip, float rounding, ImU32 face, ImU32 lip_col, ImU32 border,
               float border_w, bool down, float lift = 0);
// The last item's hover eased in and out (about 0.1 s), with the pointing hand while hovered.
float theme_hover(bool clickable = true);
// The last item has keyboard or controller focus (not a mouse click's).
bool theme_nav_focused();
// The Importer's pill button with a lip: `face` kThemeBoard (quiet paper), kThemeSun (the one action that matters),
// kThemeCoral (quit, delete) or any colour. size.x: 0 the text's width, < 0 the rest of the line; its height is the
// frame height. Navigable and disabled like ImGui::Button.
bool theme_button(const char* label, ImVec2 size = ImVec2(0, 0), ImU32 face = kThemeBoard);

ImU32 theme_rgb(int r, int g, int b, int a = 255);

enum ThemeIcon {
    kIconLeaf, kIconScreen, kIconNote, kIconController, kIconHouse, kIconInfo,  // the settings pages
    kIconFolder, kIconKey, kIconBox, kIconPlus, kIconCheck, kIconCross, kIconPlay, kIconGear, kIconGlobe,
    kIconWarning, kIconText,
};

// An icon at centre c with radius r: Lucide's glyph with the embedded icon font (theme_set_embedded_icons), else a
// drawn line icon. Play is always the drawn, filled triangle.
void theme_icon(ImDrawList* dl, int icon, ImVec2 c, float r, ImU32 col);

}  // namespace nx::ui
