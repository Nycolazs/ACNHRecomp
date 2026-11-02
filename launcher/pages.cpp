// The launcher's pages (launcher.h), in the settings screen's style (ui/theme.h).
//
//   welcome   two ways in: set up from the game files, or pick a build folder made before
//   files     step 1: a card each for the keys, base game, update (any version, optional), DLC and the pt-BR translation: Browse…,
//             Search (the usual folders, results to pick from) or a file dropped on it; each file checked when chosen
//   install   step 2: where the build goes, and what this computer has (with the command to install the rest)
//   build     step 3: tools/setup_acnh.py running: stages, progress, log, stop; the error state
//   done      step 4: Play
//   home      later starts, with a valid build remembered: Play, Settings, Rebuild, Change build folder
//   settings  a few of the game's config.ini settings (the rest is in the game, F1)
//
// Layout: a cream card over a mint background, a green ribbon with the title and the language at the top, the
// page under it and its buttons in a footer line. Sizes are design pixels times `scale` (main.cpp); below an aspect
// of 1.2 the two-column parts stack.
#include "launcher.h"
#include "ldn_upnp.h"

#include <thread>
#include <atomic>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>

#include "imgui.h"

#include "ui/i18n.h"
#include "ui/theme.h"

namespace launcher {

namespace {

using nx::ui::theme_icon;
using nx::ui::tr;
using nx::ui::trf;

ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
const ImU32 kInk = rgb(107, 79, 54), kInkSoft = rgb(150, 128, 100), kPaper = rgb(248, 239, 214);
const ImU32 kGreen = rgb(124, 194, 103), kMint = rgb(118, 200, 170), kYellow = rgb(255, 217, 102);
const ImU32 kOrange = rgb(245, 166, 35), kRed = rgb(232, 115, 122), kBlue = rgb(79, 179, 217);
// The project's Discord, where ACNHRecomp-Importer (which prepares the game folder) is downloaded
const char* const kDiscordUrl = "https://discord.gg/VJW9Dtdu7a";
const ImU32 kPurple = rgb(155, 125, 212), kBrown = rgb(176, 140, 100), kGrey = rgb(214, 200, 168);
const ImU32 kWhite = rgb(255, 255, 255);

ImVec4 col4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

enum class Page { Welcome, Files, Install, Build, Done, Home, Settings, Online };

Page g_page = Page::Welcome;
SDL_Window* g_window = nullptr;
float g_scale = 1;
ImFont* g_title = nullptr;
ImFont* g_body = nullptr;
bool g_portrait = false;
bool g_exit = false;

struct Item {
    std::string path;
    FileCheck check;
};
Item g_game, g_ptbr;
std::string g_install;
std::vector<Prereq> g_prereqs;
Build g_build;
const char* g_build_problem = "launcher.reason.none";  // "" when g_build is valid
std::string g_banner;  // an error under the page title (a refused file or folder)
std::string g_toast;
double g_toast_until = 0;

// The files page's cards; a file dialog's target is its card (kPickGame, kPickMod in the same order).
enum Slot { kSlotGame, kSlotMod, kSlotCount };
struct SlotState {
    bool open = false;         // the search's results shown
    std::vector<Found> found;  // its results
    ImVec2 min, max;           // the card on screen in the last frame, for drops
};
SlotState g_slot[kSlotCount];
bool g_dragging = false;  // files dragged over the window: the card under g_drag_pos lights up
ImVec2 g_drag_pos;
ImVec2 g_page_min, g_page_max;  // the visible part of the page (it scrolls)

// File dialogs answer on their own time (and maybe on another thread): queued here, applied in the frame.
enum Target { kPickGame, kPickMod, kPickInstall, kPickBuild };
struct Picked {
    int target;
    std::vector<std::string> paths;
};
std::mutex g_pick_lock;
std::vector<Picked> g_picked;

// Build page.
std::vector<std::string> g_log;
bool g_rebuild = false;  // started from home: no files page to go back to
// Opened with nothing set up: the game folder is asked for at once; once it is chosen the setup runs by itself and
// the game starts when it is done (the pages only show up when something is missing or fails).
bool g_express = false;
// Opened with a build: the game starts after a short countdown, which a click or a key stops (-2: from the first frame).
double g_autostart_at = -1;
void start_setup(bool rebuild);
bool prereqs_block();
void play();
bool g_confirm_stop = false, g_confirm_quit = false;
bool g_quit_after_stop = false;

// The game's config.ini (settings page).
Ini g_game_ini;

float S(float v) { return v * g_scale; }

void toast(const std::string& t) {
    g_toast = t;
    g_toast_until = ImGui::GetTime() + 4.0;
}

// ---- persistence ----------------------------------------------------------------------------------------------

void save_files() {
    Ini& s = settings();
    s.set("files.game", g_game.path);
    s.set("files.ptbr", g_ptbr.path);
    s.set("launcher.install", g_install);
    s.save();
}

void set_item(Item& it, Kind kind, const std::string& path) {
    it.path = path;
    it.check = check_file(kind, path);
}

// The checks again (their texts are translated).
void recheck_files() {
    if (!g_game.path.empty()) g_game.check = check_file(Kind::Game, g_game.path);
    if (!g_ptbr.path.empty()) g_ptbr.check = check_file(Kind::Mod, g_ptbr.path);
    for (SlotState& s : g_slot)
        for (Found& f : s.found) f.check = check_file(f.kind, f.path);
}

void set_build(const std::string& folder, bool remember) {
    g_build = Build();
    g_build_problem = find_build(folder, g_build);
    if (*g_build_problem == 0 && remember) {
        settings().set("launcher.build", folder);
        settings().save();
    }
}

void load_game_ini() { g_game_ini.load(g_build.data + "/config.ini"); }

// ---- dialogs --------------------------------------------------------------------------------------------------

void SDLCALL on_picked(void* user, const char* const* list, int) {
    Picked p{int(reinterpret_cast<intptr_t>(user)), {}};
    if (list)
        for (; *list; list++) p.paths.push_back(*list);
    std::lock_guard lk(g_pick_lock);
    g_picked.push_back(std::move(p));
}

void pick(Target t) {
    static const SDL_DialogFileFilter zip[] = {{"ZIP", "zip"}, {"*", "*"}};
    // The folder of the last file chosen, else Downloads.
    static std::string last;
    last = settings().get("files.last_dir");
    if (!is_dir(last))
        if (const char* d = SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS)) last = d;
    const char* where = is_dir(last) ? last.c_str() : nullptr;
    void* user = reinterpret_cast<void*>(intptr_t(t));
    if (const char* fake = std::getenv("NX_LAUNCHER_PICK"); fake && t == kPickGame) {  // tests: the dialog's answer
        const char* list[] = {fake, nullptr};
        on_picked(user, list, 0);
        return;
    }
    switch (t) {
        case kPickGame: SDL_ShowOpenFolderDialog(on_picked, user, g_window, where, false); break;
        case kPickMod: SDL_ShowOpenFileDialog(on_picked, user, g_window, zip, 2, where, false); break;
        case kPickInstall:
            SDL_ShowOpenFolderDialog(on_picked, user, g_window, is_dir(g_install) ? g_install.c_str() : nullptr, false);
            break;
        case kPickBuild: SDL_ShowOpenFolderDialog(on_picked, user, g_window, nullptr, false); break;
    }
}

// A game folder or package chosen for `kind`, by a dialog, a search result or a drop.
void use_file(Kind kind, const std::string& path) {
    set_item(kind == Kind::Game ? g_game : g_ptbr, kind, path);
    settings().set("files.last_dir", parent_dir(path));
    save_files();
}

// The express path (g_express): after the game folder, straight into the setup unless the folder or this computer
// needs something first; a pt-BR translation zip in Downloads is taken along.
void express_continue() {
    if (g_game.check.status == Status::Error || g_game.check.status == Status::Empty) {
        g_page = Page::Files;
        return;
    }
    if (g_ptbr.path.empty()) {
        std::vector<std::string> near;
        if (const char* d = SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS)) near.push_back(d);
        near.push_back(parent_dir(g_game.path));
        std::vector<Found> found = search_files(Kind::Mod, near);
        // Only when there is one usable (they come first): with several, the player picks on the files page.
        if (!found.empty() && found[0].check.status == Status::Ok &&
            (found.size() == 1 || found[1].check.status != Status::Ok))
            set_item(g_ptbr, Kind::Mod, found[0].path);
    }
    g_prereqs = check_prerequisites(g_install);
    if (prereqs_block()) {
        g_page = Page::Install;
        return;
    }
    save_files();
    start_setup(false);
}

// What a file or folder given to a card is: the card's kind (a wrong one then shows why).
Kind slot_kind(int slot, const std::string&) { return slot == kSlotGame ? Kind::Game : Kind::Mod; }

int slot_at(ImVec2 p) {
    if (g_page != Page::Files) return -1;
    for (int k = 0; k < kSlotCount; k++) {
        const SlotState& s = g_slot[k];
        if (p.x >= s.min.x && p.x < s.max.x && p.y >= s.min.y && p.y < s.max.y) return k;
    }
    return -1;
}

// Search: the game folders (or packages) in the usual folders and near what was chosen so far.
void search(int slot) {
    std::vector<std::string> near = {settings().get("files.last_dir"), g_game.path, g_ptbr.path};
    SlotState& s = g_slot[slot];
    s.found = search_files(slot == kSlotGame ? Kind::Game : Kind::Mod, near);
    s.open = true;
}

void use_build_folder(const std::string& folder) {
    Build b;
    const char* why = find_build(folder, b);
    if (*why) {
        // a release (the player beside) takes game folders: the reason alone says what is wrong with this one
        g_banner = player_beside().empty() ? trf("launcher.build_invalid", tr(why)) : std::string(tr(why));
        return;
    }
    g_banner.clear();
    set_build(folder, true);
    load_game_ini();
    g_page = Page::Home;
}

void apply_picks() {
    std::vector<Picked> picked;
    {
        std::lock_guard lk(g_pick_lock);
        picked.swap(g_picked);
    }
    for (const Picked& p : picked) {
        if (p.paths.empty()) continue;  // cancelled
        const std::string& first = p.paths[0];
        switch (p.target) {
            case kPickInstall: g_install = first, g_prereqs = check_prerequisites(g_install); break;
            case kPickBuild: use_build_folder(first); break;
            default:  // a card
                use_file(slot_kind(p.target, first), first);
                if (g_express && p.target == kPickGame) express_continue();
        }
        save_files();
    }
}

// ---- drawing helpers ------------------------------------------------------------------------------------------

float font_size(ImFont* f) { return f->FontSize * ImGui::GetIO().FontGlobalScale; }

ImVec2 measure(ImFont* f, float size, const char* text, float wrap) {
    return f->CalcTextSizeA(size, FLT_MAX, wrap, text);
}

