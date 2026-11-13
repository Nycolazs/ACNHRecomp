// The settings screen's pages (settings.cpp), drawn by ui.cpp inside an ImGui frame.
#pragma once

struct ImFont;

namespace nx::ui {

void settings_show_online();  // the next time the screen opens: its Online page
void settings_must_quit();    // any thread: from now on the screen only offers to quit (see g_must_quit)
bool settings_quit_pending();  // that is so: the screen stays open
void settings_nav_item();      // the widget just drawn joins the controller navigation, with the focus ring
bool settings_draw(float scale, ImFont* title, float opening);  // false once the player closed the screen
void settings_hint(float scale, float t);           // the "F1: settings" hint, t seconds into its showing (< 0: not yet)
void hdr_panel_draw(float scale);                   // F2: the HDR settings in a small window over the game

}  // namespace nx::ui
