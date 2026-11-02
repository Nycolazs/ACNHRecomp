// The settings screen's pages: game, video, audio, controls, saves, about. Every choice is written to config.ini at
// once (config.h); the ones the runtime can change live are applied here, the others say they need a restart.
//
// Layout: a cream card over the dimmed game, the pages as round coloured buttons down its left side (along its top
// when the screen is narrow or portrait), each setting a row with its name and a short explanation on the left and
// the control on the right (under them when narrow). Sizes are in design pixels times `scale` (ui.cpp).
#include "../hle_ldn.h"
#include "ui/settings.h"

#if !defined(__ANDROID__)
#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
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
#include "platform.h"
#include "saves.h"
#include "ui/i18n.h"
#include "ui/theme.h"
#include "ui/ui_internal.h"

namespace nx::nvn {
float uw_factor();      // nvn_ultrawide.cpp: the picture's aspect over 16:9
float uw_resolution();  // and its rows over the game's
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
    {"tab.controls", rgb(155, 125, 212), 3}, {"tab.saves", rgb(232, 115, 122), 4}, {"tab.online", rgb(72, 190, 170), kIconBox}, {"tab.about", rgb(176, 140, 100), 5},
};
constexpr int kPageCount = int(sizeof(kPages) / sizeof(kPages[0]));

int g_page = 0;
constexpr int kOnlinePage = 5;
std::atomic<bool> g_show_online{false};  // the game asked for local play with the setting off (hle_ldn.cpp)
float g_scale = 1;
std::string g_toast;
double g_toast_until = 0;
bool g_restart_note = false;
void drag_scroll(bool horizontal = false);  // below: a finger dragged over a list scrolls it

void toast(const std::string& text) {
    g_toast = text;
    g_toast_until = ImGui::GetTime() + 4.0;
}

// ---- settings helpers -----------------------------------------------------------------------------------------

// Whether a setting waits for the game to start again. On phones the picture's upscaling, sharpness, anti-aliasing and
// frame generation are read while the game runs (nvn_present.cpp, nvn_framegen.cpp); elsewhere they are not (the
// desktop's present pass is chosen at start).
bool needs_restart(const std::string& key) {
    if (key.rfind("online.", 0) == 0) return false;  // read when the game next starts local play (hle_ldn.cpp)
#if defined(__ANDROID__)
    for (const char* k : {"video.upscaler", "video.fsr_sharpness_pct", "video.antialiasing", "video.frame_gen"})
        if (key == k) return false;
#endif
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
    if (needs_restart(key)) g_restart_note = true;
}

void store(const std::string& key, const std::string& value) {
    if (config_get(key) == value) return;
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
    ImGui::TextUnformatted(tr(label));
    if (desc && *desc) {
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
        ImGui::SameLine(avail - ctrl_w + ImGui::GetStyle().WindowPadding.x * 0.0f);
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
    if (ImGui::BeginCombo("##c", shown.c_str())) {
        for (auto& [v, t] : choices) {
            bool sel = v == cur;
            if (ImGui::Selectable(t.c_str(), sel)) store(key, v);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        drag_scroll();  // a long list (the resolutions) scrolls under the finger too
        ImGui::EndCombo();
    }
    if (full != shown && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", full.c_str());
    bool focused = ImGui::IsItemFocused();
    if (focused) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 rmin = ImGui::GetItemRectMin();
        ImVec2 rmax = ImGui::GetItemRectMax();
        dl->AddRect(rmin, rmax, rgb(246, 170, 40), 6.0f * g_scale, 0, 2.5f * g_scale);
        if (cur_idx >= 0 && choices.size() > 1) {
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
                int next_idx = (cur_idx - 1 + int(choices.size())) % int(choices.size());
                store(key, choices[next_idx].first);
            } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
                int next_idx = (cur_idx + 1) % int(choices.size());
                store(key, choices[next_idx].first);
            }
        }
    }
}