// The background: mint to leaf green with a soft dot pattern, as the game's menus.
void draw_background(ImVec2 ds) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ds, rgb(160, 222, 200), rgb(160, 222, 200), rgb(196, 230, 160),
                                rgb(196, 230, 160));
    float step = S(46), r = S(5);
    int row = 0;
    for (float y = step * 0.5f; y < ds.y + step; y += step * 0.866f, row++)
        for (float x = (row & 1) ? step * 0.5f : 0; x < ds.x + step; x += step)
            dl->AddCircleFilled(ImVec2(x, y), r, rgb(255, 255, 255, 46));
}

ImU32 status_color(Status s) {
    switch (s) {
        case Status::Ok: return kGreen;
        case Status::Warning: return kOrange;
        case Status::Error: return kRed;
        default: return kGrey;
    }
}

// A small round status mark at centre c.
void status_mark(ImDrawList* dl, ImVec2 c, float r, Status s) {
    dl->AddCircleFilled(c, r, status_color(s));
    int icon = s == Status::Ok ? nx::ui::kIconCheck : s == Status::Error ? nx::ui::kIconCross
             : s == Status::Warning ? nx::ui::kIconWarning : -1;
    if (icon >= 0) theme_icon(dl, icon, c, r * 0.8f, kWhite);
}

float button_width(const char* label) { return std::max(S(120), ImGui::CalcTextSize(label).x + S(40)); }

bool button(const char* label, ImU32 color = kMint, bool enabled = true, float w = 0) {
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, col4(color));
    ImGui::PushStyleColor(ImGuiCol_Text, col4(color == kMint || color == kWhite ? kInk : kWhite));
    bool r = ImGui::Button(label, ImVec2(w > 0 ? w : button_width(label), 0));
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    return r;
}

// Puts the next `count` buttons at the right of the current line.
void align_right(std::initializer_list<const char*> labels) {
    float w = 0;
    for (const char* l : labels) w += button_width(l);
    w += ImGui::GetStyle().ItemSpacing.x * float(labels.size() - 1);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - w));
}

void text_small(const char* text, ImU32 color = kInkSoft) {
    ImGui::PushStyleColor(ImGuiCol_Text, col4(color));
    ImGui::SetWindowFontScale(0.84f);
    ImGui::TextWrapped("%s", text);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
}

void heading(const char* title, const char* text) {
    if (*title) {
        ImGui::PushFont(g_title);
        ImGui::TextWrapped("%s", title);
        ImGui::PopFont();
    }
    if (text && *text) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextWrapped("%s", text);
        ImGui::PopTextWrapPos();
    }
    if (!g_banner.empty()) {
        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(rgb(253, 226, 222)));
        ImGui::BeginChild("##banner", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushStyleColor(ImGuiCol_Text, col4(rgb(170, 70, 70)));
        ImGui::TextWrapped("%s", g_banner.c_str());
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, S(4)));
}

std::string clock_text(double seconds) {
    int s = int(std::max(0.0, seconds));
    char b[32];
    std::snprintf(b, sizeof b, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    return b;
}

// ---- the frame: ribbon and steps ------------------------------------------------------------------------------

std::string language_shown() {
    std::string cur = settings().get("launcher.language", "auto");
    std::string shown = tr("common.auto");
    for (const nx::ui::UiLanguage& l : nx::ui::ui_languages()) {
        if (cur == l.code) shown = l.native_name;
        if (cur == "auto" && nx::ui::ui_language() == l.code) shown += std::string(" · ") + l.native_name;
    }
    return shown;
}

void language_combo(float width) {
    std::string cur = settings().get("launcher.language", "auto");
    std::string shown = language_shown();
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col4(rgb(255, 255, 255, 235)));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col4(kYellow));
    ImGui::PushStyleColor(ImGuiCol_Button, col4(rgb(255, 255, 255, 235)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col4(kYellow));
    if (ImGui::BeginCombo("##lang", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
        auto choose = [&](const char* code, const char* name) {
            bool sel = cur == code;
            if (ImGui::Selectable(name, sel)) {
                settings().set("launcher.language", code);
                settings().save();
                nx::ui::ui_set_language(code);
                recheck_files();
                if (!g_prereqs.empty()) g_prereqs = check_prerequisites(g_install);
            }
            if (sel) ImGui::SetItemDefaultFocus();
        };
        choose("auto", tr("common.auto"));
        for (const nx::ui::UiLanguage& l : nx::ui::ui_languages()) choose(l.code, l.native_name);
        ImGui::EndCombo();
    }
    ImGui::PopStyleColor(4);
}

void ribbon() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = S(64), w = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kGreen, h * 0.5f);
    dl->AddCircleFilled(ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.36f, rgb(255, 255, 255, 60));
    theme_icon(dl, nx::ui::kIconLeaf, ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.3f, kWhite);
    // Measured in the combo's own font, with room for the frame padding and the arrow.
    float combo_w = measure(g_body, font_size(g_body), language_shown().c_str(), -1).x + ImGui::GetFrameHeight() +
                    ImGui::GetStyle().FramePadding.x * 2 + S(24);
    combo_w = std::clamp(combo_w, S(150), g_portrait ? w * 0.5f : S(420));
    float tsz = font_size(g_title);
    ImVec2 ts = measure(g_title, tsz, "ACNHRecomp", -1);
    float x = p.x + h * 1.0f;
    dl->AddText(g_title, tsz, ImVec2(x, p.y + (h - ts.y) * 0.5f), kWhite, "ACNHRecomp");
    const char* sub = tr("launcher.subtitle");
    float ssz = font_size(g_body) * 0.82f;
    ImVec2 ss = measure(g_body, ssz, sub, -1);
    float room = w - (x - p.x) - ts.x - combo_w - h * 0.9f;
    if (ss.x < room)
        dl->AddText(g_body, ssz, ImVec2(x + ts.x + S(16), p.y + (h - ss.y) * 0.5f + S(2)), rgb(255, 255, 255, 215), sub);
    // The language, always reachable.
    float gx = p.x + w - combo_w - h * 0.35f - S(30);
    theme_icon(dl, nx::ui::kIconGlobe, ImVec2(gx, p.y + h * 0.5f), S(14), kWhite);
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - combo_w - h * 0.25f, p.y + (h - ImGui::GetFrameHeight()) * 0.5f));
    language_combo(combo_w);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + S(12)));
}

// Step 1..4 of the setup, the current one highlighted.
void steps(int current) {
    const char* labels[] = {tr("launcher.step.files"), tr("launcher.step.install"), tr("launcher.step.build"),
                            tr("launcher.step.done")};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, r = S(15), h = S(40);
    float seg = w / 4;
    const float fs = font_size(g_body) * 0.85f;
    for (int k = 0; k < 4; k++) {
        float cx = p.x + seg * k + r + S(2), cy = p.y + h * 0.5f;
        bool label = !g_portrait || k == current;
        float lw = label ? measure(g_body, fs, labels[k], -1).x + S(16) : 0;
        float x0 = cx + r + S(6) + lw, x1 = p.x + seg * (k + 1) - S(6);
        if (k < 3 && x1 > x0)
            dl->AddRectFilled(ImVec2(x0, cy - S(1.5f)), ImVec2(x1, cy + S(1.5f)), k < current ? kGreen : kGrey, S(1.5f));
        ImU32 c = k < current ? kGreen : k == current ? kYellow : kGrey;
        dl->AddCircleFilled(ImVec2(cx, cy), r, c);
        if (k < current) {
            theme_icon(dl, nx::ui::kIconCheck, ImVec2(cx, cy), r * 0.75f, kWhite);
        } else {
            char n[4];
            std::snprintf(n, sizeof n, "%d", k + 1);
            ImVec2 ns = measure(g_body, fs, n, -1);
            dl->AddText(g_body, fs, ImVec2(cx - ns.x * 0.5f, cy - ns.y * 0.5f), k == current ? kInk : kWhite, n);
        }
        if (label) {
            ImVec2 ls = measure(g_body, fs, labels[k], -1);
            dl->AddText(g_body, fs, ImVec2(cx + r + S(8), cy - ls.y * 0.5f), k == current ? kInk : kInkSoft, labels[k]);
        }
    }
    ImGui::Dummy(ImVec2(w, h + S(4)));
}

// ---- welcome --------------------------------------------------------------------------------------------------

struct Card {
    int icon;
    ImU32 color;
    const char* title;
    const char* desc;
    const char* note;  // in orange under the text, or nullptr
    bool enabled;
};

float card_height(const Card& c, float w) {
    float pad = S(24), r = S(34), inner = w - 2 * pad;
    float hgt = pad + 2 * r + S(14) + measure(g_title, font_size(g_title) * 0.85f, c.title, inner).y + S(8) +
                measure(g_body, font_size(g_body), c.desc, inner).y + pad;
    if (c.note) hgt += S(10) + measure(g_body, font_size(g_body) * 0.85f, c.note, inner).y;
    return hgt;
}

bool card(const char* id, const Card& c, ImVec2 size) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!c.enabled);
    bool clicked = ImGui::InvisibleButton(id, size);
    ImGui::EndDisabled();
    bool hov = c.enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + size.x, p.y + size.y);
    dl->AddRectFilled(p, q, hov ? rgb(255, 240, 196) : kPaper, S(26));
    if (hov) dl->AddRect(p, q, rgb(246, 170, 40), S(26), 0, S(3));
    float pad = S(24), r = S(34), inner = size.x - 2 * pad;
    ImU32 icol = c.enabled ? c.color : kGrey;
    dl->AddCircleFilled(ImVec2(p.x + pad + r, p.y + pad + r), r, icol);
    theme_icon(dl, c.icon, ImVec2(p.x + pad + r, p.y + pad + r), r * 0.6f, kWhite);
    float y = p.y + pad + 2 * r + S(14);
    float ts = font_size(g_title) * 0.85f;
    dl->AddText(g_title, ts, ImVec2(p.x + pad, y), c.enabled ? kInk : kInkSoft, c.title, nullptr, inner);
    y += measure(g_title, ts, c.title, inner).y + S(8);
    dl->AddText(g_body, font_size(g_body), ImVec2(p.x + pad, y), kInkSoft, c.desc, nullptr, inner);
    y += measure(g_body, font_size(g_body), c.desc, inner).y + S(10);
    if (c.note) dl->AddText(g_body, font_size(g_body) * 0.85f, ImVec2(p.x + pad, y), rgb(200, 120, 20), c.note, nullptr, inner);
    // An arrow in a circle, bottom right: the card leads somewhere.
    if (c.enabled) {
        ImVec2 a(q.x - pad - S(16), p.y + pad + r);
        dl->AddCircleFilled(a, S(16), hov ? kYellow : rgb(240, 228, 196));
        theme_icon(dl, nx::ui::kIconPlay, a, S(11), kInk);
    }
    return clicked && c.enabled;
}

