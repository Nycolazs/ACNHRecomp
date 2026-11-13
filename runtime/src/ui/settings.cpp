// The settings screen's pages: game, video, audio, controls, saves, about. Every choice is written to config.ini at
// once (config.h); the ones the runtime can change live are applied here, the others say they need a restart.
//
// Layout: a cream card over the dimmed game, the pages as round coloured buttons down its left side (along its top
// when the screen is narrow or portrait), each setting a row with its name and a short explanation on the left and
// the control on the right (under them when narrow). Sizes are in design pixels times `scale` (ui.cpp).
#include "../hle_ldn.h"
#include "../nextendo.h"
#include "../save_unlink.h"
#include "ui/settings.h"

#if !defined(__ANDROID__)
#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"  // ClearActiveID: a drag that scrolls is not a tap (drag_scroll)

#include <nx/runtime.h>

#include "bug_report.h"
#include "config.h"
#include "touch_layout.h"
#include "game_clock.h"
#include "platform.h"
#include "saves.h"
#include "ui/amiibo_importer.h"
#include "ui/i18n.h"
#include "ui/theme.h"
#include "ui/ui_internal.h"

namespace nx::nvn {
float uw_factor();      // nvn_ultrawide.cpp: the picture's aspect over 16:9
float uw_resolution();  // and its rows over the game's
bool gpu_is_nvidia();   // nvn_vk.cpp: the device in use is NVIDIA's (DLSS)
}  // namespace nx::nvn

namespace nx::ui {

namespace {

namespace fs = std::filesystem;

ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

struct Page {
    const char* key;
    ImU32 color;
    int icon;
};
const Page kPages[] = {
    {"tab.game", rgb(124, 194, 103), 0},  {"tab.video", rgb(79, 179, 217), 1},   {"tab.audio", rgb(245, 166, 35), 2},
    {"tab.controls", rgb(155, 125, 212), 3}, {"tab.saves", rgb(232, 115, 122), 4}, {"tab.online", rgb(72, 190, 170), kIconBox},
    {"tab.amiibo", rgb(255, 180, 80), kIconKey}, {"tab.discord", rgb(88, 101, 242), kIconGlobe},
    {"tab.about", rgb(176, 140, 100), 5},
};
constexpr int kPageCount = int(sizeof(kPages) / sizeof(kPages[0]));

int g_page = 0;
constexpr int kOnlinePage = 5;
std::atomic<bool> g_show_online{false};  // the game asked for local play with the setting off (hle_ldn.cpp)
float g_scale = 1;
int g_nav_index = 0, g_nav_target = -1, g_nav_count = 0;
bool g_page_navigation = false;
bool g_nav_sidebar = false, g_sidebar_focus = false;
int g_sidebar_index = 0;
std::vector<ImVec2> g_nav_positions;
constexpr int kResumeFocus = -2, kQuitFocus = -3;

void action_nav_item(int index) {
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    if (g_nav_target == index) {
        ImGuiContext& ctx = *ImGui::GetCurrentContext();
        ImGui::ClearActiveID();
        ImGui::FocusWindow(ctx.CurrentWindow);
        ImGui::SetFocusID(ImGui::GetItemID(), ctx.CurrentWindow);
        ctx.NavInitRequest = false;
        ctx.NavInitResult.Clear();
        ImGui::NavMoveRequestCancel();
        ImGui::SetNavCursorVisible(true);
        g_nav_target = -1;
        g_nav_index = index;
        g_nav_sidebar = false;
    } else if (ImGui::IsItemFocused() && g_nav_target == -1) {
        g_nav_index = index;
        g_nav_sidebar = false;
    }
}

// Register controls in display order instead of relying on geometric navigation between columns.
void page_nav_item() {
    if (!g_page_navigation || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    ImGuiContext& ctx = *ImGui::GetCurrentContext();
    if (!ctx.LastItemData.ID || (ctx.LastItemData.ItemFlags & ImGuiItemFlags_Disabled)) return;
    const int index = g_nav_count++;
    const ImRect rect = ctx.LastItemData.Rect;
    g_nav_positions.push_back(rect.GetCenter());
    if (index == g_nav_target) {
        ImGui::ClearActiveID();
        ImGui::FocusWindow(ctx.CurrentWindow);
        ImGui::SetFocusID(ctx.LastItemData.ID, ctx.CurrentWindow);
        ImGui::SetNavCursorVisible(true);
        ImGui::SetScrollHereY();
        g_nav_index = index;
        g_nav_target = -1;
        g_nav_sidebar = false;
    } else if (ImGui::IsItemFocused() && g_nav_target < 0) {
        g_nav_index = index;
        g_nav_sidebar = false;
    }
}
std::string g_toast;
double g_toast_until = 0;
bool g_restart_note = false;
void drag_scroll(bool horizontal = false);  // below: a finger dragged over a list scrolls it
void section(const char* key);

void toast(const std::string& text) {
    g_toast = text;
    g_toast_until = ImGui::GetTime() + 4.0;
}

// ---- settings helpers -----------------------------------------------------------------------------------------

// Whether a setting waits for the game to start again. On phones the picture's upscaling, sharpness, anti-aliasing and
// frame generation are read while the game runs (nvn_present.cpp, nvn_framegen.cpp); elsewhere they are not (the
// desktop's present pass is chosen at start).
bool needs_restart(const std::string& key) {
    if (key.rfind("online.", 0) == 0) {
        if (key == "online.mode")
            ::nx::nextendo::presence_start();  // starts once, when signed in and in Nextendo mode; idles while not online
        return false;  // LDN reads online.enabled at session start; Nextendo reads mode live
    }
#if defined(__ANDROID__)
    for (const char* k : {"video.upscaler", "video.fsr_sharpness_pct", "video.antialiasing", "video.frame_gen"})
        if (key == k) return false;
#endif
    // Supersampling (ssaa15 / ssaa2) is the screen scale, taken live (nvn_ultrawide.cpp); FXAA is the present pass's,
    // chosen at start: only a change to or from it waits.
    if (key == "video.antialiasing") {
        static const bool fxaa_at_start = config_get("video.antialiasing", "off") == "fxaa";
        return fxaa_at_start != (config_get("video.antialiasing", "off") == "fxaa");
    }
    return config_needs_restart(key);
}

std::string g_row_key;  // the setting the row's control changes (combo, toggle, slider set it): its restart tag
void apply_live(const std::string& key, const std::string& value) {
    if (key == "video.fullscreen") plat::set_fullscreen(value == "1");
    else if (key == "video.show_fps") {
#if defined(_WIN32)
        _putenv_s("NX_SHOW_FPS", value.c_str());
#else
        setenv("NX_SHOW_FPS", value.c_str(), 1);
#endif
    }
    else if (key == "video.mode") {
#if defined(_WIN32)
        if (value == "handheld") _putenv_s("NX_HANDHELD", "1");
        else _putenv_s("NX_HANDHELD", "0");
#else
        if (value == "handheld") setenv("NX_HANDHELD", "1", 1);
        else unsetenv("NX_HANDHELD");
#endif
    }
    else if (key == "video.upscaler") {
#if defined(_WIN32)
        if (value == "fsr") _putenv_s("NX_FSR", "1");
        else _putenv_s("NX_FSR", "0");
#else
        if (value == "fsr") setenv("NX_FSR", "1", 1);
        else unsetenv("NX_FSR");
#endif
    }
    else if (key == "audio.volume") plat::set_master_volume(std::clamp(std::atoi(value.c_str()), 0, 100) / 100.0f);
    else if (key == "ui.language") ui_set_language(value);
    else if (key == "game.time_travel_enabled") game_clock_apply_config();
    if (needs_restart(key)) g_restart_note = true;
}

void store(const std::string& key, const std::string& value) {
    if (config_get(key) == value) return;
    // Leaving real-time sync: start custom time at the computer's clock, not the last offset.
    if (key == "game.time_travel_enabled" && value == "0") config_set("game.time_travel_offset", "0");
    config_set(key, value);
    if (key == "video.preset") {  // a preset writes its settings
        for (auto& [k, v] : config_preset(value))
            if (config_get(k) != v) config_set(k, v), apply_live(k, v);
    } else {  // a setting changed by hand away from the preset's value: the preset becomes "custom"
        for (auto& [k, v] : config_preset(config_get("video.preset", "custom")))
            if (k == key && v != value) config_set("video.preset", "custom");
    }
    config_save();
    apply_live(key, value);
}

// A setting row: name and explanation on the left, the control on the right (below when narrow).
void row(const char* label, const char* desc, const std::function<void(float width)>& control) {
    float avail = ImGui::GetContentRegionAvail().x;
    bool narrow = avail < 560.0f * g_scale;
    float ctrl_w = narrow ? avail : std::min(360.0f * g_scale, avail * 0.45f);
    ImGui::PushID(label);
    ImGui::BeginGroup();
    float text_w = narrow ? avail : avail - ctrl_w - 16.0f * g_scale;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + text_w);
    if (!narrow) ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr(label));
    if (desc && *desc) {
        ImVec2 next = ImGui::GetCursorScreenPos();
        next.y = ImGui::GetItemRectMax().y + 4.0f * g_scale;
        ImGui::SetCursorScreenPos(next);
        // A darker brown than TextDisabled: the descriptions were hard to read on the cream background.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.47f, 0.39f, 0.31f, 1.0f));
        ImGui::SetWindowFontScale(0.86f);
        ImGui::TextWrapped("%s", tr(desc));
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    const ImVec2 text_min = ImGui::GetItemRectMin(), text_max = ImGui::GetItemRectMax();
    if (!narrow) {
        ImGui::SameLine(ImGui::GetCursorPosX() + avail - ctrl_w);
    }
    ImGui::SetNextItemWidth(ctrl_w);
    g_row_key.clear();
    control(ctrl_w);
    // A setting that waits for the game to start again says so under the row's text and control.
    if (!g_row_key.empty() && needs_restart(g_row_key)) {
        const float y = std::max(text_max.y, ImGui::GetItemRectMax().y) + 3.0f * g_scale;
        ImGui::SetCursorScreenPos(ImVec2(text_min.x, y));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.66f, 0.42f, 0.18f, 1));
        ImGui::SetWindowFontScale(0.86f);
        ImGui::TextUnformatted(tr("common.on_restart"));
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, 4.0f * g_scale));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImGui::GetStyle().Colors[ImGuiCol_Border]);
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 2.0f * g_scale));
}

using Choices = std::vector<std::pair<std::string, std::string>>;  // value, shown text (already translated)

// A combo whose choice is not one key's value: `pick` stores what it means (the anti-aliasing choice writes several).
// `restart_key`: the key whose restart tag the row shows.
void choice(const std::string& restart_key, const std::string& cur, const Choices& choices, float width,
            const std::function<void(const std::string&)>& pick) {
    g_row_key = restart_key;
    std::string shown = cur;
    for (auto& [v, t] : choices)
        if (v == cur) shown = t;
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##c", shown.c_str())) {
        for (auto& [v, t] : choices) {
            bool sel = v == cur;
            if (ImGui::Selectable(t.c_str(), sel) && !sel) pick(v);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

void combo(const std::string& key, const std::string& fallback, const Choices& choices, float width) {
    g_row_key = key;
    std::string cur = config_get(key, fallback);
    std::string shown = cur;
    int cur_idx = -1;
    for (size_t i = 0; i < choices.size(); i++) {
        if (choices[i].first == cur) {
            shown = choices[i].second;
            cur_idx = int(i);
            break;
        }
    }
    // A value wider than the box ends in "…" (the full text in the list and on hover), instead of being cut mid-word.
    std::string full = shown;
    float room = (width > 0 ? width : ImGui::CalcItemWidth()) - ImGui::GetFrameHeight() - ImGui::GetStyle().FramePadding.x * 2;
    if (room > 0 && ImGui::CalcTextSize(shown.c_str()).x > room) {
        while (!shown.empty() && ImGui::CalcTextSize((shown + "…").c_str()).x > room) {
            shown.pop_back();
            while (!shown.empty() && (uint8_t(shown.back()) & 0xC0) == 0x80) shown.pop_back();  // whole UTF-8 characters
            if (!shown.empty() && uint8_t(shown.back()) >= 0xC0) shown.pop_back();
        }
        shown += "…";
    }
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    bool combo_open = ImGui::BeginCombo("##c", shown.c_str());
    ImGui::PopStyleColor();
    if (combo_open) {
        for (auto& [v, t] : choices) {
            bool sel = v == cur;
            if (ImGui::Selectable(t.c_str(), sel)) store(key, v);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        drag_scroll();  // a long list (the resolutions) scrolls under the finger too
        ImGui::EndCombo();
    }
    if (full != shown && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", full.c_str());
    page_nav_item();
    bool focused = ImGui::IsItemFocused();
    if (focused) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 rmin = ImGui::GetItemRectMin();
        ImVec2 rmax = ImGui::GetItemRectMax();
        dl->AddRect(rmin, rmax, rgb(246, 170, 40), ImGui::GetStyle().FrameRounding, 0, 2.5f * g_scale);
        if (cur_idx >= 0 && choices.size() > 1) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
                int next_idx = (cur_idx - 1 + int(choices.size())) % int(choices.size());
                store(key, choices[next_idx].first);
            } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
                int next_idx = (cur_idx + 1) % int(choices.size());
                store(key, choices[next_idx].first);
            }
        }
    }
}