// An on/off pill like the game's.
void toggle(const std::string& key, bool fallback, float width = 0) {
    g_row_key = key;
    bool on = config_get_bool(key, fallback);
    float h = ImGui::GetFrameHeight(), pill_w = h * 1.9f;
    const char* label = tr(on ? "common.on" : "common.off");
    ImVec2 ts = ImGui::CalcTextSize(label);
    float natural_w = pill_w + 10.0f * g_scale + ts.x;
    float hit_w = width > 0 ? width : natural_w;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##t", ImVec2(hit_w, h), ImGuiButtonFlags_EnableNav);
    bool focused = ImGui::IsItemFocused();
    bool hov = ImGui::IsItemHovered();
    bool was_on = on;
    bool changed = false;

    if (clicked) {
        on = !on;
        changed = true;
    } else if (focused) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
            if (on) {
                on = false;
                changed = true;
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
            if (!on) {
                on = true;
                changed = true;
            }
        }
    }

    if (changed) {
        store(key, on ? "1" : "0");
        if (key == "input.rumble" && !was_on && on) {
            plat::rumble(0.45f, 0.70f, 180);
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = on ? rgb(118, 200, 170) : rgb(214, 200, 168);
    dl->AddRectFilled(p, ImVec2(p.x + pill_w, p.y + h), bg, h * 0.5f);
    if (hov || focused) {
        dl->AddRect(p, ImVec2(p.x + pill_w, p.y + h), rgb(246, 170, 40), h * 0.5f, 0, 2.5f * g_scale);
        if (focused) {
            dl->AddRect(ImVec2(p.x - 2.0f * g_scale, p.y - 2.0f * g_scale),
                        ImVec2(p.x + hit_w + 2.0f * g_scale, p.y + h + 2.0f * g_scale),
                        rgb(246, 170, 40), 6.0f * g_scale, 0, 2.0f * g_scale);
        }
    }
    float r = h * 0.5f - 3.0f * g_scale;
    ImVec2 knob(on ? p.x + pill_w - h * 0.5f : p.x + h * 0.5f, p.y + h * 0.5f);
    dl->AddCircleFilled(knob, r, rgb(255, 255, 255));
    dl->AddText(ImVec2(p.x + pill_w + 10.0f * g_scale, p.y + (h - ts.y) * 0.5f), rgb(107, 79, 54), label);
}

bool g_slider_held = false;  // a slider under the finger this frame: it takes the drag, the page does not scroll

void slider(const std::string& key, int lo, int hi, int fallback, const char* fmt, float width) {
    g_row_key = key;
    int v = config_get_int(key, fallback);
    ImGui::SetNextItemWidth(width);
    const bool changed = ImGui::SliderInt("##s", &v, lo, hi, fmt);
    g_slider_held |= ImGui::IsItemActive();
    if (changed) {
        config_set(key, std::to_string(v));
        apply_live(key, std::to_string(v));
    }
    if (ImGui::IsItemFocused()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 rmin = ImGui::GetItemRectMin();
        ImVec2 rmax = ImGui::GetItemRectMax();
        dl->AddRect(rmin, rmax, rgb(246, 170, 40), 6.0f * g_scale, 0, 2.5f * g_scale);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) config_save();
}

bool button(const char* text, float width = 0) {
    bool res = ImGui::Button(text, ImVec2(width, 0));
    if (ImGui::IsItemFocused()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 rmin = ImGui::GetItemRectMin();
        ImVec2 rmax = ImGui::GetItemRectMax();
        dl->AddRect(rmin, rmax, rgb(246, 170, 40), 6.0f * g_scale, 0, 2.5f * g_scale);
    }
    return res;
}

// ---- pages ----------------------------------------------------------------------------------------------------

bool ptbr_installed() {
    std::error_code ec;
    return fs::is_directory(fs::u8path(plat::data_root() + "/mods/ptbr/romfs"), ec);
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

// Direct play over the internet: the switch, what the router allows, the address to share, a friend's address.
void page_online() {
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
        if (!s.hosting && !busy && ImGui::Button(tr("online.test"), ImVec2(w, 0))) link_check();
    });
    if (!s.address.empty()) {
        static bool shown = false;
        row("online.address", tr("online.address.desc"), [&](float w) {
            ImGui::TextUnformatted(shown ? s.address.c_str() : "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2");
            const float half = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button(tr(shown ? "online.hide" : "online.show"), ImVec2(half, 0))) shown = !shown;
            ImGui::SameLine();
            if (ImGui::Button(tr("online.copy"), ImVec2(half, 0))) {
                ImGui::SetClipboardText(s.address.c_str());
                toast(tr("online.copied"));
            }
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
        if (ImGui::InputText("##online_join", join, sizeof join)) store("online.join", join);
        if (ImGui::IsItemDeactivatedAfterEdit()) address_check_start(join);
        if (join[0] && ImGui::Button(tr("online.join.test"), ImVec2(w, 0))) address_check_start(join);
    });
    if (s.players > 0) ImGui::TextUnformatted(trf("online.players", std::to_string(s.players)).c_str());
}

