// The Animal Crossing look shared by the settings screen (ui.cpp, settings.cpp) and the launcher (launcher/):
// ImGui colours and rounded shapes, the menu fonts and the small vector icons.
//
// Sizes are "design pixels" for a 1280x720 window times a scale (theme_scale), so a screen reads the same on a 13"
// laptop, a 4K monitor or a portrait phone.
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

// Cream paper, brown ink, mint and yellow accents; every size times `scale`.
void theme_apply_style(float scale);

ImU32 theme_rgb(int r, int g, int b, int a = 255);

enum ThemeIcon {
    kIconLeaf, kIconScreen, kIconNote, kIconController, kIconHouse, kIconInfo,  // the settings pages
    kIconFolder, kIconKey, kIconBox, kIconPlus, kIconCheck, kIconCross, kIconPlay, kIconGear, kIconGlobe,
    kIconWarning, kIconText,
};

// A line icon at centre c with radius r.
void theme_icon(ImDrawList* dl, int icon, ImVec2 c, float r, ImU32 col);

}  // namespace nx::ui
