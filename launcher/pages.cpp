// The launcher's pages (launcher.h), in the settings screen's style (ui/theme.h).
//
//   welcome   two ways in: prepare the game from the files extracted with an emulator, or pick a game folder prepared
//             before (a release); from a checkout, set up from the game folder or pick a build folder made before
//   prepare   the preparation's picks: the extracted ExeFS and RomFS, add-on content, the game folder, the platforms and
//             60 FPS; each pick checked by prepare/io's probe (prepare_job.h), its findings shown translated
//   preparing the preparation running (prepare/prepare.h): stages, progress, log, stop; the error explained; then Play
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
#include "ldn_lan.h"
#include "nextendo.h"
#include "prepare_job.h"

#include <thread>
#include <atomic>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>

#include "imgui.h"
#include "imgui_internal.h"

#include "mod_manager.h"
#include "save_profiles.h"

#include "ui/i18n.h"
#include "ui/theme.h"

#include "IconsLucide.h"  // IconFontCppHeaders: the codepoints of assets/fonts/Lucide-launcher.ttf

namespace launcher {

namespace {

using nx::ui::tr;
using nx::ui::trf;

ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
// ACNHRecomp-Importer's island notice board: warm sand, a paper board, bark-brown ink (never black), leaf green for
// what is done, lagoon teal for water and focus, sun yellow for the one action that matters, bell orange and coral for
// warnings and errors.
const ImU32 kInk = rgb(107, 84, 68), kInkSoft = rgb(154, 133, 113);                    // bark, bark soft
const ImU32 kSand = rgb(251, 244, 220), kSandDeep = rgb(241, 230, 194), kBoard = rgb(255, 252, 240);
const ImU32 kPaper = kSand;  // cards and panels on the board
const ImU32 kGreen = rgb(120, 195, 106), kLeafDeep = rgb(78, 154, 69), kLeafPale = rgb(227, 243, 220);
const ImU32 kMint = rgb(78, 195, 179), kLagoonDeep = rgb(46, 156, 144);               // lagoon
const ImU32 kYellow = rgb(246, 211, 74), kSunDeep = rgb(217, 174, 34);                // sun
const ImU32 kOrange = rgb(240, 164, 58), kRed = rgb(236, 122, 107), kBlue = rgb(92, 172, 222);
// The project's Discord: help, news
const char* const kDiscordUrl = "https://discord.gg/VJW9Dtdu7a";
const ImU32 kPurple = rgb(166, 138, 214), kBrown = rgb(176, 140, 100), kGrey = rgb(218, 204, 168);
const ImU32 kWhite = rgb(255, 255, 255);

// `c` darkened (the lip under a button) or lightened (hovered), alpha kept.
ImU32 shade(ImU32 c, float k) {
    auto ch = [&](int s) {
        float v = float((c >> s) & 0xFF);
        v = k < 1 ? v * k : v + (255 - v) * (k - 1);
        return int(std::clamp(v, 0.0f, 255.0f));
    };
    return IM_COL32(ch(IM_COL32_R_SHIFT), ch(IM_COL32_G_SHIFT), ch(IM_COL32_B_SHIFT), (c >> IM_COL32_A_SHIFT) & 0xFF);
}
// Bark on light faces, paper on dark ones.
ImU32 ink_on(ImU32 c) {
    float l = (0.299f * float(c & 0xFF) + 0.587f * float((c >> 8) & 0xFF) + 0.114f * float((c >> 16) & 0xFF)) / 255;
    return l > 0.62f ? kInk : kBoard;
}

ImVec4 col4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

ImFont* g_icons = nullptr;  // Lucide (main.cpp), nullptr when the fonts fell back to the built-in one

// The launcher's own icons past ui/theme.h's.
enum { kIconHammer = 100, kIconMonitorDown, kIconRefresh, kIconHeart, kIconPuzzle, kIconDownload };

// Every icon is Lucide's glyph, drawn so it fills about the drawn icons' 1.4 r; without the icon font, ui/theme.h's
// line drawing (the launcher's own icons as the nearest of those).
void draw_icon(ImDrawList* dl, int icon, ImVec2 c, float r, ImU32 col) {
    const char* g = nullptr;
    int drawn = icon;
    switch (icon) {
        case nx::ui::kIconLeaf: g = ICON_LC_LEAF; break;
        case nx::ui::kIconScreen: g = ICON_LC_MONITOR; break;
        case nx::ui::kIconNote: g = ICON_LC_MUSIC; break;
        case nx::ui::kIconController: g = ICON_LC_GAMEPAD_2; break;
        case nx::ui::kIconHouse: g = ICON_LC_HOUSE; break;
        case nx::ui::kIconInfo: g = ICON_LC_INFO; break;
        case nx::ui::kIconFolder: g = ICON_LC_FOLDER_OPEN; break;
        case nx::ui::kIconKey: g = ICON_LC_KEY; break;
        case nx::ui::kIconBox: g = ICON_LC_PACKAGE; break;
        case nx::ui::kIconPlus: g = ICON_LC_PLUS; break;
        case nx::ui::kIconCheck: g = ICON_LC_CHECK; break;
        case nx::ui::kIconCross: g = ICON_LC_X; break;
        case nx::ui::kIconPlay: return nx::ui::theme_icon(dl, icon, c, r, col);  // filled, as Lucide's is only outlined
        case nx::ui::kIconGear: g = ICON_LC_SETTINGS; break;
        case nx::ui::kIconGlobe: g = ICON_LC_GLOBE; break;
        case nx::ui::kIconWarning: g = ICON_LC_TRIANGLE_ALERT; break;
        case nx::ui::kIconText: g = ICON_LC_FILE_TEXT; break;
        case kIconHammer: g = ICON_LC_HAMMER, drawn = nx::ui::kIconLeaf; break;
        case kIconMonitorDown: g = ICON_LC_MONITOR_DOWN, drawn = nx::ui::kIconPlus; break;
        case kIconRefresh: g = ICON_LC_REFRESH_CW, drawn = nx::ui::kIconLeaf; break;
        case kIconHeart: g = ICON_LC_HEART, drawn = nx::ui::kIconInfo; break;
        case kIconPuzzle: g = ICON_LC_PUZZLE, drawn = nx::ui::kIconText; break;
        case kIconDownload: g = ICON_LC_DOWNLOAD, drawn = nx::ui::kIconPlus; break;
        default: g = ICON_LC_INFO; break;
    }
    if (!g_icons) return nx::ui::theme_icon(dl, drawn, c, r, col);
    const float fs = r * 1.7f;
    const ImVec2 ts = g_icons->CalcTextSizeA(fs, FLT_MAX, 0, g);
    dl->AddText(g_icons, fs, ImVec2(std::round(c.x - ts.x * 0.5f), std::round(c.y - ts.y * 0.5f)), col, g);
}

enum class Page { Welcome, Files, Install, Build, Done, Home, Settings, Online, Credits, Prepare, Preparing, Count };

Page g_page = Page::Welcome;

struct NavItem {
    ImGuiID id;
    ImGuiWindow* window;
    ImGuiID focus_scope;
    ImRect rect;
    int section;  // 0: header, 1: page, 2: footer
    bool primary;
    bool home_action;
};
std::vector<NavItem> g_nav_items;
int g_nav_section = -1;

void nav_item(bool primary = false, bool home_action = false) {
    if (g_nav_section < 0) return;
    ImGuiContext& ctx = *ImGui::GetCurrentContext();
    const ImGuiLastItemData& item = ctx.LastItemData;
    if (ctx.CurrentWindow->SkipItems || !item.ID || (item.ItemFlags & ImGuiItemFlags_Disabled)) return;
    g_nav_items.push_back({item.ID, ctx.CurrentWindow, ctx.CurrentFocusScopeId, item.NavRect, g_nav_section, primary, home_action});
}

void nav_focus(const NavItem& item) {
    ImGui::ClearActiveID();
    ImGui::FocusWindow(item.window);
    // Discard ImGui's pending default focus, which otherwise selects the header on the next frame.
    ImGuiContext& ctx = *ImGui::GetCurrentContext();
    ctx.NavInitRequest = false;
    ctx.NavInitRequestFromMove = false;
    ctx.NavInitResult.Clear();
    ImGui::NavMoveRequestCancel();
    ImRect relative(ImVec2(item.rect.Min.x - item.window->Pos.x, item.rect.Min.y - item.window->Pos.y),
                    ImVec2(item.rect.Max.x - item.window->Pos.x, item.rect.Max.y - item.window->Pos.y));
    ImGui::SetNavID(item.id, ImGuiNavLayer_Main, item.focus_scope, relative);
    ctx.NavIdIsAlive = true;
    ImGui::SetNavCursorVisible(true);
    ImGui::ScrollToRectEx(item.window, item.rect, ImGuiScrollFlags_KeepVisibleEdgeY);
}

void nav_move(int dx, int dy, int only_section = -1) {
    ImGuiContext& ctx = *ImGui::GetCurrentContext();
    auto current = std::find_if(g_nav_items.begin(), g_nav_items.end(), [&](const NavItem& item) {
        return item.id == ctx.NavId && (only_section < 0 || item.section == only_section);
    });
    if (current == g_nav_items.end()) return;
    // Settings form a vertical list even when some rows are outside the scrolling viewport.
    if (only_section < 0 && (g_page == Page::Settings || g_page == Page::Online || g_page == Page::Credits) && dy) {
        const int index = int(current - g_nav_items.begin());
        const int target_section = current->section == 1 ? 1 : (dy > 0 ? 1 : (current->section == 2 ? 1 : -1));
        for (int i = index + dy; i >= 0 && i < int(g_nav_items.size()); i += dy) {
            if (g_nav_items[i].section == target_section) {
                nav_focus(g_nav_items[i]);
                return;
            }
        }
        if (current->section == 1) {
            const int edge_section = dy < 0 ? 0 : 2;
            auto edge = std::find_if(g_nav_items.begin(), g_nav_items.end(),
                                     [edge_section](const NavItem& item) { return item.section == edge_section; });
            if (edge != g_nav_items.end()) nav_focus(*edge);
        }
        return;
    }
    // On the home screen Play leads to the first action; the actions are a list, and the top one (or left, from the
    // list beside the island panel) leads back to Play.
    if (g_page == Page::Home && (current->primary || current->home_action)) {
        if (current->primary && dy > 0) {
            auto first = std::find_if(g_nav_items.begin(), g_nav_items.end(), [](const NavItem& item) { return item.home_action; });
            if (first != g_nav_items.end()) nav_focus(*first);
            return;
        }
        const bool top = std::none_of(g_nav_items.begin(), g_nav_items.end(), [&](const NavItem& item) {
            return item.home_action && item.rect.Max.y <= current->rect.Min.y;
        });
        if (current->home_action && ((dy < 0 && top) || dx < 0)) {
            auto play = std::find_if(g_nav_items.begin(), g_nav_items.end(), [](const NavItem& item) { return item.primary; });
            if (play != g_nav_items.end()) nav_focus(*play);
            return;
        }
    }
    const ImVec2 from = current->rect.GetCenter();
    const NavItem* best = nullptr;
    float best_score = FLT_MAX;
    for (const NavItem& item : g_nav_items) {
        if (item.id == current->id || (only_section >= 0 && item.section != only_section)) continue;
        const ImVec2 to = item.rect.GetCenter();
        const float forward = dx ? (to.x - from.x) * dx : (to.y - from.y) * dy;
        if (forward <= 1.0f) continue;
        const float overlap = std::min(current->rect.Max.y, item.rect.Max.y) - std::max(current->rect.Min.y, item.rect.Min.y);
        const bool same_row = overlap >= 0.5f * std::min(current->rect.GetHeight(), item.rect.GetHeight());
        if ((dx && !same_row) || (dy && same_row)) continue;
        const float side = dx ? std::max(0.0f, std::max(current->rect.Min.y - item.rect.Max.y, item.rect.Min.y - current->rect.Max.y))
                              : std::max(0.0f, std::max(current->rect.Min.x - item.rect.Max.x, item.rect.Min.x - current->rect.Max.x));
        const float score = forward + side * 2.0f;
        if (score < best_score) best = &item, best_score = score;
    }
    if (best) nav_focus(*best);
}
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
std::vector<nx::save_profiles::ProfileInfo> g_save_profiles;
std::string g_save_profile_active;
bool g_saves_open = false;
double g_saves_since = 0;
std::string g_saves_delete_id;
void select_save_profile(const std::string& id);
std::vector<nx::mod_manager::ModInfo> g_mods;
bool g_mods_open = false;
double g_mods_since = 0;
std::string g_mods_delete_id;
bool g_mods_refresh = false;           // reload list after the grid finishes drawing
bool g_mods_refresh_wait_pad = false;  // gamepad: wait until FaceDown is released before reshuffling
void refresh_mods();
std::string g_banner;  // an error under the page title (a refused file or folder)
std::string g_toast;
double g_toast_until = 0;

// The cards files are given to: the files page's (the game folder, the pt-BR package) and the prepare page's (the
// extraction, add-on content, the game folder to write). slot_target() is the file dialog of each.
enum Slot { kSlotGame, kSlotMod, kSlotPrepGame, kSlotPrepDlc, kSlotPrepOut, kSlotCount };
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
enum Target { kPickGame, kPickMod, kPickInstall, kPickBuild, kPickSaveImport, kPickModImportFolder, kPickModImportZip,
              kPickPrepGame, kPickPrepDlc, kPickPrepOut };
Target slot_target(int slot) {
    switch (slot) {
        case kSlotMod: return kPickMod;
        case kSlotPrepGame: return kPickPrepGame;
        case kSlotPrepDlc: return kPickPrepDlc;
        case kSlotPrepOut: return kPickPrepOut;
        default: return kPickGame;
    }
}
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
// Snapshot for the frame: countdown was active before click-cancel, so the Play button does not also launch.
bool g_autostart_ui = false;
// After cancelling the countdown, keep Play green until the pointer leaves.
bool g_play_calm = false;
void start_setup(bool rebuild);
bool prereqs_block();
void play();
bool g_confirm_stop = false, g_confirm_quit = false;
bool g_quit_after_stop = false;

// The preparation of the game folder (prepare and preparing pages): what the player picked, and the probe's answer.
struct PrepPicks {
    std::vector<std::string> inputs;  // one folder (holding the ExeFS and the RomFS) or two (the ExeFS, the RomFS)
    std::vector<std::string> dlc;     // add-on content's RomFS folders
    std::string out;                  // the game folder to write
    bool in_place = false;            // the extraction folder becomes the game folder (its RomFS stays loose there)
    bool fps60 = true;
    std::vector<std::string> extra;   // platforms besides this computer's
    int serial = 0;                   // the probe's answer shown
    bool probed = false;              // `probe` is the answer to the picks as they are
    nx::prepare::ProbeResult probe;
};
PrepPicks g_prep;
bool g_prep_flow = false;             // the steps shown are the preparation's (your files, prepare, play)
Page g_prep_back = Page::Welcome;     // where Back goes from the prepare page
std::vector<std::string> g_prep_log;  // the preparing page's log
void prep_pick(int slot, const std::string& path);
void prep_open(const std::vector<std::string>& inputs, const std::string& out, bool in_place);
bool prep_offer(const std::string& folder);

// The game's config.ini (settings page).
Ini g_game_ini;

float S(float v) { return v * g_scale; }

// These controls draw their own focus outline but still use ImGui's gamepad navigation.
bool nav_card(const char* id, ImVec2 size, bool primary = false, bool home_action = false) {
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    bool pressed = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    nav_item(primary, home_action);
    return pressed;
}

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

void refresh_save_profiles();

// The game folder's library was built for another ACNHRecomp (the player would refuse it): prepared again first.
bool g_build_stale = false;

void set_build(const std::string& folder, bool remember) {
    g_build = Build();
    g_build_problem = find_build(folder, g_build);
    g_build_stale = *g_build_problem == 0 && !player_beside().empty() && prepare_library_stale(g_build.data);
    if (*g_build_problem == 0 && remember) {
        settings().set("launcher.build", folder);
        settings().save();
    }
    if (*g_build_problem == 0) {
        refresh_save_profiles();
        refresh_mods();
    }
}

void refresh_mods() {
    g_mods.clear();
    if (*g_build_problem != 0) return;
    g_mods = nx::mod_manager::list_mods(g_build.data);
}

void refresh_save_profiles() {
    g_save_profiles.clear();
    g_save_profile_active.clear();
    if (*g_build_problem != 0) return;
    std::string err;
    if (!nx::save_profiles::ensure_migrated(g_build.data, &err)) {
        g_banner = trf("launcher.profiles.switch_failed", err);
        return;
    }
    std::string want = settings().get("launcher.save_profile");
    if (want.empty()) want = nx::save_profiles::read_active_id(g_build.data);
    if (want.empty()) want = "default";
    g_save_profiles = nx::save_profiles::list_profiles(g_build.data);
    bool want_ok = false;
    for (const nx::save_profiles::ProfileInfo& p : g_save_profiles)
        if (p.id == want) want_ok = true;
    if (!want_ok) {
        want = nx::save_profiles::read_active_id(g_build.data);
        if (want.empty()) want = "default";
        settings().set("launcher.save_profile", want);
        settings().save();
    }
    std::string active = nx::save_profiles::read_active_id(g_build.data);
    if (want != active) {
        if (!nx::save_profiles::activate_profile(g_build.data, want, &err)) {
            g_banner = trf("launcher.profiles.switch_failed", err);
            return;
        }
        settings().set("launcher.save_profile", want);
        settings().save();
    }
    g_save_profile_active = nx::save_profiles::read_active_id(g_build.data);
    if (g_save_profile_active.empty()) g_save_profile_active = want;
    if (g_save_profiles.empty()) g_save_profiles = nx::save_profiles::list_profiles(g_build.data);
    if (settings().get("launcher.save_profile").empty()) {
        settings().set("launcher.save_profile", g_save_profile_active);
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
        case kPickSaveImport: SDL_ShowOpenFolderDialog(on_picked, user, g_window, where, false); break;
        case kPickModImportFolder: SDL_ShowOpenFolderDialog(on_picked, user, g_window, where, false); break;
        case kPickModImportZip: SDL_ShowOpenFileDialog(on_picked, user, g_window, zip, 2, where, false); break;
        case kPickPrepGame:
        case kPickPrepDlc: SDL_ShowOpenFolderDialog(on_picked, user, g_window, where, false); break;
        case kPickPrepOut: {
            static std::string out_dir;
            out_dir = parent_dir(g_prep.out);
            SDL_ShowOpenFolderDialog(on_picked, user, g_window, is_dir(out_dir) ? out_dir.c_str() : where, false);
            break;
        }
    }
}

// A game folder or package chosen for `kind`, by a dialog, a search result or a drop.
void use_file(Kind kind, const std::string& path) {
    if (kind == Kind::Extract) return prep_pick(kSlotPrepGame, path);
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
Kind slot_kind(int slot, const std::string&) {
    return slot == kSlotGame ? Kind::Game : slot == kSlotMod ? Kind::Mod : Kind::Extract;
}

int slot_at(ImVec2 p) {
    int lo = kSlotGame, hi = kSlotMod;
    if (g_page == Page::Prepare)
        lo = kSlotPrepGame, hi = kSlotPrepOut;
    else if (g_page != Page::Files)
        return -1;
    for (int k = lo; k <= hi; k++) {
        const SlotState& s = g_slot[k];
        if (p.x >= s.min.x && p.x < s.max.x && p.y >= s.min.y && p.y < s.max.y) return k;
    }
    return -1;
}

// Search: the game folders (or packages) in the usual folders and near what was chosen so far.
void search(int slot) {
    std::vector<std::string> near = {settings().get("files.last_dir"), g_game.path, g_ptbr.path};
    for (const std::string& i : g_prep.inputs) near.push_back(i);
    SlotState& s = g_slot[slot];
    s.found = search_files(slot_kind(slot, ""), near);
    s.open = true;
}

void use_build_folder(const std::string& folder) {
    Build b;
    const char* why = find_build(folder, b);
    // a release: the game's files not prepared yet (or prepared without this computer's code) are prepared here
    if (*why && !player_beside().empty() && prep_offer(folder)) return;
    if (std::strcmp(why, "launcher.err.game_unprepared") == 0 && !player_beside().empty() && prepare_built())
        why = "launcher.err.not_game";  // neither prepared nor extracted files: nothing to prepare from
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
            case kPickSaveImport: {
                std::string err;
                std::string id = nx::save_profiles::create_empty_profile(g_build.data, &err);
                if (id.empty()) {
                    g_banner = trf("launcher.saves.import_failed", err);
                    break;
                }
                if (nx::save_profiles::import_into_profile(g_build.data, id, first, &err)) {
                    toast(tr("launcher.saves.import_ok"));
                    select_save_profile(id);
                    refresh_save_profiles();
                } else
                    g_banner = trf("launcher.saves.import_failed", err);
                break;
            }
            case kPickModImportFolder:
            case kPickModImportZip: {
                std::string err, id;
                bool had_exefs = false;
                if (nx::mod_manager::import_mod(g_build.data, first, &id, &had_exefs, &err)) {
                    load_game_ini();
                    refresh_mods();
                    toast(had_exefs ? trf("launcher.mods.import_ok_exefs", id) : trf("launcher.mods.import_ok", id));
                } else
                    g_banner = trf("launcher.mods.import_failed", err);
                break;
            }
            case kPickPrepGame: prep_pick(kSlotPrepGame, first); break;
            case kPickPrepDlc: prep_pick(kSlotPrepDlc, first); break;
            case kPickPrepOut: prep_pick(kSlotPrepOut, first); break;
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

// Wrap to at most `max_lines` (ellipsis if longer). Used for mod tile titles.
std::string fit_wrapped_lines(const char* text, float wrap_w, int max_lines) {
    if (!text || !*text || max_lines < 1) return {};
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const float line_h = ImGui::GetTextLineHeight();
    const float limit = line_h * float(max_lines) + 0.5f;
    if (font->CalcTextSizeA(fs, FLT_MAX, wrap_w, text).y <= limit) return text;
    const std::string src(text);
    size_t lo = 0, hi = src.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        const std::string cand = src.substr(0, mid) + "...";
        if (font->CalcTextSizeA(fs, FLT_MAX, wrap_w, cand.c_str()).y <= limit) lo = mid;
        else hi = mid - 1;
    }
    // Prefer cutting on a space when the ellipsis still fits.
    size_t cut = lo;
    while (cut > 0 && src[cut - 1] != ' ') --cut;
    if (cut < lo / 2) cut = lo;
    while (cut > 0 && src[cut - 1] == ' ') --cut;
    return src.substr(0, cut) + "...";
}

// The sand: a sparse, fixed scatter of small dots, like the paths on the island.
void draw_background(ImVec2 ds) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), ds, kSand);
    uint32_t seed = 7;
    auto rnd = [&] { return float((seed = seed * 1664525u + 1013904223u) >> 8) / float(1u << 24); };
    const float cell = S(46);
    for (float y = 0; y < ds.y + cell; y += cell)
        for (float x = 0; x < ds.x + cell; x += cell) {
            float dx = rnd() * cell, dy = rnd() * cell;
            dl->AddCircleFilled(ImVec2(x + dx, y + dy), S(1.6f + rnd() * 1.8f), kSandDeep, 10);
        }
}