void page_video() {
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
    row("video.show_fps", "video.show_fps.desc", [&](float w) { toggle("video.show_fps", config_show_fps(), w); });

    section("video.section.picture");
    // The screen's own first: the picture drawn at the window's rows and in its shape.
    Choices resolutions = {{"window", tr("video.resolution.window")}, {"native", console("video.resolution.native")}};
#if defined(__ANDROID__)
    resolutions.insert(resolutions.begin(), {"auto", tr("video.resolution.auto")});
#endif
    for (const char* r : {"540", "720", "900", "1080", "1440"}) resolutions.push_back({r, std::string(r) + "p"});
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
    row("video.upscaler", nullptr, [&](float w) {
        combo("video.upscaler", plat::kDefaultUpscaler, {{"linear", tr("video.upscaler.linear")}, {"fsr", tr("video.upscaler.fsr")}}, w);
    });
    if (config_get("video.upscaler", plat::kDefaultUpscaler) == "fsr")
        row("video.sharpness", nullptr, [&](float w) { slider("video.fsr_sharpness_pct", 0, 100, 80, "%d%%", w); });
    row("video.aa", "video.aa.desc", [&](float w) {
        combo("video.antialiasing", "off",
              {{"off", console("video.aa.off")}, {"fxaa", tr("video.aa.fxaa")}, {"ssaa15", tr("video.aa.ssaa15")},
               {"ssaa2", tr("video.aa.ssaa2")}},
              w);
    });
    row("video.aniso", "video.aniso.desc", [&](float w) {
        combo("video.anisotropy", "game",
              {{"game", console("video.aniso.game")}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}}, w);
    });

    section("video.section.performance");
    row("video.preset", "video.preset.desc", [&](float w) {
        combo("video.preset", "custom",
              {{"custom", tr("video.preset.custom")}, {"quality", tr("video.preset.quality")},
               {"balanced", tr("video.preset.balanced")}, {"low", tr("video.preset.low")}},
              w);
    });
    row("video.framerate", "video.framerate.desc", [&](float w) {
        // No choice yet: 60 when the SpeedFix is in the game folder (config.cpp applies the same default).
        std::error_code ec;
        const bool sf = fs::is_directory(fs::u8path(plat::data_root() + "/mods/speedfix60/romfs"), ec);
        combo("video.frame_rate", sf ? "60" : "30",
              {{"30", console("video.framerate.30")}, {"60", tr("video.framerate.60")}}, w);
    });
    // Built-in (nvn_framegen.cpp) always; lsfg-vk when it is installed (or already chosen).
    bool lsfg = lsfg_available();
    const char* fg_default = "off";  // nvn_framegen.cpp
    std::string fg = config_get("video.frame_gen", fg_default);
    Choices fg_choices = {{"off", console("video.framegen.off")}, {"builtin", tr("video.framegen.builtin")}};
    if (lsfg || fg == "lsfg") fg_choices.push_back({"lsfg", tr("video.framegen.lsfg")});
    const char* fg_desc = fg == "lsfg" ? (lsfg ? "video.framegen.desc" : "video.framegen.missing") : "video.framegen.builtin.desc";
    row("video.framegen", fg_desc, [&](float w) { combo("video.frame_gen", fg_default, fg_choices, w); });
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
#endif
}