// An on/off pill like the game's. Returns whether the value flipped this frame.
bool draw_toggle_pill(bool& on, float width = 0) {
    float h = ImGui::GetFrameHeight(), pill_w = h * 1.9f;
    const char* label = tr(on ? "common.on" : "common.off");
    ImVec2 ts = ImGui::CalcTextSize(label);
    float natural_w = pill_w + 10.0f * g_scale + ts.x;
    float hit_w = width > 0 ? width : natural_w;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    bool clicked = ImGui::InvisibleButton("##t", ImVec2(hit_w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    page_nav_item();
    bool focused = ImGui::IsItemFocused();
    bool hov = ImGui::IsItemHovered();
    bool changed = false;

    if (clicked) {
        on = !on;
        changed = true;
    } else if (focused) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
            if (on) {
                on = false;
                changed = true;
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
            if (!on) {
                on = true;
                changed = true;
            }
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = on ? rgb(118, 200, 170) : rgb(214, 200, 168);
    dl->AddRectFilled(p, ImVec2(p.x + pill_w, p.y + h), bg, h * 0.5f);
    if (hov || focused) {
        dl->AddRect(p, ImVec2(p.x + pill_w, p.y + h), rgb(246, 170, 40), h * 0.5f, 0, 2.5f * g_scale);
    }
    float r = h * 0.5f - 3.0f * g_scale;
    ImVec2 knob(on ? p.x + pill_w - h * 0.5f : p.x + h * 0.5f, p.y + h * 0.5f);
    dl->AddCircleFilled(knob, r, rgb(255, 255, 255));
    dl->AddText(ImVec2(p.x + pill_w + 10.0f * g_scale, p.y + (h - ts.y) * 0.5f), rgb(107, 79, 54), label);
    return changed;
}

// An on/off pill like the game's.
void toggle(const std::string& key, bool fallback, float width = 0) {
    g_row_key = key;
    bool on = config_get_bool(key, fallback);
    const bool was_on = on;
    if (draw_toggle_pill(on, width)) {
        store(key, on ? "1" : "0");
        if (key == "input.rumble" && !was_on && on) {
            plat::rumble(0.45f, 0.70f, 180);
        }
    }
}

bool g_slider_held = false;  // a slider under the finger this frame: it takes the drag, the page does not scroll


void slider(const std::string& key, int lo, int hi, int fallback, const char* fmt, float width) {
    g_row_key = key;
    int v = config_get_int(key, fallback);
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    const bool changed = ImGui::SliderInt("##s", &v, lo, hi, fmt);
    ImGui::PopStyleColor();
    page_nav_item();
    g_slider_held |= ImGui::IsItemActive();
    if (changed) {
        config_set(key, std::to_string(v));
        apply_live(key, std::to_string(v));
    }
    if (ImGui::IsItemFocused()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 rmin = ImGui::GetItemRectMin();
        ImVec2 rmax = ImGui::GetItemRectMax();
        dl->AddRect(rmin, rmax, rgb(246, 170, 40), ImGui::GetStyle().FrameRounding, 0, 2.5f * g_scale);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) config_save();
}

bool button(const char* text, float width = 0) {
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    bool res = ImGui::Button(text, ImVec2(width, 0));
    ImGui::PopStyleColor();
    page_nav_item();
    if (ImGui::IsItemFocused()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 rmin = ImGui::GetItemRectMin();
        ImVec2 rmax = ImGui::GetItemRectMax();
        dl->AddRect(rmin, rmax, rgb(246, 170, 40), ImGui::GetStyle().FrameRounding, 0, 2.5f * g_scale);
    }
    return res;
}

#if defined(__ANDROID__)
// Settings > Controls > Edit layout (#42): the screen in small, with the on-screen buttons where they are drawn. A
// finger drags a button, and the group it belongs to (the face buttons and the d-pad move as a whole), to where it
// should be; input.touch_layout keeps the offsets (touch_layout.cpp). Reset puts every button back.
bool g_touch_edit = false;

void touch_layout_editor() {
    ImGuiIO& io = ImGui::GetIO();
    const float aspect = io.DisplaySize.y > 0 ? io.DisplaySize.x / io.DisplaySize.y : 16.0f / 9.0f;
    const float w = ImGui::GetContentRegionAvail().x, h = w / aspect;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##touch_layout", ImVec2(w, h));
    const bool active = ImGui::IsItemActive();
    g_slider_held |= active;  // the drag moves a button, not the page
    static int dragging = -1;
    size_t n = 0;
    const plat::TouchButton* b = plat::touch_buttons(n, aspect);
    if (ImGui::IsItemActivated()) {
        dragging = -1;
        float best = 1e9f;
        for (size_t k = 0; k < n; k++) {
            float d = std::hypot(io.MousePos.x - (p0.x + b[k].x * w), io.MousePos.y - (p0.y + b[k].y * h)) / (b[k].r * h);
            if (d < 1.35f && d < best) best = d, dragging = int(k);
        }
    }
    if (active && dragging >= 0 && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
        plat::touch_layout_move(plat::touch_group(size_t(dragging)), io.MouseDelta.x / h, io.MouseDelta.y / h, aspect);
        b = plat::touch_buttons(n, aspect);
    }
    if (ImGui::IsItemDeactivated()) {
        if (dragging >= 0) config_save();
        dragging = -1;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), rgb(58, 92, 70), 10.0f * g_scale);
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), rgb(107, 79, 54), 10.0f * g_scale, 0, 2.0f * g_scale);
    float sx, sy, sr;
    plat::touch_stick_rest(aspect, plat::touch_mirrored(), sx, sy, sr);
    dl->AddCircle(ImVec2(p0.x + sx * w, p0.y + sy * h), sr * h, rgb(255, 255, 255, 110), 0, 2.0f * g_scale);
    const char* held = dragging >= 0 ? plat::touch_group(size_t(dragging)) : nullptr;
    for (size_t k = 0; k < n; k++) {
        const ImVec2 c(p0.x + b[k].x * w, p0.y + b[k].y * h);
        const bool hot = held && std::strcmp(held, plat::touch_group(k)) == 0;
        dl->AddCircleFilled(c, b[k].r * h, hot ? rgb(246, 170, 40, 230) : rgb(255, 255, 255, 150));
        const char* label = plat::touch_label(k);
        if (*label) {
            ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), rgb(60, 45, 30), label);
        }
    }
    ImGui::Dummy(ImVec2(0, 4.0f * g_scale));
    if (button(tr("controls.touch_layout.reset"))) {
        config_set("input.touch_layout", "");
        config_save();
    }
    ImGui::SameLine();
    if (button(tr("controls.touch_layout.done"))) g_touch_edit = false;
}
#endif

// ---- pages ----------------------------------------------------------------------------------------------------

bool ptbr_installed() {
    std::error_code ec;
    return fs::is_directory(fs::u8path(plat::data_root() + "/mods/ptbr/romfs"), ec);
}

std::tm local_tm_from_unix(int64_t t) {
    time_t tt = time_t(t);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    return tm;
}

int days_in_month(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    month = std::clamp(month, 1, 12);
    int d = kDays[month - 1];
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        if (leap) d = 29;
    }
    return d;
}

// A dropdown of the numbers lo..hi, not tied to config.ini (the time-travel pickers). `fmt` shows each one.
// Registered like the other controls: d-pad moves between them, A opens the list, left/right on a keyboard changes it.
bool int_dropdown(const char* id, int& value, int lo, int hi, const char* fmt, float width) {
    char text[16];
    std::snprintf(text, sizeof text, fmt, value);
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    const bool open = ImGui::BeginCombo("##pick", text);
    ImGui::PopStyleColor();
    bool changed = false;
    if (open) {
        for (int v = lo; v <= hi; v++) {
            std::snprintf(text, sizeof text, fmt, v);
            if (ImGui::Selectable(text, v == value)) value = v, changed = true;
            if (v == value) ImGui::SetItemDefaultFocus();
        }
        drag_scroll();
        ImGui::EndCombo();
    }
    page_nav_item();
    if (ImGui::IsItemFocused()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), rgb(246, 170, 40), ImGui::GetStyle().FrameRounding, 0,
                    2.5f * g_scale);
        if (hi > lo) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
                value = value <= lo ? hi : value - 1;
                changed = true;
            } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
                value = value >= hi ? lo : value + 1;
                changed = true;
            }
        }
    }
    ImGui::PopID();
    return changed;
}

// The custom date and time, kept as an offset from the computer's clock so it goes on ticking from there.
void store_custom_game_time(int year, int month, int day, int hour, int minute) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_isdst = -1;
    config_set("game.time_travel_offset", std::to_string(int64_t(std::mktime(&tm)) - game_clock_real_unix_seconds()));
    config_save();
    game_clock_apply_config();
}

void page_game() {
    Choices langs = {{"auto", tr("game.language.system")},
                     {"en-US", "English (US)"},     {"en-GB", "English (UK)"},  {"es", "Español (España)"},
                     {"es-419", "Español (Latinoamérica)"}, {"fr", "Français"},   {"fr-CA", "Français (Canada)"},
                     {"de", "Deutsch"},             {"it", "Italiano"},         {"nl", "Nederlands"},
                     {"ru", "Русский"},             {"ja", "日本語"},            {"ko", "한국어"},
                     {"zh-CN", "简体中文"},          {"zh-TW", "繁體中文"},       {"pt-BR", tr("game.language.ptbr")}};
    row("game.language", "game.language.desc", [&](float w) { combo("game.language", "auto", langs, w); });
    if (config_get("game.language") == "pt-BR") {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(0.35f, 0.55f, 0.35f, 1), "%s",
                           tr(ptbr_installed() ? "game.language.ptbr_note" : "game.language.ptbr_missing"));
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 6 * g_scale));
    }
    Choices ui_langs = {{"auto", tr("common.auto")}};
    for (const UiLanguage& l : ui_languages()) ui_langs.push_back({l.code, l.native_name});
    row("ui.language", "ui.language.desc", [&](float w) { combo("ui.language", "auto", ui_langs, w); });
    row("video.show_fps", "video.show_fps.desc", [&](float w) { toggle("video.show_fps", config_show_fps(), w); });

    section("game.section.time");
    row("game.time_travel.sync", "game.time_travel.sync.desc", [&](float w) { toggle("game.time_travel_enabled", true, w); });
    if (!config_get_bool("game.time_travel_enabled", true)) {
        static int s_year = 2020, s_month = 1, s_day = 1, s_hour = 0, s_minute = 0;
        static uint64_t s_time_fields_gen = 0;
        const uint64_t gen = config_generation();
        if (s_time_fields_gen != gen) {
            const std::tm tm = local_tm_from_unix(game_clock_unix_seconds());
            s_year = tm.tm_year + 1900;
            s_month = tm.tm_mon + 1;
            s_day = tm.tm_mday;
            s_hour = tm.tm_hour;
            s_minute = tm.tm_min;
            s_time_fields_gen = gen;
        }
        const float gap = 8.0f * g_scale;
        row("game.time_travel.date", nullptr, [&](float w) {
            const float part = (w - gap * 2.0f) / 3.0f;
            bool changed = int_dropdown("day", s_day, 1, days_in_month(s_year, s_month), "%02d", part);
            ImGui::SameLine(0, gap);
            changed |= int_dropdown("month", s_month, 1, 12, "%02d", part);
            ImGui::SameLine(0, gap);
            changed |= int_dropdown("year", s_year, 2000, 2060, "%d", part);
            if (changed) {
                s_day = std::min(s_day, days_in_month(s_year, s_month));  // 31 March -> February: its last day
                store_custom_game_time(s_year, s_month, s_day, s_hour, s_minute);
            }
        });
        row("game.time_travel.time", nullptr, [&](float w) {
            const float part = (w - gap) / 2.0f;
            bool changed = int_dropdown("hour", s_hour, 0, 23, "%02d", part);
            ImGui::SameLine(0, gap);
            changed |= int_dropdown("minute", s_minute, 0, 59, "%02d", part);
            if (changed) store_custom_game_time(s_year, s_month, s_day, s_hour, s_minute);
        });
        {
            const std::tm now_tm = local_tm_from_unix(game_clock_unix_seconds());
            char when[64];
            std::snprintf(when, sizeof when, "%04d-%02d-%02d %02d:%02d", now_tm.tm_year + 1900, now_tm.tm_mon + 1,
                          now_tm.tm_mday, now_tm.tm_hour, now_tm.tm_min);
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ImVec4(0.35f, 0.55f, 0.35f, 1), "%s",
                               trf("game.time_travel.island_now", when).c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 6 * g_scale));
        }
    }
}