// The two-lobed leaf, the Importer's emblem, centred at c and `size` tall, turned by `turn` radians.
void draw_leaf(ImDrawList* dl, ImVec2 c, float size, float turn, ImU32 color = kGreen, ImU32 vein = kLeafDeep) {
    float cs = std::cos(turn), sn = std::sin(turn), w = size * 0.42f, h = size * 0.5f;
    auto P = [&](float x, float y) { return ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs); };
    dl->PathLineTo(P(0, h));
    dl->PathBezierCubicCurveTo(P(-w * 1.25f, h * 0.35f), P(-w * 0.9f, -h * 0.75f), P(0, -h));
    dl->PathBezierCubicCurveTo(P(w * 0.9f, -h * 0.75f), P(w * 1.25f, h * 0.35f), P(0, h));
    dl->PathFillConvex(color);
    dl->AddLine(P(0, h * 1.12f), P(0, -h * 0.62f), vein, size * 0.055f);
    for (float t : {0.35f, -0.1f}) {
        dl->AddLine(P(0, h * t), P(-w * 0.5f, h * (t - 0.32f)), vein, size * 0.04f);
        dl->AddLine(P(0, h * t), P(w * 0.5f, h * (t - 0.32f)), vein, size * 0.04f);
    }
}

// The board's stitched edge: a dashed rounded rectangle.
void draw_stitch(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, ImU32 color, float thick) {
    dl->PathRect(a, b, rounding);
    ImVector<ImVec2> pts;
    pts.swap(dl->_Path);
    dl->PathClear();
    if (pts.Size < 2) return;
    pts.push_back(pts[0]);
    const float on = S(6), period = S(12);
    float d = 0;
    for (int k = 1; k < pts.Size; k++) {
        ImVec2 p = pts[k - 1], q = pts[k];
        float len = std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y));
        for (float s = 0; s < len;) {
            float phase = std::fmod(d + s, period), step = std::min(len - s, phase < on ? on - phase : period - phase);
            if (phase < on) {
                float t0 = s / len, t1 = (s + step) / len;
                dl->AddLine(ImVec2(p.x + (q.x - p.x) * t0, p.y + (q.y - p.y) * t0),
                            ImVec2(p.x + (q.x - p.x) * t1, p.y + (q.y - p.y) * t1), color, thick);
            }
            s += std::max(step, 0.01f);
        }
        d += len;
    }
}

// A row of short dashes `w` wide (0: the content width): the Importer's divider between steps.
void dashes(float w = 0) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (w <= 0) w = ImGui::GetContentRegionAvail().x;
    float y = p.y + S(1.25f);
    for (float x = p.x; x + S(7) <= p.x + w; x += S(14))
        dl->AddRectFilled(ImVec2(x, y - S(1.25f)), ImVec2(x + S(7), y + S(1.25f)), kSandDeep, S(1.25f));
    ImGui::Dummy(ImVec2(w, S(2.5f)));
}

// A face with a lip under it (the Importer's buttons); pressed, it sinks onto the lip.
// `lift` (hovered) raises the face off its lip, which stays where it is.
void lip_shape(ImDrawList* dl, ImVec2 p, ImVec2 size, float lip, float rounding, ImU32 face, ImU32 lip_col,
               ImU32 border, float border_w, bool down, float lift = 0) {
    if (down) lift = 0;
    if (!down)
        dl->AddRectFilled(ImVec2(p.x, p.y + lip - lift), ImVec2(p.x + size.x, p.y + size.y + lip), lip_col, rounding);
    ImVec2 a(p.x, p.y + (down ? lip : -lift)), b(a.x + size.x, a.y + size.y);
    dl->AddRectFilled(a, b, face, rounding);
    if (border_w > 0) dl->AddRect(a, b, border, rounding, 0, border_w);
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
    return ImGui::ColorConvertFloat4ToU32(ImLerp(col4(a), col4(b), std::clamp(t, 0.0f, 1.0f)));
}

// The last item's hover, eased in and out (about 0.1 s) and kept per item in the window's storage; a hovered
// clickable item shows the pointing hand. Frames are drawn at full rate while one is on its way (pages_busy).
bool g_animating = false;
float hover_anim(bool clickable = true) {
    bool hov = clickable && ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    float* v = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID() ^ 0x5eedu, 0.0f);
    float target = hov ? 1.0f : 0.0f;
    *v += (target - *v) * std::min(1.0f, ImGui::GetIO().DeltaTime * 18.0f);
    if (std::fabs(target - *v) < 0.01f) *v = target;
    else g_animating = true;
    return *v;
}

// A dropdown or text field: the pointing hand and a lagoon edge fading in on hover (`edge`).
void frame_hover(bool edge) {
    if (ImGui::GetCurrentContext()->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) return;
    float t = hover_anim();
    if (edge && t > 0)
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                            mix(kSandDeep, kMint, t), ImGui::GetStyle().FrameRounding, 0, S(2));
}

bool nav_focused();

// An on/off choice as the Importer's platform chips: a pill, pale leaf with a leaf edge and a filled check when on,
// paper with a sand edge and an empty ring when off, the change fading over 0.16 s. True when clicked (`*v` flipped).
float toggle_width(const char* label) {
    return S(10) + 2 * S(8.5f) + S(6) + measure(g_body, font_size(g_body) * 0.87f, label, -1).x + S(14);
}

bool toggle(const char* label, bool* v) {
    float fs = font_size(g_body) * 0.87f, ir = S(8.5f);
    ImVec2 ts = measure(g_body, fs, label, -1);
    ImVec2 size(toggle_width(label), std::max(ts.y, 2 * ir) + S(14));
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = nav_card(label, size);
    if (clicked) *v = !*v;
    hover_anim();
    float* on = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID() ^ 0x70661eu, *v ? 1.0f : 0.0f);
    float target = *v ? 1.0f : 0.0f;
    *on += (target - *on) * std::min(1.0f, ImGui::GetIO().DeltaTime / 0.16f * 3);
    if (std::fabs(target - *on) < 0.01f) *on = target;
    else g_animating = true;
    float t = *on;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + size.x, p.y + size.y);
    bool focus = nav_focused();
    dl->AddRectFilled(p, q, mix(kBoard, kLeafPale, t), size.y * 0.5f);
    dl->AddRect(p, q, focus ? kLagoonDeep : mix(kSandDeep, kGreen, t), size.y * 0.5f, 0, focus ? S(3) : S(1.5f));
    ImVec2 c(p.x + S(10) + ir, p.y + size.y * 0.5f);
    if (t > 0) dl->AddCircleFilled(c, ir * t, kLeafDeep);
    if (t < 1) dl->AddCircle(c, ir - S(1), mix(kInkSoft, kLeafDeep, t), 0, S(1.8f));
    if (t > 0.5f) draw_icon(dl, nx::ui::kIconCheck, c, ir * 0.75f, kWhite);
    dl->AddText(g_body, fs, ImVec2(c.x + ir + S(6), p.y + (size.y - ts.y) * 0.5f), kInk, label);
    return clicked;
}

// The toggle chips' mark alone, `size` square: a filled leaf circle with a check when on, an empty ring when off
// (lagoon as the pointer nears). For on/off inside a card (the save files, the mods). True when clicked (`*v` flipped).
bool check_mark(const char* id, bool* v, float size) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = nav_card(id, ImVec2(size, size));
    if (clicked) *v = !*v;
    const bool off = ImGui::GetCurrentContext()->LastItemData.ItemFlags & ImGuiItemFlags_Disabled;
    float t = off ? 0.0f : hover_anim();
    bool focus = !off && nav_focused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    float r = size * 0.36f;
    if (*v) {
        dl->AddCircleFilled(c, r, off ? shade(kLeafDeep, 1.4f) : kLeafDeep);
        draw_icon(dl, nx::ui::kIconCheck, c, r * 0.75f, kWhite);
    } else {
        dl->AddCircle(c, r - S(1), mix(kInkSoft, kLagoonDeep, t), 0, S(1.8f));
    }
    if (focus) dl->AddCircle(c, r + S(4), kLagoonDeep, 0, S(2.5f));
    return clicked;
}

// Keyboard or controller focus (not a mouse click's): the Importer's lagoon outline.
bool nav_focused() { return ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible; }

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
    if (icon >= 0) draw_icon(dl, icon, c, r * 0.8f, kWhite);
}

float button_width(const char* label) { return std::max(S(120), ImGui::CalcTextSize(label).x + S(40)); }

// A pill with a lip: kGreen is the one action that matters (sun yellow), kWhite and kMint the quiet paper ones, any
// other colour its own face.
bool button(const char* label, ImU32 color = kMint, bool enabled = true, float w = 0) {
    const float lip = S(3.5f);
    ImVec2 size(w > 0 ? w : button_width(label), ImGui::GetFrameHeight());
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    bool r = ImGui::InvisibleButton(label, ImVec2(size.x, size.y + lip), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    nav_item();
    ImGui::EndDisabled();
    const bool quiet = color == kWhite || color == kMint;
    ImU32 face = color == kGreen ? kYellow : quiet ? kBoard : color;
    ImU32 lip_col = color == kGreen ? kSunDeep : quiet ? kSandDeep : shade(face, 0.8f);
    ImU32 ink = ink_on(face);
    const float t = enabled ? hover_anim() : 0.0f;
    ImU32 border = lip_col;
    if (!enabled) face = kSandDeep, lip_col = rgb(224, 211, 172), border = lip_col, ink = kInkSoft;
    else face = mix(face, quiet ? kBoard : shade(face, 1.15f), t), border = quiet ? mix(lip_col, kMint, t) : lip_col;
    const bool focus = enabled && nav_focused(), down = enabled && ImGui::IsItemActive();
    const float lift = down ? 0 : t * S(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    lip_shape(dl, p, size, lip, size.y * 0.5f, face, lip_col, focus ? kLagoonDeep : border, focus ? S(3) : S(1.5f), down,
              lift);
    const char* end = ImGui::FindRenderedTextEnd(label);
    ImVec2 ts = ImGui::CalcTextSize(label, end);
    float y = p.y + (down ? lip : -lift) + (size.y - ts.y) * 0.5f;
    dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, y), ink, label, end);
    return r && enabled;
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
    // A paper pill with a sand edge, room at its left for the globe (ribbon)
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col4(kBoard));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col4(kSand));
    ImGui::PushStyleColor(ImGuiCol_Button, col4(kBoard));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col4(kSand));
    ImGui::PushStyleColor(ImGuiCol_Border, col4(kSandDeep));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, S(1.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(40), ImGui::GetStyle().FramePadding.y));
    const bool open = ImGui::BeginCombo("##lang", shown.c_str(), ImGuiComboFlags_HeightLarge);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (open) {
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
    nav_item();
    frame_hover(true);
    ImGui::PopStyleColor(4);
}

// A newer release (desktop.cpp): what to say and the button's label, from where its install stands (in place when it
// can be, update_can_install, else the release page; then the restart into it). False when there is none.
bool update_text(std::string& msg, const char*& get, bool& busy) {
    const UpdateInfo u = update_info();
    if (!u.available) return false;
    const UpdateProgress pr = update_progress();
    busy = pr.stage != UpdateStage::Idle && pr.stage != UpdateStage::Done && pr.stage != UpdateStage::Failed;
    const bool in_place = update_can_install() || pr.stage != UpdateStage::Idle;
    msg = trf("launcher.update.available", u.version);
    get = tr(in_place ? "launcher.update.install" : "launcher.update.get");
    switch (pr.stage) {
        case UpdateStage::Downloading: msg = tr("launcher.update.downloading"); break;
        case UpdateStage::Checking: msg = tr("launcher.update.checking"); break;
        case UpdateStage::Unpacking:
        case UpdateStage::Installing: msg = tr("launcher.update.installing"); break;
        case UpdateStage::Done: msg = trf("launcher.update.installed", u.version), get = tr("launcher.update.restart"); break;
        case UpdateStage::Failed: msg = trf("launcher.update.failed", pr.error), get = tr("launcher.update.get"); break;
        default: break;
    }
    return true;
}

// The update's button: install, restart into it, or the release page.
bool update_button(const char* get, bool busy) {
    if (!button(get, kGreen, !busy)) return false;
    const UpdateInfo u = update_info();
    const UpdateProgress pr = update_progress();
    const bool in_place = update_can_install() || pr.stage != UpdateStage::Idle;
    if (pr.stage == UpdateStage::Done) update_restart();
    else if (pr.stage != UpdateStage::Failed && in_place) update_install_start();
    else SDL_OpenURL(u.url.c_str());
    return true;
}

bool g_update_in_header = false;  // ribbon() shows the new release, so the home page does not

// The Importer's header: the leaf, the name in bark ink and the tagline under it; the language as a pill at the right.
void ribbon() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = S(64), w = ImGui::GetContentRegionAvail().x;
    draw_leaf(dl, ImVec2(p.x + S(24), p.y + h * 0.5f), S(46), -0.5f);
    // Measured in the combo's own font, with room for the frame padding, the arrow and the globe.
    float combo_w = measure(g_body, font_size(g_body), language_shown().c_str(), -1).x + ImGui::GetFrameHeight() +
                    ImGui::GetStyle().FramePadding.x * 2 + S(48);
    combo_w = std::clamp(combo_w, S(150), g_portrait ? w * 0.5f : S(420));
    float tsz = font_size(g_title) * 1.05f;
    ImVec2 ts = measure(g_title, tsz, "ACNHRecomp", -1);
    float x = p.x + S(58);
    const char* sub = tr("launcher.subtitle");
    float ssz = font_size(g_body) * 0.82f;
    ImVec2 ss = measure(g_body, ssz, sub, -1);
    bool show_sub = ss.x < w - (x - p.x) - combo_w - S(16);
    // A newer release (desktop.cpp): a sand pill between the name and the language, its text and Download. The
    // tagline gives way to it, then the text (the version alone); where even that does not fit, the home page says it.
    g_update_in_header = false;
    std::string full, shortv;
    const char* get = "";
    bool busy = false;
    float nfs = font_size(g_body) * 0.82f, bw = 0, gap = S(16), right = p.x + w - combo_w - gap;
    std::string* note = nullptr;
    if (update_text(full, get, busy)) {
        const UpdateInfo u = update_info();
        shortv = update_progress().stage == UpdateStage::Idle ? u.version : full;
        bw = button_width(get);
        auto need = [&](const std::string& t) { return S(14) + S(20) + S(8) + measure(g_body, nfs, t.c_str(), -1).x + S(12) + bw + S(8); };
        for (int pass = 0; pass < 4 && !note; pass++) {
            bool sub_on = show_sub && pass % 2 == 0;
            std::string& t = pass < 2 ? full : shortv;
            float left = x + std::max(ts.x, sub_on ? ss.x : 0.0f) + gap;
            if (right - left >= need(t)) note = &t, show_sub = sub_on;
        }
    }
    float ty = show_sub ? p.y + (h - ts.y - ss.y) * 0.5f : p.y + (h - ts.y) * 0.5f;
    dl->AddText(g_title, tsz, ImVec2(x, ty), kInk, "ACNHRecomp");
    if (show_sub) dl->AddText(g_body, ssz, ImVec2(x + S(1), ty + ts.y - S(2)), kInkSoft, sub);
    if (note) {
        g_update_in_header = true;
        float fh = ImGui::GetFrameHeight(), ph = fh + S(14);
        ImVec2 ns = measure(g_body, nfs, note->c_str(), -1);
        float pw = S(14) + S(20) + S(8) + ns.x + S(12) + bw + S(8);
        ImVec2 a(right - pw, p.y + (h - ph) * 0.5f), b(right, a.y + ph);
        dl->AddRectFilled(a, b, kSand, ph * 0.5f);
        dl->AddRect(a, b, kSandDeep, ph * 0.5f, 0, S(1.5f));
        ImVec2 c(a.x + S(14) + S(10), a.y + ph * 0.5f);
        dl->AddCircleFilled(c, S(10), kOrange);
        draw_icon(dl, kIconDownload, c, S(6), kWhite);
        dl->AddText(g_body, nfs, ImVec2(c.x + S(18), a.y + (ph - ns.y) * 0.5f), kInk, note->c_str());
        if (note == &shortv && ImGui::IsMouseHoveringRect(a, b)) ImGui::SetTooltip("%s", full.c_str());
        ImGui::SetCursorScreenPos(ImVec2(b.x - S(8) - bw, a.y + (ph - fh) * 0.5f - S(1.5f)));
        update_button(get, busy);
    }
    // The language, always reachable.
    ImVec2 cp(p.x + w - combo_w, p.y + (h - ImGui::GetFrameHeight()) * 0.5f);
    ImGui::SetCursorScreenPos(cp);
    language_combo(combo_w);
    draw_icon(dl, nx::ui::kIconGlobe, ImVec2(cp.x + S(22), cp.y + ImGui::GetFrameHeight() * 0.5f), S(11), kInk);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + S(8)));
    dashes();
    ImGui::Dummy(ImVec2(0, S(4)));
}