void go_files() {
    g_banner.clear();
    g_page = Page::Files;
}

// A release: the player is beside the launcher and the game comes ready in the folder ACNHRecomp-Importer prepared,
// so the one thing to do is to choose that folder.
void page_welcome_play() {
    heading(tr("launcher.welcome.title"), tr("launcher.welcome.text"));
    Card play{nx::ui::kIconLeaf, kGreen, tr("launcher.welcome.play"), tr("launcher.welcome.play.desc"), nullptr, true};
    float avail = ImGui::GetContentRegionAvail().x;
    if (card("##play", play, ImVec2(avail, card_height(play, avail)))) pick(kPickBuild);
    ImGui::Dummy(ImVec2(0, S(6)));
    text_small(tr("launcher.files.importer"));
    if (button(tr("launcher.files.discord"), kBlue)) SDL_OpenURL(kDiscordUrl);
    if (g_portrait) {  // in landscape the footer says it
        ImGui::Dummy(ImVec2(0, S(4)));
        text_small(tr("launcher.welcome.legal"));
    }
}

void page_welcome() {
    if (!player_beside().empty()) return page_welcome_play();
    heading(tr("launcher.welcome.title"), tr("launcher.welcome.text"));
    Card setup{nx::ui::kIconLeaf, kGreen, tr("launcher.welcome.setup"), tr("launcher.welcome.setup.desc"),
               setup_supported() ? nullptr : tr("launcher.welcome.windows"), setup_supported()};
    Card existing{nx::ui::kIconFolder, kBlue, tr("launcher.welcome.existing"), tr("launcher.welcome.existing.desc"),
                  nullptr, true};
    float avail = ImGui::GetContentRegionAvail().x, gap = S(20);
    if (g_portrait) {
        if (card("##setup", setup, ImVec2(avail, card_height(setup, avail)))) go_files();
        ImGui::Dummy(ImVec2(0, S(2)));
        if (card("##existing", existing, ImVec2(avail, card_height(existing, avail)))) pick(kPickBuild);
        ImGui::Dummy(ImVec2(0, S(4)));
        text_small(tr("launcher.welcome.legal"));
    } else {
        float w = (avail - gap) * 0.5f;
        float h = std::max(card_height(setup, w), card_height(existing, w));
        if (card("##setup", setup, ImVec2(w, h))) go_files();
        ImGui::SameLine(0, gap);
        if (card("##existing", existing, ImVec2(w, h))) pick(kPickBuild);
    }
}

void footer_welcome() {
    if (!settings().get("launcher.build").empty()) {
        if (button(tr("common.back"), kWhite)) g_banner.clear(), g_page = Page::Home;
        ImGui::SameLine();
    }
    if (g_portrait) return;  // the note is on the page
    ImGui::AlignTextToFramePadding();
    ImGui::SetWindowFontScale(0.84f);
    ImGui::TextColored(col4(kInkSoft), "%s", tr("launcher.welcome.legal"));
    ImGui::SetWindowFontScale(1.0f);
}

// ---- files ----------------------------------------------------------------------------------------------------

// The colour of a check's text.
ImU32 status_ink(Status s) {
    return s == Status::Ok ? rgb(70, 140, 60) : s == Status::Warning ? rgb(190, 120, 20) : rgb(190, 80, 80);
}

// A small rounded label after the last item (on its line when it fits in `room`, else under it).
void badge(const char* text, ImU32 bg, ImU32 fg, float line_h, float room) {
    float fs = font_size(g_body) * 0.78f;
    ImVec2 ts = measure(g_body, fs, text, -1);
    ImVec2 size(ts.x + S(18), ts.y + S(6));
    bool same = ImGui::GetItemRectSize().x + S(10) + size.x <= room;
    if (same) ImGui::SameLine(0, S(10));
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (same) p.y += (line_h - size.y) * 0.5f;
    ImGui::Dummy(ImVec2(size.x, same ? line_h : size.y));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, size.y * 0.5f);
    dl->AddText(g_body, fs, ImVec2(p.x + S(9), p.y + S(3)), fg, text);
}

// A small square button at the right of a row.
bool row_button(const char* label, float size) {
    ImGui::PushStyleColor(ImGuiCol_Button, col4(rgb(240, 228, 196)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    bool r = ImGui::Button(label, ImVec2(size, size));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    return r;
}

// One chosen file in a card, `w` wide: its check's mark and text, its name and folder, and × to clear it. Without
// a file, a hollow mark and `empty`.
bool file_row(const char* id, const Item& it, bool auto_found, const char* empty, bool required, float w) {
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x0 = ImGui::GetCursorPosX(), r = S(10), lh = ImGui::GetTextLineHeight();
    const bool has = !it.path.empty();
    if (has)
        status_mark(dl, ImVec2(p.x + r, p.y + lh * 0.5f), r, it.check.status);
    else
        dl->AddCircle(ImVec2(p.x + r, p.y + lh * 0.5f), r - S(1), required ? kBrown : kGrey, 0, S(2));
    const float bs = ImGui::GetFrameHeight() * 0.8f;
    ImGui::SetCursorPosX(x0 + 2 * r + S(10));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(x0 + w - (has ? bs + S(10) : 0));
    if (!has) {
        text_small(empty, required ? rgb(190, 120, 60) : kInkSoft);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, col4(status_ink(it.check.status)));
        ImGui::TextWrapped("%s", it.check.text.c_str());
        ImGui::PopStyleColor();
        std::string name = auto_found ? trf("launcher.status.found", file_name(it.path)) : file_name(it.path);
        text_small((name + "   ·   " + short_dir(it.path)).c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    if (has && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.path.c_str());
    bool cleared = false;
    if (has) {
        ImGui::SameLine(x0 + w - bs);
        cleared = row_button("×", bs);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("launcher.remove"));
    }
    ImGui::PopID();
    return cleared;
}

bool chosen(const std::string& path) { return path == g_game.path || path == g_ptbr.path; }

// The search's results under a card, `w` wide: name, size and folder, the check, and Use (Add for the DLC).
void found_list(int slot, float w) {
    SlotState& s = g_slot[slot];
    if (!s.open) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(rgb(240, 228, 196)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(10)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(14));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, S(290)));
    // A few results fit: no scrollbar (an auto-sized child can show one for its first frames).
    ImGui::BeginChild("##found", ImVec2(w, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      s.found.size() > 3 ? 0 : ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    const float iw = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
    const char* hide = tr("launcher.hide");
    const float hw = ImGui::CalcTextSize(hide).x + S(24);
    ImGui::PushTextWrapPos(x0 + iw - hw - S(10));
    ImGui::AlignTextToFramePadding();
    if (s.found.empty())
        ImGui::TextWrapped("%s", tr("launcher.search.none"));
    else
        ImGui::TextWrapped("%s", trf("launcher.search.found", std::to_string(s.found.size())).c_str());
    ImGui::PopTextWrapPos();
    ImGui::SameLine(x0 + iw - hw);
    if (button(hide, kWhite, true, hw)) s.open = false;
    const char* use = tr("launcher.use");
    const char* in_use = tr("launcher.in_use");
    const float uw = std::max(ImGui::CalcTextSize(use).x, ImGui::CalcTextSize(in_use).x) + S(28);
    for (size_t k = 0; k < s.found.size(); k++) {
        const Found& f = s.found[k];
        ImGui::PushID(int(k));
        ImGui::Separator();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = S(9), lh = ImGui::GetTextLineHeight();
        status_mark(dl, ImVec2(p.x + r, p.y + lh * 0.5f), r, f.check.status);
        ImGui::SetCursorPosX(x0 + 2 * r + S(10));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(x0 + iw - uw - S(10));
        ImGui::TextWrapped("%s", file_name(f.path).c_str());
        text_small((f.size ? human_size(f.size) + "   ·   " + short_dir(f.path) : short_dir(f.path)).c_str());
        text_small(f.check.text.c_str(), status_ink(f.check.status));
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.path.c_str());
        ImGui::SameLine(x0 + iw - uw);
        bool taken = chosen(f.path);
        if (button(taken ? in_use : use, taken ? kWhite : kMint, !taken, uw)) {
            use_file(f.kind, f.path);
            s.open = false;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

struct FileCard {
    const char* id;
    int slot;
    int icon;
    const char* title;
    const char* desc;
    bool required;
};

// A card of the files page: its icon (coloured by its files' checks), title, Required or Optional and what it is,
// with Search and Browse… at the right (under the files when narrow); then `rows` draws its files at the given
// width, and the search's results follow. A file dropped on the card goes to it.
void file_card(const FileCard& c, Status st, const std::function<void(float)>& rows) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kPaper));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(g_portrait ? 14 : 20), S(16)));
    ImGui::BeginChild(c.id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10), S(6)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float avail = ImGui::GetContentRegionAvail().x, sp = ImGui::GetStyle().ItemSpacing.x;
    const char* search_l = tr("launcher.search");
    const char* browse_l = tr("launcher.browse");
    const float sw = button_width(search_l), bw = button_width(browse_l), btns = sw + sp + bw;
    const bool narrow = avail < btns + S(380);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float x0 = ImGui::GetCursorPosX(), r = S(g_portrait ? 20 : 24);
    // The icon, coloured by the checks, with their mark.
    ImVec2 ic(p0.x + r, p0.y + r);
    dl->AddCircleFilled(ic, r, st == Status::Empty ? (c.required ? kBrown : kGrey) : status_color(st));
    theme_icon(dl, c.icon, ic, r * 0.6f, kWhite);
    if (st != Status::Empty) status_mark(dl, ImVec2(ic.x + r * 0.75f, ic.y + r * 0.75f), S(9), st);
    // Title, Required/Optional, what it is.
    const float tx = 2 * r + S(14), text_w = narrow ? avail - tx : avail - tx - btns - S(20);
    ImGui::SetCursorPosX(x0 + tx);
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(x0 + tx + text_w);
    ImGui::PushFont(g_title);
    ImGui::SetWindowFontScale(0.72f);
    ImGui::TextWrapped("%s", c.title);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    const float title_h = ImGui::GetItemRectSize().y;
    if (c.required)
        badge(tr("launcher.badge.required"), rgb(252, 226, 176), rgb(170, 100, 20), title_h, text_w);
    else
        badge(tr("launcher.badge.optional"), rgb(234, 224, 198), kInkSoft, title_h, text_w);
    text_small(c.desc);
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    const float head_h = std::max(ImGui::GetItemRectSize().y, 2 * r);
    bool do_search = false, do_browse = false;
    if (!narrow) {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + avail - btns, p0.y + std::max(0.0f, r - ImGui::GetFrameHeight() * 0.5f)));
        do_search = button(search_l, kWhite, true, sw);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("launcher.search.where"));
        ImGui::SameLine();
        do_browse = button(browse_l, kMint, true, bw);
    }
    // The files, under the title (from the card's edge on a phone).
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + head_h + S(10)));
    const float indent = avail > S(460) ? tx : 0, rw = avail - indent;
    if (indent > 0) ImGui::Indent(indent);
    rows(rw);
    if (narrow) {
        ImGui::Dummy(ImVec2(0, S(2)));
        float half = (rw - sp) * 0.5f;
        do_search = button(search_l, kWhite, true, half);
        ImGui::SameLine();
        do_browse = button(browse_l, kMint, true, half);
    }
    if (g_slot[c.slot].open) {
        ImGui::Dummy(ImVec2(0, S(2)));
        found_list(c.slot, rw);
    }
    if (indent > 0) ImGui::Unindent(indent);
    if (do_search) search(c.slot);
    if (do_browse) pick(Target(c.slot));
    // Where it is for drops (only its visible part), lit while files are dragged over it.
    SlotState& s = g_slot[c.slot];
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    s.min = ImVec2(std::max(wp.x, g_page_min.x), std::max(wp.y, g_page_min.y));
    s.max = ImVec2(std::min(wp.x + ws.x, g_page_max.x), std::min(wp.y + ws.y, g_page_max.y));
    if (g_dragging && slot_at(g_drag_pos) == c.slot) {
        ImGui::PushClipRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), false);
        dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), rgb(255, 240, 196, 200), ImGui::GetStyle().ChildRounding);
        dl->AddRect(ImVec2(wp.x + S(1.5f), wp.y + S(1.5f)), ImVec2(wp.x + ws.x - S(1.5f), wp.y + ws.y - S(1.5f)), kOrange,
                    ImGui::GetStyle().ChildRounding, 0, S(3));
        const char* drop = tr("launcher.drop_here");
        ImVec2 ts = ImGui::CalcTextSize(drop);
        ImVec2 bs(ts.x + S(36), ts.y + S(14));
        ImVec2 bp(wp.x + (ws.x - bs.x) * 0.5f, wp.y + (ws.y - bs.y) * 0.5f);
        dl->AddRectFilled(bp, ImVec2(bp.x + bs.x, bp.y + bs.y), kOrange, bs.y * 0.5f);
        dl->AddText(ImVec2(bp.x + S(18), bp.y + S(7)), kWhite, drop);
        ImGui::PopClipRect();
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

