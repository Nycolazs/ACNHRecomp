// Drawn touch controls (docs/nvn/ANDROID-PORT.md A4): a fixed layout of round buttons per game, hit-tested by
// platform_android_input.cpp and drawn by the NVN present pass (nvn_present.cpp) over the picture.
//
// Super Mario Bros. Wonder, landscape: A (jump, the big one) at the bottom right with B (jump), Y (dash, action) and
// X (action) around it; ZR (roll), R, L, + and - along the top. The left half is the stick (platform_android_input.cpp):
// it appears where the thumb lands, and pushed nearly all the way it also holds Y, so running needs no second thumb.
// Games without a layout here keep their own touch handling (Hades: zones around the thumb).
//
// Elsewhere touch_overlay() draws nothing, except with NX_OVERLAY_DEMO=1 (the buttons at rest), to check the look
// on the Mac with NX_PRESENT_TEST.
#include "touch_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <nx/game.h>

#include "config.h"

namespace nx::plat {

namespace {

// x, y: 0..1 across the landscape screen; r: a fraction of the screen height.
const TouchButton kWonder[] = {
    {0.905f, 0.74f, 0.105f, kA},     {0.775f, 0.86f, 0.085f, kB},    {0.775f, 0.60f, 0.085f, kY},
    {0.905f, 0.47f, 0.075f, kX},     {0.955f, 0.25f, 0.055f, kZR},   {0.955f, 0.07f, 0.045f, kR},
    {0.045f, 0.07f, 0.045f, kL},     {0.545f, 0.07f, 0.040f, kPlus}, {0.455f, 0.07f, 0.040f, kMinus},
};

const char* const kNames[] = {"A", "B", "Y", "X", "ZR", "R", "L", "PLUS", "MINUS"};  // the order of kWonder

// Animal Crossing: New Horizons, landscape: the face buttons in the Switch's diamond at the bottom right (A talk and
// pick, B run and cancel, X pockets, Y pick up), L/R (tool ring, camera) and ZL/ZR (NookPhone, reactions) along the
// top corners, + and - at the top centre with the settings gear between them, and a small d-pad (tools, camera
// zoom) at the bottom left, under where the stick usually lands. The rest of the left half is the stick.
const TouchButton kAcnh[] = {
    {0.915f, 0.700f, 0.080f, kA},     {0.835f, 0.845f, 0.080f, kB},     {0.835f, 0.555f, 0.080f, kX},
    {0.755f, 0.700f, 0.080f, kY},     {0.955f, 0.090f, 0.050f, kR},     {0.880f, 0.090f, 0.050f, kZR},
    {0.045f, 0.090f, 0.050f, kL},     {0.120f, 0.090f, 0.050f, kZL},    {0.560f, 0.070f, 0.040f, kPlus},
    {0.440f, 0.070f, 0.040f, kMinus}, {0.500f, 0.070f, 0.036f, kSettingsButton},
    {0.075f, 0.700f, 0.040f, kUp},    {0.075f, 0.900f, 0.040f, kDown},  {0.030f, 0.800f, 0.040f, kLeft},
    {0.120f, 0.800f, 0.040f, kRight},
};

int env_int(const char* name, int fallback) {
    const char* e = std::getenv(name);
    return e && *e ? std::atoi(e) : fallback;
}

// The layout as the launcher set it (config.txt, which becomes the environment): NX_TOUCH_LAYOUT=A:x,y,r;B:... replaces
// the positions and radii of the named buttons, NX_TOUCH_SIZE (50..160, percent) scales every radius. Read once.
const std::vector<TouchButton>& wonder_buttons() {
    static const std::vector<TouchButton> v = [] {
        std::vector<TouchButton> out(std::begin(kWonder), std::end(kWonder));
        if (const char* e = std::getenv("NX_TOUCH_LAYOUT")) {
            std::string all = e;
            size_t pos = 0;
            while (pos < all.size()) {
                size_t end = all.find(';', pos);
                if (end == std::string::npos) end = all.size();
                std::string item = all.substr(pos, end - pos);
                pos = end + 1;
                size_t colon = item.find(':');
                float x, y, r;
                if (colon == std::string::npos || std::sscanf(item.c_str() + colon + 1, "%f,%f,%f", &x, &y, &r) != 3) continue;
                for (size_t k = 0; k < out.size(); k++)
                    if (item.compare(0, colon, kNames[k]) == 0 && x >= 0 && x <= 1 && y >= 0 && y <= 1 && r > 0.01f && r < 0.3f)
                        out[k].x = x, out[k].y = y, out[k].r = r;
            }
        }
        float scale = float(std::clamp(env_int("NX_TOUCH_SIZE", 100), 50, 160)) / 100.0f;
        for (auto& b : out) b.r *= scale;
        return out;
    }();
    return v;
}

}  // namespace

// NX_TOUCH_SHOW=0 (launcher: On-screen buttons off), or Settings > Controls > On-screen controls off (read live):
// nothing is drawn and touches are ignored, except by the settings screen.
bool touch_enabled() {
    static const bool env_on = [] {
        const char* e = std::getenv("NX_TOUCH_SHOW");
        return !(e && e[0] == '0');
    }();
    return env_on && config_get_bool("input.touch_controls", true);
}

// NX_TOUCH_MIRROR=1 (left-handed): the stick takes the right half of the screen instead of the left.
bool touch_mirrored() {
    static const bool on = [] {
        const char* e = std::getenv("NX_TOUCH_MIRROR");
        return e && e[0] == '1';
    }();
    return on;
}

// NX_TOUCH_OPACITY (10..100, default 85): the drawn buttons' brightness relative to the original look.
float touch_opacity_scale() {
    static const int env = env_int("NX_TOUCH_OPACITY", 0);
    int pct = env ? env : std::atoi(config_get("input.touch_opacity", "60").c_str());
    return float(std::clamp(pct, 10, 100)) / 85.0f;
}

const TouchButton* touch_buttons(size_t& count) {
    static const bool wonder = std::strcmp(game::kInfo.id, "wonder") == 0;
    static const bool acnh = std::strcmp(game::kInfo.id, "acnh") == 0;
    if (acnh) {
        count = sizeof kAcnh / sizeof kAcnh[0];
        return kAcnh;
    }
    count = wonder ? wonder_buttons().size() : 0;
    return wonder ? wonder_buttons().data() : nullptr;
}

uint64_t touch_hit(float x, float y, float aspect) {
    size_t n = 0;
    const TouchButton* b = touch_buttons(n);
    uint64_t best = 0;
    float best_d = 1e9f;
    for (size_t k = 0; k < n; k++) {
        float d = std::hypot((x - b[k].x) * aspect, y - b[k].y) / b[k].r;
        if (d < 1.35f && d < best_d) best = b[k].button, best_d = d;  // a little past the drawn edge still counts
    }
    return best;
}

#if !defined(__ANDROID__)
size_t touch_overlay(OverlayShape* out, size_t max) {
    static const bool demo = std::getenv("NX_OVERLAY_DEMO") != nullptr;
    if (!demo) return 0;
    size_t n = 0, k = 0;
    const TouchButton* b = touch_buttons(n);
    for (; k < n && k < max; k++) out[k] = {b[k].x, b[k].y, b[k].r, 0.22f};
    if (k < max) out[k++] = {0.15f, 0.72f, 0.12f, 0.10f};  // where the stick usually goes
    return k;
}
#endif

}  // namespace nx::plat