// The steps of the setup (1..4) or, in the preparation's flow (g_prep_flow), of the preparation (1..3), the current
// one highlighted.
void steps(int current) {
    const char* setup[] = {tr("launcher.step.files"), tr("launcher.step.install"), tr("launcher.step.build"),
                           tr("launcher.step.done")};
    const char* prep[] = {tr("launcher.step.files"), tr("launcher.step.prepare"), tr("launcher.step.done")};
    const char* const* labels = g_prep_flow ? prep : setup;
    const int n = g_prep_flow ? 3 : 4;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, r = S(15), h = S(40);
    float seg = w / float(n);
    const float fs = font_size(g_body) * 0.85f;
    for (int k = 0; k < n; k++) {
        float cx = p.x + seg * k + r + S(2), cy = p.y + h * 0.5f;
        bool label = !g_portrait || k == current;
        float lw = label ? measure(g_body, fs, labels[k], -1).x + S(16) : 0;
        float x0 = cx + r + S(6) + lw, x1 = p.x + seg * (k + 1) - S(6);
        if (k < n - 1 && x1 > x0)
            dl->AddRectFilled(ImVec2(x0, cy - S(1.5f)), ImVec2(x1, cy + S(1.5f)), k < current ? kGreen : kSandDeep, S(1.5f));
        // The Importer's step badges: a number on sand, the current one on sun yellow, a leaf once done.
        if (k < current) {
            dl->AddCircleFilled(ImVec2(cx, cy), r, kLeafPale);
            draw_leaf(dl, ImVec2(cx, cy), r * 1.4f, 0.35f);
        } else {
            dl->AddCircleFilled(ImVec2(cx, cy), r, k == current ? kYellow : kSand);
            dl->AddCircle(ImVec2(cx, cy), r - S(1), k == current ? kSunDeep : kSandDeep, 0, S(2));
            char n[4];
            std::snprintf(n, sizeof n, "%d", k + 1);
            ImVec2 ns = measure(g_title, fs, n, -1);
            dl->AddText(g_title, fs, ImVec2(cx - ns.x * 0.5f, cy - ns.y * 0.5f), k == current ? kInk : kInkSoft, n);
        }
        if (label) {
            ImVec2 ls = measure(g_body, fs, labels[k], -1);
            dl->AddText(g_body, fs, ImVec2(cx + r + S(8), cy - ls.y * 0.5f), k == current ? kInk : kInkSoft, labels[k]);
        }
    }
    ImGui::Dummy(ImVec2(w, h + S(4)));
}

// ---- welcome --------------------------------------------------------------------------------------------------

// A card on the board: sand with a lip under it, paper when hovered, a lagoon edge under keyboard or controller focus.
void card_face(ImDrawList* dl, ImVec2 p, ImVec2 size, float rounding, bool hov, bool focus) {
    lip_shape(dl, p, ImVec2(size.x, size.y - S(4)), S(4), rounding, hov ? kBoard : kSand, kSandDeep,
              focus ? kLagoonDeep : kSandDeep, focus ? S(3) : S(1.5f), false);
}

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
    bool clicked = nav_card(id, size);
    ImGui::EndDisabled();
    if (c.enabled) hover_anim();
    bool hov = c.enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + size.x, p.y + size.y);
    card_face(dl, p, size, S(26), hov, c.enabled && nav_focused());
    float pad = S(24), r = S(34), inner = size.x - 2 * pad;
    ImU32 icol = c.enabled ? c.color : kGrey;
    dl->AddCircleFilled(ImVec2(p.x + pad + r, p.y + pad + r), r, icol);
    draw_icon(dl, c.icon, ImVec2(p.x + pad + r, p.y + pad + r), r * 0.6f, kWhite);
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
        dl->AddCircleFilled(a, S(16), hov ? kYellow : kSandDeep);
        draw_icon(dl, nx::ui::kIconPlay, a, S(11), kInk);
    }
    return clicked && c.enabled;
}

void go_files() {
    g_banner.clear();
    g_prep_flow = false;  // the setup's steps
    g_page = Page::Files;
}

// A release: the player is beside the launcher and plays a prepared game folder. Two ways in: prepare it here from the
// ExeFS and RomFS extracted with an emulator, or choose one prepared before (by ACNHRecomp or ACNHRecomp-Importer).
void page_welcome_play() {
    heading(tr("launcher.welcome.title"), tr("launcher.welcome.text"));
    const bool can_prepare = prepare_built();
    Card prep{nx::ui::kIconLeaf, kGreen, tr("launcher.welcome.prepare"), tr("launcher.welcome.prepare.desc"),
              can_prepare && !prepare_tools().problem.empty() ? tr("launcher.prep.tools_missing_short") : nullptr,
              can_prepare};
    Card play{nx::ui::kIconFolder, kBlue, tr("launcher.welcome.play"), tr("launcher.welcome.play.desc"), nullptr, true};
    float avail = ImGui::GetContentRegionAvail().x, gap = S(20);
    auto open_prepare = [] { g_prep_back = Page::Welcome, prep_open({}, "", false); };
    if (g_portrait) {
        if (card("##prepare", prep, ImVec2(avail, card_height(prep, avail)))) open_prepare();
        ImGui::Dummy(ImVec2(0, S(2)));
        if (card("##play", play, ImVec2(avail, card_height(play, avail)))) pick(kPickBuild);
    } else {
        float w = (avail - gap) * 0.5f;
        float h = std::max(card_height(prep, w), card_height(play, w));
        if (card("##prepare", prep, ImVec2(w, h))) open_prepare();
        ImGui::SameLine(0, gap);
        if (card("##play", play, ImVec2(w, h))) pick(kPickBuild);
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    text_small(tr("launcher.welcome.extract"));
    if (button(tr("launcher.help.discord"), kBlue)) SDL_OpenURL(kDiscordUrl);
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
    ImGui::PushStyleColor(ImGuiCol_Button, col4(kSandDeep));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    bool r = ImGui::Button(label, ImVec2(size, size));
    nav_item();
    frame_hover(false);
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

bool chosen(const std::string& path) {
    for (const std::string& i : g_prep.inputs)
        if (path == i) return true;
    return path == g_game.path || path == g_ptbr.path;
}

// The search's results under a card, `w` wide: name, size and folder, the check, and Use (Add for the DLC).
void found_list(int slot, float w) {
    SlotState& s = g_slot[slot];
    if (!s.open) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kSandDeep));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(10)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(14));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, S(290)));
    // A few results fit: no scrollbar (an auto-sized child can show one for its first frames).
    ImGui::BeginChild("##found", ImVec2(w, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
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
    bool can_search = true;  // Search beside Browse…
};

// A card of the files page: its icon (coloured by its files' checks), title, Required or Optional and what it is,
// with Search and Browse… at the right (under the files when narrow); then `rows` draws its files at the given
// width, and the search's results follow. A file dropped on the card goes to it.
void file_card(const FileCard& c, Status st, const std::function<void(float)>& rows) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kPaper));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(g_portrait ? 14 : 20), S(16)));
    ImGui::BeginChild(c.id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10), S(6)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float avail = ImGui::GetContentRegionAvail().x, sp = ImGui::GetStyle().ItemSpacing.x;
    const char* search_l = tr("launcher.search");
    const char* browse_l = tr("launcher.browse");
    const float sw = c.can_search ? button_width(search_l) : 0, bw = button_width(browse_l);
    const float btns = c.can_search ? sw + sp + bw : bw;
    const bool narrow = avail < btns + S(380);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float x0 = ImGui::GetCursorPosX(), r = S(g_portrait ? 20 : 24);
    // The icon, coloured by the checks, with their mark.
    ImVec2 ic(p0.x + r, p0.y + r);
    dl->AddCircleFilled(ic, r, st == Status::Empty ? (c.required ? kBrown : kGrey) : status_color(st));
    draw_icon(dl, c.icon, ic, r * 0.6f, kWhite);
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
        if (c.can_search) {
            do_search = button(search_l, kWhite, true, sw);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("launcher.search.where"));
            ImGui::SameLine();
        }
        do_browse = button(browse_l, kMint, true, bw);
    }
    // The files, under the title (from the card's edge on a phone).
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + head_h + S(10)));
    const float indent = avail > S(460) ? tx : 0, rw = avail - indent;
    if (indent > 0) ImGui::Indent(indent);
    rows(rw);
    if (narrow) {
        ImGui::Dummy(ImVec2(0, S(2)));
        float half = c.can_search ? (rw - sp) * 0.5f : rw;
        if (c.can_search) {
            do_search = button(search_l, kWhite, true, half);
            ImGui::SameLine();
        }
        do_browse = button(browse_l, kMint, true, half);
    }
    if (g_slot[c.slot].open) {
        ImGui::Dummy(ImVec2(0, S(2)));
        found_list(c.slot, rw);
    }
    if (indent > 0) ImGui::Unindent(indent);
    if (do_search) search(c.slot);
    if (do_browse) pick(slot_target(c.slot));
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
    // where the game folder comes from: prepared here from the files the player extracted with their emulator
    ImGui::Dummy(ImVec2(0, S(6)));
    text_small(tr("launcher.files.prepare"));
    if (g_game.check.unprepared && prepare_built()) {
        if (button(tr("launcher.prep.this_folder"), kGreen))
            g_prep_back = Page::Files, prep_open({g_game.path}, "", true);
        ImGui::SameLine();
    }
    if (button(tr("launcher.help.discord"), kBlue)) SDL_OpenURL(kDiscordUrl);
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
        draw_icon(dl, nx::ui::kIconFolder, ImVec2(p.x + r, p.y + r), r * 0.6f, kWhite);
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
    g_prep_flow = false;
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
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kSandDeep, h * 0.5f);
    float fw = std::max(h, w * fraction);
    if (fraction > 0) dl->AddRectFilled(p, ImVec2(p.x + fw, p.y + h), failed ? kRed : kMint, h * 0.5f);
    char pct[16];
    std::snprintf(pct, sizeof pct, "%d%%", int(fraction * 100 + 0.5f));
    ImVec2 ts = ImGui::CalcTextSize(pct);
    dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), kInk, pct);
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

// The setup's (or the preparation's) output, following the end unless scrolled up.
void log_view(float height, const std::vector<std::string>& lines = g_log) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(rgb(92, 70, 50)));
    ImGui::BeginChild("##log", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Text, col4(rgb(255, 244, 214)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(2)));
    ImGui::SetWindowFontScale(0.78f);
    bool follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4;
    ImGuiListClipper clip;
    clip.Begin(int(lines.size()));
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) ImGui::TextUnformatted(lines[size_t(i)].c_str());
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
        bool details_open = ImGui::CollapsingHeader(tr("launcher.build.details"));
        nav_item();
        if (details_open)
            log_view(std::max(S(160), ImGui::GetContentRegionAvail().y - S(4)));
        ImGui::EndGroup();
    } else {
        stage_list(s, failed);
        ImGui::Dummy(ImVec2(0, S(2)));
        if (failed) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        bool details_open = ImGui::CollapsingHeader(tr("launcher.build.details"));
        nav_item();
        if (details_open)
            log_view(std::max(S(200), ImGui::GetContentRegionAvail().y - S(4)));
    }
}