void page_audio() {
    row("audio.volume", nullptr, [&](float w) { slider("audio.volume", 0, 100, 100, "%d%%", w); });
    row("audio.reverb", "audio.reverb.desc", [&](float w) { toggle("audio.reverb", true, w); });
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
};
int g_capturing = -1;

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
#else
    ImGui::Dummy(ImVec2(0, 6 * g_scale));
    ImGui::PushFont(nullptr);
    ImGui::TextUnformatted(tr("controls.keyboard"));
    ImGui::PopFont();
    // A capture in progress: the next key pressed (ui.cpp) becomes the binding; Escape cancels.
    if (g_capturing >= 0) {
        int sc = ui_take_captured_key();
        if (sc == SDL_SCANCODE_ESCAPE) {
            g_capturing = -1;
        } else if (sc > 0) {
            store(std::string("input.key.") + kBindings[g_capturing].key, SDL_GetScancodeName(SDL_Scancode(sc)));
            g_capturing = -1;
        }
    }
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (int k = 0; k < int(sizeof(kBindings) / sizeof(kBindings[0])); k++) {
            const Binding& b = kBindings[k];
            ImGui::PushID(k);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", tr(b.action), arrow_for(b.key));
            ImGui::TableNextColumn();
            std::string cur = config_get(std::string("input.key.") + b.key, b.def);
            std::string text = g_capturing == k ? std::string(tr("controls.press_key")) : cur;
            if (ImGui::Button(text.c_str(), ImVec2(-1, 0))) {
                g_capturing = k;
                ui_begin_key_capture();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (button(tr("controls.reset_keys"))) {
        for (const Binding& b : kBindings) config_set(std::string("input.key.") + b.key, b.def);
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
// one): the screen then only offers to quit.
bool g_must_quit = false;

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
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 6 * g_scale));
    ImGui::TextUnformatted(tr("saves.backups"));
    for (const std::string& b : saves::list_backups()) {
        ImGui::PushID(b.c_str());
        ImGui::BulletText("%s", b.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("saves.restore"))) g_confirm_restore = b;
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
        if (button(tr("common.cancel"))) g_confirm_import.clear(), ImGui::CloseCurrentPopup();
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
        if (button(tr("common.cancel"))) g_confirm_restore.clear(), ImGui::CloseCurrentPopup();
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

void settings_show_online() { g_show_online = true; }

bool settings_draw(float scale, ImFont* title) {
    g_scale = scale;
    if (g_show_online.exchange(false)) g_page = kOnlinePage;
    static const bool cycle = std::getenv("NX_UI_CYCLE") != nullptr;  // tests: every page in turn, 10 s each
    if (cycle) g_page = int(double(plat::monotonic_ns()) / 1e10) % kPageCount;
    static const char* page = std::getenv("NX_UI_PAGE");  // tests: that page (0 game, 1 video, ...) when opened
    static bool paged = false;
    if (page && !paged) g_page = std::clamp(std::atoi(page), 0, kPageCount - 1), paged = true;
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 ds = io.DisplaySize;
    bool portrait = ds.x < ds.y * 1.2f;
    g_slider_held = false;  // set again by a slider still held (slider)
    // Dim the game.
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ds, rgb(40, 28, 12, 120));
    ImVec2 size = portrait ? ImVec2(ds.x * 0.96f, ds.y * 0.94f)
                           : ImVec2(std::min(ds.x * 0.92f, 1180.0f * scale), std::min(ds.y * 0.9f, 680.0f * scale));
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
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
        if (ImGui::Button(resume_text.c_str(), ImVec2(bw, 0))) open = false;
        if (ImGui::IsItemFocused()) {
            dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), rgb(246, 170, 40), ImGui::GetFrameHeight() * 0.5f, 0, 2.5f * scale);
        }
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 14 * scale));
    }
    // Gamepad shoulder buttons (L/R/ZL/ZR) & PageUp/PageDown to switch tabs from anywhere
    if (g_capturing < 0 && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadL2, false) ||
            ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) {
            g_page = (g_page - 1 + kPageCount) % kPageCount;
        } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false) ||
                   ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) {
            g_page = (g_page + 1) % kPageCount;
        }
    }
    // Page buttons: a column on the left, or a row on top when narrow. The footer keeps its line below both.
    float footer = ImGui::GetFrameHeightWithSpacing() + 10.0f * scale;
    float tab_w = portrait ? ImGui::GetContentRegionAvail().x : 210.0f * scale;
    ImGui::BeginChild("##tabs", ImVec2(tab_w, portrait ? 64.0f * scale : -footer), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NavFlattened | (portrait ? ImGuiWindowFlags_HorizontalScrollbar : 0));

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
        bool sel = g_page == k;
        if (ImGui::InvisibleButton("##p", ImVec2(bw < 0 ? ImGui::GetContentRegionAvail().x : bw, bh), ImGuiButtonFlags_EnableNav)) g_page = k;
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
    ImGui::BeginChild("##page", ImVec2(0, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_NavFlattened);
    ImGui::Dummy(ImVec2(0, 4 * scale));
    switch (g_page) {
        case 0: page_game(); break;
        case 1: page_video(); break;
        case 2: page_audio(); break;
        case 3: page_controls(); break;
        case 4: page_saves(); break;
        case 5: page_online(); break;
        default: page_about(); break;
    }
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
        if (ImGui::Button(tr("common.quit_game"), ImVec2(qw, 0))) g_quit_confirm = true;
        if (ImGui::IsItemFocused()) {
            dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), rgb(246, 170, 40), ImGui::GetFrameHeight() * 0.5f, 0, 2.5f * scale);
        }
        ImGui::PopStyleColor();
    }
    if (g_quit_confirm) ImGui::OpenPopup("quit");
    if (ImGui::BeginPopupModal("quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextWrapped("%s", tr("common.quit_confirm"));
        if (ImGui::Button(tr("common.yes"))) {
            config_save();
            plat::exit_process(0);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("common.no"))) g_quit_confirm = false, ImGui::CloseCurrentPopup();
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
    if (g_capturing < 0 && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
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

}  // namespace nx::ui
