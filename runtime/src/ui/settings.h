// The settings screen's pages (settings.cpp), drawn by ui.cpp inside an ImGui frame.
#pragma once

struct ImFont;

namespace nx::ui {

void settings_show_online();  // the next time the screen opens: its Online page
bool settings_draw(float scale, ImFont* title);  // false once the player closed the screen
void settings_hint(float scale, float t);           // the "F1: settings" hint, t seconds into its showing (< 0: not yet)

}  // namespace nx::ui