bool lsfg_available() {
    // lsfg-vk installs an implicit Vulkan layer manifest.
    std::error_code ec;
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    for (const std::string& d : {std::string("/usr/share/vulkan/implicit_layer.d"),
                                 std::string("/etc/vulkan/implicit_layer.d"),
                                 home + "/.local/share/vulkan/implicit_layer.d"}) {
        if (!fs::is_directory(d, ec)) continue;
        for (auto& e : fs::directory_iterator(d, ec))
            if (e.path().filename().string().find("lsfg") != std::string::npos) return true;
    }
    return false;
}

// A heading over a group of rows.
void section(const char* key) {
    ImGui::Dummy(ImVec2(0, 8 * g_scale));
    ImGui::SetWindowFontScale(1.18f);
    ImGui::TextColored(ImVec4(0.24f, 0.55f, 0.36f, 1), "%s", tr(key));
    ImGui::SetWindowFontScale(1.0f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 2 * g_scale),
                                              ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y + 5 * g_scale),
                                              ImGui::GetColorU32(ImVec4(0.49f, 0.76f, 0.42f, 1)), 2 * g_scale);
    ImGui::Dummy(ImVec2(0, 12 * g_scale));
}

// The choice that matches the console, tagged as such ("1080p (TV mode) · console original").
std::string console(const char* key) { return std::string(tr(key)) + " · " + tr("common.console_tag"); }

// ---- Nextendo: server status and the Friends view of the Online page ------------------------------------------

namespace nxo = ::nx::nextendo;

// The status check runs on a worker thread (its HTTP requests take a moment); the page shows the last result.
struct StatusCheck {
    std::mutex mu;
    std::atomic<bool> running{false};
    bool done = false;
    nxo::ServiceStatus result;
} g_status;

void status_check_start() {
    if (g_status.running.exchange(true)) return;
    std::thread([] {
        const nxo::ServiceStatus s = nxo::check_service_status();
        std::lock_guard lk(g_status.mu);
        g_status.result = s;
        g_status.done = true;
        g_status.running = false;
    }).detach();
}

// One system's line: "Account service: OK" / "...: degraded (HTTP 502)" / "...: no answer". `any_answer`: any HTTP
// status means up (the game servers answer their root with an error page).
std::string status_line(const char* name_key, int http, bool any_answer, int& ok, int& total) {
    total++;
    std::string s = std::string(tr(name_key)) + ": ";
    if (http >= 200 && (any_answer || http < 300)) {
        ok++;
        return s + tr("online.server.ok");
    }
    if (http) return s + trf("online.server.degraded", std::to_string(http));
    return s + tr("online.server.down");
}

std::string status_text() {
    if (g_status.running) return tr("online.server.checking");
    std::lock_guard lk(g_status.mu);
    if (!g_status.done) return tr("online.server.desc");
    const nxo::ServiceStatus& s = g_status.result;
    int ok = 0, total = 0;
    std::string lines = status_line("online.server.health", s.health, false, ok, total) + "\n";
    if (s.account) lines += status_line("online.server.account", s.account, false, ok, total) + "\n";
    lines += status_line("online.server.game", s.game, true, ok, total);
    if (s.players >= 0) lines += "\n" + trf("online.server.players", std::to_string(s.players));
    const char* overall = ok == total ? "online.server.operational" : ok ? "online.server.partial" : "online.server.offline";
    return std::string(tr(overall)) + "\n" + lines;
}

// Friend actions (send a request, accept, decline) run on a worker thread too; their outcome shows on the Friends view.
struct FriendAction {
    std::mutex mu;
    std::atomic<bool> busy{false};
    std::string message;
} g_friend_action;

void friend_action(std::function<bool(std::string&)> fn, const char* done_key) {
    if (g_friend_action.busy.exchange(true)) return;
    std::thread([fn = std::move(fn), done_key] {
        std::string message;
        const bool ok = fn(message);
        std::lock_guard lk(g_friend_action.mu);
        g_friend_action.message = ok && message.empty() ? tr(done_key) : message;
        g_friend_action.busy = false;
    }).detach();
}

bool g_friends_view = false;  // the Online page shows the Friends view instead of its rows

std::string friend_state(const nxo::Friend& f) {
    if (f.status == 0) return tr("online.friends.offline");
    if (!f.app_id.empty() && std::strtoull(f.app_id.c_str(), nullptr, 16) == nxo::kAcnhTitleId)
        return tr(f.status == 2 ? "online.friends.acnh_online" : "online.friends.acnh");
    return tr(f.app_id.empty() ? "online.friends.online" : "online.friends.other_game");
}

// A line of the Friends view: the name and its state on the left, `buttons` (may be none) on the right.
void friend_line(const nxo::Friend& f, const std::string& state, const std::vector<std::pair<const char*, std::function<void()>>>& buttons) {
    ImGui::PushID(int(f.pid));
    const float avail = ImGui::GetContentRegionAvail().x;
    const float bw = 150.0f * g_scale, gap = ImGui::GetStyle().ItemSpacing.x;
    ImGui::BeginGroup();
    ImGui::TextUnformatted(f.name.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.47f, 0.39f, 0.31f, 1.0f));
    ImGui::SetWindowFontScale(0.86f);
    ImGui::TextUnformatted(state.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    float x = avail - float(buttons.size()) * (bw + gap) + gap;
    for (const auto& [label, act] : buttons) {
        ImGui::SameLine(x);
        if (ImGui::Button(tr(label), ImVec2(bw, 0)) && !g_friend_action.busy) act();
        page_nav_item();
        x += bw + gap;
    }
    ImGui::PopID();
    ImGui::Separator();
}

void page_friends() {
    row("online.friends.back", nullptr, [&](float w) {
        if (ImGui::Button(tr("online.friends.back"), ImVec2(w, 0))) g_friends_view = false;
        page_nav_item();
    });
    const nxo::Account acc = nxo::account();
    std::string add_desc;
    {
        std::lock_guard lk(g_friend_action.mu);
        add_desc = g_friend_action.busy ? tr("online.friends.sending") : g_friend_action.message;
    }
    if (add_desc.empty()) add_desc = trf("online.friends.add.desc", acc.friend_code.empty() ? "-" : acc.friend_code);
    row("online.friends.add", add_desc.c_str(), [&](float w) {
        static char code[40] = {};
        ImGui::SetNextItemWidth(w);
        ImGuiContext& ctx = *ImGui::GetCurrentContext();
        if (ctx.NavActivateId == ImGui::GetID("##friend_code") && ctx.NavInputSource == ImGuiInputSource_Gamepad)
            ctx.NavActivateFlags |= ImGuiActivateFlags_PreferInput;
        ImGui::InputTextWithHint("##friend_code", "SW-0000-0000-0000", code, sizeof code);
        page_nav_item();
        if (ImGui::Button(tr("online.friends.send"), ImVec2(w, 0)) && code[0] && !g_friend_action.busy) {
            const std::string c = code;
            friend_action([c](std::string& m) { return nxo::send_friend_request(c, m); }, "online.friends.sent");
            code[0] = 0;
        }
        page_nav_item();
    });
    const nxo::FriendLists lists = nxo::friend_lists_cached();
    if (!lists.loaded) {
        ImGui::TextWrapped("%s", tr("online.friends.loading"));
        return;
    }
    if (!lists.incoming.empty()) {
        section("online.friends.incoming");
        for (const auto& f : lists.incoming) {
            const uint64_t pid = f.pid;
            friend_line(f, tr("online.friends.wants"),
                        {{"online.friends.accept",
                          [pid] {
                              friend_action([pid](std::string& m) { return nxo::answer_friend_request(pid, true, m); },
                                            "online.friends.accepted");
                          }},
                         {"online.friends.decline", [pid] {
                              friend_action([pid](std::string& m) { return nxo::answer_friend_request(pid, false, m); },
                                            "online.friends.declined");
                          }}});
        }
    }
    if (!lists.outgoing.empty()) {
        section("online.friends.outgoing");
        for (const auto& f : lists.outgoing) friend_line(f, tr("online.friends.pending"), {});
    }
    section("online.friends.list");
    if (lists.friends.empty()) ImGui::TextWrapped("%s", tr("online.friends.none"));
    std::vector<nxo::Friend> sorted = lists.friends;  // online first, then by name
    std::stable_sort(sorted.begin(), sorted.end(), [](const nxo::Friend& a, const nxo::Friend& b) {
        if ((a.status != 0) != (b.status != 0)) return a.status != 0;
        return a.name < b.name;
    });
    for (const auto& f : sorted) friend_line(f, friend_state(f), {});
}

bool g_fix_link_view = false;  // the Online page shows the "Fix Save Linked Account" page instead of its rows

bool red_button(const char* text, float width) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.16f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.22f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.58f, 0.12f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    const bool pressed = ImGui::Button(text, ImVec2(width, 0));
    ImGui::PopStyleColor(4);
    return pressed;
}

#if !defined(__ANDROID__)
// Everything the player should know before the fix, then the fix itself (save_unlink.h): the game restarts, the save
// is copied unaltered to backups/save-link-fix, and the online account link is cleared from every resident.
void page_fix_link() {
    row("online.friends.back", nullptr, [&](float w) {
        if (ImGui::Button(tr("online.friends.back"), ImVec2(w, 0))) g_fix_link_view = false;
        page_nav_item();
    });
    section("online.fix_link");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.42f, 0.42f, 1.0f));
    ImGui::TextWrapped("%s", tr("online.fix_link.experimental"));
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::TextWrapped("%s", tr("online.fix_link.what"));
    section("online.fix_link.when_title");
    ImGui::TextWrapped("%s", tr("online.fix_link.when"));
    section("online.fix_link.warn_title");
    ImGui::TextWrapped("%s", tr("online.fix_link.warn"));
    section("online.fix_link.backup_title");
    ImGui::TextWrapped("%s", tr("online.fix_link.backup"));
    if (const std::string r = save_unlink::last_result(); !r.empty()) {
        section("online.fix_link.result_title");
        ImGui::TextWrapped("%s", r.c_str());
    }
    ImGui::Spacing();
    if (red_button(tr("online.fix_link.button"), ImGui::GetContentRegionAvail().x) && !save_unlink::request_restart())
        toast(tr("online.fix_link.fail"));  // request_restart returns only on failure
    page_nav_item();
}
#endif