std::string log_text(const std::vector<std::string>& lines = g_log) {
    std::string t;
    for (const std::string& l : lines) t += l + "\n";
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

// ---- prepare: the game folder from the ExeFS and RomFS the player extracted (prepare_job.h) ---------------------

const BuildContents& contents(bool refresh);

std::string lower_text(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

Status severity_status(nx::prepare::Severity s) {
    return s == nx::prepare::Severity::Error ? Status::Error
         : s == nx::prepare::Severity::Warning ? Status::Warning : Status::Ok;
}

// A launcher text with its "{}" filled in order.
std::string trn(const char* key, const std::vector<std::string>& args) { return translate(key, args, key); }

std::string message_text(const nx::prepare::Message& m) { return translate(m.key, m.args, m.text); }

// The add-on folder picked that a finding is about, -1 for the game's files.
int message_dlc(const nx::prepare::Message& m) {
    for (size_t i = 0; i < g_prep.dlc.size(); i++)
        if (path_within(m.path, g_prep.dlc[i])) return int(i);
    return -1;
}

void prep_probe() {
    g_prep.probed = false;
    if (!g_prep.inputs.empty()) probe_start(g_prep.inputs, g_prep.dlc);
}

void prep_poll() {
    if (probe_poll(g_prep.serial, g_prep.probe)) g_prep.probed = true;
}

// The folder holding the RomFS found (as romfs/, extract/romfs/, or a game folder's romfs.pak): the game folder when
// it is prepared where it is. "" when the RomFS is no such folder's (two picks named otherwise, one folder holding all).
std::string prep_romfs_home() {
    if (!g_prep.probed || g_prep.probe.romfs.path.empty()) return "";
    std::string r = g_prep.probe.romfs.path;
    while (r.size() > 1 && (r.back() == '/' || r.back() == '\\')) r.pop_back();
    const std::string name = lower_text(file_name(r));
    if (name != "romfs" && name != "romfs.pak") return "";
    std::string d = parent_dir(r);
    if (name == "romfs" && lower_text(file_name(d)) == "extract") d = parent_dir(d);
    if (same_path(d, g_prep.probe.exefs)) return "";  // the ExeFS folder itself is no game folder
    return d;
}

// Where the game folder is written, and whether that is where the RomFS already is (nothing copied).
std::string prep_out_dir(bool* here = nullptr) {
    const std::string home = prep_romfs_home();
    const bool in_place = g_prep.in_place && !home.empty();
    const std::string out = in_place ? home : g_prep.out;
    if (here) *here = !home.empty() && (in_place || same_path(out, home));
    return out;
}

std::vector<std::string> prep_platforms_chosen() {
    std::vector<std::string> out;
    const std::string host = prepare_host_platform();
    for (const std::string& p : prepare_platforms())
        if (p == host || std::find(g_prep.extra.begin(), g_prep.extra.end(), p) != g_prep.extra.end()) out.push_back(p);
    return out;
}

const char* platform_label(const std::string& p) {
    if (p == "windows-x64") return tr("launcher.prep.platform.windows");
    if (p == "linux-x64") return tr("launcher.prep.platform.linux");
    if (p == "macos-arm64") return tr("launcher.prep.platform.macos");
    if (p == "android-arm64") return tr("launcher.prep.platform.android");
    return p.c_str();
}

// The game folder's check: usable or why not, the free space against what the preparation needs.
FileCheck prep_out_check() {
    bool here = false;
    const std::string out = prep_out_dir(&here);
    if (out.empty()) return {};
    if (is_file(out)) return {Status::Error, tr("launcher.prep.out.file")};
    if (!here && dir_has_other_things(out)) return {Status::Error, tr("launcher.prep.out.busy")};
    if (g_prep.probed && !here && path_within(out, g_prep.probe.romfs.path))
        return {Status::Error, tr("launcher.prep.out.in_romfs")};
    const uint64_t free = free_space(out);
    std::string again = is_prepared_folder(out) ? std::string(" · ") + tr("launcher.prep.out.again") : std::string();
    if (!g_prep.probed || !g_prep.probe.ok) return {Status::Ok, trf("launcher.install.free", human_size(free)) + again};
    const PrepareSpace sp = prepare_space(g_prep.probe, std::max<size_t>(1, prep_platforms_chosen().size()), here);
    if (free < sp.need)
        return {Status::Error, trn("launcher.prep.out.space_low", {human_size(sp.need), human_size(free)})};
    return {Status::Ok, trn("launcher.prep.out.space", {human_size(free), human_size(sp.need), human_size(sp.result)}) + again};
}

// What the prepare page lacks before Prepare can start; nullptr when it can.
const char* prep_not_ready() {
    if (!prepare_tools().problem.empty()) return tr("launcher.prep.why.tools");
    if (g_prep.inputs.empty()) return tr("launcher.prep.why.game");
    if (!g_prep.probed) return tr("launcher.prep.checking");
    if (!g_prep.probe.ok) return tr("launcher.prep.why.fix");
    const Status out = prep_out_check().status;
    if (out == Status::Error || out == Status::Empty) return tr("launcher.prep.why.out");
    if (prep_platforms_chosen().empty()) return tr("launcher.prep.why.platform");
    return nullptr;
}

void prep_pick(int slot, const std::string& path) {
    g_banner.clear();
    settings().set("files.last_dir", parent_dir(path));
    settings().save();
    if (slot == kSlotPrepOut) {
        // A folder holding other things (Documents, a games folder): the game folder goes inside it.
        g_prep.out = dir_has_other_things(path) ? path_join(path, "ACNHRecomp Game") : path;
        g_prep.in_place = false;
        return;
    }
    if (slot == kSlotPrepDlc) {
        if (std::find(g_prep.dlc.begin(), g_prep.dlc.end(), path) == g_prep.dlc.end()) g_prep.dlc.push_back(path);
    } else {
        // A second folder joins the first when the first is one half only (an ExeFS, or a RomFS); else it replaces it.
        bool join = false;
        if (g_prep.inputs.size() == 1 && g_prep.inputs[0] != path) {
            nx::prepare::ProbeOptions po;
            po.verify_nso = false, po.walk_romfs = false;
            const nx::prepare::ProbeResult r = nx::prepare::probe({g_prep.inputs[0]}, po);
            join = r.has("prepare.input.no_exefs") || r.has("prepare.input.no_romfs");
        }
        if (join)
            g_prep.inputs.push_back(path);
        else
            g_prep.inputs = {path};
        g_slot[kSlotPrepGame].open = false;
    }
    prep_probe();
}

void prep_open(const std::vector<std::string>& inputs, const std::string& out, bool in_place) {
    Ini& s = settings();
    g_prep.inputs = inputs;
    g_prep.dlc.clear();
    g_prep.in_place = in_place;
    g_prep.out = !out.empty() ? out : s.get("prepare.out", default_game_dir());
    g_prep.fps60 = s.get("prepare.60fps", "1") != "0";
    g_prep.extra.clear();
    const std::string extra = s.get("prepare.platforms");
    for (size_t a = 0; a < extra.size();) {
        size_t b = extra.find(',', a);
        if (b == std::string::npos) b = extra.size();
        if (b > a) g_prep.extra.push_back(extra.substr(a, b - a));
        a = b + 1;
    }
    for (int k = kSlotPrepGame; k <= kSlotPrepOut; k++) g_slot[k].open = false;
    g_prep_flow = true;
    g_banner.clear();
    prepare_tools(true);  // again: the player may have put them back
    prep_probe();
    g_page = Page::Prepare;
}

// A folder chosen as the game folder that is not one yet (a release): the files of a game the player extracted are
// prepared where they are; a folder prepared without this computer's code (or for another ACNHRecomp) is prepared
// again, keeping what is right. False when it is neither (the reason is then said as before).
bool prep_offer(const std::string& folder) {
    if (!prepare_built() || !is_dir(folder)) return false;
    const Page back = g_page == Page::Prepare || g_page == Page::Preparing ? Page::Welcome : g_page;
    if (is_prepared_folder(folder)) {
        g_prep_back = back;
        prep_open({folder}, folder, false);
        return true;
    }
    nx::prepare::ProbeOptions po;
    po.verify_nso = false, po.walk_romfs = false;
    const nx::prepare::ProbeResult r = nx::prepare::probe({folder}, po);
    if (r.exefs.empty() && r.romfs.path.empty()) return false;
    g_prep_back = back;
    prep_open({folder}, "", true);
    return true;
}

void prep_start() {
    PrepareArgs a;
    a.inputs = g_prep.inputs;
    a.dlc = g_prep.dlc;
    bool here = false;
    a.out = prep_out_dir(&here);
    a.in_place = g_prep.in_place && here;
    a.platforms = prep_platforms_chosen();
    a.variant_60fps = g_prep.fps60;
    std::string err;
    g_prep_log.clear();
    if (!prepare_start(a, err)) {
        g_banner = trf("launcher.prep.start_failed", err);
        return;
    }
    Ini& s = settings();
    if (!here) s.set("prepare.out", a.out);
    s.set("prepare.60fps", g_prep.fps60 ? "1" : "0");
    std::string extra;
    for (const std::string& p : g_prep.extra) extra += (extra.empty() ? "" : ",") + p;
    s.set("prepare.platforms", extra);
    s.save();
    g_banner.clear();
    g_prep_flow = true;
    g_page = Page::Preparing;
}

// A turning arc, as the stage running.
void spinner(ImDrawList* dl, ImVec2 c, float r) {
    float t = float(ImGui::GetTime());
    dl->AddCircle(c, r, kGrey, 0, S(3));
    dl->PathArcTo(c, r, t * 5.0f, t * 5.0f + 2.0f, 16);
    dl->PathStroke(kOrange, 0, S(3));
}

// One finding of the probe, `w` wide: its mark and its text, translated.
void message_row(const nx::prepare::Message& m, float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x0 = ImGui::GetCursorPosX(), r = S(7), lh = ImGui::GetTextLineHeight() * 0.84f;
    const Status st = severity_status(m.severity);
    if (m.severity == nx::prepare::Severity::Info)
        dl->AddCircleFilled(ImVec2(p.x + r, p.y + lh * 0.5f + S(1)), S(3), kInkSoft);
    else
        status_mark(dl, ImVec2(p.x + r, p.y + lh * 0.5f + S(1)), r, st);
    ImGui::SetCursorPosX(x0 + 2 * r + S(8));
    ImGui::PushTextWrapPos(x0 + w);
    text_small(message_text(m).c_str(), m.severity == nx::prepare::Severity::Info ? kInkSoft : status_ink(st));
    ImGui::PopTextWrapPos();
}

// What a picked folder was found to be: the ExeFS, the RomFS, both, or neither.
const char* prep_role(const std::string& in) {
    const nx::prepare::ProbeResult& r = g_prep.probe;
    const bool ex = !r.exefs.empty() && path_within(r.exefs, in);
    const bool ro = !r.romfs.path.empty() && path_within(r.romfs.path, in);
    if (ex && ro) return tr("launcher.prep.role.both");
    if (ex) return tr("launcher.prep.role.exefs");
    if (ro) return tr("launcher.prep.role.romfs");
    return tr("launcher.prep.role.none");
}

void card_title(const char* title) {
    ImGui::PushFont(g_title);
    ImGui::SetWindowFontScale(0.72f);
    ImGui::TextWrapped("%s", title);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
}

// The platforms the game's code is compiled for (this computer's always) and the native 60 FPS.
void prep_options_card() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kPaper));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(g_portrait ? 14 : 20), S(16)));
    ImGui::BeginChild("##prepopts", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding |
                      ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float r = S(g_portrait ? 20 : 24), x0 = ImGui::GetCursorPosX(), tx = 2 * r + S(14);
    dl->AddCircleFilled(ImVec2(p0.x + r, p0.y + r), r, kPurple);
    draw_icon(dl, nx::ui::kIconGear, ImVec2(p0.x + r, p0.y + r), r * 0.6f, kWhite);
    ImGui::SetCursorPosX(x0 + tx);
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(0);
    card_title(tr("launcher.prep.options"));
    text_small(tr("launcher.prep.platforms"));
    ImGui::Dummy(ImVec2(0, S(2)));
    const std::string host = prepare_host_platform();
    float x = 0;
    const float room = ImGui::GetContentRegionAvail().x;
    for (const std::string& p : prepare_platforms()) {
        const bool own = p == host;
        bool on = own || std::find(g_prep.extra.begin(), g_prep.extra.end(), p) != g_prep.extra.end();
        std::string label = platform_label(p);
        if (own) label += std::string(" · ") + tr("launcher.prep.platform.this");
        const float w = toggle_width(label.c_str());
        if (x > 0 && x + S(24) + w <= room)
            ImGui::SameLine(0, S(24));
        else
            x = 0;
        x += (x > 0 ? S(24) : 0) + w;
        ImGui::BeginDisabled(own);  // Play needs it: always built
        if (toggle(label.c_str(), &on) && !own) {
            auto it = std::find(g_prep.extra.begin(), g_prep.extra.end(), p);
            if (on && it == g_prep.extra.end()) g_prep.extra.push_back(p);
            if (!on && it != g_prep.extra.end()) g_prep.extra.erase(it);
        }
        nav_item();
        ImGui::EndDisabled();
    }
    ImGui::Dummy(ImVec2(0, S(4)));
    bool fps = g_prep.fps60;
    if (toggle(tr("launcher.prep.fps60"), &fps)) g_prep.fps60 = fps;
    text_small(tr("launcher.prep.fps60.desc"));
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void page_prepare() {
    prep_poll();
    steps(0);
    heading(tr("launcher.prep.title"), tr("launcher.prep.text"));
    g_page_min = ImGui::GetWindowPos();
    g_page_max = ImVec2(g_page_min.x + ImGui::GetWindowSize().x, g_page_min.y + ImGui::GetWindowSize().y);
    if (const PrepareTools& t = prepare_tools(); !t.problem.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(rgb(253, 226, 222)));
        ImGui::BeginChild("##tools", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushStyleColor(ImGuiCol_Text, col4(rgb(170, 70, 70)));
        ImGui::TextWrapped("%s", trf("launcher.prep.tools_missing", t.problem).c_str());
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, S(2)));
    }
    const bool probing = !g_prep.inputs.empty() && !g_prep.probed;
    // The findings: the game's files', and each add-on's.
    Status game_st = Status::Empty;
    std::vector<const nx::prepare::Message*> game_msgs;
    std::vector<std::vector<const nx::prepare::Message*>> dlc_msgs(g_prep.dlc.size());
    std::vector<Status> dlc_st(g_prep.dlc.size(), Status::Empty);
    if (g_prep.probed)
        for (const nx::prepare::Message& m : g_prep.probe.messages) {
            const int d = message_dlc(m);
            if (d >= 0) {
                dlc_msgs[size_t(d)].push_back(&m);
                dlc_st[size_t(d)] = std::max(dlc_st[size_t(d)], severity_status(m.severity));
            } else {
                game_st = std::max(game_st, severity_status(m.severity));
                if (m.key != "prepare.input.found") game_msgs.push_back(&m);  // the rows say which is which
            }
        }
    // what stops the preparation first, then what is only said
    std::stable_sort(game_msgs.begin(), game_msgs.end(), [](const nx::prepare::Message* a, const nx::prepare::Message* b) {
        return int(a->severity) > int(b->severity);
    });
    if (g_prep.probed && !g_prep.inputs.empty() && game_st == Status::Empty) game_st = Status::Ok;

    file_card({"##prepgame", kSlotPrepGame, nx::ui::kIconLeaf, tr("launcher.prep.game"), tr("launcher.prep.game.desc"), true},
              game_st, [&](float w) {
        if (g_prep.inputs.empty()) file_row("none", Item(), false, tr("launcher.prep.game.empty"), true, w);
        for (size_t k = 0; k < g_prep.inputs.size(); k++) {
            Item it{g_prep.inputs[k], {}};
            const char* role = probing ? tr("launcher.prep.checking") : prep_role(it.path);
            it.check = {probing ? Status::Empty : role == tr("launcher.prep.role.none") ? Status::Error : game_st, role};
            if (file_row(k == 0 ? "in0" : "in1", it, false, "", true, w)) {
                g_prep.inputs.erase(g_prep.inputs.begin() + long(k));
                prep_probe();
                break;
            }
        }
        if (probing) {
            const ImVec2 c = ImGui::GetCursorScreenPos();
            spinner(ImGui::GetWindowDrawList(), ImVec2(c.x + S(8), c.y + ImGui::GetTextLineHeight() * 0.45f), S(7));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(24));
            text_small(tr("launcher.prep.checking_long"));
        } else {
            for (const nx::prepare::Message* m : game_msgs) message_row(*m, w);
        }
        if (!probing && g_prep.inputs.size() == 1 &&
            (g_prep.probe.has("prepare.input.no_exefs") || g_prep.probe.has("prepare.input.no_romfs")))
            text_small(tr("launcher.prep.game.add_other"), rgb(190, 120, 20));
    });
    ImGui::Dummy(ImVec2(0, S(2)));

    Status all_dlc = Status::Empty;
    for (Status s : dlc_st) all_dlc = std::max(all_dlc, s == Status::Empty && g_prep.probed ? Status::Ok : s);
    file_card({"##prepdlc", kSlotPrepDlc, nx::ui::kIconHouse, tr("launcher.prep.dlc"), tr("launcher.prep.dlc.desc"),
               false, false}, all_dlc, [&](float w) {
        if (g_prep.dlc.empty()) file_row("none", Item(), false, tr("launcher.prep.dlc.empty"), false, w);
        for (size_t k = 0; k < g_prep.dlc.size(); k++) {
            ImGui::PushID(int(k));
            Item it{g_prep.dlc[k], {}};
            // its row says its first error, else what it holds (prepare.dlc.summary); the other findings follow
            std::vector<const nx::prepare::Message*> ms = dlc_msgs[k];
            std::stable_sort(ms.begin(), ms.end(), [](const nx::prepare::Message* a, const nx::prepare::Message* b) {
                const int ka = a->severity == nx::prepare::Severity::Error ? 0 : a->key == "prepare.dlc.summary" ? 1 : 2;
                const int kb = b->severity == nx::prepare::Severity::Error ? 0 : b->key == "prepare.dlc.summary" ? 1 : 2;
                return ka < kb;
            });
            if (probing)
                it.check = {Status::Empty, tr("launcher.prep.checking")};
            else if (!g_prep.probed)
                it.check = {Status::Empty, tr("launcher.prep.dlc.with_game")};
            else
                it.check = {dlc_st[k] == Status::Empty ? Status::Ok : dlc_st[k],
                            ms.empty() ? std::string(tr("launcher.prep.dlc.ok")) : message_text(*ms[0])};
            const bool cleared = file_row("dlc", it, false, "", false, w);
            for (size_t i = 1; g_prep.probed && i < ms.size(); i++) message_row(*ms[i], w);
            ImGui::PopID();
            if (cleared) {
                g_prep.dlc.erase(g_prep.dlc.begin() + long(k));
                prep_probe();
                break;
            }
        }
    });
    ImGui::Dummy(ImVec2(0, S(2)));

    const FileCheck oc = prep_out_check();
    file_card({"##prepout", kSlotPrepOut, nx::ui::kIconFolder, tr("launcher.prep.out"), tr("launcher.prep.out.desc"),
               true, false}, oc.status, [&](float w) {
        Item it{prep_out_dir(), oc};
        if (file_row("out", it, false, tr("launcher.prep.out.empty"), true, w))
            g_prep.out = default_game_dir(), g_prep.in_place = false;
        const std::string home = prep_romfs_home();
        if (!home.empty() && !same_path(g_prep.out, home)) {
            ImGui::Dummy(ImVec2(0, S(2)));
            bool v = g_prep.in_place;
            if (toggle(tr("launcher.prep.in_place"), &v)) g_prep.in_place = v;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
            text_small(trf("launcher.prep.in_place.desc", home).c_str());
            ImGui::PopTextWrapPos();
        }
    });
    ImGui::Dummy(ImVec2(0, S(2)));
    prep_options_card();
    ImGui::Dummy(ImVec2(0, S(4)));
    text_small(tr("launcher.prep.how"));
    if (g_portrait) {
        ImGui::Dummy(ImVec2(0, S(2)));
        text_small(tr("launcher.welcome.legal"));
    }
}

void footer_prepare() {
    if (button(tr("common.back"), kWhite)) g_banner.clear(), g_page = g_prep_back;
    const char* why = prep_not_ready();
    const char* go = tr("launcher.prep.start");
    if (why) {  // why Prepare is off, when it fits beside it
        ImGui::SameLine();
        float room = ImGui::GetContentRegionAvail().x - button_width(go) - ImGui::GetStyle().ItemSpacing.x;
        if (ImGui::CalcTextSize(why).x * 0.84f <= room) {
            ImGui::AlignTextToFramePadding();
            ImGui::SetWindowFontScale(0.84f);
            ImGui::TextColored(col4(kInkSoft), "%s", why);
            ImGui::SetWindowFontScale(1.0f);
        }
    }
    align_right({go});
    if (button(go, kGreen, why == nullptr)) prep_start();
}

// ---- preparing: the stages running ----------------------------------------------------------------------------

const char* prep_stage_label(int s) {
    static const char* keys[] = {"launcher.prep.stage.check",  "launcher.prep.stage.exefs",   "launcher.prep.stage.recompile",
                                 "launcher.prep.stage.shaders", "launcher.prep.stage.packs",  "launcher.prep.stage.compile",
                                 "launcher.prep.stage.finish"};
    return s >= kPrepCheck && s <= kPrepFinish ? tr(keys[s - kPrepCheck]) : tr("launcher.stage.prepare");
}

void prep_stage_list(const PrepareState& s, bool failed) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kPaper));
    ImGui::BeginChild("##prepstages", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    for (int k = kPrepCheck; k <= kPrepFinish; k++) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        const float r = S(11), lh = ImGui::GetTextLineHeight();
        const ImVec2 m(c.x + r, c.y + lh * 0.5f);
        const bool current = s.stage == k && !s.done[k];
        if (s.done[k])
            status_mark(dl, m, r, Status::Ok);
        else if (current && failed)
            status_mark(dl, m, r, Status::Error);
        else if (current && s.running)
            spinner(dl, m, r);
        else
            dl->AddCircle(m, r, kGrey, 0, S(2));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2 * r + S(10));
        ImGui::PushStyleColor(ImGuiCol_Text, col4(s.done[k] || current ? kInk : kInkSoft));
        std::string label = prep_stage_label(k);
        if (k == kPrepCompile && current && s.platforms > 1)
            label += "  " + trn("launcher.prep.platform_n", {std::to_string(s.platform), std::to_string(s.platforms)});
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopStyleColor();
        if (current && s.running && s.fraction > 0) {
            ImGui::SameLine();
            ImGui::TextColored(col4(kInkSoft), "%d%%", int(s.fraction * 100));
        }
        if (current && s.running && !s.item.empty()) {  // what it is doing
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2 * r + S(10));
            ImGui::PushTextWrapPos(0);
            text_small(s.item.substr(0, 160).c_str());
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// The heading's text when the preparation stopped on an error: where, and what to do.
std::string prep_failure_text(const PrepareState& s) {
    if (s.rc == 2) return tr("launcher.prep.refused");
    const char* hint = s.stage == kPrepCompile ? tr("launcher.prep.hint.compile")
                     : s.stage == kPrepShaders ? tr("launcher.prep.hint.shaders") : tr("launcher.prep.hint.other");
    return trf("launcher.prep.failed", prep_stage_label(s.stage)) + " " + hint;
}

void prep_done(const PrepareState& s) {
    g_banner.clear();
    if (!player_beside().empty()) {  // a release: the folder is the game folder, played as it is
        set_build(s.out, true);
        if (*g_build_problem == 0) {
            load_game_ini();
            contents(true);
            g_page = Page::Done;
        } else {
            g_banner = trf("launcher.build_invalid", tr(g_build_problem));
            g_page = Page::Prepare;
        }
        return;
    }
    use_file(Kind::Game, s.out);  // a checkout: the setup builds the game from it
    g_prep_flow = false;
    g_page = Page::Files;
}

void page_preparing() {
    const PrepareState s = prepare_state();
    prepare_lines(g_prep_log.size(), g_prep_log);
    if (s.finished && s.rc == 0) return prep_done(s);
    const bool failed = s.finished && s.rc != 4, stopped = s.finished && s.rc == 4;
    steps(1);
    if (failed)
        heading(tr("launcher.prep.failed_title"), prep_failure_text(s).c_str());
    else if (stopped)
        heading(tr("launcher.prep.stopped_title"), tr("launcher.prep.cancelled"));
    else
        heading(tr("launcher.prep.running_title"), tr("launcher.prep.running_text"));
    progress_bar(float(s.overall), failed);
    text_small(trf("launcher.build.elapsed", clock_text(s.elapsed)).c_str());
    ImGui::Dummy(ImVec2(0, S(2)));
    if (failed) {  // what went wrong: the check's findings translated, else the stage's own words
        std::vector<const nx::prepare::Message*> errors;
        for (const nx::prepare::Message& m : s.messages)
            if (m.severity == nx::prepare::Severity::Error) errors.push_back(&m);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(rgb(253, 226, 222)));
        ImGui::BeginChild("##preperror", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        const float w = ImGui::GetContentRegionAvail().x;
        if (s.stage == kPrepCheck && !errors.empty()) {
            for (const nx::prepare::Message* m : errors) message_row(*m, w);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, col4(rgb(170, 70, 70)));
            ImGui::TextWrapped("%s", s.error.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, S(2)));
    }
    const float avail = ImGui::GetContentRegionAvail().x;
    if (!g_portrait && avail > S(820)) {
        const float left = std::min(S(400), avail * 0.42f);
        ImGui::BeginChild("##left", ImVec2(left, 0), ImGuiChildFlags_AutoResizeY);
        prep_stage_list(s, failed);
        ImGui::EndChild();
        ImGui::SameLine(0, S(16));
        ImGui::BeginGroup();
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        const bool details_open = ImGui::CollapsingHeader(tr("launcher.build.details"));
        nav_item();
        if (details_open) log_view(std::max(S(160), ImGui::GetContentRegionAvail().y - S(4)), g_prep_log);
        ImGui::EndGroup();
    } else {
        prep_stage_list(s, failed);
        ImGui::Dummy(ImVec2(0, S(2)));
        if (failed) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        const bool details_open = ImGui::CollapsingHeader(tr("launcher.build.details"));
        nav_item();
        if (details_open) log_view(std::max(S(200), ImGui::GetContentRegionAvail().y - S(4)), g_prep_log);
    }
}