Status combined(std::initializer_list<const Item*> items) {
    Status st = Status::Empty;
    for (const Item* it : items) st = std::max(st, it->check.status);
    return st;
}

bool files_ready() {
    if (g_game.path.empty() || g_game.check.status == Status::Error) return false;
    return g_ptbr.path.empty() || g_ptbr.check.status != Status::Error;
}

void page_files() {
    steps(0);
    heading(tr("launcher.files.title"), tr("launcher.files.text"));
    g_page_min = ImGui::GetWindowPos();
    g_page_max = ImVec2(g_page_min.x + ImGui::GetWindowSize().x, g_page_min.y + ImGui::GetWindowSize().y);
    auto single = [&](const char* id, int slot, int icon, const char* title, const char* desc, bool required,
                      const char* empty, Item& it) {
        file_card({id, slot, icon, title, desc, required}, combined({&it}), [&](float w) {
            if (file_row("file", it, false, empty, required, w)) {
                it = Item();
                save_files();
            }
        });
    };
    single("##game", kSlotGame, nx::ui::kIconLeaf, tr("launcher.item.game"), tr("launcher.item.game.desc"), true,
           tr("launcher.status.game_missing"), g_game);
    single("##ptbr", kSlotMod, nx::ui::kIconText, tr("launcher.item.ptbr"), tr("launcher.item.ptbr.desc"), false,
           tr("launcher.status.none_drop"), g_ptbr);
    // where the game folder comes from: ACNHRecomp-Importer, on the Discord
    ImGui::Dummy(ImVec2(0, S(6)));
    text_small(tr("launcher.files.importer"));
    if (button(tr("launcher.files.discord"), kBlue)) SDL_OpenURL(kDiscordUrl);
}

void footer_files() {
    if (button(tr("common.back"), kWhite)) g_banner.clear(), g_page = Page::Welcome;
    bool ready = files_ready();
    if (!ready) {  // why Next is off, when it fits beside it (the cards show it too)
        bool all = !g_game.path.empty();
        const char* why = tr(all ? "launcher.files.fix_errors" : "launcher.files.missing_required");
        ImGui::SameLine();
        float room = ImGui::GetContentRegionAvail().x - button_width(tr("launcher.next")) - ImGui::GetStyle().ItemSpacing.x;
        if (ImGui::CalcTextSize(why).x * 0.84f <= room) {
            ImGui::AlignTextToFramePadding();
            ImGui::SetWindowFontScale(0.84f);
            ImGui::TextColored(col4(kInkSoft), "%s", why);
            ImGui::SetWindowFontScale(1.0f);
        }
    }
    align_right({tr("launcher.next")});
    if (button(tr("launcher.next"), kGreen, ready)) {
        g_banner.clear();
        g_prereqs = check_prerequisites(g_install);
        g_page = Page::Install;
    }
}

// ---- install --------------------------------------------------------------------------------------------------

bool prereqs_block() {
    for (const Prereq& p : g_prereqs)
        if (p.status == Status::Error) return true;
    return false;
}

void page_install() {
    steps(1);
    heading(tr("launcher.install.title"), tr("launcher.install.text"));
    // The folder.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kPaper));
    ImGui::BeginChild("##where", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    {
        float avail = ImGui::GetContentRegionAvail().x, bw = button_width(tr("launcher.change"));
        bool narrow = avail < S(560);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = S(22);
        dl->AddCircleFilled(ImVec2(p.x + r, p.y + r), r, kBlue);
        theme_icon(dl, nx::ui::kIconFolder, ImVec2(p.x + r, p.y + r), r * 0.6f, kWhite);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2 * r + S(14));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (narrow ? avail - 2 * r - S(14) : avail - 2 * r - bw - S(34)));
        ImGui::TextWrapped("%s", g_install.c_str());
        text_small(trf("launcher.install.free", human_size(free_space(g_install))).c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        if (!narrow) ImGui::SetCursorScreenPos(ImVec2(p.x + avail - bw, p.y + std::max(0.0f, r - ImGui::GetFrameHeight() * 0.5f)));
        if (button(tr("launcher.change"), kMint, true, bw)) pick(kPickInstall);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(4)));
    // This computer.
    ImGui::PushFont(g_title);
    ImGui::SetWindowFontScale(0.75f);
    ImGui::TextUnformatted(tr("launcher.checks.title"));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    bool missing_tools = false;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kPaper));
    ImGui::BeginChild("##checks", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    float col = g_portrait ? 0 : std::min(S(380), ImGui::GetContentRegionAvail().x * 0.45f);
    for (const Prereq& p : g_prereqs) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 c = ImGui::GetCursorScreenPos();
        float r = S(11), lh = ImGui::GetTextLineHeight();
        status_mark(dl, ImVec2(c.x + r, c.y + lh * 0.5f), r, p.status);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2 * r + S(10));
        ImGui::BeginGroup();
        ImGui::TextUnformatted(tr(p.label));
        if (col > 0) ImGui::SameLine(col);
        ImU32 tc = p.status == Status::Ok ? rgb(70, 140, 60) : p.status == Status::Warning ? rgb(190, 120, 20) : rgb(190, 80, 80);
        ImGui::PushStyleColor(ImGuiCol_Text, col4(tc));
        ImGui::TextWrapped("%s", p.detail.c_str());
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        std::string l = p.label;
        if (p.status != Status::Ok && l != "launcher.check.disk" && l != "launcher.check.ram" && l != "launcher.check.scripts")
            missing_tools = true;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    std::string cmd = install_command();
    if (missing_tools && !cmd.empty()) {
        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::TextWrapped("%s", tr("launcher.install.hint"));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(rgb(92, 70, 50)));
        ImGui::BeginChild("##cmd", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushStyleColor(ImGuiCol_Text, col4(rgb(255, 244, 214)));
        ImGui::SetWindowFontScale(0.8f);
        ImGui::TextWrapped("%s", cmd.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        if (button(tr("launcher.copy"))) SDL_SetClipboardText(cmd.c_str()), toast(tr("launcher.copied"));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        text_small(tr("launcher.install.other_distro"));
    }
    ImGui::Dummy(ImVec2(0, S(2)));
    text_small(tr("launcher.install.download"));
}

void start_setup(bool rebuild) {
    SetupArgs a;
    a.install_dir = rebuild ? g_build.data : g_install;
    // A rebuild keeps the game folder the install folder links to; a new setup links the chosen one.
    if (!rebuild && is_dir(g_game.path)) a.game = g_game.path;
    a.ptbr_zip = is_file(g_ptbr.path) ? g_ptbr.path : std::string();
    std::string err;
    g_log.clear();
    g_rebuild = rebuild;
    if (!setup_start(a, err)) {
        g_banner = trf("launcher.build.start_failed", err);
        return;
    }
    g_banner.clear();
    g_page = Page::Build;
}

void footer_install() {
    if (button(tr("common.back"), kWhite)) g_page = Page::Files;
    align_right({tr("launcher.recheck"), tr("launcher.start")});
    if (button(tr("launcher.recheck"), kWhite)) g_prereqs = check_prerequisites(g_install);
    ImGui::SameLine();
    if (button(tr("launcher.start"), kGreen, !prereqs_block())) {
        save_files();
        start_setup(false);
    }
}

// ---- build ----------------------------------------------------------------------------------------------------

const char* stage_label(int s) {
    static const char* keys[] = {"launcher.stage.tools",   "launcher.stage.game",   "launcher.stage.mods",
                                 "launcher.stage.recompile", "launcher.stage.shaders", "launcher.stage.fonts",
                                 "launcher.stage.build"};
    return s >= 0 && s < kStageCount ? tr(keys[s]) : tr("launcher.stage.prepare");
}

void progress_bar(float fraction, bool failed) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = S(30);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), rgb(240, 228, 196), h * 0.5f);
    float fw = std::max(h, w * fraction);
    if (fraction > 0) dl->AddRectFilled(p, ImVec2(p.x + fw, p.y + h), failed ? kRed : kGreen, h * 0.5f);
    char pct[16];
    std::snprintf(pct, sizeof pct, "%d%%", int(fraction * 100 + 0.5f));
    ImVec2 ts = ImGui::CalcTextSize(pct);
    dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), fraction > 0.52f ? kWhite : kInk, pct);
    ImGui::Dummy(ImVec2(w, h));
}