// Network mode: Local Only (no Nextendo at all) or Nextendo (account, server status, friends). Local Play peer visits
// (docs/LOCAL_PLAY.md) follow their own switch in both.
void page_online() {
    const bool nextendo = config_get("online.mode", "nextendo") == "nextendo";
    const nxo::Account acc = nxo::account();
    if (g_friends_view && nextendo && acc.signed_in) return page_friends();
    g_friends_view = false;
#if !defined(__ANDROID__)
    if (g_fix_link_view) return page_fix_link();
#endif
    row("online.mode", tr("online.mode.desc"), [&](float w) {
        [[maybe_unused]] const float x = ImGui::GetCursorPosX();  // the control column, on the right
        combo("online.mode", "nextendo", {{"local", tr("online.mode.local")}, {"nextendo", tr("online.mode.nextendo")}}, w);
#if !defined(__ANDROID__)
        // A save linked on another network (Rustendo, Pretendo, a console) refuses to load here: the red button,
        // under the dropdown in either mode, opens the page that explains the fix and runs it (save_unlink.h).
        ImGui::SetCursorPosX(x);
        if (red_button(tr("online.fix_link"), w)) g_fix_link_view = true;
        page_nav_item();
#endif
    });
    if (nextendo) {
        section("online.section.nextendo");
        if (!nxo::version_ok_for_nextendo()) {
            ImGui::TextWrapped("%s", tr("online.nextendo.need_303"));
        } else if (acc.signed_in) {
            std::string desc = trf("online.nextendo.signed_in", acc.username);
            if (!acc.friend_code.empty()) desc += "\n" + trf("online.nextendo.friend_code", acc.friend_code);
            row("online.nextendo.account", desc.c_str(), [&](float w) {
                if (ImGui::Button(tr("online.nextendo.sign_out"), ImVec2(w, 0))) nxo::sign_out();
                page_nav_item();
            });
            const nxo::FriendLists lists = nxo::friend_lists_cached();
            int online = 0;
            for (const auto& f : lists.friends) online += f.status != 0;
            std::string fdesc = trf("online.friends.desc", std::to_string(online));
            if (!lists.incoming.empty()) fdesc += "\n" + trf("online.friends.desc.requests", std::to_string(lists.incoming.size()));
            row("online.friends", fdesc.c_str(), [&](float w) {
                if (ImGui::Button(tr("online.friends.open"), ImVec2(w, 0))) g_friends_view = true;
                page_nav_item();
            });
        } else {
            std::string desc = tr("online.nextendo.desc");
            if (nxo::client_id().empty()) desc += std::string("\n") + tr("online.nextendo.need_client_id");
            if (!nxo::last_error().empty()) desc += "\n" + nxo::last_error();
            row("online.nextendo.account", desc.c_str(), [&](float w) {
                const bool busy = nxo::sign_in_busy();
                if (ImGui::Button(tr(busy ? "online.nextendo.signing_in" : "online.nextendo.sign_in"), ImVec2(w, 0)) &&
                    !busy)
                    nxo::sign_in_start();
                page_nav_item();
            });
        }
        const std::string sdesc = status_text();
        row("online.server", sdesc.c_str(), [&](float w) {
            if (ImGui::Button(tr(g_status.running ? "online.server.checking_button" : "online.server.check"),
                              ImVec2(w, 0)))
                status_check_start();
            page_nav_item();
        });
    }
    section("online.section.local");
    const bool busy = ldn_enabled();  // held until the session ends (hle_ldn.cpp): no switch to touch
    row("online.enabled", tr(busy ? "online.enabled.busy" : "online.enabled.desc"), [&](float w) {
        if (busy) ImGui::TextDisabled("%s", tr("common.on"));
        else toggle("online.enabled", false, w);
    });
    if (config_get("online.enabled", "0") != "1") return;
    static bool checked = false;
    if (!checked) {  // once per session, as soon as it is on
        checked = true;
        link_check();
        address_check_start(config_get("online.join"));
    }
    const LinkStatus s = link_status();
    const char* text = "online.status.idle";
    switch (s.check) {
    case LinkCheck::Running: text = "online.status.running"; break;
    case LinkCheck::NoRouter: text = "online.status.no_router"; break;
    case LinkCheck::Refused: text = "online.status.refused"; break;
    case LinkCheck::NoPublicAddress: text = "online.status.no_public"; break;
    case LinkCheck::Open: text = "online.status.open"; break;
    case LinkCheck::OpenVerified: text = "online.status.verified"; break;
    case LinkCheck::Idle: break;
    }
    if (s.hosting && (s.check == LinkCheck::Open || s.check == LinkCheck::OpenVerified)) text = "online.status.hosting";
    row("online.status", tr(text), [&](float w) {
        if (!s.hosting && !busy) {
            if (ImGui::Button(tr("online.test"), ImVec2(w, 0))) link_check();
            page_nav_item();
        }
    });
    if (!s.address.empty()) {
        static bool shown = false;
        row("online.address", tr("online.address.desc"), [&](float w) {
            ImGui::TextUnformatted(shown ? s.address.c_str() : "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2");
            const float half = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button(tr(shown ? "online.hide" : "online.show"), ImVec2(half, 0))) shown = !shown;
            page_nav_item();
            ImGui::SameLine();
            if (ImGui::Button(tr("online.copy"), ImVec2(half, 0))) {
                ImGui::SetClipboardText(s.address.c_str());
                toast(tr("online.copied"));
            }
            page_nav_item();
        });
    }
    const AddressResult a = address_check_result();
    std::string join_text = tr("online.join.desc");
    switch (a.check) {
    case AddressCheck::Running: join_text = tr("online.join.running"); break;
    case AddressCheck::Bad: join_text = tr("online.join.bad"); break;
    case AddressCheck::NoAnswer: join_text = tr("online.join.no_answer"); break;
    case AddressCheck::Found: join_text = trf("online.join.found", a.island); break;
    case AddressCheck::Idle: break;
    }
    row("online.join", join_text.c_str(), [&](float w) {
        static char join[128] = {};
        static bool loaded = false;
        if (!loaded) {
            loaded = true;
            std::snprintf(join, sizeof join, "%s", config_get("online.join").c_str());
        }
        ImGui::SetNextItemWidth(w);
        ImGuiContext& ctx = *ImGui::GetCurrentContext();
        if (ctx.NavActivateId == ImGui::GetID("##online_join") && ctx.NavInputSource == ImGuiInputSource_Gamepad)
            ctx.NavActivateFlags |= ImGuiActivateFlags_PreferInput;
        if (ImGui::InputText("##online_join", join, sizeof join)) store("online.join", join);
        page_nav_item();
        if (ImGui::IsItemDeactivatedAfterEdit()) address_check_start(join);
        if (join[0]) {
            if (ImGui::Button(tr("online.join.test"), ImVec2(w, 0))) address_check_start(join);
            page_nav_item();
        }
    });
    if (s.players > 0) ImGui::TextUnformatted(trf("online.players", std::to_string(s.players)).c_str());
}

void page_video() {
    // Essential settings stay visible. The Advanced toggle (session-only) reveals fine-tuning rows below.
    section("video.section.screen");
#if !defined(__ANDROID__)  // a phone's game is always the whole screen
    row("video.display", nullptr, [&](float w) {
        combo("video.fullscreen", "0", {{"0", tr("video.display.windowed")}, {"1", tr("video.display.fullscreen")}}, w);
    });
    if (config_get("video.fullscreen", "0") != "1")
        row("video.window_size", nullptr, [&](float w) {
            combo("video.window", "auto",
                  {{"auto", tr("common.auto")}, {"1280x720", "1280 × 720"}, {"1600x900", "1600 × 900"},
                   {"1920x1080", "1920 × 1080"}, {"2560x1440", "2560 × 1440"}, {"3840x2160", "3840 × 2160"}},
                  w);
        });
#endif
    row("video.vsync", "video.vsync.desc", [&](float w) { toggle("video.vsync", true, w); });

    section("video.section.picture");
    // The screen's own first: the picture drawn at the window's rows and in its shape.
    Choices resolutions = {{"window", tr("video.resolution.window")}, {"native", console("video.resolution.native")}};
#if defined(__ANDROID__)
    resolutions.insert(resolutions.begin(), {"auto", tr("video.resolution.auto")});
#endif
    // Below 540 for weak phones and integrated GPUs (#82): blurrier, but the 3D scene's cost falls with the rows.
    for (const char* r : {"360", "432", "480", "540", "720", "900", "1080", "1440"})
        resolutions.push_back({r, std::string(r) + "p"});
    resolutions.push_back({"2160", "2160p (4K)"});
    row("video.resolution", "video.resolution.desc", [&](float w) {
        combo("video.resolution", plat::kDefaultResolution, resolutions, w);
    });
    row("video.aspect", "video.aspect.desc", [&](float w) {
        combo("video.aspect", plat::kDefaultAspect,
              {{"auto", tr("video.aspect.screen")}, {"16:9", console("video.aspect.original")}, {"16:10", "16:10"},
               {"4:3", "4:3"}, {"21:9", "21:9"}, {"32:9", "32:9"}, {"20:9", trf("video.aspect.phone", "20:9")},
               {"19.5:9", trf("video.aspect.phone", "19.5:9")}},
              w);
    });
    {   // What the two above give now: the 3D picture's rows and shape.
        const float s = nvn::uw_resolution(), k = nvn::uw_factor();
        const int rows = int(std::lround(s * (config_is_handheld() ? 720.0f : 1080.0f) * std::max(1.0f, 1.0f / k)));
        char now[160];
        std::snprintf(now, sizeof now, tr("video.picture.now"), int(std::lround(rows * 16.0f / 9.0f * k)), rows,
                      double(16.0f / 9.0f * k));
        // And the size it is shown at: the screen's rows that hold the picture's shape (bars elsewhere).
        uint32_t sw = 0, sh = 0;
        plat::screen_size(sw, sh);
        char shown[200] = {};
        if (sw && sh) {
            const double aspect = 16.0 / 9.0 * k;
            const int out_h = int(std::min(double(sh), double(sw) / aspect) + 0.5), out_w = int(out_h * aspect + 0.5);
            if (std::abs(out_h - rows) <= 2)
                std::snprintf(shown, sizeof shown, tr("video.picture.screen_size"), out_w, out_h);
            else if (out_h < rows)
                std::snprintf(shown, sizeof shown, tr("video.picture.downscaled"), out_w, out_h);
            else {
                const bool fsr = config_get("video.upscaler", plat::kDefaultUpscaler) == "fsr";
                std::snprintf(shown, sizeof shown, tr("video.picture.upscaled"), out_w, out_h,
                              tr(fsr ? "video.upscaler.fsr" : "video.upscaler.linear"));
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.55f, 0.35f, 1));
        ImGui::TextWrapped("%s", now);
        if (shown[0]) ImGui::TextWrapped("%s", shown);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 4 * g_scale));
    }
    row("video.mode", nullptr, [&](float w) {
        combo("video.mode", "docked", {{"docked", console("video.render.docked")}, {"handheld", tr("video.render.handheld")}}, w);
    });

    section("video.section.performance");
    row("video.preset", "video.preset.desc", [&](float w) {
        combo("video.preset", "custom",
              {{"custom", tr("video.preset.custom")}, {"quality", tr("video.preset.quality")},
               {"balanced", tr("video.preset.balanced")}, {"low", tr("video.preset.low")}},
              w);
    });
    row("video.framerate", "video.framerate.desc", [&](float w) {
        // No choice yet: 30, as config.cpp applies.
        combo("video.frame_rate", "30",
              {{"30", console("video.framerate.30")}, {"60", tr("video.framerate.60")}}, w);
    });
    // Anti-aliasing method stays essential; its quality knobs live under Advanced.
    const std::string temporal = config_get("video.temporal", "off"), aa = config_get("video.antialiasing", "off");
#if defined(_WIN32)
    const bool nvidia = nvn::gpu_is_nvidia();
    const std::string method = temporal != "off" ? temporal : aa == "fxaa" ? "fxaa" : "off";
#else
    const bool nvidia = false;
    const std::string method = aa == "fxaa" ? "fxaa" : "off";
#endif
    row("video.aa", "video.aa.desc", [&](float w) {
        Choices methods;
#if defined(_WIN32)
        if (nvidia || method == "dlaa") methods.push_back({"dlaa", tr("video.aa.dlss")});
        methods.push_back({"fsr", tr("video.aa.fsr")});
#endif
        methods.push_back({"fxaa", tr("video.aa.fxaa")});
        methods.push_back({"off", console("video.aa.off")});
        choice("video.temporal", method, methods, w, [&](const std::string& m) {
            const bool temporal_m = m == "dlaa" || m == "fsr";
            store("video.temporal", temporal_m ? m : "off");
            if (m == "fxaa") store("video.antialiasing", "fxaa");
            else if (aa == "fxaa") store("video.antialiasing", "off");  // supersampling kept otherwise
            if (!temporal_m && config_get("video.frame_gen", "off") == "fsr") store("video.frame_gen", "off");
        });
    });
    // Built-in (nvn_framegen.cpp) always; lsfg-vk when it is installed (or already chosen).
    bool lsfg = lsfg_available();
    const char* fg_default = "off";  // nvn_framegen.cpp
    std::string fg = config_get("video.frame_gen", fg_default);
    Choices fg_choices = {{"off", console("video.framegen.off")}, {"builtin", tr("video.framegen.builtin")}};
    if (lsfg || fg == "lsfg") fg_choices.push_back({"lsfg", tr("video.framegen.lsfg")});
#if defined(_WIN32)
    // AMD FSR 3 (nvn_temporal.cpp): needs DLSS or FSR anti-aliasing (their depth and motion vectors).
    const bool temporal_on = config_get("video.temporal", "off") != "off";
    if (temporal_on || fg == "fsr") fg_choices.push_back({"fsr", tr("video.framegen.fsr")});
#else
    const bool temporal_on = false;
