// Drawn touch controls: the per-game button layout (touch_layout.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

#include "platform.h"

namespace nx::plat {

struct TouchButton {
    float x, y, r;  // centre across the landscape screen (0..1), radius as a fraction of its height
    uint64_t button;
};
// The game's layout (count 0: the game handles touch its own way) for a screen of that shape (width / height).
const TouchButton* touch_buttons(size_t& count, float aspect = 16.0f / 9.0f);
// Where the stick's hint is drawn while no thumb is on it (x, y: 0..1 across the screen; r: of its height).
void touch_stick_rest(float aspect, bool mirrored, float& x, float& y, float& r);
// Launcher settings (config.txt NX_TOUCH_*): drawn and accepted at all, stick on the right half, brightness scale.
bool touch_enabled();
bool touch_mirrored();
float touch_opacity_scale();
// The layout editor (settings.cpp): the group a button of touch_buttons() moves with ("FACE", "DPAD", "L", ...; nullptr:
// the game's layout cannot be edited), and moving a group by (dx, dy) screen heights on a screen of that shape (input.touch_layout, not saved).
const char* touch_group(size_t index);
const char* touch_label(size_t index);  // "A", "ZR", "+"; "" for the d-pad and the gear
void touch_layout_move(const char* group, float dx, float dy, float aspect);
// The button under (x, y) (0..1 across the screen; aspect = width / height), or 0.
uint64_t touch_hit(float x, float y, float aspect);

}  // namespace nx::plat