void stage_list(const SetupState& s, bool failed) {
    const bool has_mod = !g_ptbr.path.empty() && !g_rebuild;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kPaper));
    ImGui::BeginChild("##stages", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    for (int k = 0; k < kStageCount; k++) {
        if (k == kStageMods && !has_mod) continue;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 c = ImGui::GetCursorScreenPos();
        float r = S(11), lh = ImGui::GetTextLineHeight();
        ImVec2 m(c.x + r, c.y + lh * 0.5f);
        bool current = s.stage == k && !s.done[k];
        if (s.done[k]) {
            status_mark(dl, m, r, Status::Ok);
        } else if (current && failed) {
            status_mark(dl, m, r, Status::Error);
        } else if (current && s.running) {  // a turning arc
            float t = float(ImGui::GetTime());
            dl->AddCircle(m, r, kGrey, 0, S(3));
            dl->PathArcTo(m, r, t * 5.0f, t * 5.0f + 2.0f, 16);
            dl->PathStroke(kOrange, 0, S(3));
        } else {
            dl->AddCircle(m, r, kGrey, 0, S(2));
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2 * r + S(10));
        ImGui::PushStyleColor(ImGuiCol_Text, col4(s.done[k] || current ? kInk : kInkSoft));
        ImGui::TextUnformatted(stage_label(k));
        ImGui::PopStyleColor();
        if (current && s.running && k == kStageBuild && s.build_fraction > 0) {
            ImGui::SameLine();
            ImGui::TextColored(col4(kInkSoft), "%d%%", int(s.build_fraction * 100));
        }
        if (current && !s.last_line.empty()) {  // what it is doing, or the error
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2 * r + S(10));
            ImGui::PushTextWrapPos(0);
            text_small(s.last_line.c_str(), failed ? rgb(190, 80, 80) : kInkSoft);
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// The setup's output, following the end unless scrolled up.
void log_view(float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(rgb(92, 70, 50)));
    ImGui::BeginChild("##log", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Text, col4(rgb(255, 244, 214)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(2)));
    ImGui::SetWindowFontScale(0.78f);
    bool follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4;
    ImGuiListClipper clip;
    clip.Begin(int(g_log.size()));
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) ImGui::TextUnformatted(g_log[size_t(i)].c_str());
    if (follow) ImGui::SetScrollHereY(1.0f);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void page_build() {
    SetupState s = setup_state();
    setup_lines(g_log.size(), g_log);
    if (s.finished && s.exit_code == 0 && !s.cancelled) {
        set_build(g_rebuild ? g_build.data : g_install, true);
        load_game_ini();
        g_page = Page::Done;
        if (g_express) g_express = false, play();
        return;
    }
    const bool failed = s.finished && !s.cancelled, stopped = s.finished && s.cancelled;
    steps(2);
    if (failed)
        heading(tr("launcher.build.failed_title"),
                (trf("launcher.build.failed", stage_label(s.stage)) + " " + tr("launcher.build.failed_hint")).c_str());
    else if (stopped)
        heading(tr("launcher.build.stopped_title"), tr("launcher.build.cancelled"));
    else
        heading(tr("launcher.build.title"), tr("launcher.build.text"));
    progress_bar(setup_progress(s), failed);
    std::string times = trf("launcher.build.elapsed", clock_text(s.elapsed));
    if (s.running && s.eta >= 0) times += "   ·   " + trf("launcher.build.eta", clock_text(s.eta));
    text_small(times.c_str());
    ImGui::Dummy(ImVec2(0, S(2)));
    // Stages and log side by side, or stacked when narrow (the log then folded unless something failed).
    float avail = ImGui::GetContentRegionAvail().x;
    if (!g_portrait && avail > S(820)) {
        float left = std::min(S(400), avail * 0.42f);
        ImGui::BeginChild("##left", ImVec2(left, 0), ImGuiChildFlags_AutoResizeY);
        stage_list(s, failed);
        ImGui::EndChild();
        ImGui::SameLine(0, S(16));
        ImGui::BeginGroup();
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::CollapsingHeader(tr("launcher.build.details")))
            log_view(std::max(S(160), ImGui::GetContentRegionAvail().y - S(4)));
        ImGui::EndGroup();
    } else {
        stage_list(s, failed);
        ImGui::Dummy(ImVec2(0, S(2)));
        if (failed) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        if (ImGui::CollapsingHeader(tr("launcher.build.details")))
            log_view(std::max(S(200), ImGui::GetContentRegionAvail().y - S(4)));
    }
}

std::string log_text() {
    std::string t;
    for (const std::string& l : g_log) t += l + "\n";
    return t;
}

void footer_build() {
    SetupState s = setup_state();
    if (s.running) {
        align_right({tr("launcher.stop")});
        if (button(tr("launcher.stop"), kRed)) g_confirm_stop = true;
        return;
    }
    if (button(tr("common.back"), kWhite)) g_page = g_rebuild ? Page::Home : Page::Install;
    const char* logs = tr("launcher.open_logs");
    align_right({tr("launcher.copy_log"), logs, tr("launcher.retry")});
    if (button(tr("launcher.copy_log"), kWhite)) SDL_SetClipboardText(log_text().c_str()), toast(tr("launcher.copied"));
    ImGui::SameLine();
    if (button(logs, kWhite)) open_folder(parent_dir(setup_log_path()));
    ImGui::SameLine();
    if (button(tr("launcher.retry"), kGreen)) start_setup(g_rebuild);
}

// ---- done and home --------------------------------------------------------------------------------------------

// Set by play(): the launcher stays up, saying so, until the game's window is there to take its place.
double g_opening_since = -1;

void play() {
    if (g_opening_since >= 0) return;  // already starting
    std::string err;
    if (launch_game(g_build, err, g_window))
        g_opening_since = ImGui::GetTime(), start_sound_play();
    else
        g_banner = trf("launcher.launch_failed", err);
}

// The window of the game that play() started opens over this one when its first picture is ready (it is made hidden
// till then, platform_sdl.cpp), and the launcher goes. Its boot screen is black with a small mark at the bottom right,
// so this screen fades to the same black and waits with its own mark there: the game's takes its place. Where the
// game's window cannot be seen (not Windows), after a few seconds.
void opening_overlay() {
    if (g_opening_since < 0) return;
    double t = ImGui::GetTime() - g_opening_since;
#if defined(_WIN32)
    const double kGiveUp = 90;  // the game shows its window after a minute at most
#else
    const double kGiveUp = 3;
#endif
    // Once the game's window is there the launcher's hides, and goes when its sound logo has played out.
    static bool hidden = false;
    if (!hidden && (game_window_shown() || t > kGiveUp)) SDL_HideWindow(g_window), hidden = true;
    if (hidden && !start_sound_playing()) g_exit = true;
    auto out_quart = [](double x) { return float(1 - std::pow(1 - std::clamp(x, 0.0, 1.0), 4.0)); };
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    // The page dims to black (0.55 s, decelerating): the card's colours sink first, as a lamp turned down.
    float dim = out_quart(t / 0.55);
    dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, int(255 * dim)));
    // The mark: a ring turning, where the game's own will be (its centre at 92 % x 87.5 % of a 16:9 picture).
    float a = out_quart((t - 0.35) / 0.4);
    if (a <= 0) return;
    float pw = std::min(size.x, size.y * 16.0f / 9.0f), ph = pw * 9.0f / 16.0f;
    ImVec2 c((size.x - pw) * 0.5f + pw * 0.92f, (size.y - ph) * 0.5f + ph * 0.875f + (1 - a) * S(6));
    float r = ph * 0.03f, th = std::max(2.0f, r * 0.22f);
    dl->AddCircle(c, r, IM_COL32(255, 249, 232, int(40 * a)), 48, th);
    float spin = float(t * 4.2);  // radians per second: quick, so the wait feels short
    dl->PathArcTo(c, r, spin, spin + 1.9f, 32);
    dl->PathStroke(IM_COL32(118, 200, 170, int(255 * a)), 0, th);
    const char* text = tr("launcher.opening");
    float fs = font_size(g_body);
    ImVec2 ts = g_body->CalcTextSizeA(fs, FLT_MAX, 0, text);
    dl->AddText(g_body, fs, ImVec2(c.x - r - S(14) - ts.x, c.y - ts.y * 0.5f), IM_COL32(255, 249, 232, int(170 * a)), text);
}

bool big_button(const char* label, int icon, ImU32 color, float w, float h) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
    bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    if (ImGui::IsItemActive()) {  // pressed: the button gives a little under the finger (3 %, from its centre)
        float dx = w * 0.015f, dy = h * 0.015f;
        p = ImVec2(p.x + dx, p.y + dy * 2), w -= dx * 2, h -= dy * 2;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p.x, p.y + S(4)), ImVec2(p.x + w, p.y + h + S(4)), rgb(80, 140, 60, 120), h * 0.5f);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov ? kYellow : color, h * 0.5f);
    float fs = font_size(g_title);
    ImVec2 ts = measure(g_title, fs, label, -1);
    float ir = h * 0.22f, total = ts.x + ir * 2 + S(12);
    float x = p.x + (w - total) * 0.5f;
    theme_icon(dl, icon, ImVec2(x + ir, p.y + h * 0.5f), ir, hov ? kInk : kWhite);
    dl->AddText(g_title, fs, ImVec2(x + ir * 2 + S(12), p.y + (h - ts.y) * 0.5f), hov ? kInk : kWhite, label);
    return clicked;
}

void centered(const char* text, ImFont* font, float size_mul, ImU32 color) {
    float w = ImGui::GetContentRegionAvail().x;
    float fs = font_size(font) * size_mul;
    float wrap = std::min(w, S(760));
    ImVec2 ts = measure(font, fs, text, wrap);
    ImVec2 p = ImGui::GetCursorScreenPos();
    // Line by line, each centred.
    const char* s = text;
    float y = p.y;
    while (*s) {
        const char* e = font->CalcWordWrapPositionA(fs / font->FontSize, s, s + std::strlen(s), wrap);
        if (e == s) e++;
        ImVec2 ls = font->CalcTextSizeA(fs, FLT_MAX, 0, s, e);
        ImGui::GetWindowDrawList()->AddText(font, fs, ImVec2(p.x + (w - ls.x) * 0.5f, y), color, s, e);
        y += fs * 1.25f;
        s = e;
        while (*s == ' ' || *s == '\n') s++;
    }
    ImGui::Dummy(ImVec2(w, std::max(ts.y, y - p.y)));
}

void hero(int icon, ImU32 color, float r) {
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + w * 0.5f, p.y + r);
    dl->AddCircleFilled(c, r + S(10), rgb(255, 255, 255, 140));
    dl->AddCircleFilled(c, r, color);
    theme_icon(dl, icon, c, r * 0.6f, kWhite);
    ImGui::Dummy(ImVec2(w, 2 * r + S(14)));
}