#endif
    const char* fg_desc = fg == "lsfg"  ? (lsfg ? "video.framegen.desc" : "video.framegen.missing")
                          : fg == "fsr" ? (temporal_on ? "video.framegen.fsr.desc" : "video.framegen.fsr.needs")
                                        : temporal_on ? "video.framegen.builtin.desc" : "video.framegen.builtin.only";
    row("video.framegen", fg_desc, [&](float w) { combo("video.frame_gen", fg_default, fg_choices, w); });

    section("video.section.advanced");
    static bool show_advanced = false;
    row("video.advanced", "video.advanced.desc", [&](float w) { draw_toggle_pill(show_advanced, w); });
    if (!show_advanced) return;

    // Upscaling quality, supersampling, HDR, filtering, legacy FSR 1, GPU / shaders — tweak only if needed.
    section("video.section.aa");
    if (method != "fxaa")
        row("video.supersample", "video.supersample.desc", [&](float w) {
            combo("video.antialiasing", "off",
                  {{"off", tr("video.supersample.native")}, {"ssaa15", tr("video.supersample.15")}, {"ssaa2", tr("video.supersample.2")}}, w);
        });
    if (method == "dlaa" || method == "fsr") {
        row("video.upscale", "video.upscale.desc", [&](float w) {
            combo("video.upscale", "native",
                  {{"native", tr(method == "dlaa" ? "video.upscale.dlaa" : "video.upscale.native")},
                   {"quality", tr("video.upscale.quality")}, {"balanced", tr("video.upscale.balanced")},
                   {"performance", tr("video.upscale.performance")}, {"ultra", tr("video.upscale.ultra")}},
                  w);
        });
        if (method == "dlaa")  // nvn_temporal_dlss.cpp
            row("video.dlss_model", "video.dlss_model.desc", [&](float w) {
                combo("video.dlss_model", "4.5", {{"4.5", tr("video.dlss_model.45")}, {"4", tr("video.dlss_model.4")}}, w);
            });
        if (method == "fsr")
            row("video.fsr_version", nullptr, [&](float w) {
                combo("video.fsr_version", "auto", {{"auto", tr("video.fsr_version.auto")}, {"3", tr("video.fsr_version.3")}}, w);
            });
        // FSR's own RCAS, or ours after DLSS (nvn_temporal.cpp)
        row("video.sharpness", nullptr, [&](float w) { slider("video.temporal_sharpness_pct", 0, 100, 60, "%d%%", w); });
    }
    row("video.hdr", "video.hdr.desc", [&](float w) {
        combo("video.hdr", "off", {{"off", tr("video.hdr.off")}, {"scrgb", "scRGB"}, {"hdr10", "HDR10"}}, w);
    });
    if (config_get("video.hdr", "off") != "off") {  // read live by the present pass (nvn_present.cpp)
        row("video.hdr_tonemap", "video.hdr_tonemap.desc", [&](float w) {
            combo("video.hdr_tonemap", "hdr", {{"hdr", tr("video.hdr_tonemap.hdr")}, {"renodrt", tr("video.hdr_tonemap.renodrt")}, {"vanilla", tr("video.hdr_tonemap.vanilla")}}, w);
        });
        row("video.hdr_peak", "video.hdr_peak.desc", [&](float w) { slider("video.hdr_peak_nits", 400, 4000, 1000, "%d nits", w); });
        row("video.hdr_game", "video.hdr_game.desc", [&](float w) { slider("video.hdr_game_nits", 80, 500, 203, "%d nits", w); });
        row("video.hdr_ui", "video.hdr_ui.desc", [&](float w) { slider("video.hdr_ui_nits", 80, 500, 203, "%d nits", w); });
        row("video.hdr_gamma", "video.hdr_gamma.desc", [&](float w) {
            combo("video.hdr_gamma", "2.2", {{"2.2", "2.2"}, {"srgb", "sRGB"}}, w);
        });
    }
    row("video.aniso", "video.aniso.desc", [&](float w) {
        combo("video.anisotropy", "game",
              {{"game", console("video.aniso.game")}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}}, w);
    });

    // FSR 1: a spatial upscale of the finished picture to a window larger than it (what DLSS and FSR do better).
    section("video.section.legacy");
    row("video.upscaler", "video.upscaler.desc", [&](float w) {
        combo("video.upscaler", plat::kDefaultUpscaler, {{"linear", tr("video.upscaler.linear")}, {"fsr", tr("video.upscaler.fsr")}}, w);
    });
    if (config_get("video.upscaler", plat::kDefaultUpscaler) == "fsr")
        row("video.sharpness", nullptr, [&](float w) { slider("video.fsr_sharpness_pct", 0, 100, 80, "%d%%", w); });

#if !defined(__ANDROID__)  // one GPU in a phone
    row("video.gpu", nullptr, [&](float w) {
        combo("video.gpu", "auto",
              {{"auto", tr("common.auto")}, {"discrete", tr("video.gpu.discrete")}, {"integrated", tr("video.gpu.integrated")}}, w);
    });
#endif
    row("video.shaders", "video.shaders.desc", [&](float w) { toggle("video.shader_prepare", true, w); });
#if defined(__ANDROID__)
    row("video.async_shaders", "video.async_shaders.desc", [&](float w) { toggle("video.async_shaders", true, w); });
#else
    row("video.async_shaders", "video.async_shaders.desc", [&](float w) { toggle("video.async_shaders", false, w); });
#endif
#if defined(__ANDROID__)
    // Read live (thermal.cpp): no restart.
    row("video.thermal", "video.thermal.desc", [&](float w) {
        combo("video.thermal", "balanced",
              {{"performance", tr("video.thermal.performance")}, {"balanced", tr("video.thermal.balanced")},
               {"cool", tr("video.thermal.cool")}},
              w);
    });
    row("video.root_clocks", "video.root_clocks.desc", [&](float w) { toggle("video.root_clocks", false, w); });
    // Read when Vulkan is loaded (platform_android.cpp load_vulkan): a restart (issue #47).
    row("video.gpu_driver", "video.gpu_driver.desc", [&](float w) {
        combo("video.gpu_driver", "auto",
              {{"auto", tr("common.auto")}, {"turnip", tr("video.gpu_driver.turnip")},
               {"system", tr("video.gpu_driver.system")}, {"custom", tr("video.gpu_driver.custom")}},
              w);
    });
#endif
}

void page_audio() {
    row("audio.volume", nullptr, [&](float w) { slider("audio.volume", 0, 100, 100, "%d%%", w); });
    row("audio.reverb", "audio.reverb.desc", [&](float w) { toggle("audio.reverb", true, w); });
#if !defined(__ANDROID__)
    row("audio.surround", "audio.surround.desc", [&](float w) { toggle("audio.surround", false, w); });
#endif
    row("audio.background", nullptr, [&](float w) { toggle("audio.mute_background", false, w); });
}

// Keyboard bindings: the action, its config key and its default key (SDL scancode name).
struct Binding {
    const char* action;  // i18n key
    const char* key;     // input.key.<key>
    const char* def;
};
const Binding kBindings[] = {
    {"action.move", "move_up", "W"},      {"action.move", "move_down", "S"},   {"action.move", "move_left", "A"},
    {"action.move", "move_right", "D"},   {"action.camera", "cam_up", "I"},    {"action.camera", "cam_down", "K"},
    {"action.camera", "cam_left", "J"},   {"action.camera", "cam_right", "L"}, {"action.a", "a", "Space"},
    {"action.b", "b", "Left Shift"},      {"action.x", "x", "X"},              {"action.y", "y", "F"},
    {"action.l", "l", "Q"},               {"action.r", "r", "E"},              {"action.zl", "zl", "Z"},
    {"action.zr", "zr", "C"},             {"action.plus", "plus", "Return"},   {"action.minus", "minus", "Escape"},
    {"action.dpad", "dpad_up", "Up"},     {"action.dpad", "dpad_down", "Down"}, {"action.dpad", "dpad_left", "Left"},
    {"action.dpad", "dpad_right", "Right"},
    {"action.screenshot", "screenshot", "F12"},  // not a game button: ui.cpp (an HDR PNG with HDR on, nvn_screenshot.cpp)
};
int g_capturing = -1;
bool g_capturing_alt = false;  // the capture is for the action's second key (input.key2.<key>)
constexpr int kAltKeys = 22;   // the game buttons: the screenshot key has one only

const char* arrow_for(const char* key) {
    if (std::strstr(key, "_up")) return " ↑";
    if (std::strstr(key, "_down")) return " ↓";
    if (std::strstr(key, "_left")) return " ←";
    if (std::strstr(key, "_right")) return " →";
    return "";
}

