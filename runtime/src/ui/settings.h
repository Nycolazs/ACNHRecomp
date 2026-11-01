// The settings screen's pages (settings.cpp), drawn by ui.cpp inside an ImGui frame.
#pragma once

struct ImFont;

namespace nx::ui {

bool settings_draw(float scale, ImFont* title);  // false once the player closed the screen
void settings_hint(float scale, float t);           // the "F1: settings" hint, t seconds into its showing (< 0: not yet)
void hdr_panel_draw(float scale);                   // F2: the HDR settings in a small window over the game

}  // namespace nx::ui