void footer_preparing() {
    if (prepare_running()) {
        align_right({tr("launcher.stop")});
        if (button(tr("launcher.stop"), kRed)) g_confirm_stop = true;
        return;
    }
    if (button(tr("common.back"), kWhite)) g_page = Page::Prepare, prep_probe();
    const char* logs = tr("launcher.open_logs");
    align_right({tr("launcher.copy_log"), logs, tr("launcher.retry")});
    if (button(tr("launcher.copy_log"), kWhite))
        SDL_SetClipboardText(log_text(g_prep_log).c_str()), toast(tr("launcher.copied"));
    ImGui::SameLine();
    if (button(logs, kWhite)) open_folder(parent_dir(prepare_log_path()));
    ImGui::SameLine();
    if (button(tr("launcher.retry"), kGreen)) prep_start();
}

// ---- done and home --------------------------------------------------------------------------------------------

// Set by play(): the launcher stays up, saying so, until the game's window is there to take its place.
double g_opening_since = -1;

void play() {
    if (g_opening_since >= 0) return;  // already starting
    if (const char* missing = cpu_missing_feature()) {  // the game would stop at once (issue #46)
        g_banner = trf("launcher.cpu_unsupported", missing);
        g_autostart_at = -1;
        return;
    }
    std::string err;
    std::string profile = settings().get("launcher.save_profile", g_save_profile_active);
    if (profile.empty()) profile = "default";
    if (!nx::save_profiles::activate_profile(g_build.data, profile, &err)) {
        g_banner = trf("launcher.profiles.switch_failed", err);
        return;
    }
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
    // Play hides the launcher and then quits it: an update being installed finishes first (closing the window is
    // still possible until its files are being swapped, pages_close_requested)
    if (hidden && !start_sound_playing() && !update_busy()) g_exit = true;
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

// The Importer's big button: sun yellow on a deeper lip, bark ink in the title font; hovered it lifts off the lip,
// pressed it sinks onto it. During the countdown (`sub`, its second line) it stays lifted; after the countdown was
// cancelled it stays calm until the pointer leaves. `color` is unused: Play is the one action that matters.
bool big_button(const char* label, int icon, ImU32 color, float w, float h, const char* sub = nullptr) {
    (void)color;
    const float lip = S(5);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = nav_card("##play", ImVec2(w, h + lip), true);
    if (g_play_calm && !ImGui::IsItemHovered()) g_play_calm = false;
    const bool live = !sub && !g_autostart_ui && !g_play_calm;
    float t = hover_anim();
    if (sub) t = 1;
    else if (!live) t = 0;
    const bool down = live && ImGui::IsItemActive(), focus = nav_focused();
    const float lift = down ? 0 : t * S(3);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float round = std::min(h * 0.5f, S(40));
    lip_shape(dl, p, ImVec2(w, h), lip, round, mix(kYellow, shade(kYellow, 1.15f), t), kSunDeep,
              focus ? kLagoonDeep : kSunDeep, focus ? S(3) : S(1.5f), down, lift);
    p.y += down ? lip : -lift;
    float ir = h * 0.2f, pad = S(12), avail = w - ir * 2 - pad - S(24);
    if (ir > S(16)) ir = S(16);
    float fs = font_size(g_title) * 0.9f;
    ImVec2 ts = measure(g_title, fs, label, -1);
    if (ts.x > avail && avail > 0) fs *= avail / ts.x, ts = measure(g_title, fs, label, -1);
    float sub_fs = 0, gap = 0;
    ImVec2 ss(0, 0);
    if (sub && *sub) {
        sub_fs = font_size(g_body) * 0.8f;
        ss = measure(g_body, sub_fs, sub, -1);
        if (ss.x > avail && avail > 0) sub_fs *= avail / ss.x, ss = measure(g_body, sub_fs, sub, -1);
        gap = S(2);
    }
    float text_h = ts.y + (sub ? gap + ss.y : 0);
    float total = std::max(ts.x, ss.x) + ir * 2 + pad;
    float x = p.x + (w - total) * 0.5f;
    float ty = p.y + (h - text_h) * 0.5f;
    draw_icon(dl, icon, ImVec2(x + ir, p.y + h * 0.5f), ir, kInk);
    float tx = x + ir * 2 + pad;
    dl->AddText(g_title, fs, ImVec2(tx + (std::max(ts.x, ss.x) - ts.x) * 0.5f, ty), kInk, label);
    if (sub && *sub)
        dl->AddText(g_body, sub_fs, ImVec2(tx + (std::max(ts.x, ss.x) - ss.x) * 0.5f, ty + ts.y + gap), kInkSoft, sub);
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
    dl->AddCircleFilled(c, r + S(10), kSandDeep);
    dl->AddCircleFilled(c, r, color);
    draw_icon(dl, icon, c, r * 0.6f, kWhite);
    ImGui::Dummy(ImVec2(w, 2 * r + S(14)));
}

void select_save_profile(const std::string& id) {
    if (id == g_save_profile_active) return;
    std::string err;
    if (!nx::save_profiles::activate_profile(g_build.data, id, &err)) {
        g_banner = trf("launcher.profiles.switch_failed", err);
        return;
    }
    settings().set("launcher.save_profile", id);
    settings().save();
    g_save_profile_active = id;
    g_save_profiles = nx::save_profiles::list_profiles(g_build.data);
    g_banner.clear();
}

// Same size for countdown and Play so cancel does not resize/recolor the control.
void play_button(float w, float h = 0) {
    if (h <= 0) h = S(80);
    if (g_autostart_at >= 0) {
        int left = std::max(1, int(std::ceil(g_autostart_at - ImGui::GetTime())));
        std::string label = trf("launcher.autostart", std::to_string(left));
        big_button(label.c_str(), nx::ui::kIconPlay, kGreen, w, h, tr("launcher.autostart.stay"));
    } else if (big_button(tr("launcher.play"), nx::ui::kIconPlay, kGreen, w, h) && !g_autostart_ui) {
        play();
    }
}

void play_block() {
    float w = std::min(ImGui::GetContentRegionAvail().x, S(420));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - w) * 0.5f);
    play_button(w);
    ImGui::Dummy(ImVec2(0, S(8)));
}

void page_done() {
    steps(g_prep_flow ? 2 : 3);
    if (!g_banner.empty()) heading("", "");
    ImGui::Dummy(ImVec2(0, S(10)));
    hero(nx::ui::kIconCheck, kGreen, S(48));
    centered(tr("launcher.done.title"), g_title, 1.15f, kInk);
    ImGui::Dummy(ImVec2(0, S(4)));
    centered(trf(g_prep_flow ? "launcher.prep.done.text" : "launcher.done.text", g_build.data).c_str(), g_body, 1.0f,
             kInkSoft);
    ImGui::Dummy(ImVec2(0, S(18)));
    play_block();
}

void footer_done() {
    align_right({tr("launcher.home")});
    if (button(tr("launcher.home"), kWhite)) g_page = Page::Home;
}

// A secondary action as a row of the home page's list: a tinted badge with its icon, the title in the title font and
// one line under it, an arrow at the right. Hovered it gets a sand back, focused a lagoon edge.
bool action_card(const char* id, int icon, ImU32 color, const char* title, const char* desc, ImVec2 size, bool enabled) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    bool clicked = nav_card(id, size, false, true);
    ImGui::EndDisabled();
    const float t = enabled ? hover_anim() : 0.0f;
    bool hov = enabled && ImGui::IsItemHovered(), focus = enabled && nav_focused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + size.x, p.y + size.y);
    if (t > 0 || focus) dl->AddRectFilled(p, q, mix(kBoard, kSand, focus ? 1.0f : t), S(18));
    if (focus) dl->AddRect(p, q, kLagoonDeep, S(18), 0, S(3));
    float pad = S(12), r = S(22);
    ImVec2 c(p.x + pad + r, p.y + size.y * 0.5f);
    ImU32 col = enabled ? color : kGrey;
    dl->AddCircleFilled(c, r, shade(col, 1.62f));
    dl->AddCircle(c, r - S(1), shade(col, 1.25f), 0, S(2));
    draw_icon(dl, icon, c, r * 0.6f, shade(col, 0.72f));
    float x = c.x + r + S(14), arrow = S(30), inner = q.x - pad - arrow - x;
    float tfs = font_size(g_title) * 0.72f, dfs = font_size(g_body) * 0.82f;
    float th = measure(g_title, tfs, title, inner).y, dh = measure(g_body, dfs, desc, inner).y;
    float y = p.y + (size.y - th - S(2) - dh) * 0.5f;
    dl->AddText(g_title, tfs, ImVec2(x, y), enabled ? kInk : kInkSoft, title, nullptr, inner);
    dl->AddText(g_body, dfs, ImVec2(x, y + th + S(2)), kInkSoft, desc, nullptr, inner);
    if (enabled) {
        ImVec2 a(q.x - pad - S(12), c.y);
        a.x += t * S(3);  // the arrow leans the way it leads
        dl->AddCircleFilled(a, S(13), focus ? kYellow : mix(kSand, kYellow, t));
        draw_icon(dl, nx::ui::kIconPlay, ImVec2(a.x + S(1), a.y), S(8), hov || focus ? kInk : kInkSoft);
    }
    return clicked && enabled;
}

float action_height(const char* title, const char* desc, float w) {
    float pad = S(12), r = S(22), inner = w - 2 * pad - 2 * r - S(14) - S(30);
    float h = measure(g_title, font_size(g_title) * 0.72f, title, inner).y + S(2) +
              measure(g_body, font_size(g_body) * 0.82f, desc, inner).y;
    return std::max(2 * r, h) + 2 * S(12);
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

// A chip (the Importer's): paper with a sand edge, an icon in a coloured disc, then the text. A clickable one answers
// the pointer with a lagoon edge and gives a little under it; `dot` marks it live (a green dot on the disc).
bool pill(const char* id, int icon, ImU32 color, const char* text, bool clickable, bool dot, float x) {
    float h = S(38), r = S(12), pad = S(7), fs = font_size(g_body) * 0.86f;
    ImVec2 ts = measure(g_body, fs, text, -1);
    float w = pad + 2 * r + S(9) + ts.x + S(14);
    ImGui::SetCursorScreenPos(ImVec2(x, ImGui::GetCursorScreenPos().y));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!clickable);
    bool clicked = nav_card(id, ImVec2(w, h));
    ImGui::EndDisabled();
    const float t = clickable ? hover_anim() : 0.0f;
    bool focus = clickable && nav_focused();
    if (clickable && ImGui::IsItemActive()) p.y += S(1.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + w, p.y + h);
    dl->AddRectFilled(p, q, kBoard, S(12));
    dl->AddRect(p, q, focus ? kLagoonDeep : mix(kSandDeep, kMint, t), S(12), 0, focus ? S(3) : S(1.5f));
    ImVec2 c(p.x + pad + r, p.y + h * 0.5f);
    dl->AddCircleFilled(c, r, color);
    draw_icon(dl, icon, c, r * 0.62f, kWhite);
    if (dot) {
        ImVec2 d(c.x + r * 0.72f, c.y - r * 0.72f);
        dl->AddCircleFilled(d, S(5), kBoard);
        dl->AddCircleFilled(d, S(3.5f), kGreen);
    }
    dl->AddText(g_body, fs, ImVec2(c.x + r + S(9), p.y + (h - ts.y) * 0.5f), kInk, text);
    return clicked && clickable;
}

float pill_width(const char* text) {
    float r = S(12), fs = font_size(g_body) * 0.86f;
    return S(7) + 2 * r + S(9) + measure(g_body, fs, text, -1).x + S(14);
}

std::string profile_save_folder(const std::string& id) {
    std::string base = g_build.data + "/user/profiles/" + id;
    std::string save = base + "/device_save";
    return is_dir(save) ? save : base;
}

std::string active_island_pill_text() {
    for (const nx::save_profiles::ProfileInfo& p : g_save_profiles) {
        if (p.id != g_save_profile_active) continue;
        if (p.has_save && !p.island_name.empty()) return trf("launcher.home.save_file", p.island_name);
        return trf("launcher.home.save_file", tr("launcher.profiles.empty"));
    }
    return trf("launcher.home.save_file", tr("launcher.profiles.empty"));
}