void page_controls() {
    std::string name = plat::controller_name();
    row("controls.device", nullptr, [&](float) {
        ImGui::TextWrapped("%s", name.empty() ? tr("controls.none") : name.c_str());
    });
    row("controls.layout", nullptr, [&](float w) {
        combo("input.layout", "console",
              {{"console", tr("controls.layout.console")}, {"xbox", tr("controls.layout.xbox")}}, w);
    });
    row("controls.glyphs", nullptr, [&](float w) {
        combo("controls.glyphs", "auto",
              {{"auto", tr("controls.glyphs.auto")}, {"switch", tr("controls.glyphs.switch")},
               {"xbox", tr("controls.glyphs.xbox")}, {"playstation", tr("controls.glyphs.playstation")}},
              w);
    });
    row("controls.rumble", nullptr, [&](float w) { toggle("input.rumble", true, w); });
    row("controls.deadzone", nullptr, [&](float w) { slider("input.deadzone", 5, 50, 24, "%d%%", w); });
#if defined(__ANDROID__)
    // Phones: the on-screen controls instead of keyboard keys.
    row("controls.touch", "controls.touch.desc", [&](float w) { toggle("input.touch_controls", true, w); });
    row("controls.touch_opacity", nullptr, [&](float w) { slider("input.touch_opacity", 10, 100, 60, "%d%%", w); });
    row("controls.touch_size", nullptr, [&](float w) { slider("input.touch_size", 50, 150, 100, "%d%%", w); });
    if (plat::touch_group(0)) {
        row("controls.touch_layout", "controls.touch_layout.desc", [&](float w) {
            if (button(tr(g_touch_edit ? "controls.touch_layout.done" : "controls.touch_layout.edit"), w))
                g_touch_edit = !g_touch_edit;
        });
        if (g_touch_edit) touch_layout_editor();
    }
#else
    ImGui::Dummy(ImVec2(0, 6 * g_scale));
    ImGui::PushFont(nullptr);
    ImGui::TextUnformatted(tr("controls.keyboard"));
    ImGui::PopFont();
    // A capture in progress: the next key pressed (ui.cpp) becomes the binding; Escape cancels.
    if (g_capturing >= 0) {
        int sc = ui_take_captured_key();
        const std::string prefix = g_capturing_alt ? "input.key2." : "input.key.";
        if (sc == SDL_SCANCODE_ESCAPE) {
            g_capturing = -1;
        } else if (sc == SDL_SCANCODE_BACKSPACE && g_capturing_alt) {  // no second key
            store(prefix + kBindings[g_capturing].key, "");
            g_capturing = -1;
        } else if (sc > 0) {
            store(prefix + kBindings[g_capturing].key, SDL_GetScancodeName(SDL_Scancode(sc)));
            g_capturing = -1;
        }
    }
    ImGui::TextDisabled("%s", tr("controls.alt_key.desc"));
    if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (int k = 0; k < int(sizeof(kBindings) / sizeof(kBindings[0])); k++) {
            const Binding& b = kBindings[k];
            ImGui::PushID(k);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", tr(b.action), arrow_for(b.key));
            for (int alt = 0; alt < 2; alt++) {
                ImGui::TableNextColumn();
                if (alt && k >= kAltKeys) continue;
                ImGui::PushID(alt);
                std::string cur = alt ? config_get(std::string("input.key2.") + b.key, "")
                                      : config_get(std::string("input.key.") + b.key, b.def);
                if (cur.empty()) cur = "-";
                const bool capturing = g_capturing == k && g_capturing_alt == bool(alt);
                std::string text = capturing ? std::string(tr(alt ? "controls.press_key_alt" : "controls.press_key")) : cur;
                if (ImGui::Button(text.c_str(), ImVec2(-1, 0))) {
                    g_capturing = k;
                    g_capturing_alt = alt;
                    ui_begin_key_capture();
                }
                page_nav_item();
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (button(tr("controls.reset_keys"))) {
        for (const Binding& b : kBindings) config_set(std::string("input.key.") + b.key, b.def);
        for (int k = 0; k < kAltKeys; k++) config_set(std::string("input.key2.") + kBindings[k].key, "");
        config_save();
        toast(tr("common.saved"));
    }
#endif
}

// Saves: what is there, backups, import/export (saves.h). Folder answers come from the platform's picker.
enum class Pick { None, Import, Export };
Pick g_pick = Pick::None;
std::string g_confirm_import;  // a path waiting for the player's yes
std::string g_confirm_restore;

// After an import or a restore the running game must not save again (it would write the old island over the new
// one): the screen then only offers to quit. Also set by the game's own Island Backup restore (hle_olsc.cpp).
std::atomic<bool> g_must_quit{false};

void do_import(const std::string& from) {
    std::string err;
    if (saves::import_save(from, &err))
        g_must_quit = true;
    else
        toast(trf("saves.import.fail", err));
}

void page_saves() {
    bool has = saves::has_save();
    row("saves.current", nullptr, [&](float) {
        if (has)
            ImGui::TextWrapped("%s", trf("saves.last_modified", saves::last_modified()).c_str());
        else
            ImGui::TextWrapped("%s", tr("saves.none"));
    });
    float bw = std::min(ImGui::GetContentRegionAvail().x, 300.0f * g_scale);
    if (button(tr("saves.open_folder"), bw)) {
        std::error_code ec;
        fs::create_directories(fs::u8path(saves::save_dir()), ec);
        plat::open_path(saves::save_dir());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!has);
    if (button(tr("saves.backup"), bw)) {
        std::string err, name = saves::backup_now(&err);
        toast(name.empty() ? err : trf("saves.backup.ok", name));
    }
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, 4 * g_scale));
    ImGui::TextWrapped("%s", tr("saves.import.desc"));
    ImGui::TextColored(ImVec4(0.75f, 0.35f, 0.3f, 1), "%s", tr("saves.import.warning"));
    if (button(tr("saves.import"), bw)) {
        g_pick = Pick::Import;
        plat::pick_path(false, tr("saves.pick_folder"));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!has);
    if (button(tr("saves.export"), bw)) {
        g_pick = Pick::Export;
        plat::pick_path(false, tr("saves.pick_folder"));
    }
    ImGui::EndDisabled();
    std::string picked;
    if (g_pick != Pick::None && plat::pick_result(picked)) {
        Pick what = g_pick;
        g_pick = Pick::None;
        if (!picked.empty()) {
            if (what == Pick::Import) {
                std::string why;
                if (saves::looks_like_acnh_save(picked, &why))
                    g_confirm_import = picked;
                else
                    toast(trf("saves.import.fail", why));
            } else {
                std::string written, err;
                toast(saves::export_save(picked, &written, &err) ? trf("saves.export.ok", written)
                                                                 : trf("saves.export.fail", err));
            }
        }
    }
    ImGui::Dummy(ImVec2(0, 6 * g_scale));
    ImGui::TextUnformatted(tr("saves.detected"));
    static std::vector<saves::Found> found;
    static double scanned = -100;
    if (ImGui::GetTime() - scanned > 5) found = saves::detect_emulator_saves(), scanned = ImGui::GetTime();
    if (found.empty()) ImGui::TextDisabled("%s", tr("saves.detected.none"));
    for (size_t k = 0; k < found.size(); k++) {
        ImGui::PushID(int(k));
        ImGui::BulletText("%s", found[k].label.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("saves.import"))) g_confirm_import = found[k].path;
        page_nav_item();
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 6 * g_scale));
    ImGui::TextUnformatted(tr("saves.backups"));
    std::string island;  // list_backups gives "<island folder>/<backup>", grouped by island
    for (const std::string& b : saves::list_backups()) {
        const size_t slash = b.find('/');
        if (b.compare(0, slash, island) != 0) {
            island = b.substr(0, slash);
            ImGui::TextDisabled("%s", island.c_str());
        }
        ImGui::PushID(b.c_str());
        ImGui::BulletText("%s", b.c_str() + slash + 1);
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("saves.restore"))) g_confirm_restore = b;
        page_nav_item();
        ImGui::PopID();
    }
    // Confirmations.
    if (!g_confirm_import.empty()) ImGui::OpenPopup("confirm_import");
    if (ImGui::BeginPopupModal("confirm_import", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextWrapped("%s", tr("saves.import.warning"));
        ImGui::TextDisabled("%s", g_confirm_import.c_str());
        if (button(tr("common.yes"))) {
            do_import(g_confirm_import);
            g_confirm_import.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        bool cancel = button(tr("common.cancel"));
        ImGui::SetItemDefaultFocus();
        if (cancel || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            g_confirm_import.clear(), ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!g_confirm_restore.empty()) ImGui::OpenPopup("confirm_restore");
    if (ImGui::BeginPopupModal("confirm_restore", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextWrapped("%s", tr("saves.restore.confirm"));
        if (button(tr("common.yes"))) {
            std::string err;
            if (saves::restore_backup(g_confirm_restore, &err))
                g_must_quit = true;
            else
                toast(err);
            g_confirm_restore.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        bool cancel = button(tr("common.cancel"));
        ImGui::SetItemDefaultFocus();
        if (cancel || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            g_confirm_restore.clear(), ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// A problem report (bug_report.h), written on a thread: zipping a long log takes a moment.
std::atomic<int> g_report{0};  // 0 none, 1 writing, 2 written, 3 failed
std::mutex g_report_lock;
std::string g_report_path;  // the .zip (or folder) written, or the error

void report_section() {
    static const bool failed_before = previous_session_failed();
    ImGui::Dummy(ImVec2(0, 10 * g_scale));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4 * g_scale));
    ImGui::TextColored(ImVec4(0.42f, 0.31f, 0.21f, 1), "%s", tr("report.title"));
    if (failed_before) ImGui::TextColored(ImVec4(0.80f, 0.33f, 0.30f, 1), "%s", tr("report.prev_failed"));
    ImGui::TextWrapped("%s", tr("report.desc"));
    int state = g_report.load();
    if (state == 1) {
        ImGui::TextUnformatted(tr("report.working"));
        return;
    }
    if (button(tr("report.make"))) {
        g_report = 1;
        std::thread([] {
            std::string error, path = write_bug_report(error);
            std::lock_guard lk(g_report_lock);
            g_report_path = path.empty() ? error : path;
            g_report = path.empty() ? 3 : 2;
        }).detach();
    }
    std::string path;
    {
        std::lock_guard lk(g_report_lock);
        path = g_report_path;
    }
    if (state == 3) ImGui::TextColored(ImVec4(0.80f, 0.33f, 0.30f, 1), "%s", trf("report.failed", path).c_str());
    if (state != 2) return;
    ImGui::TextWrapped("%s", trf("report.done", path).c_str());
    if (button(tr("report.show"))) {
        auto dir = fs::u8path(path).parent_path().u8string();
        plat::open_path(std::string(dir.begin(), dir.end()));
    }
    ImGui::SameLine();
#if !defined(__ANDROID__)
    if (button(tr("report.github"))) SDL_OpenURL("https://github.com/Nycolazs/ACNHRecomp/issues/new");
#endif
}

// The project's Discord server (also launcher/pages.cpp)
const char* const kDiscordUrl = "https://discord.gg/VJW9Dtdu7a";

void page_discord() {
#if !defined(__ANDROID__)  // Rich Presence talks to the Discord desktop app (discord_presence.cpp)
    row("discord.presence", "discord.presence.desc", [&](float w) { toggle("discord.enabled", true, w); });
    ImGui::Dummy(ImVec2(0, 6 * g_scale));
#endif
    ImGui::TextWrapped("%s", tr("discord.server.desc"));
#if !defined(__ANDROID__)
    if (button(tr("discord.server"))) SDL_OpenURL(kDiscordUrl);
#else
    if (button(tr("discord.server"))) plat::open_url_in_app(kDiscordUrl, "com.discord");
#endif
}

void page_about() {
    ImGui::TextWrapped("%s", trf("about.version", NX_VERSION_STRING).c_str());
    ImGui::Dummy(ImVec2(0, 6 * g_scale));
    ImGui::TextWrapped("%s", tr("about.desc"));
    ImGui::Dummy(ImVec2(0, 6 * g_scale));
    ImGui::TextWrapped("%s", tr("about.author"));
#if !defined(__ANDROID__)
    if (button("Instagram: @nycolazs")) SDL_OpenURL("https://www.instagram.com/nycolazs/");
    ImGui::SameLine();
    if (button("GitHub: nycolazs")) SDL_OpenURL("https://github.com/nycolazs");
#else  // in the Instagram / GitHub app when it is installed
    if (button("Instagram: @nycolazs")) plat::open_url_in_app("https://www.instagram.com/nycolazs/", "com.instagram.android");
    ImGui::SameLine();
    if (button("GitHub: nycolazs")) plat::open_url_in_app("https://github.com/nycolazs", "com.github.android");
#endif
    ImGui::Dummy(ImVec2(0, 6 * g_scale));
    ImGui::TextWrapped("%s", tr("about.credits"));
    ImGui::TextWrapped("%s", tr("about.license"));
    report_section();
}

// Android without a controller: the screen is used by touch, so no keyboard keys are shown (the footer's hints, the
// PgUp / PgDn page buttons, "(Esc)" on the resume button); with a controller its buttons are.
// A finger dragged over a list scrolls it, as on any phone (ImGui alone scrolls with a wheel or the bar, so on a touch
// screen the rows below the first screenful could not be reached). Past a few pixels the press is no longer a tap:
// the item under the finger lets go, so lifting it does not press a button or open a list. Phones only.
void drag_scroll(bool horizontal) {
#if defined(__ANDROID__)
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || g_slider_held) return;
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) return;
    const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 12.0f * g_scale);
    if (horizontal ? d.x == 0 : d.y == 0) return;
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::GetActiveID() != 0) ImGui::ClearActiveID();
    if (horizontal) ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseDelta.x);
    else ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
#else
    (void)horizontal;
#endif
}

bool touch_only() {
#if defined(__ANDROID__)
    return plat::controller_kind().empty();
#else
    return false;
#endif
}

// The button names the hints show (the footer, "Back to the game (B)"): the keyboard's keys with no controller, else
// the controller's (or the chosen icon style's): the bottom face button confirms and the right one cancels.
struct NavNames {
    std::string tabs, move, ok, back;
};
NavNames nav_names() {
    std::string style = config_get("controls.glyphs", "auto"), kind = plat::controller_kind();
    if (kind.empty()) return {"PgUp / PgDn", tr("nav.arrows"), "Enter", "Esc"};
    std::string k = style == "auto" ? kind : style;
    if (k == "playstation") return {"L1 / R1", "D-Pad", "X", "O"};
    if (k == "xbox") return {"LB / RB", "D-Pad", "A", "B"};
    return {"L / R", "D-Pad", "A", "B"};
}

// nav.hint with its button names ("L / R", "D-Pad", "A", "B" in every language) replaced by n's.
std::string nav_hint(const NavNames& n) {
    std::string h = tr("nav.hint");
    auto sub = [&](const std::string& from, const std::string& to) {
        if (size_t at = h.find(from); at != std::string::npos) h.replace(at, from.size(), to);
    };
    sub("L / R  ", n.tabs + "  ");
    sub("D-Pad  ", n.move + "  ");
    sub("    A  ", "    " + n.ok + "  ");
    sub("    B  ", "    " + n.back + "  ");
    return h;
}

// A shoulder button's label with a drawn arrow: "◀ L1" or "R1 ▶", centred in the button at p of the given size.
void bumper_label(ImDrawList* dl, ImVec2 p, ImVec2 size, const std::string& label, bool left, ImU32 col) {
    ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    float a = ts.y * 0.42f, gap = ts.y * 0.35f;  // arrow height, space to the text
    float w = a * 0.85f + gap + ts.x;
    float x = p.x + (size.x - w) * 0.5f, cy = p.y + size.y * 0.5f;
    if (left) {
        dl->AddTriangleFilled(ImVec2(x, cy), ImVec2(x + a * 0.85f, cy - a * 0.5f), ImVec2(x + a * 0.85f, cy + a * 0.5f), col);
        dl->AddText(ImVec2(x + a * 0.85f + gap, cy - ts.y * 0.5f), col, label.c_str());
    } else {
        dl->AddText(ImVec2(x, cy - ts.y * 0.5f), col, label.c_str());
        float ax = x + ts.x + gap;
        dl->AddTriangleFilled(ImVec2(ax + a * 0.85f, cy), ImVec2(ax, cy - a * 0.5f), ImVec2(ax, cy + a * 0.5f), col);
    }
}

bool g_quit_confirm = false;

}  // namespace

void settings_nav_item() {
    page_nav_item();
    if (!ImGui::IsItemFocused()) return;
    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), rgb(246, 170, 40),
                                        ImGui::GetStyle().FrameRounding, 0, 2.5f * g_scale);
}

void settings_show_online() { g_show_online = true; }
void settings_must_quit() { g_must_quit = true; }