void play_block() {
    float w = std::min(ImGui::GetContentRegionAvail().x, S(360));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - w) * 0.5f);
    if (big_button(tr("launcher.play"), nx::ui::kIconPlay, kGreen, w, S(72))) play();
    ImGui::Dummy(ImVec2(0, S(8)));
}

void page_done() {
    steps(3);
    if (!g_banner.empty()) heading("", "");
    ImGui::Dummy(ImVec2(0, S(10)));
    hero(nx::ui::kIconCheck, kGreen, S(48));
    centered(tr("launcher.done.title"), g_title, 1.15f, kInk);
    ImGui::Dummy(ImVec2(0, S(4)));
    centered(trf("launcher.done.text", g_build.data).c_str(), g_body, 1.0f, kInkSoft);
    ImGui::Dummy(ImVec2(0, S(18)));
    play_block();
}

void footer_done() {
    align_right({tr("launcher.home")});
    if (button(tr("launcher.home"), kWhite)) g_page = Page::Home;
}

// A secondary action as a small card: icon, title, one line.
bool action_card(const char* id, int icon, ImU32 color, const char* title, const char* desc, ImVec2 size, bool enabled) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton(id, size);
    ImGui::EndDisabled();
    bool hov = enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + size.x, p.y + size.y);
    dl->AddRectFilled(p, q, hov ? rgb(255, 240, 196) : kPaper, S(22));
    if (hov) dl->AddRect(p, q, rgb(246, 170, 40), S(22), 0, S(3));
    float pad = S(16), r = S(22);
    dl->AddCircleFilled(ImVec2(p.x + pad + r, p.y + pad + r), r, enabled ? color : kGrey);
    theme_icon(dl, icon, ImVec2(p.x + pad + r, p.y + pad + r), r * 0.6f, kWhite);
    float x = p.x + pad + 2 * r + S(12), inner = q.x - pad - x;
    float fs = font_size(g_body);
    dl->AddText(g_body, fs, ImVec2(x, p.y + pad + S(2)), enabled ? kInk : kInkSoft, title, nullptr, inner);
    float ty = measure(g_body, fs, title, inner).y;
    dl->AddText(g_body, fs * 0.82f, ImVec2(x, p.y + pad + S(6) + ty), kInkSoft, desc, nullptr, inner);
    return clicked && enabled;
}

float action_height(const char* title, const char* desc, float w) {
    float pad = S(16), r = S(22), inner = w - 2 * pad - 2 * r - S(12);
    float fs = font_size(g_body);
    return std::max(2 * r, S(8) + measure(g_body, fs, title, inner).y + measure(g_body, fs * 0.82f, desc, inner).y) + 2 * pad;
}

// ---- what the build holds: its version and its add-on content ---------------------------------------------------

bool g_addons_open = false;
double g_addons_since = 0;

const BuildContents& contents(bool refresh = false) {
    static std::string for_data;
    static BuildContents c;
    if (refresh || for_data != g_build.data) for_data = g_build.data, c = build_contents(g_build.data);
    return c;
}

std::string size_text(uint64_t bytes) {
    char t[32];
    if (bytes >= (1ull << 30)) std::snprintf(t, sizeof t, "%.1f GB", double(bytes) / double(1ull << 30));
    else std::snprintf(t, sizeof t, "%.0f MB", std::max(1.0, double(bytes) / double(1ull << 20)));
    return t;
}

float out_cubic(double x) { return float(1 - std::pow(1 - std::clamp(x, 0.0, 1.0), 3.0)); }

// A pill: an icon in a coloured disc, then the text. A clickable one answers the pointer like the action cards and
// gives a little under it; `dot` marks it live (a green dot on the disc).
bool pill(const char* id, int icon, ImU32 color, const char* text, bool clickable, bool dot, float x) {
    float h = S(38), r = S(13), pad = S(6), fs = font_size(g_body) * 0.9f;
    ImVec2 ts = measure(g_body, fs, text, -1);
    float w = pad + 2 * r + S(10) + ts.x + S(16);
    ImGui::SetCursorScreenPos(ImVec2(x, ImGui::GetCursorScreenPos().y));
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    bool hov = clickable && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
    if (clickable && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (clickable && ImGui::IsItemActive()) p = ImVec2(p.x + w * 0.015f, p.y + h * 0.03f), w *= 0.97f, h *= 0.94f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + w, p.y + h);
    dl->AddRectFilled(ImVec2(p.x, p.y + S(2)), ImVec2(q.x, q.y + S(2)), rgb(120, 90, 50, 22), h * 0.5f);
    dl->AddRectFilled(p, q, hov ? rgb(255, 240, 196) : rgb(255, 252, 242), h * 0.5f);
    dl->AddRect(p, q, hov ? rgb(246, 170, 40) : rgb(232, 220, 190), h * 0.5f, 0, hov ? S(2) : S(1));
    ImVec2 c(p.x + pad + r, p.y + h * 0.5f);
    dl->AddCircleFilled(c, r, color);
    theme_icon(dl, icon, c, r * 0.62f, kWhite);
    if (dot) {
        ImVec2 d(c.x + r * 0.72f, c.y - r * 0.72f);
        dl->AddCircleFilled(d, S(5), rgb(255, 252, 242));
        dl->AddCircleFilled(d, S(3.5f), kGreen);
    }
    dl->AddText(g_body, fs, ImVec2(c.x + r + S(10), p.y + (h - ts.y) * 0.5f), kInk, text);
    return clicked && clickable;
}

float pill_width(const char* text) {
    float r = S(13), fs = font_size(g_body) * 0.9f;
    return S(6) + 2 * r + S(10) + measure(g_body, fs, text, -1).x + S(16);
}

// Version and add-ons, side by side under the title (one under the other when they do not fit).
void contents_row() {
    const BuildContents& c = contents();
    std::string ver = c.version.empty() ? tr("launcher.home.version_unknown") : trf("launcher.home.version", c.version);
    std::string add = c.addons.empty() ? tr("launcher.addons.none")
                                       : trf("launcher.addons.count", std::to_string(c.addons.size()));
    float avail = ImGui::GetContentRegionAvail().x, gap = S(10), x0 = ImGui::GetCursorScreenPos().x;
    float w1 = pill_width(ver.c_str()), w2 = pill_width(add.c_str());
    bool stack = w1 + gap + w2 > avail;
    float y = ImGui::GetCursorScreenPos().y;
    pill("##version", nx::ui::kIconLeaf, kMint, ver.c_str(), false, false,
         x0 + std::floor((avail - (stack ? w1 : w1 + gap + w2)) * 0.5f));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g_build.data.c_str());
    if (!stack) ImGui::SetCursorScreenPos(ImVec2(x0, y));
    float x2 = std::floor(stack ? x0 + (avail - w2) * 0.5f : x0 + (avail - w1 - gap - w2) * 0.5f + w1 + gap);
    if (pill("##addons", nx::ui::kIconBox, c.addons.empty() ? kGrey : kPurple, add.c_str(), true, !c.addons.empty(), x2)) {
        contents(true);  // read again: the setup may have added some since
        g_addons_open = true, g_addons_since = ImGui::GetTime();
    }
    ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y));
}

// The add-on content, one card each: its number, size and files, and that the game loads it.
void addons_popup() {
    if (g_addons_open) ImGui::OpenPopup("addons");
    // Fades in (180 ms, decelerating), right after the pill was pressed.
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, out_cubic((ImGui::GetTime() - g_addons_since) / 0.18));
    ImGui::SetNextWindowSize(ImVec2(std::min(ImGui::GetIO().DisplaySize.x * 0.9f, S(520)), 0));
    if (ImGui::BeginPopupModal("addons", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
        const BuildContents& c = contents();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = S(18);
        dl->AddCircleFilled(ImVec2(p.x + r, p.y + r), r, c.addons.empty() ? kGrey : kPurple);
        theme_icon(dl, nx::ui::kIconBox, ImVec2(p.x + r, p.y + r), r * 0.6f, kWhite);
        float fs = font_size(g_title) * 0.8f;
        dl->AddText(g_title, fs, ImVec2(p.x + 2 * r + S(12), p.y + r - fs * 0.6f), kInk, tr("launcher.addons.title"));
        ImGui::Dummy(ImVec2(0, 2 * r + S(12)));
        if (c.addons.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, col4(kInkSoft));
            ImGui::TextWrapped("%s", tr("launcher.addons.empty"));
            ImGui::PopStyleColor();
        }
        for (size_t k = 0; k < c.addons.size(); k++) {
            const AddOn& o = c.addons[k];
            float w = ImGui::GetContentRegionAvail().x, h = S(58);
            ImVec2 q = ImGui::GetCursorScreenPos();
            // The cards settle into place one after the other (40 ms apart, 6 px).
            float e = out_cubic((ImGui::GetTime() - g_addons_since - 0.04 * double(k)) / 0.22);
            q.y += (1 - e) * S(6);
            dl->AddRectFilled(q, ImVec2(q.x + w, q.y + h), rgb(255, 252, 242, int(255 * e)), S(16));
            dl->AddRect(q, ImVec2(q.x + w, q.y + h), rgb(232, 220, 190, int(255 * e)), S(16), 0, S(1));
            float cr = S(15);
            ImVec2 cc(q.x + S(14) + cr, q.y + h * 0.5f);
            dl->AddCircleFilled(cc, cr, kPurple);
            theme_icon(dl, nx::ui::kIconBox, cc, cr * 0.6f, kWhite);
            std::string title = trf("launcher.addons.item", std::to_string(o.index));
            std::string sub = size_text(o.bytes) + "  \xC2\xB7  " + trf("launcher.addons.files", std::to_string(o.files));
            float bfs = font_size(g_body);
            dl->AddText(g_body, bfs, ImVec2(cc.x + cr + S(12), q.y + S(9)), kInk, title.c_str());
            dl->AddText(g_body, bfs * 0.8f, ImVec2(cc.x + cr + S(12), q.y + S(9) + bfs * 1.15f), kInkSoft, sub.c_str());
            // Loaded: a green check and the word, at the right.
            const char* ok = tr("launcher.addons.loaded");
            ImVec2 os = measure(g_body, bfs * 0.85f, ok, -1);
            float ox = q.x + w - S(14) - os.x;
            dl->AddText(g_body, bfs * 0.85f, ImVec2(ox, q.y + (h - os.y) * 0.5f), rgb(70, 140, 60), ok);
            ImVec2 kc(ox - S(16), q.y + h * 0.5f);
            dl->AddCircleFilled(kc, S(10), kGreen);
            theme_icon(dl, nx::ui::kIconCheck, kc, S(6), kWhite);
            ImGui::Dummy(ImVec2(w, h + S(8)));
        }
        ImGui::Dummy(ImVec2(0, S(4)));
        align_right({tr("launcher.addons.close")});
        if (button(tr("launcher.addons.close"), kWhite) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            g_addons_open = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// A newer release on GitHub (desktop.cpp): what it is, and a button to its page.
void update_notice() {
    const UpdateInfo u = update_info();
    if (!u.available) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kPaper));
    ImGui::BeginChild("##update", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::TextWrapped("%s", trf("launcher.update.available", u.version).c_str());
    text_small(trf("launcher.version", current_version()).c_str());
    if (button(tr("launcher.update.get"), kGreen)) SDL_OpenURL(u.url.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(6)));
}