// Version, add-ons, save file and mods under the title.
void contents_row() {
    const BuildContents& c = contents();
    std::string ver = c.version.empty() ? tr("launcher.home.version_unknown") : trf("launcher.home.version", c.version);
    std::string save = active_island_pill_text();
    std::string add = c.addons.empty() ? tr("launcher.addons.none")
                                       : trf("launcher.addons.count", std::to_string(c.addons.size()));
    size_t mods_on = 0;
    for (const nx::mod_manager::ModInfo& mod : g_mods)
        if (mod.enabled) ++mods_on;
    std::string mods_arg = std::to_string(mods_on) + "/" + std::to_string(g_mods.size());
    std::string mods = g_mods.empty() ? tr("launcher.mods.none") : trf("launcher.mods.count", mods_arg);
    bool has_save = false;
    for (const nx::save_profiles::ProfileInfo& p : g_save_profiles)
        if (p.id == g_save_profile_active && p.has_save) has_save = true;
    // Two columns meeting at the middle: version over add-ons, save file over mods; one centred column when the
    // two do not fit side by side.
    float avail = ImGui::GetContentRegionAvail().x, gap = S(8), x0 = ImGui::GetCursorScreenPos().x;
    const float y0 = ImGui::GetCursorScreenPos().y, row_h = S(38);
    const float wl = std::max(pill_width(ver.c_str()), pill_width(add.c_str()));
    const float wr = std::max(pill_width(save.c_str()), pill_width(mods.c_str()));
    const bool two = wl + gap + wr <= avail;
    const float mid = x0 + std::floor(two ? (avail - (wl + gap + wr)) * 0.5f + wl : avail * 0.5f);
    auto at = [&](int i, const std::string& t) {
        const float w = pill_width(t.c_str());
        if (!two) return ImVec2(std::floor(mid - w * 0.5f), y0 + float(i) * (row_h + gap));
        const bool left = i == 0 || i == 1;
        return ImVec2(left ? mid - w : mid + gap, y0 + float(i % 2) * (row_h + gap));
    };
    // Order: version, add-ons, save file, mods (top to bottom when stacked).
    ImVec2 p = at(0, ver);
    ImGui::SetCursorScreenPos(p);
    pill("##version", nx::ui::kIconLeaf, kGreen, ver.c_str(), false, false, p.x);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g_build.data.c_str());
    p = at(1, add);
    ImGui::SetCursorScreenPos(p);
    if (pill("##addons", nx::ui::kIconBox, c.addons.empty() ? kGrey : kPurple, add.c_str(), true, !c.addons.empty(), p.x)) {
        contents(true);
        g_addons_open = true, g_addons_since = ImGui::GetTime();
    }
    p = at(2, save);
    ImGui::SetCursorScreenPos(p);
    if (pill("##savefile", nx::ui::kIconHouse, has_save ? kRed : kGrey, save.c_str(), true, has_save, p.x)) {
        refresh_save_profiles();
        g_saves_open = true;
        g_saves_since = ImGui::GetTime();
    }
    p = at(3, mods);
    ImGui::SetCursorScreenPos(p);
    if (pill("##mods", kIconPuzzle, g_mods.empty() ? kGrey : kOrange, mods.c_str(), true, mods_on > 0, p.x)) {
        refresh_mods();
        g_mods_open = true;
        g_mods_since = ImGui::GetTime();
    }
    ImGui::SetCursorScreenPos(ImVec2(x0, y0 + float(two ? 2 : 4) * (row_h + gap) - gap + S(4)));
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
        draw_icon(dl, nx::ui::kIconBox, ImVec2(p.x + r, p.y + r), r * 0.6f, kWhite);
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
            draw_icon(dl, nx::ui::kIconBox, cc, cr * 0.6f, kWhite);
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
            draw_icon(dl, nx::ui::kIconCheck, kc, S(6), kWhite);
            ImGui::Dummy(ImVec2(w, h + S(8)));
        }
        ImGui::Dummy(ImVec2(0, S(4)));
        align_right({tr("launcher.addons.close")});
        if (button(tr("launcher.addons.close"), kWhite) || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            g_addons_open = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void saves_popup() {
    if (g_saves_open) ImGui::OpenPopup("saves");
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, out_cubic((ImGui::GetTime() - g_saves_since) / 0.18));
    // Hide ImGui's square nav outline for the whole popup; tiles draw a rounded focus outline below.
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(ds.x * 0.9f, S(720)), std::min(ds.y * 0.85f, S(520))));
    if (ImGui::BeginPopupModal("saves", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = S(18);
        dl->AddCircleFilled(ImVec2(p.x + r, p.y + r), r, kRed);
        draw_icon(dl, nx::ui::kIconHouse, ImVec2(p.x + r, p.y + r), r * 0.6f, kWhite);
        float fs = font_size(g_title) * 0.8f;
        dl->AddText(g_title, fs, ImVec2(p.x + 2 * r + S(12), p.y + r - fs * 0.6f), kInk, tr("launcher.profiles.title"));
        ImGui::Dummy(ImVec2(0, 2 * r + S(8)));
        ImGui::PushStyleColor(ImGuiCol_Text, col4(kInkSoft));
        ImGui::TextWrapped("%s", tr("launcher.profiles.hint"));
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, S(10)));

        const char* new_l = tr("launcher.profiles.new");
        const char* import_l = tr("launcher.saves.import_new");
        const char* close_l = tr("launcher.addons.close");
        float footer = S(56) + ImGui::GetFrameHeight();
        if (!g_saves_delete_id.empty()) footer += S(72);
        float grid_h = std::max(S(140), ImGui::GetContentRegionAvail().y - footer);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(12)));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(16));
        ImGui::BeginChild("##saves_grid", ImVec2(0, grid_h),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar(2);

        const bool only_one = g_save_profiles.size() <= 1;
        if (g_save_profiles.empty()) {
            ImGui::TextDisabled("%s", tr("launcher.profiles.empty"));
        } else {
            const char* open_l = tr("launcher.saves.open");
            const char* del_l = tr("launcher.saves.delete");
            float avail = ImGui::GetContentRegionAvail().x, gap = S(12);
            const int cols = 2;
            float tile_w = (avail - gap * float(cols - 1)) / float(cols);
            // Size tiles so roughly two rows fit in the visible grid.
            float tile_h = std::max(S(100), (grid_h - S(24) - gap) / 2.f);
            float action_w = std::min(S(110), std::max(S(72), (tile_w - S(28)) / 2.f));

            for (size_t i = 0; i < g_save_profiles.size(); ++i) {
                const nx::save_profiles::ProfileInfo& prof = g_save_profiles[i];
                if (i % size_t(cols) != 0) ImGui::SameLine(0, gap);
                ImGui::PushID(prof.id.c_str());

                const bool active = prof.id == g_save_profile_active;
                ImVec2 tile_min = ImGui::GetCursorScreenPos();
                // The Importer's chips: the chosen one pale leaf with a leaf edge, the others paper with a sand edge.
                ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(active ? kLeafPale : kBoard));
                ImGui::PushStyleColor(ImGuiCol_Border, col4(active ? kGreen : kSandDeep));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, S(1.5f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(14));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
                ImGui::BeginChild("##tile", ImVec2(tile_w, tile_h),
                                  ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
                ImGui::PopStyleVar(3);
                ImGui::PopStyleColor(2);

                std::string label =
                    prof.has_save && !prof.island_name.empty() ? prof.island_name : tr("launcher.profiles.empty");
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
                // Content area above the action row selects / activates the profile.
                const float actions_h = ImGui::GetFrameHeight() + S(4);
                const float select_h =
                    std::max(ImGui::GetTextLineHeightWithSpacing() * 1.5f,
                             tile_h - ImGui::GetStyle().WindowPadding.y * 2.f - actions_h - S(4));
                bool selected = ImGui::Selectable("##pick", active, ImGuiSelectableFlags_None, ImVec2(0, select_h));
                ImVec2 sel_min = ImGui::GetItemRectMin();
                ImGui::PopStyleColor(4);
                if (selected) select_save_profile(prof.id);
                nav_item();
                const float pick_t = hover_anim();

                ImDrawList* tdl = ImGui::GetWindowDrawList();
                float name_fs = font_size(g_body);
                const float tile_pad = ImGui::GetStyle().WindowPadding.x;
                // The toggle's mark before the name: filled with a check on the save in use, an empty ring on the others.
                const float mr = S(8.5f);
                const ImVec2 mc(sel_min.x + S(4) + mr, sel_min.y + S(4) + name_fs * 0.55f);
                if (active) {
                    tdl->AddCircleFilled(mc, mr, kLeafDeep);
                    draw_icon(tdl, nx::ui::kIconCheck, mc, mr * 0.75f, kWhite);
                } else {
                    tdl->AddCircle(mc, mr - S(1), mix(kInkSoft, kLagoonDeep, pick_t), 0, S(1.8f));
                }
                ImVec2 name_pos(mc.x + mr + S(8), sel_min.y + S(4));
                ImVec2 name_sz = measure(g_body, name_fs, label.c_str(), -1);
                float name_right = tile_min.x + tile_w - tile_pad;
                if (active) {
                    std::string active_mark = std::string("(") + tr("launcher.profiles.active") + ")";
                    float active_fs = font_size(g_title) * 0.7f;
                    ImVec2 as = measure(g_title, active_fs, active_mark.c_str(), -1);
                    // Same row as the island name: right-aligned, vertically centered to the name.
                    ImVec2 active_pos(tile_min.x + tile_w - tile_pad - as.x, name_pos.y + (name_sz.y - as.y) * 0.5f);
                    tdl->AddText(g_title, active_fs, active_pos, kInk, active_mark.c_str());
                    name_right = active_pos.x - S(8);
                }
                tdl->AddText(g_body, name_fs, name_pos, kInk, label.c_str(), nullptr, name_right - name_pos.x);
                float y = name_pos.y + name_sz.y + S(4);
                if (prof.has_save && !prof.last_modified.empty()) {
                    std::string meta = trf("launcher.profiles.last_saved", prof.last_modified);
                    tdl->AddText(g_body, font_size(g_body) * 0.8f, ImVec2(name_pos.x, y), kInkSoft, meta.c_str());
                }

                ImGui::SetCursorPosY(tile_h - ImGui::GetStyle().WindowPadding.y - ImGui::GetFrameHeight() - S(4));
                if (button(open_l, kBlue, true, action_w)) open_folder(profile_save_folder(prof.id));
                ImGui::SameLine();
                ImGui::BeginDisabled(only_one);
                if (button(del_l, kRed, true, action_w)) g_saves_delete_id = prof.id;
                ImGui::EndDisabled();

                ImGuiWindow* tile_win = ImGui::GetCurrentWindow();
                ImGuiContext& gui = *ImGui::GetCurrentContext();
                ImGui::EndChild();
                ImVec2 a = tile_min, b(tile_min.x + tile_w, tile_min.y + tile_h);
                // Unclipped hit test — IsMouseHoveringRect(..., clip=true) against the grid
                // ClipRect was missing the tiles after the open animation settled.
                const ImVec2 mp = ImGui::GetIO().MousePos;
                const bool mouse_over = mp.x >= a.x && mp.x < b.x && mp.y >= a.y && mp.y < b.y;
                const bool nav_here = gui.NavInputSource == ImGuiInputSource_Gamepad && gui.NavIdIsAlive &&
                                     gui.NavWindow == tile_win;
                float e = out_cubic((ImGui::GetTime() - g_saves_since - 0.03 * double(i)) / 0.2);
                if (e < 1.f) {
                    ImGui::GetWindowDrawList()->AddRectFilled(a, b, rgb(248, 239, 214, int(180 * (1 - e))), S(14));
                }
                if (mouse_over || nav_here) {
                    const float thick = S(2);
                    const float inset = ImGui::GetStyle().ChildBorderSize + thick * 0.5f;
                    const float round = std::max(0.f, S(14) - inset);
                    // Foreground so the stroke is not covered by later tiles / fade.
                    ImGui::GetForegroundDrawList()->AddRect(ImVec2(a.x + inset, a.y + inset),
                                                           ImVec2(b.x - inset, b.y - inset), kLagoonDeep, round, 0,
                                                           thick);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        if (!g_saves_delete_id.empty()) {
            ImGui::Dummy(ImVec2(0, S(6)));
            ImGui::PushStyleColor(ImGuiCol_Text, col4(kInkSoft));
            ImGui::TextWrapped("%s", tr("launcher.saves.delete_confirm"));
            ImGui::PopStyleColor();
            if (button(tr("launcher.saves.delete"), kRed)) {
                std::string err;
                std::string id = g_saves_delete_id;
                g_saves_delete_id.clear();
                if (nx::save_profiles::delete_profile(g_build.data, id, &err)) {
                    refresh_save_profiles();
                    settings().set("launcher.save_profile", g_save_profile_active);
                    settings().save();
                    toast(tr("launcher.saves.deleted"));
                } else
                    g_banner = trf("launcher.saves.delete_failed", err);
            }
            ImGui::SameLine();
            if (button(tr("common.no"), kWhite)) g_saves_delete_id.clear();
        }

        ImGui::Dummy(ImVec2(0, S(8)));
        float pair_w = std::max(button_width(new_l), button_width(import_l));
        if (button(new_l, kBlue, true, pair_w)) {
            std::string err;
            std::string id = nx::save_profiles::create_empty_profile(g_build.data, &err);
            if (id.empty())
                g_banner = trf("launcher.profiles.switch_failed", err);
            else
                select_save_profile(id);
        }
        ImGui::SameLine();
        if (button(import_l, kBlue, true, pair_w)) pick(kPickSaveImport);
        align_right({close_l});
        if (button(close_l, kWhite) || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            g_saves_open = false, g_saves_delete_id.clear(), ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

const char* managed_mod_text(const std::string& id) {
    if (id == "ptbr") return tr("launcher.mods.managed.language");
    if (id == "speedfix60") return tr("launcher.mods.managed.fps");
    return tr("launcher.mods.managed");
}

void mods_popup() {
    if (g_mods_open) ImGui::OpenPopup("mods");
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, out_cubic((ImGui::GetTime() - g_mods_since) / 0.18));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(ds.x * 0.94f, S(1180)), std::min(ds.y * 0.9f, S(700))));
    // Keep the fixed modal centered when the display size changes (for example, entering fullscreen).
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("mods", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = S(18);
        dl->AddCircleFilled(ImVec2(p.x + r, p.y + r), r, kOrange);
        draw_icon(dl, nx::ui::kIconText, ImVec2(p.x + r, p.y + r), r * 0.6f, kWhite);
        float fs = font_size(g_title) * 0.8f;
        dl->AddText(g_title, fs, ImVec2(p.x + 2 * r + S(12), p.y + r - fs * 0.6f), kInk, tr("launcher.mods.title"));
        ImGui::Dummy(ImVec2(0, 2 * r + S(8)));
        ImGui::PushStyleColor(ImGuiCol_Text, col4(kInkSoft));
        ImGui::TextWrapped("%s", tr("launcher.mods.hint"));
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, S(10)));

        const char* folder_l = tr("launcher.mods.import_folder");
        const char* zip_l = tr("launcher.mods.import_zip");
        const char* open_l = tr("launcher.mods.open");
        const char* close_l = tr("launcher.addons.close");
        float footer = S(56) + ImGui::GetFrameHeight();
        if (!g_mods_delete_id.empty()) footer += S(72);
        float grid_h = std::max(S(140), ImGui::GetContentRegionAvail().y - footer);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(12)));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(16));
        ImGui::BeginChild("##mods_grid", ImVec2(0, grid_h),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar(2);

        if (g_mods.empty()) {
            ImGui::TextDisabled("%s", tr("launcher.mods.empty"));
        } else {
            std::vector<int> order(g_mods.size(), 0);
            int next = 1;
            for (size_t i = 0; i < g_mods.size(); ++i)
                if (g_mods[i].enabled) order[i] = next++;
            const int enabled_count = next - 1;

            float avail = ImGui::GetContentRegionAvail().x, gap = S(12);
            int cols = avail >= S(900) ? 3 : (avail >= S(520) ? 2 : 1);
            float tile_w = (avail - gap * float(cols - 1)) / float(cols);
            const float tile_pad_x = S(12), tile_pad_y = S(10);
            const float br = S(14);
            const float fh = ImGui::GetFrameHeight();
            const float line_h = ImGui::GetTextLineHeight();
            const float item_sp = ImGui::GetStyle().ItemSpacing.y;
            const float border = ImGui::GetStyle().ChildBorderSize;
            const float inner_w = std::max(1.f, tile_w - 2.f * tile_pad_x - 2.f * border);
            const float name_wrap =
                std::max(1.f, inner_w - (2.f * br + S(10)) - fh - ImGui::GetStyle().ItemSpacing.x);
            const float btn_gap = S(4);
            const char* up_l = tr("launcher.mods.up");
            const char* down_l = tr("launcher.mods.down");
            const char* del_l = tr("launcher.mods.delete");
            float action_w = std::min(S(96), std::max(S(64), (tile_w - S(36)) / 3.f));

            // Snapshot ids so toggles cannot reshuffle g_mods while this frame still walks the grid.
            const size_t mod_count = g_mods.size();
            // Per-tile content height, then equalize within each grid row for a consistent band.
            std::vector<float> tile_hs(mod_count, 0.f);
            std::vector<std::string> name_labels(mod_count);
            for (size_t i = 0; i < mod_count; ++i) {
                const nx::mod_manager::ModInfo& m = g_mods[i];
                name_labels[i] = fit_wrapped_lines(m.id.c_str(), name_wrap, 2);
                float name_h = ImGui::CalcTextSize(name_labels[i].c_str(), nullptr, false, name_wrap).y;
                name_h = std::min(name_h, line_h * 2.f);
                float content = std::max(fh, name_h);
                if (m.managed)
                    content += item_sp + ImGui::CalcTextSize(managed_mod_text(m.id), nullptr, false, inner_w).y;
                if (m.has_exefs)
                    content += item_sp + ImGui::CalcTextSize(tr("launcher.mods.exefs_warn"), nullptr, false, inner_w).y;
                // Button row + spacing; extra item_sp so actions stay inside bottom padding.
                if (!m.managed) content += item_sp + btn_gap + fh + item_sp;
                tile_hs[i] = 2.f * tile_pad_y + 2.f * border + content;
            }
            for (size_t row = 0; row < mod_count; row += size_t(cols)) {
                const size_t row_end = std::min(row + size_t(cols), mod_count);
                float row_max = 0.f;
                for (size_t i = row; i < row_end; ++i) row_max = std::max(row_max, tile_hs[i]);
                for (size_t i = row; i < row_end; ++i) tile_hs[i] = row_max;
            }

            for (size_t i = 0; i < mod_count; ++i) {
                const nx::mod_manager::ModInfo mod = g_mods[i];
                if (i % size_t(cols) != 0) ImGui::SameLine(0, gap);
                ImGui::PushID(mod.id.c_str());

                const std::string& name_label = name_labels[i];
                float name_h = ImGui::CalcTextSize(name_label.c_str(), nullptr, false, name_wrap).y;
                name_h = std::min(name_h, line_h * 2.f);
                const float title_h = std::max(fh, name_h);
                const float tile_h = tile_hs[i];

                ImVec2 tile_min = ImGui::GetCursorScreenPos();
                ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(mod.enabled ? kLeafPale : kBoard));
                ImGui::PushStyleColor(ImGuiCol_Border, col4(mod.enabled ? kGreen : kSandDeep));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, S(1.5f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(14));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(tile_pad_x, tile_pad_y));
                ImGui::BeginChild("##tile", ImVec2(tile_w, tile_h),
                                  ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                ImGui::PopStyleVar(3);
                ImGui::PopStyleColor(2);

                // Load-order badge (1-based among enabled mods), centred on the checkbox.
                ImDrawList* tdl = ImGui::GetWindowDrawList();
                ImVec2 row_min = ImGui::GetCursorScreenPos();
                ImVec2 bc(row_min.x + br, row_min.y + fh * 0.5f);
                tdl->AddCircleFilled(bc, br, mod.enabled ? kOrange : kRed);
                char num[8];
                if (mod.enabled)
                    std::snprintf(num, sizeof num, "%d", order[i]);
                else
                    std::snprintf(num, sizeof num, "-");
                ImVec2 ns = measure(g_body, font_size(g_body) * 0.85f, num, -1);
                tdl->AddText(g_body, font_size(g_body) * 0.85f, ImVec2(bc.x - ns.x * 0.5f, bc.y - ns.y * 0.5f), kWhite,
                             num);

                // Checkbox + name share the top of the row; wrapped lines grow downward.
                ImGui::SetCursorScreenPos(ImVec2(row_min.x + 2 * br + S(10), row_min.y));
                const float row_top_y = ImGui::GetCursorPosY();
                bool enabled = mod.enabled;
                // Bundled mods (ptbr / speedfix60) are driven by language / 60 FPS settings — not toggled here.
                ImGui::BeginDisabled(mod.managed);
                if (check_mark("##on", &enabled, ImGui::GetFrameHeight()) && !mod.managed) {
                    std::string err;
                    if (nx::mod_manager::set_enabled(g_build.data, mod.id, enabled, &err)) {
                        load_game_ini();
                        // Keep the tile put for this frame; reshuffle (disabled → bottom) after the grid,
                        // and on gamepad only once FaceDown is released so activate cannot hit another tile.
                        if (i < g_mods.size() && g_mods[i].id == mod.id) g_mods[i].enabled = enabled;
                        g_mods_refresh = true;
                        ImGuiContext& ctx = *ImGui::GetCurrentContext();
                        if (ctx.NavInputSource == ImGuiInputSource_Gamepad) {
                            g_mods_refresh_wait_pad = true;
                            ctx.NavActivateId = 0;
                            ctx.NavActivateDownId = 0;
                            ctx.NavActivatePressedId = 0;
                        } else {
                            g_mods_refresh_wait_pad = false;
                        }
                    } else
                        g_banner = trf("launcher.mods.toggle_failed", err);
                }
                ImGui::EndDisabled();
                // Max two lines; first line flush with the top of the checkbox (not its baseline).
                // SameLine+Text would apply CurrLineTextBaseOffset and drop the name to the bottom of the box.
                {
                    const ImVec2 check_max = ImGui::GetItemRectMax();
                    ImGuiWindow* tw = ImGui::GetCurrentWindow();
                    tw->DC.CursorPos = ImVec2(check_max.x + ImGui::GetStyle().ItemInnerSpacing.x, row_min.y);
                    tw->DC.CursorMaxPos = ImMax(tw->DC.CursorMaxPos, tw->DC.CursorPos);
                    tw->DC.CurrLineSize = ImVec2(0, 0);
                    tw->DC.CurrLineTextBaseOffset = 0.f;
                    const float name_x = ImGui::GetCursorPosX();
                    ImGui::PushTextWrapPos(name_x + name_wrap);
                    ImGui::TextUnformatted(name_label.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), row_top_y + title_h));
                }
                if (mod.managed) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::TextWrapped("%s", managed_mod_text(mod.id));
                    ImGui::PopStyleColor();
                }
                if (mod.has_exefs) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::TextWrapped("%s", tr("launcher.mods.exefs_warn"));
                    ImGui::PopStyleColor();
                }

                // Bundled mods have no reorder / delete controls (settings-linked).
                // Disabled mods keep Delete only — Up/Down apply to load order among enabled packs.
                // Park actions at the bottom (SetCursorPos — Dummy would add ItemSpacing and clip past the tile).
                if (!mod.managed) {
                    const float y_gap = ImGui::GetCursorPosY() + btn_gap;
                    const float y_bottom = ImGui::GetCursorPosY() + ImGui::GetContentRegionAvail().y - fh;
                    ImGui::SetCursorPosY(std::max(y_gap, y_bottom));
                    if (mod.enabled) {
                        if (button(up_l, kBlue, order[i] > 1, action_w)) {
                            std::string err;
                            if (nx::mod_manager::move_enabled(g_build.data, mod.id, -1, &err)) {
                                load_game_ini();
                                g_mods_refresh = true;
                                if (ImGui::GetCurrentContext()->NavInputSource == ImGuiInputSource_Gamepad)
                                    g_mods_refresh_wait_pad = true;
                            }
                        }
                        ImGui::SameLine();
                        if (button(down_l, kBlue, order[i] < enabled_count, action_w)) {
                            std::string err;
                            if (nx::mod_manager::move_enabled(g_build.data, mod.id, 1, &err)) {
                                load_game_ini();
                                g_mods_refresh = true;
                                if (ImGui::GetCurrentContext()->NavInputSource == ImGuiInputSource_Gamepad)
                                    g_mods_refresh_wait_pad = true;
                            }
                        }
                        ImGui::SameLine();
                    }
                    if (button(del_l, kRed, true, action_w)) g_mods_delete_id = mod.id;
                }

                ImGui::EndChild();
                // Soft settle animation like the add-ons cards.
                float e = out_cubic((ImGui::GetTime() - g_mods_since - 0.03 * double(i)) / 0.2);
                if (e < 1.f) {
                    ImVec2 a = tile_min, b(tile_min.x + tile_w, tile_min.y + tile_h);
                    ImGui::GetWindowDrawList()->AddRectFilled(a, b, rgb(248, 239, 214, int(180 * (1 - e))), S(14));
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        if (g_mods_refresh) {
            const bool pad_held = g_mods_refresh_wait_pad && ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown);
            if (!pad_held) {
                g_mods_refresh = false;
                g_mods_refresh_wait_pad = false;
                refresh_mods();  // enabled stay on top; disabled move to the bottom
            }
        }

        if (!g_mods_delete_id.empty()) {
            ImGui::Dummy(ImVec2(0, S(6)));
            ImGui::PushStyleColor(ImGuiCol_Text, col4(kInkSoft));
            const bool managed = [&] {
                for (const nx::mod_manager::ModInfo& m : g_mods)
                    if (m.id == g_mods_delete_id) return m.managed;
                return false;
            }();
            ImGui::TextWrapped("%s", tr(managed ? "launcher.mods.delete_confirm_managed" : "launcher.mods.delete_confirm"));
            ImGui::PopStyleColor();
            if (button(tr("launcher.mods.delete"), kRed)) {
                std::string err;
                std::string id = g_mods_delete_id;
                g_mods_delete_id.clear();
                if (nx::mod_manager::delete_mod(g_build.data, id, &err)) {
                    load_game_ini();
                    refresh_mods();
                    toast(tr("launcher.mods.deleted"));
                } else
                    g_banner = trf("launcher.mods.delete_failed", err);
            }
            ImGui::SameLine();
            if (button(tr("common.no"), kWhite)) g_mods_delete_id.clear();
        }

        ImGui::Dummy(ImVec2(0, S(8)));
        float pair_w = std::max({button_width(folder_l), button_width(zip_l), button_width(open_l)});
        if (button(folder_l, kBlue, true, pair_w)) pick(kPickModImportFolder);
        ImGui::SameLine();
        if (button(zip_l, kBlue, true, pair_w)) pick(kPickModImportZip);
        ImGui::SameLine();
        if (button(open_l, kWhite, true, pair_w)) open_folder(g_build.data + "/mods");
        align_right({close_l});
        if (button(close_l, kWhite) || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            g_mods_open = false, g_mods_delete_id.clear(), ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// A newer release on GitHub (desktop.cpp): a centred paper pill, warning mark, "New update detected" and a
// button to its page.
void update_notice() {
    const UpdateInfo u = update_info();
    if (!u.available || g_update_in_header) return;
    // In place when it can be (update_can_install), else the release page; then the restart into it.
    const UpdateProgress pr = update_progress();
    const bool busy = pr.stage != UpdateStage::Idle && pr.stage != UpdateStage::Done && pr.stage != UpdateStage::Failed;
    const bool in_place = update_can_install() || pr.stage != UpdateStage::Idle;
    std::string msg = trf("launcher.update.available", u.version);
    const char* get = tr(in_place ? "launcher.update.install" : "launcher.update.get");
    switch (pr.stage) {
        case UpdateStage::Downloading: msg = tr("launcher.update.downloading"); break;
        case UpdateStage::Checking: msg = tr("launcher.update.checking"); break;
        case UpdateStage::Unpacking:
        case UpdateStage::Installing: msg = tr("launcher.update.installing"); break;
        case UpdateStage::Done: msg = trf("launcher.update.installed", u.version), get = tr("launcher.update.restart"); break;
        case UpdateStage::Failed: msg = trf("launcher.update.failed", pr.error), get = tr("launcher.update.get"); break;
        default: break;
    }
    const float r = S(11), pad = S(12), vpad = S(6), gap = ImGui::GetStyle().ItemSpacing.x;
    const float h = ImGui::GetFrameHeight() + 2 * vpad;
    const float w = pad + 2 * r + gap + ImGui::CalcTextSize(msg.c_str()).x + gap + button_width(get) + pad;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 p(at.x + std::max(0.0f, (ImGui::GetContentRegionAvail().x - w) * 0.5f), at.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kSand, h * 0.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), kSandDeep, h * 0.5f, 0, S(1.5f));
    status_mark(dl, ImVec2(p.x + pad + r, p.y + h * 0.5f), r, Status::Warning);
    ImGui::SetCursorScreenPos(ImVec2(p.x + pad + 2 * r + gap, p.y + vpad));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(msg.c_str());
    ImGui::SameLine();
    if (busy) {
        ImGui::BeginDisabled();
        button(get, kGreen);
        ImGui::EndDisabled();
    } else if (button(get, kGreen)) {
        if (pr.stage == UpdateStage::Done) update_restart();
        else if (pr.stage != UpdateStage::Failed && in_place) update_install_start();
        else SDL_OpenURL(u.url.c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(at.x, p.y + h));
    ImGui::Dummy(ImVec2(0, S(6)));
}

// The main menu. In landscape: the island panel at the left (the leaf, the welcome, what the build holds and Play) and
// the list of the other actions at the right, divided by the Importer's dashes; stacked when narrow.
void page_home() {
    const bool valid = *g_build_problem == 0;
    if (!valid) {
        heading(tr("launcher.home.title_missing"),
                trf("launcher.home.missing", tr(g_build_problem)).c_str());
    } else if (!g_banner.empty()) {
        heading("", "");
    }
    // Settings, Online, Rebuild (or Prepare again), Desktop, Change build folder, Credits.
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
    if (valid) acts.push_back({"##online", nx::ui::kIconGlobe, kMint, tr("tab.online"), tr("online.home.desc"), true, 4});
    const bool player = !player_beside().empty();  // a release: the folder is played as it is, nothing to rebuild
    if (setup_supported() && !player)
        acts.push_back({"##rebuild", kIconHammer, kOrange, tr("launcher.home.rebuild"),
                        tr(can_rebuild ? "launcher.home.rebuild.desc" : "launcher.home.rebuild.unavailable"), can_rebuild, 1});
    // The game folder's code built for another ACNHRecomp, or not for this computer: prepared again here, keeping the
    // rest (prepare/README.md: run again on the same folder).
    const std::string folder = settings().get("launcher.build");
    const bool again = player && prepare_built() && is_prepared_folder(folder) &&
                       ((valid && g_build_stale) || std::strcmp(g_build_problem, "launcher.reason.no_game_lib") == 0);
    if (again)
        acts.push_back({"##again", kIconRefresh, kGreen, tr("launcher.home.prepare_again"),
                        tr(valid ? "launcher.home.prepare_again.stale" : "launcher.home.prepare_again.desc"), true, 5});
#if defined(__APPLE__)
    acts.push_back({"##desktop", kIconMonitorDown, kGreen, tr("launcher.home.install_app"),
                    tr("launcher.home.install_app.desc"), true, 3});
#else
    acts.push_back({"##desktop", kIconMonitorDown, kGreen, tr("launcher.home.desktop"), tr("launcher.home.desktop.desc"),
                    true, 3});
#endif
    acts.push_back({"##change", nx::ui::kIconFolder, kBlue, tr(player ? "launcher.home.change_game" : "launcher.home.change"),
                    tr(player ? "launcher.home.change_game.desc" : "launcher.home.change.desc"), true, 2});
    acts.push_back({"##credits", kIconHeart, kRed, tr("launcher.credits"), tr("launcher.credits.text"), true, 6});

    const float avail = ImGui::GetContentRegionAvail().x, gap = S(26);
    const bool two = valid && !g_portrait && avail >= S(820);
    // Heights from the last frame: the panel is as tall as the list beside it, its content centred in it.
    static float list_h = 0, hero_h = 0;
    if (valid) {
        const float hero_w = two ? std::floor(avail * 0.47f) : avail;
        const float pad = S(g_portrait ? 18 : 26);
        float h = std::max(hero_h > 0 ? hero_h + 2 * pad : S(420), two ? list_h : 0.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kSand));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(28));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        ImGui::BeginChild("##island", ImVec2(hero_w, std::floor(h)),
                          ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        const float room = ImGui::GetContentRegionAvail().y;
        if (hero_h > 0 && hero_h < room) ImGui::SetCursorPosY(std::floor(ImGui::GetCursorPosY() + (room - hero_h) * 0.5f));
        const float y0 = ImGui::GetCursorPosY();
        {
            float w = ImGui::GetContentRegionAvail().x, r = S(g_portrait ? 36 : 44);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 c(p.x + w * 0.5f, p.y + r);
            dl->AddCircleFilled(c, r, kLeafPale);
            draw_leaf(dl, c, r * 1.35f, -0.5f);
            ImGui::Dummy(ImVec2(w, 2 * r + S(12)));
        }
        centered(tr("launcher.home.title"), g_title, 1.1f, kInk);
        ImGui::Dummy(ImVec2(0, S(8)));
        contents_row();
        ImGui::Dummy(ImVec2(0, S(14)));
        {
            const float pw = std::min(ImGui::GetContentRegionAvail().x, S(380));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::floor((ImGui::GetContentRegionAvail().x - pw) * 0.5f));
            play_button(pw, S(72));
        }
        hero_h = ImGui::GetCursorPosY() - y0;
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        draw_stitch(ImGui::GetWindowDrawList(), ImVec2(wp.x + S(8), wp.y + S(8)),
                    ImVec2(wp.x + ws.x - S(8), wp.y + ws.y - S(8)), S(21), kSandDeep, S(2));
        ImGui::EndChild();
        if (two) ImGui::SameLine(0, gap);
        else ImGui::Dummy(ImVec2(0, S(10)));
    }
    ImGui::BeginGroup();
    const float y0 = ImGui::GetCursorPosY();
    float w = ImGui::GetContentRegionAvail().x;
    if (!valid) w = std::min(w, S(720)), ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - w) * 0.5f);
    const float x = ImGui::GetCursorPosX();
    // Beside the island panel the list fits the page: the rows keep their size and only the space around the dashes
    // between them gives way, so a sixth row (Rebuild, Prepare again) needs no scrolling.
    float sp = ImGui::GetStyle().ItemSpacing.y;
    if (two && acts.size() > 1) {
        float rows = 0;
        for (const A& a : acts) rows += action_height(a.title, a.desc, w);
        const float n = float(acts.size() - 1);
        const float room = ImGui::GetContentRegionAvail().y - S(4);
        sp = std::clamp((room - rows - n * S(2.5f)) / (2 * n), S(2), sp);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, sp));
    for (size_t k = 0; k < acts.size(); k++) {
        const A& a = acts[k];
        if (k) {
            ImGui::SetCursorPosX(x + S(12));
            dashes(w - S(24));
        }
        ImGui::SetCursorPosX(x);
        if (action_card(a.id, a.icon, a.color, a.title, a.desc, ImVec2(w, action_height(a.title, a.desc, w)), a.enabled)) {
            if (a.action == 0) load_game_ini(), g_page = Page::Settings;
            if (a.action == 4) load_game_ini(), g_page = Page::Online;
            if (a.action == 1) start_setup(true);
            if (a.action == 2) pick(kPickBuild);
            if (a.action == 5) g_prep_back = Page::Home, prep_open({folder}, folder, false);
            if (a.action == 6) g_page = Page::Credits;
            if (a.action == 3) {
                DesktopResult r = add_to_desktop();
                toast(r.ok ? trf("launcher.desktop.done", r.where) : std::string(tr("launcher.desktop.failed")));
            }
        }
    }
    list_h = ImGui::GetCursorPosY() - y0 - ImGui::GetStyle().ItemSpacing.y;
    ImGui::PopStyleVar();
    ImGui::EndGroup();
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
        for (auto& [v, t] : choices) {
            if (ImGui::Selectable(t.c_str(), v == cur) && v != cur) {
                g_game_ini.set(key, v);
                if (!g_game_ini.save()) toast(tr("launcher.settings.save_failed"));
            }
            if (v == cur) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    nav_item();
    frame_hover(true);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, S(2)));
}