bool settings_draw(float scale, ImFont* title, float opening) {
    g_scale = scale;
    if (g_show_online.exchange(false)) g_page = kOnlinePage;
    static const bool cycle = std::getenv("NX_UI_CYCLE") != nullptr;  // tests: every page in turn, 10 s each
    if (cycle) g_page = int(double(plat::monotonic_ns()) / 1e10) % kPageCount;
    static const char* page = std::getenv("NX_UI_PAGE");  // tests: that page (0 game, 1 video, ...) when opened
    static bool paged = false;
    if (page && !paged) g_page = std::clamp(std::atoi(page), 0, kPageCount - 1), paged = true;
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 ds = io.DisplaySize;
    static int last_page = -1;
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    bool back_to_sidebar = false;
    const bool cancel_capture = g_capturing >= 0 && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    if (cancel_capture) {
        g_capturing = -1;
        ui_cancel_key_capture();
        ImGui::ClearActiveID();
    }
    if (!popup && g_capturing < 0 && !cancel_capture) {
        if (opening == 0.0f) {
            g_nav_sidebar = true;
            g_sidebar_index = g_page;
            g_sidebar_focus = true;
            g_nav_target = -1;
        }
        if (!g_nav_sidebar && opening > 0.0f && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            ImGui::ClearActiveID();
            g_nav_sidebar = true;
            g_sidebar_index = g_page;
            g_sidebar_focus = true;
            g_nav_target = -1;
            back_to_sidebar = true;
        }
        const int direction = (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, true) ? 1 : 0) -
                              (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, true) ? 1 : 0);
        const int horizontal = (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) ? 1 : 0) -
                               (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false) ? 1 : 0);
        ImGuiContext& ctx = *ImGui::GetCurrentContext();
        const bool editing = ctx.ActiveId && ctx.ActiveId == ctx.NavId;
        if (direction && g_nav_sidebar) {
            int next = g_sidebar_index + direction;
            if (next < 0 || next >= kPageCount) {
                g_nav_sidebar = false;
                g_nav_target = next < 0 ? kResumeFocus : kQuitFocus;
            } else {
                g_sidebar_index = next;
                g_sidebar_focus = true;
            }
        } else if (direction && g_nav_index == kResumeFocus) {
            if (direction > 0) g_nav_target = g_nav_count > 0 ? 0 : kQuitFocus;
        } else if (direction && g_nav_index == kQuitFocus) {
            if (direction < 0) g_nav_target = g_nav_count > 0 ? g_nav_count - 1 : kResumeFocus;
        } else if (direction && g_nav_count > 0) {
            int next = g_nav_index + direction;
            g_nav_target = next < 0 ? kResumeFocus : next >= g_nav_count ? kQuitFocus : next;
            ImGui::ClearActiveID();
        }
        if (horizontal && !editing) {
            if (g_nav_sidebar && horizontal > 0 && g_nav_count > 0) {
                g_nav_sidebar = false;
                g_nav_target = std::clamp(g_nav_index, 0, g_nav_count - 1);
            } else if (!g_nav_sidebar && g_nav_index < 0 && horizontal < 0) {
                g_nav_sidebar = true;
                g_sidebar_index = g_page;
                g_sidebar_focus = true;
                g_nav_target = -1;
            } else if (!g_nav_sidebar && g_nav_index >= 0 && g_nav_index < g_nav_count) {
                const ImVec2 origin = g_nav_positions[g_nav_index];
                int neighbour = -1;
                float best = 1e30f;
                for (int i = 0; i < int(g_nav_positions.size()); ++i) {
                    const ImVec2 p = g_nav_positions[i];
                    float dx = (p.x - origin.x) * horizontal;
                    if (dx > 1.0f && std::abs(p.y - origin.y) < 20.0f * scale && dx < best)
                        neighbour = i, best = dx;
                }
                if (neighbour >= 0) g_nav_target = neighbour;
                else if (horizontal < 0) {
                    g_nav_sidebar = true;
                    g_sidebar_index = g_page;
                    g_sidebar_focus = true;
                    g_nav_target = -1;
                }
            }
        }
        if (ctx.NavInputSource == ImGuiInputSource_Gamepad && !(editing && horizontal)) ImGui::NavMoveRequestCancel();
    }
    bool portrait = ds.x < ds.y * 1.2f;
    g_slider_held = false;  // set again by a slider still held (slider)
    // Dim the game.
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ds, rgb(40, 28, 12, 120));
    ImVec2 size = portrait ? ImVec2(ds.x * 0.96f, ds.y * 0.94f)
                           : ImVec2(std::min(ds.x * 0.92f, 1180.0f * scale), std::min(ds.y * 0.9f, 680.0f * scale));
    const float remaining = 1.0f - opening;
    const float slide = 18.0f * scale * remaining * remaining * remaining;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f + slide), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    bool open = true;
    ImGui::Begin("##settings", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Header: title on a green ribbon, close at the right.
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = 56.0f * scale, w = ImGui::GetContentRegionAvail().x;
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), rgb(124, 194, 103), h * 0.5f);
        theme_icon(dl, 0, ImVec2(p.x + h * 0.55f, p.y + h * 0.5f), h * 0.36f, rgb(255, 255, 255));
        ImGui::PushFont(title);
        ImVec2 ts = ImGui::CalcTextSize(tr("app.title"));
        dl->AddText(ImVec2(p.x + h * 1.1f, p.y + (h - ts.y) * 0.5f), rgb(255, 255, 255), tr("app.title"));
        ImGui::PopFont();
        std::string resume_text = touch_only() ? std::string(tr("common.resume"))
                                               : std::string(tr("common.resume")) + " (" + nav_names().back + ")";
        float bw = std::max(130.0f * scale, ImGui::CalcTextSize(resume_text.c_str()).x + 36 * scale);
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - 10 * scale, p.y + (h - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(255, 255, 255, 230));
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
        if (ImGui::Button(resume_text.c_str(), ImVec2(bw, 0))) open = false;
        ImGui::PopStyleColor();
        action_nav_item(kResumeFocus);
        if (ImGui::IsItemFocused()) {
            dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), rgb(246, 170, 40), ImGui::GetStyle().FrameRounding, 0, 2.5f * scale);
        }
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 14 * scale));
    }
    // Gamepad shoulder buttons (L/R/ZL/ZR) & PageUp/PageDown to switch tabs from anywhere
    if (g_capturing < 0 && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        const int previous_page = g_page;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadL2, false) ||
            ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) {
            g_page = (g_page - 1 + kPageCount) % kPageCount;
        } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false) ||
                   ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) {
            g_page = (g_page + 1) % kPageCount;
        }
        if (g_page != previous_page && g_nav_sidebar) {
            g_sidebar_index = g_page;
            g_sidebar_focus = true;
        }
    }
    // Page buttons: a column on the left, or a row on top when narrow. The footer keeps its line below both.
    float footer = ImGui::GetFrameHeightWithSpacing() + 10.0f * scale;
    float tab_w = portrait ? ImGui::GetContentRegionAvail().x : 210.0f * scale;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, portrait ? 0.0f : 8.0f * scale));
    ImGui::BeginChild("##tabs", ImVec2(tab_w, portrait ? 64.0f * scale : -footer), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NavFlattened | (portrait ? ImGuiWindowFlags_HorizontalScrollbar : 0));
    ImGui::PopStyleVar();

    std::string glyph_style = config_get("controls.glyphs", "auto");
    std::string kind = plat::controller_kind();
    std::string l_label = "L / LB", r_label = "R / RB";
    if (glyph_style == "playstation" || (glyph_style == "auto" && kind == "playstation")) {
        l_label = "L1";
        r_label = "R1";
    } else if (glyph_style == "xbox" || (glyph_style == "auto" && kind == "xbox")) {
        l_label = "LB";
        r_label = "RB";
    } else if (glyph_style == "switch" || (glyph_style == "auto" && kind == "switch")) {
        l_label = "L";
        r_label = "R";
    }
    if (kind.empty()) l_label = "PgUp", r_label = "PgDn";  // no controller: the keys that change the tab
    // The arrows are drawn (bumper_label): the UI font has no ◀ ▶, which showed as "?".
    const std::string& l_text = l_label;
    const std::string& r_text = r_label;

    bool l_down = ImGui::IsKeyDown(ImGuiKey_GamepadL1) || ImGui::IsKeyDown(ImGuiKey_GamepadL2) || ImGui::IsKeyDown(ImGuiKey_PageUp);
    bool r_down = ImGui::IsKeyDown(ImGuiKey_GamepadR1) || ImGui::IsKeyDown(ImGuiKey_GamepadR2) || ImGui::IsKeyDown(ImGuiKey_PageDown);

    if (touch_only()) {
        // No page buttons: the pages below are touched directly.
    } else if (!portrait) {
        float avail_w = ImGui::GetContentRegionAvail().x;
        float gap = 8.0f * scale;
        float bw = (avail_w - gap) * 0.5f;
        float bh = 28.0f * scale;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImVec2 p_l = ImGui::GetCursorScreenPos();
        ImGui::PushID("bumper_l");
        if (ImGui::InvisibleButton("##l", ImVec2(bw, bh))) g_page = (g_page - 1 + kPageCount) % kPageCount;
        bool l_hov = ImGui::IsItemHovered();
        bool l_active = l_down || l_hov;
        ImU32 l_bg = l_active ? rgb(246, 170, 40) : rgb(242, 233, 210);
        ImU32 l_border = l_active ? rgb(230, 140, 20) : rgb(216, 202, 174);
        ImU32 l_text_col = l_active ? rgb(255, 255, 255) : rgb(107, 79, 54);
        dl->AddRectFilled(p_l, ImVec2(p_l.x + bw, p_l.y + bh), l_bg, bh * 0.5f);
        dl->AddRect(p_l, ImVec2(p_l.x + bw, p_l.y + bh), l_border, bh * 0.5f, 0, 1.5f * scale);
        bumper_label(dl, p_l, ImVec2(bw, bh), l_text, true, l_text_col);
        ImGui::PopID();

        ImGui::SameLine(0, gap);

        ImVec2 p_r = ImGui::GetCursorScreenPos();
        ImGui::PushID("bumper_r");
        if (ImGui::InvisibleButton("##r", ImVec2(bw, bh))) g_page = (g_page + 1) % kPageCount;
        bool r_hov = ImGui::IsItemHovered();
        bool r_active = r_down || r_hov;
        ImU32 r_bg = r_active ? rgb(246, 170, 40) : rgb(242, 233, 210);
        ImU32 r_border = r_active ? rgb(230, 140, 20) : rgb(216, 202, 174);
        ImU32 r_text_col = r_active ? rgb(255, 255, 255) : rgb(107, 79, 54);
        dl->AddRectFilled(p_r, ImVec2(p_r.x + bw, p_r.y + bh), r_bg, bh * 0.5f);
        dl->AddRect(p_r, ImVec2(p_r.x + bw, p_r.y + bh), r_border, bh * 0.5f, 0, 1.5f * scale);
        bumper_label(dl, p_r, ImVec2(bw, bh), r_text, false, r_text_col);
        ImGui::PopID();

        ImGui::Dummy(ImVec2(0, 6.0f * scale));
    } else {
        float bh = 52.0f * scale;
        float bw = 64.0f * scale;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImVec2 p_l = ImGui::GetCursorScreenPos();
        ImGui::PushID("bumper_l");
        if (ImGui::InvisibleButton("##l", ImVec2(bw, bh))) g_page = (g_page - 1 + kPageCount) % kPageCount;
        bool l_hov = ImGui::IsItemHovered();
        bool l_active = l_down || l_hov;
        ImU32 l_bg = l_active ? rgb(246, 170, 40) : rgb(242, 233, 210);
        ImU32 l_border = l_active ? rgb(230, 140, 20) : rgb(216, 202, 174);
        ImU32 l_text_col = l_active ? rgb(255, 255, 255) : rgb(107, 79, 54);
        dl->AddRectFilled(p_l, ImVec2(p_l.x + bw, p_l.y + bh), l_bg, bh * 0.5f);
        dl->AddRect(p_l, ImVec2(p_l.x + bw, p_l.y + bh), l_border, bh * 0.5f, 0, 1.5f * scale);
        bumper_label(dl, p_l, ImVec2(bw, bh), l_text, true, l_text_col);
        ImGui::PopID();
        ImGui::SameLine();
    }

    for (int k = 0; k < kPageCount; k++) {
        if (portrait && k) ImGui::SameLine();
        ImGui::PushID(k);
        const char* label = tr(kPages[k].key);
        float bh = 52.0f * scale;
        float bw = portrait ? std::max(140.0f * scale, ImGui::CalcTextSize(label).x + bh + 24 * scale) : -1;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
        bool activate = ImGui::InvisibleButton("##p", ImVec2(bw < 0 ? ImGui::GetContentRegionAvail().x : bw, bh), ImGuiButtonFlags_EnableNav);
        ImGui::PopStyleColor();
        if (g_sidebar_focus && g_sidebar_index == k) {
            ImGui::ClearActiveID();
            ImGui::FocusWindow(ImGui::GetCurrentWindow());
            ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
            ImGui::SetNavCursorVisible(true);
            ImGui::SetScrollHereY();
            g_sidebar_focus = false;
        }
        // Preserve the requested destination until its tab has received focus.
        if (ImGui::IsItemFocused() && !g_sidebar_focus) {
            g_nav_sidebar = true;
            g_sidebar_index = k;
        }
        // Confirming a tab enters its content, including when it is already selected.
        activate |= ImGui::IsItemFocused() && !g_sidebar_focus &&
                    ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
        if (activate) {
            ImGui::ClearActiveID();
            g_page = k;
            g_nav_sidebar = false;
            g_nav_index = 0;
            g_nav_target = 0;
        }
        bool sel = g_page == k;
        bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        ImVec2 q = ImGui::GetItemRectMax();
        ImDrawList* cl = ImGui::GetWindowDrawList();
        cl->AddRectFilled(p, q, sel ? kPages[k].color : (hov ? rgb(255, 232, 160) : rgb(248, 239, 214)), bh * 0.5f);
        if (hov) cl->AddRect(p, q, rgb(246, 170, 40), bh * 0.5f, 0, 3.0f * scale);
        cl->AddCircleFilled(ImVec2(p.x + bh * 0.5f, p.y + bh * 0.5f), bh * 0.36f,
                            sel ? rgb(255, 255, 255, 70) : kPages[k].color);
        theme_icon(cl, kPages[k].icon, ImVec2(p.x + bh * 0.5f, p.y + bh * 0.5f), bh * 0.3f, rgb(255, 255, 255));
        ImVec2 ts = ImGui::CalcTextSize(label);
        cl->AddText(ImVec2(p.x + bh + 6 * scale, p.y + (bh - ts.y) * 0.5f), sel ? rgb(255, 255, 255) : rgb(107, 79, 54),
                    label);
        ImGui::PopID();
    }

    if (portrait) {
        ImGui::SameLine();
        float bh = 52.0f * scale;
        float bw = 64.0f * scale;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImVec2 p_r = ImGui::GetCursorScreenPos();
        ImGui::PushID("bumper_r");
        if (ImGui::InvisibleButton("##r", ImVec2(bw, bh))) g_page = (g_page + 1) % kPageCount;
        bool r_hov = ImGui::IsItemHovered();
        bool r_active = r_down || r_hov;
        ImU32 r_bg = r_active ? rgb(246, 170, 40) : rgb(242, 233, 210);
        ImU32 r_border = r_active ? rgb(230, 140, 20) : rgb(216, 202, 174);
        ImU32 r_text_col = r_active ? rgb(255, 255, 255) : rgb(107, 79, 54);
        dl->AddRectFilled(p_r, ImVec2(p_r.x + bw, p_r.y + bh), r_bg, bh * 0.5f);
        dl->AddRect(p_r, ImVec2(p_r.x + bw, p_r.y + bh), r_border, bh * 0.5f, 0, 1.5f * scale);
        bumper_label(dl, p_r, ImVec2(bw, bh), r_text, false, r_text_col);
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (!portrait) ImGui::SameLine();
    // The page.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * scale, 12.0f * scale));
    ImGui::BeginChild("##page", ImVec2(0, -footer), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NavFlattened);
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, 4 * scale));
    if (last_page != g_page) {
        g_nav_target = g_nav_sidebar ? -1 : 0;
        g_nav_index = 0;
        ImGui::SetScrollY(0);
        last_page = g_page;
    }
    g_nav_count = 0;
    g_nav_positions.clear();
    g_page_navigation = true;
    switch (g_page) {
        case 0: page_game(); break;
        case 1: page_video(); break;
        case 2: page_audio(); break;
        case 3: page_controls(); break;
        case 4: page_saves(); break;
        case 5: page_online(); break;
        case 6: settings_page_amiibo(g_scale); break;
        case 7: page_discord(); break;
        default: page_about(); break;
    }
    g_page_navigation = false;
    // Tests: NX_UI_SCROLL=<0..1> scrolls the page that far down (headless dumps show the rows further down).
    if (static const char* sc = std::getenv("NX_UI_SCROLL"); sc && ImGui::GetScrollMaxY() > 0)
        ImGui::SetScrollY(float(std::atof(sc)) * ImGui::GetScrollMaxY());
    drag_scroll();
    ImGui::EndChild();
    // Footer: restart note or toast, controller prompt bar, and quit.
    {
        const char* msg = nullptr;
        if (ImGui::GetTime() < g_toast_until)
            msg = g_toast.c_str();
        else if (g_restart_note)
            msg = tr("common.restart_needed");
        if (msg) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImVec4(0.30f, 0.55f, 0.45f, 1), "%s", msg);
            ImGui::SameLine();
