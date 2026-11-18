// Between ui.cpp (input, main thread) and settings.cpp (pages, presenter thread).
#pragma once

#include <string>

namespace nx::ui {

void ui_begin_key_capture();   // the next key pressed goes to the pages instead of the menu
void ui_cancel_key_capture();
int ui_take_captured_key();    // its SDL scancode once pressed, else 0
// A binding's key (an SDL scancode name: "Z") as the player's keyboard layout names it ("Y" on a German keyboard).
std::string ui_key_label(const std::string& name);

}  // namespace nx::ui