void page_settings() {
    heading(tr("app.title"), tr("launcher.settings.text"));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##rows", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
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
        bool changed = toggle(tr("launcher.update.check"), &check);
        if (changed) {
            li.set("launcher.update_check", check ? "1" : "0");
            li.save();
            if (check) update_check_start();
        }
        bool autostart = li.get("launcher.autostart", "1") != "0";
        bool autostart_changed = toggle(tr("launcher.autostart.setting"), &autostart);
        if (autostart_changed) {
            li.set("launcher.autostart", autostart ? "1" : "0");
            if (!li.save()) toast(tr("launcher.settings.save_failed"));
            if (!autostart) g_autostart_at = -1, g_autostart_ui = false;
        }
        bool fullscreen = li.get("launcher.fullscreen", "0") == "1";
        bool fullscreen_changed = toggle(tr("launcher.fullscreen"), &fullscreen);
        if (fullscreen_changed) {
            if (SDL_SetWindowFullscreen(g_window, fullscreen)) {
                li.set("launcher.fullscreen", fullscreen ? "1" : "0");
                if (!li.save()) toast(tr("launcher.settings.save_failed"));
            } else {
                toast(SDL_GetError());
            }
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

// ---- online: local play over the network (ldn_mitm's protocol), the same settings as the game's Online page ------

void page_online() {
    heading(tr("tab.online"), tr("online.mode.desc"));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kPaper));
    ImGui::BeginChild("##rows", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    std::string mode = g_game_ini.get("online.mode", "nextendo");
    if (mode != "local" && mode != "nextendo") mode = "nextendo";
    ImGui::TextUnformatted(tr("online.mode"));
    text_small(tr("online.mode.desc"));
    if (ImGui::RadioButton(tr("online.mode.nextendo"), mode == "nextendo")) {
        mode = "nextendo";
        g_game_ini.set("online.mode", mode);
        if (!g_game_ini.save()) toast(tr("launcher.settings.save_failed"));
    }
    nav_item();
    if (ImGui::RadioButton(tr("online.mode.local"), mode == "local")) {
        mode = "local";
        g_game_ini.set("online.mode", mode);
        if (!g_game_ini.save()) toast(tr("launcher.settings.save_failed"));
    }
    nav_item();
    ImGui::Separator();
    if (mode == "nextendo") {
        nx::nextendo::set_data_root(g_build.data);
        nx::nextendo::set_mode_override("nextendo");
        nx::nextendo::set_client_id_override(g_game_ini.get("online.nextendo.client_id", ""));
        const BuildContents& c = contents();  // cached: not a scan of the game folder every frame
        if (!c.version.empty()) nx::nextendo::set_game_version(c.version);
        static double account_read = -1.0;  // the account file again once a second (a sign-in elsewhere shows up)
        if (const double now = ImGui::GetTime(); account_read < 0.0 || now - account_read >= 1.0)
            account_read = now, nx::nextendo::load_account();
        if (!nx::nextendo::version_ok_for_nextendo()) {
            ImGui::TextWrapped("%s", tr("online.nextendo.need_303"));
        } else {
            auto acc = nx::nextendo::account();
            if (acc.signed_in) {
                ImGui::TextUnformatted(trf("online.nextendo.signed_in", acc.username).c_str());
                if (!acc.friend_code.empty())
                    ImGui::TextUnformatted(trf("online.nextendo.friend_code", acc.friend_code).c_str());
                ImGui::TextWrapped("%s", tr("online.nextendo.requirements"));
                if (button(tr("online.nextendo.sign_out"), kWhite)) {
                    nx::nextendo::sign_out();
                    toast(tr("online.nextendo.sign_out"));
                }
            } else {
                ImGui::TextWrapped("%s", tr("online.nextendo.desc"));
                ImGui::TextWrapped("%s", tr("online.nextendo.requirements"));
                std::string cid = g_game_ini.get("online.nextendo.client_id", "");
                if (cid.empty()) cid = nx::nextendo::client_id();
                if (cid.empty()) {
                    ImGui::TextWrapped("%s", tr("online.nextendo.need_client_id"));
                    static char client_id_buf[128] = {};
                    static bool loaded_cid = false;
                    if (!loaded_cid) {
                        loaded_cid = true;
                        std::snprintf(client_id_buf, sizeof client_id_buf, "%s",
                                      g_game_ini.get("online.nextendo.client_id").c_str());
                    }
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if (ImGui::InputText("##nx_client", client_id_buf, sizeof client_id_buf)) {
                        g_game_ini.set("online.nextendo.client_id", client_id_buf);
                        nx::nextendo::set_client_id_override(client_id_buf);
                        if (!g_game_ini.save()) toast(tr("launcher.settings.save_failed"));
                    }
                    nav_item();
                }
                const bool busy = nx::nextendo::sign_in_busy();
                if (button(tr(busy ? "online.nextendo.signing_in" : "online.nextendo.sign_in"), kMint, !busy))
                    nx::nextendo::sign_in_start();
                if (!nx::nextendo::last_error().empty())
                    ImGui::TextWrapped("%s", nx::nextendo::last_error().c_str());
            }
        }
        ImGui::Separator();
    }
    bool on = g_game_ini.get("online.enabled", "0") == "1";
    bool enabled_changed = toggle(tr("online.enabled"), &on);
    if (enabled_changed) {
        g_game_ini.set("online.enabled", on ? "1" : "0");
        if (!g_game_ini.save()) toast(tr("launcher.settings.save_failed"));
    }
    if (on) {
        static const std::string address = nx::ip_text(nx::local_address());
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
        ImGuiContext& ctx = *ImGui::GetCurrentContext();
        if (ctx.NavActivateId == ImGui::GetID("##join") && ctx.NavInputSource == ImGuiInputSource_Gamepad)
            ctx.NavActivateFlags |= ImGuiActivateFlags_PreferInput;
        if (ImGui::InputText("##join", join, sizeof join)) {
            g_game_ini.set("online.join", join);
            if (!g_game_ini.save()) toast(tr("launcher.settings.save_failed"));
        }
        nav_item();
        if (ImGui::IsItemDeactivatedAfterEdit()) nx::address_check_start(join);
        if (join[0] && button(tr("online.join.test"), kWhite)) nx::address_check_start(join);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void footer_online() {
    if (button(tr("common.back"), kWhite)) g_page = Page::Home;
}

// ---- credits: contributors and the project's references -------------------------------------------------------

// One credit as a row of the home page's list: the name, what they did under it, and (with a page) an arrow; the
// row opens the page, hovered with a paper back, focused with a lagoon edge. Rows are divided by the Importer's dashes.
void credit_entry(const char* name, const char* desc, const char* url = nullptr) {
    ImGui::PushID(name);
    const float w = ImGui::GetContentRegionAvail().x, pad = S(12), arrow = url && *url ? S(36) : 0;
    const float inner = w - 2 * pad - arrow;
    const float nfs = font_size(g_title) * 0.66f, dfs = font_size(g_body) * 0.84f;
    const float nh = measure(g_title, nfs, name, inner).y, dh = measure(g_body, dfs, desc, inner).y;
    const float h = pad + nh + S(3) + dh + pad;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    float t = 0;
    bool focus = false;
    if (arrow > 0) {
        if (nav_card("##link", ImVec2(w, h))) SDL_OpenURL(url);
        t = hover_anim(), focus = nav_focused();
    } else {
        ImGui::Dummy(ImVec2(w, h));
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + w, p.y + h);
    if (t > 0 || focus) dl->AddRectFilled(p, q, mix(kSand, kBoard, focus ? 1.0f : t), S(16));
    if (focus) dl->AddRect(p, q, kLagoonDeep, S(16), 0, S(3));
    dl->AddText(g_title, nfs, ImVec2(p.x + pad, p.y + pad), kInk, name, nullptr, inner);
    dl->AddText(g_body, dfs, ImVec2(p.x + pad, p.y + pad + nh + S(3)), kInkSoft, desc, nullptr, inner);
    if (arrow > 0) {
        ImVec2 c(q.x - pad - S(12) + t * S(3), p.y + h * 0.5f);
        dl->AddCircleFilled(c, S(13), focus ? kYellow : mix(kBoard, kYellow, t));
        draw_icon(dl, nx::ui::kIconPlay, ImVec2(c.x + S(1), c.y), S(8), t > 0 || focus ? kInk : kInkSoft);
    }
    ImGui::PopID();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(12));
    dashes(w - S(24));
}

void page_credits() {
    heading(tr("launcher.credits"), tr("launcher.credits.text"));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##rows", ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);

    ImGui::PushFont(g_title);
    ImGui::TextUnformatted(tr("launcher.credits.contributors"));
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, S(4)));
    text_small(tr("launcher.credits.contributors.desc"));
    ImGui::Dummy(ImVec2(0, S(6)));
    credit_entry("Nycolazs", tr("launcher.credits.contrib.nycolazs"), "https://github.com/Nycolazs");
    credit_entry("erickgodev", tr("launcher.credits.contrib.erickgodev"), "https://github.com/erickgodev");
    credit_entry("SirOMGitsYOU", tr("launcher.credits.contrib.siromgitsyou"), "https://github.com/SirOMGitsYOU");
    credit_entry("vzeol", tr("launcher.credits.contrib.vzeol"), "https://github.com/vzeol");
    credit_entry("YaBoi69er", tr("launcher.credits.contrib.yaboi69er"), "https://github.com/YaBoi69er");
    credit_entry("Rafacasari", tr("launcher.credits.contrib.rafacasari"), "https://github.com/Rafacasari");
    credit_entry("RonyNjr", tr("launcher.credits.contrib.ronynjr"), "https://github.com/RonyNjr");
    credit_entry("cashcon57", tr("launcher.credits.contrib.cashcon57"), "https://github.com/cashcon57");

    ImGui::Dummy(ImVec2(0, S(10)));
    ImGui::PushFont(g_title);
    ImGui::TextUnformatted(tr("launcher.credits.references"));
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, S(4)));
    text_small(tr("launcher.credits.references.desc"));
    ImGui::Dummy(ImVec2(0, S(6)));
    credit_entry("SuperMarioWonderRecomp", tr("launcher.credits.ref.wonder"),
                 "https://github.com/BelmanteGu/SuperMarioWonderRecomp");
    credit_entry("Ryujinx / Ryubing", tr("launcher.credits.ref.ryujinx"),
                 "https://codeberg.org/Software_arch/Ryubing");
    credit_entry("Dear ImGui", tr("launcher.credits.ref.imgui"), "https://github.com/ocornut/imgui");
    credit_entry("MHGURecomp", tr("launcher.credits.ref.mhgu"), "https://github.com/morerokk/MHGURecomp");
    credit_entry("RenoDX", tr("launcher.credits.ref.renodx"), "https://github.com/clshortfuse/renodx");
    credit_entry("ACNH 60 FPS + SpeedFix", tr("launcher.credits.ref.speedfix"),
                 "https://github.com/tmouh/acnh-60fps");
    credit_entry("SDL3, Capstone, Unicorn, libvpx, libvorbis, FSR 1", tr("launcher.credits.ref.libs"));
    credit_entry("Fredoka, Nunito, Lucide, IconFontCppHeaders", tr("launcher.credits.ref.fonts"));
    credit_entry("LASRecomp", tr("launcher.credits.ref.las"), "https://github.com/peiikai/LASRecomp");
    credit_entry("SwitchRecomp", tr("launcher.credits.ref.switchrecomp"), "https://github.com/bgyss/SwitchRecomp");
    credit_entry("uDPR", tr("launcher.credits.ref.udpr"), "https://github.com/shatter9652/uDPR");
    credit_entry(tr("launcher.credits.ref.ptbr.name"), tr("launcher.credits.ref.ptbr"));

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void footer_credits() {
    if (button(tr("common.back"), kWhite)) g_page = Page::Home;
}