#if defined(__ANDROID__)
            // A phone app is not quit and started again by hand easily: the game starts again from here.
            if (g_restart_note && msg == tr("common.restart_needed")) {
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(124, 194, 103, 255));
                if (ImGui::Button(tr("common.restart_now"))) {
                    config_save();
                    plat::restart_app();
                }
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }
#endif
        } else {
            ImGui::AlignTextToFramePadding();
            if (!touch_only()) {
                ImGui::TextColored(ImVec4(0.50f, 0.45f, 0.38f, 1.0f), "%s", nav_hint(nav_names()).c_str());
                ImGui::SameLine();
            }
        }
        float qw = ImGui::CalcTextSize(tr("common.quit_game")).x + 30 * scale;
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - qw);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(232, 115, 122, 255));
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
        if (ImGui::Button(tr("common.quit_game"), ImVec2(qw, 0))) g_quit_confirm = true;
        ImGui::PopStyleColor();
        action_nav_item(kQuitFocus);
        if (ImGui::IsItemFocused()) {
            dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), rgb(246, 170, 40), ImGui::GetStyle().FrameRounding, 0, 2.5f * scale);
        }
        ImGui::PopStyleColor();
    }
    if (g_quit_confirm) ImGui::OpenPopup("quit");
    if (g_quit_confirm) {
        const float width = std::max(1.0f, std::min(400.0f * scale, ds.x - 32.0f * scale));
        ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, ds.y));
    }
    if (ImGui::BeginPopupModal("quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextWrapped("%s", tr("common.quit_confirm"));
        ImGui::Dummy(ImVec2(0, 8.0f * scale));
        const float yes_w = std::max(90.0f * scale, ImGui::CalcTextSize(tr("common.yes")).x + 24.0f * scale);
        const float no_w = std::max(90.0f * scale, ImGui::CalcTextSize(tr("common.no")).x + 24.0f * scale);
        const float gap = 12.0f * scale;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - yes_w - no_w - gap) * 0.5f));
        if (ImGui::Button(tr("common.yes"), ImVec2(yes_w, 0))) {
            config_save();
            plat::exit_process(0);
        }
        ImGui::SameLine(0, gap);
        bool cancel = ImGui::Button(tr("common.no"), ImVec2(no_w, 0));
        ImGui::SetItemDefaultFocus();
        if (cancel || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            g_quit_confirm = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (g_must_quit) ImGui::OpenPopup("must_quit");
    if (ImGui::BeginPopupModal("must_quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextWrapped("%s", tr("saves.import.ok"));
        if (ImGui::Button(tr("common.quit_game"))) plat::exit_process(0);
        ImGui::EndPopup();
    }
    // Cancel (B / Escape) closes the screen when nothing else wants it (never after an import: see g_must_quit).
    if (g_must_quit) open = true;
    if (g_capturing < 0 && !cancel_capture && !popup && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
         (!back_to_sidebar && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))))
        open = false;
    if (g_must_quit) open = true;
    ImGui::End();
    return open;
}

void settings_hint(float scale, float t) {
    // A pill at the bottom centre (the game's own loading mark is at the bottom right): it rises into place and
    // fades in, stays a while, and leaves faster than it came.
    constexpr float kIn = 0.45f, kStay = 7.0f, kOut = 0.25f;
    if (t < 0 || t > kIn + kStay + kOut) return;
    auto out_quart = [](float x) { return 1 - std::pow(1 - std::clamp(x, 0.0f, 1.0f), 4.0f); };
    float a, dy;
    if (t < kIn + kStay) {
        float e = out_quart(t / kIn);
        a = e, dy = (1 - e) * 12.0f;
    } else {
        float e = out_quart((t - kIn - kStay) / kOut);
        a = 1 - e, dy = e * 6.0f;
    }
    auto fade = [a](int r, int g, int b, int alpha) { return rgb(r, g, b, int(float(alpha) * a)); };
    ImGuiIO& io = ImGui::GetIO();
    const char* text = tr("app.hint_open");
    ImVec2 ts = ImGui::CalcTextSize(text);
    const float h = ts.y + 20.0f * scale, ir = h * 0.5f - 5.0f * scale, gap = 10.0f * scale, pad = 18.0f * scale;
    const float w = 5.0f * scale + ir * 2 + gap + ts.x + pad;
    ImVec2 p(std::floor((io.DisplaySize.x - w) * 0.5f), io.DisplaySize.y - h - 28.0f * scale + dy * scale);
    ImVec2 q(p.x + w, p.y + h);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (int k = 3; k >= 1; k--) {  // a soft shadow below, made of widening layers
        float s = float(k) * 2.0f * scale;
        dl->AddRectFilled(ImVec2(p.x - s * 0.5f, p.y + s), ImVec2(q.x + s * 0.5f, q.y + s * 1.2f), fade(40, 30, 20, 18),
                          h * 0.5f + s);
    }
    dl->AddRectFilled(p, q, fade(255, 249, 232, 245), h * 0.5f);
    dl->AddRect(p, q, fade(255, 255, 255, 160), h * 0.5f, 0, 1.0f * scale);
    ImVec2 c(p.x + 5.0f * scale + ir, p.y + h * 0.5f);
    dl->AddCircleFilled(c, ir, fade(118, 200, 170, 255));
    theme_icon(dl, kIconGear, c, ir * 0.62f, fade(255, 255, 255, 255));
    dl->AddText(ImVec2(c.x + ir + gap, p.y + (h - ts.y) * 0.5f), fade(107, 79, 54, 255), text);
}

void hdr_panel_draw(float scale) {
    // A small see-through window in the top left corner with the Picture page's HDR settings (read live by the
    // present pass and the tonemap), so the picture stays in view while they move. The game keeps its input.
    g_scale = scale;
    ImGui::SetNextWindowPos(ImVec2(16.0f * scale, 16.0f * scale), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav;
    if (!ImGui::Begin("##hdr_panel", nullptr, flags)) {
        ImGui::End();
        return;
    }
    const float label_w = 150.0f * scale, w = 220.0f * scale;
    auto line = [&](const char* id, const char* label, const std::function<void()>& control) {
        ImGui::PushID(id);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(label_w);
        control();
        ImGui::PopID();
    };
    ImGui::TextUnformatted("HDR (F2)");
    if (config_get("video.hdr", "off") == "off" && !std::getenv("NX_HDR")) {
        ImGui::TextUnformatted(tr("video.hdr.off"));
        ImGui::End();
        return;
    }
    line("tm", tr("video.hdr_tonemap"), [&] {
        combo("video.hdr_tonemap", "hdr", {{"hdr", tr("video.hdr_tonemap.hdr")}, {"renodrt", tr("video.hdr_tonemap.renodrt")}, {"vanilla", tr("video.hdr_tonemap.vanilla")}}, w);
    });
    line("peak", tr("video.hdr_peak"), [&] { slider("video.hdr_peak_nits", 400, 4000, 1000, "%d nits", w); });
    line("game", tr("video.hdr_game"), [&] { slider("video.hdr_game_nits", 80, 500, 203, "%d nits", w); });
    line("ui", tr("video.hdr_ui"), [&] { slider("video.hdr_ui_nits", 80, 500, 203, "%d nits", w); });
    line("gamma", tr("video.hdr_gamma"), [&] { combo("video.hdr_gamma", "2.2", {{"2.2", "2.2"}, {"srgb", "sRGB"}}, w); });
    line("exp", tr("video.hdr_exposure"), [&] {
        float v = std::clamp(config_get_float("video.hdr_exposure", 0), -2.0f, 2.0f);
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat("##e", &v, -2.0f, 2.0f, "%+.2f stops")) {
            char b[16];
            std::snprintf(b, sizeof b, "%.2f", double(v));
            config_set("video.hdr_exposure", b);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) config_save();
    });
    ImGui::Separator();
    line("hi", tr("video.hdr_highlights"), [&] { slider("video.hdr_highlights", 0, 100, 50, "%d", w); });
    line("sh", tr("video.hdr_shadows"), [&] { slider("video.hdr_shadows", 0, 100, 50, "%d", w); });
    line("co", tr("video.hdr_contrast"), [&] { slider("video.hdr_contrast", 0, 100, 50, "%d", w); });
    line("sa", tr("video.hdr_saturation"), [&] { slider("video.hdr_saturation", 0, 100, 50, "%d", w); });
    line("te", tr("video.hdr_temperature"), [&] { slider("video.hdr_temperature", -100, 100, 0, "%d", w); });
    line("ti", tr("video.hdr_tint"), [&] { slider("video.hdr_tint", -100, 100, 0, "%d", w); });
    ImGui::Separator();
    // The defaults: the game's own picture (tonemap HDR, scene and UI at 203 nits, gamma 2.2, no grading). The
    // display's peak stays: it describes the display, not the look.
    if (ImGui::Button(tr("video.hdr_reset"))) {
        static const std::pair<const char*, const char*> kDefaults[] = {
            {"video.hdr_tonemap", "hdr"},     {"video.hdr_game_nits", "203"},   {"video.hdr_ui_nits", "203"},
            {"video.hdr_gamma", "2.2"},       {"video.hdr_exposure", "0.00"},   {"video.hdr_highlights", "50"},
            {"video.hdr_shadows", "50"},      {"video.hdr_contrast", "50"},     {"video.hdr_saturation", "50"},
            {"video.hdr_temperature", "0"},   {"video.hdr_tint", "0"},
        };
        for (const auto& [key, value] : kDefaults) config_set(key, value);
        config_save();
    }
    ImGui::End();
}

}  // namespace nx::ui