void page_home() {
    const bool valid = *g_build_problem == 0;
    if (!valid) {
        heading(tr("launcher.home.title_missing"),
                trf("launcher.home.missing", tr(g_build_problem)).c_str());
    } else if (!g_banner.empty()) {
        heading("", "");
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    update_notice();
    if (valid) {
        hero(nx::ui::kIconHouse, kGreen, S(46));
        centered(tr("launcher.home.title"), g_title, 1.1f, kInk);
        ImGui::Dummy(ImVec2(0, S(6)));
        if (g_autostart_at >= 0) {
            int left = std::max(1, int(std::ceil(g_autostart_at - ImGui::GetTime())));
            centered(trf("launcher.autostart", std::to_string(left)).c_str(), g_body, 0.9f, kInkSoft);
            ImGui::Dummy(ImVec2(0, S(4)));
        }
        contents_row();
        ImGui::Dummy(ImVec2(0, S(16)));
        play_block();
    }
    // Settings, Rebuild, Change build folder.
    const bool can_rebuild = valid && setup_supported() &&
                             g_build.exe.rfind(g_build.data, 0) == 0 && g_build.exe.find("build") != std::string::npos;
    struct A {
        const char* id;
        int icon;
        ImU32 color;
        const char* title;
        const char* desc;
        bool enabled;
        int action;
    };
    std::vector<A> acts;
    if (valid) acts.push_back({"##settings", nx::ui::kIconGear, kPurple, tr("launcher.home.settings"),
                               tr("launcher.home.settings.desc"), true, 0});
    if (valid) acts.push_back({"##online", nx::ui::kIconBox, kBlue, tr("tab.online"), tr("online.home.desc"), true, 4});
    const bool player = !player_beside().empty();  // a release: the folder is played as it is, nothing to rebuild
    if (setup_supported() && !player)
        acts.push_back({"##rebuild", nx::ui::kIconLeaf, kOrange, tr("launcher.home.rebuild"),
                        tr(can_rebuild ? "launcher.home.rebuild.desc" : "launcher.home.rebuild.unavailable"), can_rebuild, 1});
#if defined(__APPLE__)
    acts.push_back({"##desktop", nx::ui::kIconPlus, kMint, tr("launcher.home.install_app"),
                    tr("launcher.home.install_app.desc"), true, 3});
#else
    acts.push_back({"##desktop", nx::ui::kIconPlus, kMint, tr("launcher.home.desktop"), tr("launcher.home.desktop.desc"),
                    true, 3});
#endif
    acts.push_back({"##change", nx::ui::kIconFolder, kBlue, tr(player ? "launcher.home.change_game" : "launcher.home.change"),
                    tr(player ? "launcher.home.change_game.desc" : "launcher.home.change.desc"), true, 2});
    float avail = ImGui::GetContentRegionAvail().x, gap = S(14);
    bool stack = g_portrait || avail < S(700);
    float w = stack ? avail : (avail - gap * float(acts.size() - 1)) / float(acts.size());
    float h = 0;
    for (const A& a : acts) h = std::max(h, action_height(a.title, a.desc, w));
    for (size_t k = 0; k < acts.size(); k++) {
        if (k && !stack) ImGui::SameLine(0, gap);
        const A& a = acts[k];
        if (action_card(a.id, a.icon, a.color, a.title, a.desc, ImVec2(w, h), a.enabled)) {
            if (a.action == 0) load_game_ini(), g_page = Page::Settings;
            if (a.action == 4) load_game_ini(), g_page = Page::Online;
            if (a.action == 1) start_setup(true);
            if (a.action == 2) pick(kPickBuild);
            if (a.action == 3) {
                DesktopResult r = add_to_desktop();
                toast(r.ok ? trf("launcher.desktop.done", r.where) : std::string(tr("launcher.desktop.failed")));
            }
        }
    }
}

void footer_home() {
    if (!player_beside().empty()) return;  // a release has no setup to start over
    if (button(tr("launcher.home.start_over"), kWhite)) g_banner.clear(), g_page = Page::Welcome;
}

// ---- settings: a few of the game's config.ini keys ------------------------------------------------------------

using Choices = std::vector<std::pair<std::string, std::string>>;

void setting_row(const char* label, const char* desc, const char* key, const char* fallback, const Choices& choices) {
    float avail = ImGui::GetContentRegionAvail().x;
    bool narrow = avail < S(560);
    float ctrl_w = narrow ? avail : std::min(S(360), avail * 0.45f);
    ImGui::PushID(key);
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (narrow ? avail : avail - ctrl_w - S(16)));
    ImGui::TextUnformatted(tr(label));
    if (desc) text_small(tr(desc));
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    if (!narrow) ImGui::SameLine(avail - ctrl_w + ImGui::GetStyle().WindowPadding.x * 0.0f);
    std::string cur = g_game_ini.get(key, fallback), shown = cur;
    for (auto& [v, t] : choices)
        if (v == cur) shown = t;
    ImGui::SetNextItemWidth(ctrl_w);
    if (ImGui::BeginCombo("##c", shown.c_str())) {
        for (auto& [v, t] : choices)
            if (ImGui::Selectable(t.c_str(), v == cur) && v != cur) {
                g_game_ini.set(key, v);
                if (!g_game_ini.save()) toast(tr("launcher.settings.save_failed"));
            }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, S(2)));
}