// ---- popups ---------------------------------------------------------------------------------------------------

void popups() {
    g_nav_section = 3;
    if (g_confirm_stop) ImGui::OpenPopup("stop");
    if (g_confirm_quit) ImGui::OpenPopup("quit");
    ImGui::SetNextWindowSize(ImVec2(std::min(ImGui::GetIO().DisplaySize.x * 0.9f, S(560)), 0));
    const bool preparing = prepare_running();
    if (ImGui::BeginPopupModal("stop", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("%s", tr(preparing ? "launcher.prep.cancel_confirm" : "launcher.build.cancel_confirm"));
        ImGui::Dummy(ImVec2(0, S(6)));
        if (button(tr("launcher.stop"), kRed)) {
            if (preparing)
                prepare_cancel();
            else
                setup_cancel();
            g_confirm_stop = false, ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        bool cancel = button(tr("common.no"), kWhite);
        ImGui::SetItemDefaultFocus();
        if (cancel || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) g_confirm_stop = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowSize(ImVec2(std::min(ImGui::GetIO().DisplaySize.x * 0.9f, S(560)), 0));
    if (ImGui::BeginPopupModal("quit", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("%s", tr(preparing ? "launcher.prep.quit_confirm" : "launcher.quit_confirm"));
        ImGui::Dummy(ImVec2(0, S(6)));
        if (button(tr("launcher.stop_quit"), kRed)) {
            setup_cancel();
            prepare_cancel();
            g_quit_after_stop = true;
            g_confirm_quit = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        bool cancel = button(tr("common.no"), kWhite);
        ImGui::SetItemDefaultFocus();
        if (cancel || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) g_confirm_quit = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (g_quit_after_stop && !setup_state().running && !prepare_running()) g_exit = true;
    addons_popup();
    saves_popup();
    mods_popup();
    g_nav_section = -1;
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
        if (n == "saves") g_page = Page::Home, g_saves_open = true, g_saves_since = -1;  // the save files open
        if (n == "mods") g_page = Page::Home, g_mods_open = true, g_mods_since = -1;    // the mods open
        if (n == "settings") g_page = Page::Settings;
        if (n == "credits") g_page = Page::Credits;
        // a folder chosen as the game folder (NX_LAUNCHER_PICK), as the welcome page's card or a drop gives it
        if (const char* f = std::getenv("NX_LAUNCHER_PICK"); n == "choose" && f) g_page = Page::Welcome, use_build_folder(f);
        // the prepare page with NX_LAUNCHER_PREPARE_IN (folders, ';' between), _DLC, _OUT; "preparing": started at once
        if (n == "prepare" || n == "preparing") {
            auto list = [](const char* v) {
                std::vector<std::string> out;
                for (std::string t = v ? v : ""; !t.empty();) {
                    const size_t c = t.find(';');
                    if (c) out.push_back(t.substr(0, c));
                    t = c == std::string::npos ? std::string() : t.substr(c + 1);
                }
                return out;
            };
            const char* o = std::getenv("NX_LAUNCHER_PREPARE_OUT");
            prep_open(list(std::getenv("NX_LAUNCHER_PREPARE_IN")), o ? o : "", false);
            g_prep.dlc = list(std::getenv("NX_LAUNCHER_PREPARE_DLC"));
            prep_probe();
            if (n == "preparing") prep_start();
        }
    }
}

void pages_drop(const std::string& path, float x, float y) {
    g_dragging = false;
    if (g_page == Page::Build || g_page == Page::Preparing) return;
    // On a card it is that card's; elsewhere it goes where it looks like it belongs: on the prepare page the game's
    // files (or an add-on's, by its folder's name), in a release a folder is the game folder (prepared first when it
    // is not yet), else a game folder to the files page and another folder is taken for a build folder.
    int slot = slot_at(ImVec2(x, y));
    if (slot >= kSlotPrepGame) return prep_pick(slot, path);
    if (g_page == Page::Prepare) {
        const bool addon = nx::prepare::dlc_index_from_path(path) >= 0 && !g_prep.inputs.empty();
        return prep_pick(addon ? kSlotPrepDlc : kSlotPrepGame, path);
    }
    if (!player_beside().empty() && is_dir(path) && (g_page == Page::Welcome || g_page == Page::Home))
        return use_build_folder(path);
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
    if (update_progress().stage == UpdateStage::Installing) return;  // a moment: the files are being swapped
    if (setup_state().running || prepare_running())
        g_confirm_quit = true;
    else
        g_exit = true;
}

bool pages_busy() {
    return g_animating || setup_state().running || prepare_running() || probe_busy() || ImGui::GetTime() < g_toast_until ||
           g_opening_since >= 0;
}

void pages_style(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s.FrameBorderSize = 1.5f * scale;  // dropdowns, checkboxes and fields stand out from the sand
    s.WindowRounding *= 30.0f / 28.0f;
    s.FrameRounding *= 2;  // pills (clamped to half the height)
    s.PopupRounding *= 1.4f;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = col4(kInk);
    c[ImGuiCol_TextDisabled] = col4(rgb(176, 158, 136));
    c[ImGuiCol_WindowBg] = col4(kBoard);
    c[ImGuiCol_ChildBg] = col4(kSand);
    c[ImGuiCol_PopupBg] = col4(kBoard);
    c[ImGuiCol_Border] = col4(kSandDeep);
    c[ImGuiCol_FrameBg] = col4(kBoard);  // paper on the sand panels, edged in deep sand
    c[ImGuiCol_FrameBgHovered] = col4(rgb(255, 250, 230));
    c[ImGuiCol_FrameBgActive] = col4(kYellow);
    c[ImGuiCol_ScrollbarGrab] = col4(rgb(226, 212, 174));
    c[ImGuiCol_ScrollbarGrabHovered] = col4(rgb(208, 192, 150));
    c[ImGuiCol_ScrollbarGrabActive] = col4(rgb(190, 172, 128));
    c[ImGuiCol_CheckMark] = col4(kLeafDeep);
    c[ImGuiCol_SliderGrab] = col4(kMint);
    c[ImGuiCol_SliderGrabActive] = col4(kLagoonDeep);
    c[ImGuiCol_Button] = col4(kBoard);  // also a dropdown's arrow box: the same as its frame
    c[ImGuiCol_ButtonHovered] = col4(rgb(255, 250, 230));
    c[ImGuiCol_ButtonActive] = col4(kSandDeep);
    c[ImGuiCol_Header] = col4(rgb(250, 236, 168));
    c[ImGuiCol_HeaderHovered] = col4(rgb(249, 226, 128));
    c[ImGuiCol_HeaderActive] = col4(kYellow);
    c[ImGuiCol_Separator] = col4(kSandDeep);
    c[ImGuiCol_PlotHistogram] = col4(kGreen);
    c[ImGuiCol_NavCursor] = col4(kLagoonDeep);
    c[ImGuiCol_ModalWindowDimBg] = col4(rgb(107, 84, 68, 102));
}

bool pages_draw(float scale, ImFont* title, ImFont* icons) {
    g_animating = false;
    g_scale = scale, g_title = title, g_body = ImGui::GetFont(), g_icons = icons;
    ImGuiIO& io = ImGui::GetIO();
    ImGuiContext& ctx = *ImGui::GetCurrentContext();
    static bool controller_navigation = false, controller_connected = false, popup_last_frame = false;
    static bool nav_page_valid = false;
    static Page nav_page = Page::Welcome;
    static ImGuiID saved_focus[int(Page::Count)] = {};
    const bool connected = (io.BackendFlags & ImGuiBackendFlags_HasGamepad) != 0;
    if (connected && !controller_connected) controller_navigation = true;
    if (!connected) controller_navigation = false;
    controller_connected = connected;
    if (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1)) controller_navigation = false;
    for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown; ++key)
        if (ImGui::IsKeyPressed(ImGuiKey(key), false)) controller_navigation = true;
    const bool popup_before = popup_last_frame || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) ||
                              g_confirm_stop || g_confirm_quit || g_addons_open || g_saves_open || g_mods_open;
    auto direction = [](ImGuiKey dpad, ImGuiKey stick) {
        return ImGui::IsKeyPressed(dpad, true) || ImGui::IsKeyPressed(stick, true);
    };
    const int dx = int(direction(ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight)) -
                   int(direction(ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft));
    const int dy = int(direction(ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown)) -
                   int(direction(ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp));
    const bool popup_requested = g_confirm_stop || g_confirm_quit || g_addons_open || g_saves_open || g_mods_open;
    const bool move = controller_navigation && (!popup_before || popup_requested) && ctx.ActiveId == 0 && (dx || dy);
    if (move) ImGui::NavMoveRequestCancel();
    static bool first = true;
    if (first) {
        first = false;
        if (g_express) pick(kPickGame);
        if (g_autostart_at == -2) g_autostart_at = ImGui::GetTime() + 3;
        update_check_start();
    }
    g_autostart_ui = g_autostart_at >= 0;
    if (g_autostart_at >= 0) {
        // A new release found stops it too: the notice stays on the home page to be read.
        bool stop = g_page != Page::Home || ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) ||
                    update_info().available;
        for (int k = ImGuiKey_NamedKey_BEGIN; !stop && k < ImGuiKey_NamedKey_END; k++)
            if (ImGui::IsKeyPressed(ImGuiKey(k), false) && !(k >= ImGuiKey_ReservedForModCtrl && k <= ImGuiKey_ReservedForModSuper))
                stop = true;
        if (stop) {
            g_autostart_at = -1;
            g_play_calm = true;
            // Drop click/nav highlight so Play returns to green instead of staying yellow.
            ImGui::ClearActiveID();
            ctx.NavId = 0;
            ctx.NavIdIsAlive = false;
            ImGui::SetNavCursorVisible(false);
        } else if (ImGui::GetTime() >= g_autostart_at) {
            g_autostart_at = -1;
            g_autostart_ui = false;
            play();
        }
    }
    apply_picks();
    if (prepare_running()) prepare_state();  // follows the job (its end) whatever the page
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !popup_before && ctx.ActiveId == 0) {
        switch (g_page) {
            case Page::Settings: case Page::Online: case Page::Credits: case Page::Done: g_page = Page::Home; break;
            case Page::Files: g_page = Page::Welcome; break;
            case Page::Install: g_page = Page::Files; break;
            case Page::Prepare: g_page = g_prep_back; break;
            case Page::Preparing:
                if (prepare_running()) g_confirm_stop = true;
                else g_page = Page::Prepare;
                break;
            case Page::Build:
                if (setup_state().running) g_confirm_stop = true;
                else g_page = g_rebuild ? Page::Home : Page::Install;
                break;
            default: break;
        }
    }
    ImVec2 ds = io.DisplaySize;
    g_portrait = ds.x < ds.y * 1.2f;
    draw_background(ds);
    ImVec2 size = g_portrait ? ImVec2(ds.x - S(16), ds.y - S(16))
                             : ImVec2(std::min(ds.x - S(32), S(1180)), std::min(ds.y - S(32), S(700)));
    ImVec2 pos((ds.x - size.x) * 0.5f, (ds.y - size.y) * 0.5f);
    // The board's soft, flat drop under it; its stitched edge is drawn first thing in the window.
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(pos.x, pos.y + S(6)), ImVec2(pos.x + size.x, pos.y + size.y + S(6)),
                                                  rgb(138, 106, 58, 31), S(30));
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(g_portrait ? 14 : 22), S(g_portrait ? 14 : 20)));
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    draw_stitch(ImGui::GetWindowDrawList(), ImVec2(pos.x + S(8), pos.y + S(8)),
                ImVec2(pos.x + size.x - S(8), pos.y + size.y - S(8)), S(23), kSandDeep, S(2));
    const Page drawn_page = g_page;
    g_nav_items.clear();
    g_nav_section = 0;
    ribbon();
    float footer = ImGui::GetFrameHeight() + S(24);  // + the buttons' lip (button())
    const bool paper_page = g_page == Page::Settings || g_page == Page::Credits;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, paper_page ? col4(kPaper) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(22));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(12)));
    // A page that overflows by a few pixels only (rounding, the buttons' lip) does not scroll: the wheel moved the
    // home page by that much with nothing to see. Its overflow from the last frame.
    static float overflow[int(Page::Count)] = {};
    const bool still = overflow[int(g_page)] < S(12);
    ImGui::BeginChild("##page", ImVec2(0, -footer), ((paper_page ? ImGuiChildFlags_AlwaysUseWindowPadding : ImGuiChildFlags_None) | ImGuiChildFlags_NavFlattened),
                      still ? ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar : ImGuiWindowFlags_None);
    if (still && ImGui::GetScrollY() != 0) ImGui::SetScrollY(0);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    g_nav_section = 1;
    // The short pages sit in the middle of the card: their height from the last frame. The offset is whole pixels:
    // ImGui rounds item positions down, so a fractional start changed the measured height and the page bobbed by a
    // pixel every frame.
    static float content_h[int(Page::Count)] = {};
    const int pi = int(g_page);
    const bool center = g_page == Page::Welcome || g_page == Page::Done || g_page == Page::Home;
    // Home's update notice floats at the top of the card, in the space above the centred page: it moves nothing
    // when it appears.
    // NX_LAUNCHER_UPDATE_AUTO=1 (tests, with NX_LAUNCHER_VERSION): the update installed at once, on any page
    static const bool auto_install = std::getenv("NX_LAUNCHER_UPDATE_AUTO") != nullptr;
    if (auto_install && update_progress().stage == UpdateStage::Idle && update_can_install()) update_install_start();
    if (g_page == Page::Home) {
        const ImVec2 at = ImGui::GetCursorPos();
        update_notice();
        ImGui::SetCursorPos(at);
    }
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
        case Page::Credits: page_credits(); break;
        case Page::Prepare: page_prepare(); break;
        case Page::Preparing: page_preparing(); break;
        case Page::Count: break;
    }
    if (int(g_page) == pi) content_h[pi] = ImGui::GetCursorPosY() - start;
    static const char* scroll = std::getenv("NX_LAUNCHER_SCROLL");  // tests: 0..1 of the page's scroll range
    if (scroll) ImGui::SetScrollY(ImGui::GetScrollMaxY() * float(std::atof(scroll)));
    overflow[pi] = ImGui::GetScrollMaxY();
    ImGui::EndChild();
    g_nav_section = 2;
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
        case Page::Credits: footer_credits(); break;
        case Page::Prepare: footer_prepare(); break;
        case Page::Preparing: footer_preparing(); break;
        case Page::Count: break;
    }
    g_nav_section = -1;
    popups();
    const bool popup_active = g_confirm_stop || g_confirm_quit || g_addons_open || g_saves_open || g_mods_open;
    if (controller_navigation && popup_active && ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        drawn_page == g_page && !g_nav_items.empty()) {
        auto first_popup_item = std::find_if(g_nav_items.begin(), g_nav_items.end(), [](const NavItem& item) {
            return item.section == 3;
        });
        auto focused_popup_item = std::find_if(g_nav_items.begin(), g_nav_items.end(), [&](const NavItem& item) {
            return item.section == 3 && item.id == ctx.NavId;
        });
        if (first_popup_item != g_nav_items.end()) {
            if (focused_popup_item == g_nav_items.end())
                nav_focus(*first_popup_item);
            else if (move)
                nav_move(dx, dx ? 0 : dy, 3);
        }
    } else if (controller_navigation && !popup_before && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        !g_confirm_stop && !g_confirm_quit && !g_addons_open && !g_saves_open && !g_mods_open &&
        drawn_page == g_page && !g_nav_items.empty()) {
        const bool page_changed = !nav_page_valid || nav_page != g_page;
        auto focused = std::find_if(g_nav_items.begin(), g_nav_items.end(), [&](const NavItem& item) { return item.id == ctx.NavId; });
        if (page_changed || focused == g_nav_items.end()) {
            const ImGuiID saved = saved_focus[int(g_page)];
            auto first = std::find_if(g_nav_items.begin(), g_nav_items.end(), [saved](const NavItem& item) { return saved && item.id == saved; });
            if (first == g_nav_items.end())
                first = std::find_if(g_nav_items.begin(), g_nav_items.end(), [](const NavItem& item) { return item.primary; });
            if (first == g_nav_items.end())
                first = std::find_if(g_nav_items.begin(), g_nav_items.end(), [](const NavItem& item) { return item.section == 1; });
            if (first == g_nav_items.end()) first = g_nav_items.begin();
            nav_focus(*first);
        } else if (move) {
            // Process one axis per press, in the direction the player chose.
            nav_move(dx, dx ? 0 : dy);
        }
        nav_page = g_page;
        nav_page_valid = true;
        saved_focus[int(g_page)] = ctx.NavId;
    }
    popup_last_frame = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || g_confirm_stop || g_confirm_quit ||
                       g_addons_open || g_saves_open || g_mods_open;
    opening_overlay();
    ImGui::End();
    return !g_exit;
}

}  // namespace launcher