void page_settings() {
    heading(tr("app.title"), tr("launcher.settings.text"));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kPaper));
    ImGui::BeginChild("##rows", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    setting_row("game.language", "game.language.desc", "game.language", "auto",
                {{"auto", tr("game.language.system")}, {"en-US", "English (US)"}, {"en-GB", "English (UK)"},
                 {"es", "Español (España)"}, {"es-419", "Español (Latinoamérica)"}, {"fr", "Français"},
                 {"fr-CA", "Français (Canada)"}, {"de", "Deutsch"}, {"it", "Italiano"}, {"nl", "Nederlands"},
                 {"ru", "Русский"}, {"ja", "日本語"}, {"ko", "한국어"}, {"zh-CN", "简体中文"}, {"zh-TW", "繁體中文"},
                 {"pt-BR", tr("game.language.ptbr")}});
    if (g_game_ini.get("game.language") == "pt-BR" && !is_dir(g_build.data + "/mods/ptbr/romfs")) {
        ImGui::TextColored(col4(rgb(190, 120, 20)), "%s", tr("launcher.settings.ptbr_missing"));
        ImGui::Separator();
    }
    setting_row("video.display", nullptr, "video.fullscreen", "0",
                {{"0", tr("video.display.windowed")}, {"1", tr("video.display.fullscreen")}});
    setting_row("video.window_size", nullptr, "video.window", "auto",
                {{"auto", tr("common.auto")}, {"1280x720", "1280 × 720"}, {"1600x900", "1600 × 900"},
                 {"1920x1080", "1920 × 1080"}, {"2560x1440", "2560 × 1440"}, {"3840x2160", "3840 × 2160"}});
    setting_row("video.render", nullptr, "video.mode", "docked",
                {{"docked", tr("video.render.docked")}, {"handheld", tr("video.render.handheld")}});
    setting_row("video.upscaler", nullptr, "video.upscaler", "linear",
                {{"linear", tr("video.upscaler.linear")}, {"fsr", tr("video.upscaler.fsr")}});
    // No choice yet: 60 when the SpeedFix is in the game folder, as the game itself (config.cpp).
    setting_row("video.framerate", "video.framerate.desc", "video.frame_rate",
                is_dir(g_build.data + "/mods/speedfix60/romfs") ? "60" : "30",
                {{"30", tr("video.framerate.30")}, {"60", tr("video.framerate.60")}});
    // The launcher's own: new releases looked for at each start (launcher.ini), and the version in use.
    {
        Ini& li = settings();
        bool check = li.get("launcher.update_check", "1") != "0";
        if (ImGui::Checkbox(tr("launcher.update.check"), &check)) {
            li.set("launcher.update_check", check ? "1" : "0");
            li.save();
            if (check) update_check_start();
        }
        const UpdateInfo u = update_info();
        std::string ver = trf("launcher.version", current_version());
        if (u.done) ver += "  ·  " + (u.available ? trf("launcher.update.newer", u.version) : std::string(tr("launcher.update.latest")));
        text_small(ver.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void footer_settings() {
    if (button(tr("common.back"), kWhite)) g_page = Page::Home;
    align_right({tr("launcher.settings.open_data")});
    if (button(tr("launcher.settings.open_data"), kWhite)) open_folder(g_build.data);
}

// ---- online: local play over the internet, the same settings as the game's Online page -------------------------

struct OnlineCheck {
    std::mutex lock;
    nx::LinkCheck state = nx::LinkCheck::Idle;
    std::string address;
    std::atomic<bool> running{false};
};
OnlineCheck g_online;

void online_check_start() {
    if (g_online.running.exchange(true)) return;
    {
        std::lock_guard lk(g_online.lock);
        g_online.state = nx::LinkCheck::Running;
    }
    std::thread([] {
        std::string address;
        const nx::LinkCheck r = nx::upnp_check(39500, address);
        std::lock_guard lk(g_online.lock);
        g_online.state = r;
        g_online.address = address;
        g_online.running = false;
    }).detach();
}

void page_online() {
    heading(tr("tab.online"), tr("online.enabled.desc"));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kPaper));
    ImGui::BeginChild("##rows", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    bool on = g_game_ini.get("online.enabled", "0") == "1";
    if (ImGui::Checkbox(tr("online.enabled"), &on)) {
        g_game_ini.set("online.enabled", on ? "1" : "0");
        if (!g_game_ini.save()) toast(tr("launcher.settings.save_failed"));
        if (on) online_check_start();
    }
    if (on) {
        static bool checked = false;
        if (!checked) {
            checked = true;
            online_check_start();
        }
        nx::LinkCheck state;
        std::string address;
        {
            std::lock_guard lk(g_online.lock);
            state = g_online.state;
            address = g_online.address;
        }
        const char* text = "online.status.idle";
        switch (state) {
        case nx::LinkCheck::Running: text = "online.status.running"; break;
        case nx::LinkCheck::NoRouter: text = "online.status.no_router"; break;
        case nx::LinkCheck::Refused: text = "online.status.refused"; break;
        case nx::LinkCheck::NoPublicAddress: text = "online.status.no_public"; break;
        case nx::LinkCheck::Open: text = "online.status.open"; break;
        case nx::LinkCheck::OpenVerified: text = "online.status.verified"; break;
        case nx::LinkCheck::Idle: break;
        }
        ImGui::Separator();
        ImGui::TextUnformatted(tr("online.status"));
        text_small(tr(text));
        if (button(tr("online.test"), kWhite, !g_online.running)) online_check_start();
        if (!address.empty()) {
            static bool shown = false;
            ImGui::Separator();
            ImGui::TextUnformatted(tr("online.address"));
            text_small(tr("online.address.desc"));
            ImGui::TextUnformatted(shown ? address.c_str() : "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2");
            if (button(tr(shown ? "online.hide" : "online.show"), kWhite)) shown = !shown;
            ImGui::SameLine();
            if (button(tr("online.copy"), kMint)) {
                ImGui::SetClipboardText(address.c_str());
                toast(tr("online.copied"));
            }
        }
        ImGui::Separator();
        ImGui::TextUnformatted(tr("online.join"));
        const nx::AddressResult a = nx::address_check_result();
        std::string join_text = tr("online.join.desc");
        switch (a.check) {
        case nx::AddressCheck::Running: join_text = tr("online.join.running"); break;
        case nx::AddressCheck::Bad: join_text = tr("online.join.bad"); break;
        case nx::AddressCheck::NoAnswer: join_text = tr("online.join.no_answer"); break;
        case nx::AddressCheck::Found: join_text = trf("online.join.found", a.island); break;
        case nx::AddressCheck::Idle: break;
        }
        text_small(join_text.c_str());
        static char join[128] = {};
        static bool loaded = false;
        if (!loaded) {
            loaded = true;
            std::snprintf(join, sizeof join, "%s", g_game_ini.get("online.join").c_str());
            nx::address_check_start(join);
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputText("##join", join, sizeof join)) {
            g_game_ini.set("online.join", join);
            if (!g_game_ini.save()) toast(tr("launcher.settings.save_failed"));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) nx::address_check_start(join);
        if (join[0] && button(tr("online.join.test"), kWhite)) nx::address_check_start(join);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void footer_online() {
    if (button(tr("common.back"), kWhite)) g_page = Page::Home;
}

// ---- popups ---------------------------------------------------------------------------------------------------

void popups() {
    if (g_confirm_stop) ImGui::OpenPopup("stop");
    if (g_confirm_quit) ImGui::OpenPopup("quit");
    ImGui::SetNextWindowSize(ImVec2(std::min(ImGui::GetIO().DisplaySize.x * 0.9f, S(560)), 0));
    if (ImGui::BeginPopupModal("stop", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("%s", tr("launcher.build.cancel_confirm"));
        ImGui::Dummy(ImVec2(0, S(6)));
        if (button(tr("launcher.stop"), kRed)) setup_cancel(), g_confirm_stop = false, ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (button(tr("common.no"), kWhite)) g_confirm_stop = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowSize(ImVec2(std::min(ImGui::GetIO().DisplaySize.x * 0.9f, S(560)), 0));
    if (ImGui::BeginPopupModal("quit", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("%s", tr("launcher.quit_confirm"));
        ImGui::Dummy(ImVec2(0, S(6)));
        if (button(tr("launcher.stop_quit"), kRed)) {
            setup_cancel();
            g_quit_after_stop = true;
            g_confirm_quit = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (button(tr("common.no"), kWhite)) g_confirm_quit = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (g_quit_after_stop && !setup_state().running) g_exit = true;
    addons_popup();
}

}  // namespace

// ---- entry points ---------------------------------------------------------------------------------------------

void pages_init(SDL_Window* window) {
    g_window = window;
    Ini& s = settings();
    g_install = s.get("launcher.install", default_install_dir());
    if (std::string g = s.get("files.game"); is_dir(g)) set_item(g_game, Kind::Game, g);
    if (std::string z = s.get("files.ptbr"); is_file(z)) set_item(g_ptbr, Kind::Mod, z);
    std::string build = s.get("launcher.build");
    if (!build.empty()) {
        set_build(build, false);
        load_game_ini();
        g_page = Page::Home;
    }
    if (!std::getenv("NX_LAUNCHER_PAGE") && !std::getenv("NX_LAUNCHER_SEARCH")) {
        if (build.empty() && setup_supported() && player_beside().empty())
            g_express = true;
        else if (!build.empty() && *g_build_problem == 0 && s.get("launcher.autostart", "1") != "0")
            g_autostart_at = -2;
    }
    // Tests: a card's search shown, a page directly (main.cpp).
    if (const char* q = std::getenv("NX_LAUNCHER_SEARCH")) {
        const char* names[] = {"game", "ptbr"};
        for (int k = 0; k < kSlotCount; k++)
            if (std::strcmp(q, names[k]) == 0) search(k);
    }
    if (const char* p = std::getenv("NX_LAUNCHER_PAGE")) {
        std::string n = p;
        if (n == "welcome") g_page = Page::Welcome;
        if (n == "files") g_page = Page::Files;
        if (n == "install") g_prereqs = check_prerequisites(g_install), g_page = Page::Install;
        if (n == "build") start_setup(false);
        if (n == "done") g_page = Page::Done;
        if (n == "home") g_page = Page::Home;
        if (n == "addons") g_page = Page::Home, g_addons_open = true, g_addons_since = -1;  // the add-on panel open
        if (n == "settings") g_page = Page::Settings;
    }
}

void pages_drop(const std::string& path, float x, float y) {
    g_dragging = false;
    if (g_page == Page::Build) return;
    // On a card it is that card's; elsewhere it goes where it looks like it belongs: a game folder to the files page,
    // another folder is taken for a build folder (welcome and home).
    int slot = slot_at(ImVec2(x, y));
    Kind k;
    if (slot >= 0) {
        k = slot_kind(slot, path);
    } else if (!classify_file(path, k)) {
        if (is_dir(path) && (g_page == Page::Welcome || g_page == Page::Home)) return use_build_folder(path);
        g_banner = trf("launcher.err.drop", file_name(path));
        return;
    }
    g_banner.clear();
    use_file(k, path);
    if (g_page != Page::Files && setup_supported()) g_page = Page::Files;
}

void pages_drag(bool over, float x, float y) {
    g_dragging = over;
    g_drag_pos = ImVec2(x, y);
}

void pages_close_requested() {
    if (setup_state().running)
        g_confirm_quit = true;
    else
        g_exit = true;
}

bool pages_busy() { return setup_state().running || ImGui::GetTime() < g_toast_until || g_opening_since >= 0; }

bool pages_draw(float scale, ImFont* title) {
    g_scale = scale, g_title = title, g_body = ImGui::GetFont();
    static bool first = true;
    if (first) {
        first = false;
        if (g_express) pick(kPickGame);
        if (g_autostart_at == -2) g_autostart_at = ImGui::GetTime() + 3;
        update_check_start();
    }
    if (g_autostart_at >= 0) {
        // A new release found stops it too: the notice stays on the home page to be read.
        bool stop = g_page != Page::Home || ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) ||
                    update_info().available;
        for (int k = ImGuiKey_NamedKey_BEGIN; !stop && k < ImGuiKey_NamedKey_END; k++)
            if (ImGui::IsKeyPressed(ImGuiKey(k), false) && !(k >= ImGuiKey_ReservedForModCtrl && k <= ImGuiKey_ReservedForModSuper))
                stop = true;
        if (stop) {
            g_autostart_at = -1;
        } else if (ImGui::GetTime() >= g_autostart_at) {
            g_autostart_at = -1;
            play();
        }
    }
    apply_picks();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 ds = io.DisplaySize;
    g_portrait = ds.x < ds.y * 1.2f;
    draw_background(ds);
    ImVec2 size = g_portrait ? ImVec2(ds.x - S(16), ds.y - S(16))
                             : ImVec2(std::min(ds.x - S(32), S(1180)), std::min(ds.y - S(32), S(700)));
    ImVec2 pos((ds.x - size.x) * 0.5f, (ds.y - size.y) * 0.5f);
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(pos.x, pos.y + S(6)), ImVec2(pos.x + size.x, pos.y + size.y + S(6)),
                                                  rgb(60, 110, 70, 60), S(28));
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(g_portrait ? 14 : 22), S(g_portrait ? 14 : 20)));
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    ribbon();
    float footer = ImGui::GetFrameHeight() + S(20);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##page", ImVec2(0, -footer), ImGuiChildFlags_None);
    ImGui::PopStyleColor();
    // The short pages sit in the middle of the card: their height from the last frame. The offset is whole pixels:
    // ImGui rounds item positions down, so a fractional start changed the measured height and the page bobbed by a
    // pixel every frame.
    static float content_h[8] = {};
    const int pi = int(g_page);
    const bool center = g_page == Page::Welcome || g_page == Page::Done || g_page == Page::Home;
    const float avail_h = ImGui::GetContentRegionAvail().y, top = ImGui::GetCursorPosY();
    if (center && content_h[pi] > 0 && content_h[pi] < avail_h)
        ImGui::SetCursorPosY(std::floor(top + (avail_h - content_h[pi]) * 0.4f));
    const float start = ImGui::GetCursorPosY();
    switch (g_page) {
        case Page::Welcome: page_welcome(); break;
        case Page::Files: page_files(); break;
        case Page::Install: page_install(); break;
        case Page::Build: page_build(); break;
        case Page::Done: page_done(); break;
        case Page::Home: page_home(); break;
        case Page::Settings: page_settings(); break;
        case Page::Online: page_online(); break;
    }
    if (int(g_page) == pi) content_h[pi] = ImGui::GetCursorPosY() - start;
    static const char* scroll = std::getenv("NX_LAUNCHER_SCROLL");  // tests: 0..1 of the page's scroll range
    if (scroll) ImGui::SetScrollY(ImGui::GetScrollMaxY() * float(std::atof(scroll)));
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, S(2)));
    if (ImGui::GetTime() < g_toast_until) {  // a short confirmation, in place of the footer's own text
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(col4(rgb(70, 140, 100)), "%s", g_toast.c_str());
        ImGui::SameLine();
    }
    switch (g_page) {
        case Page::Welcome: footer_welcome(); break;
        case Page::Files: footer_files(); break;
        case Page::Install: footer_install(); break;
        case Page::Build: footer_build(); break;
        case Page::Done: footer_done(); break;
        case Page::Home: footer_home(); break;
        case Page::Settings: footer_settings(); break;
        case Page::Online: footer_online(); break;
    }
    popups();
    opening_overlay();
    ImGui::End();
    return !g_exit;
}

}  // namespace launcher
