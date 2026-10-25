// The first-run launcher: the program a player opens before there is a game build (README, "Installation").
//
// It asks for the player's own files (keys, base game, update 3.0.2 or 3.0.3, DLC, the optional pt-BR translation), checks
// what this computer has, runs tools/setup_acnh.sh as a child process and follows its stages, then starts the game
// it built. Or it is pointed at a build folder made earlier. It holds no game code.
//
//   main.cpp      SDL3 window, Dear ImGui (SDL_Renderer backend), fonts and the frame loop
//   pages.cpp     the pages: welcome, files, install, build, done, home, settings
//   files.cpp     paths, launcher.ini, checks of the chosen files and of build folders, prerequisites
//   pipeline.cpp  the setup as a child process (stages, progress, cancel) and starting the game
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct ImFont;
struct SDL_Window;

namespace launcher {

// ---- pages (pages.cpp) ----------------------------------------------------------------------------------------

void pages_init(SDL_Window* window);
// One frame of the current page inside an ImGui frame; false once the launcher should exit (the game started).
bool pages_draw(float scale, ImFont* title);
void pages_drop(const std::string& path, float x, float y);  // a file dropped on the window, at x, y
void pages_drag(bool over, float x, float y);                // files dragged over the window (or no longer)
void pages_close_requested();                                // the window's close button (asks first while it builds)
bool pages_busy();                                           // something animates: draw at full rate

// ---- a "section.key = value" file grouped under [section] headers, as config.cpp writes config.ini ----------

class Ini {
public:
    void load(const std::string& path);  // a missing file is empty
    bool save() const;
    std::string get(const std::string& key, const std::string& fallback = "") const;
    void set(const std::string& key, const std::string& value);
    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::vector<std::pair<std::string, std::string>> entries_;  // file order
};

Ini& settings();  // launcher.ini (launcher_ini_path)

// ---- paths (files.cpp) ----------------------------------------------------------------------------------------

std::string launcher_ini_path();    // NX_LAUNCHER_INI, else the user's settings folder (SDL_GetPrefPath)
std::string default_install_dir();  // ~/.local/share, ~/Library/Application Support, %LOCALAPPDATA% + /ACNHRecomp
std::string repo_root();            // the checkout holding tools/setup_acnh.sh, "" if not found
std::string human_size(uint64_t bytes);
uint64_t free_space(const std::string& dir);  // of the folder, or of its nearest existing parent
std::string parent_dir(const std::string& path);
std::string file_name(const std::string& path);
bool is_file(const std::string& path);
bool is_dir(const std::string& path);

// ---- the player's files (files.cpp) ---------------------------------------------------------------------------

enum class Kind { Keys, TitleKeys, Base, Update, Dlc, Mod };
enum class Status { Empty, Ok, Warning, Error };  // Warning: usable, but could not be checked fully

struct FileCheck {
    Status status = Status::Empty;
    std::string text;  // translated
};

// Cheap checks without keys: the container's magic, and for an NSP the title and version of its plain-text
// metadata (cnmt.xml) or of its ticket's name. The importer verifies every block later.
FileCheck check_file(Kind kind, const std::string& path);
// What a dropped file is: false when it is none of the kinds.
bool classify_file(const std::string& path, Kind& kind);
// prod.keys in the usual places (~/.switch, the emulators' folders) or next to one of `near`; "" if none.
std::string find_keys(const std::vector<std::string>& near);
std::string title_keys_for(const std::string& prod_keys);  // title.keys next to it, "" if none

struct Found {
    std::string path;
    Kind kind;
    uint64_t size;
    FileCheck check;
};
// The files that look like `kind` in the user's Downloads, Desktop and Documents (a few folders deep), the top of
// the home folder, the folders of `near` (files or folders) and, for keys, the emulators' folders; each checked, the
// usable ones first.
std::vector<Found> search_files(Kind kind, const std::vector<std::string>& near);
std::string short_dir(const std::string& path);  // the file's folder, the home folder shown as "~"

// ---- builds (files.cpp) ---------------------------------------------------------------------------------------

struct Build {
    std::string exe;   // the game's executable
    std::string data;  // its data root (NX_DATA_ROOT): analysis/, extract/romfs, config.ini, user/
};
// The build in `folder` (a launcher install, private/acnh, the repository itself, a build/ folder or a portable
// folder with data/): "" and `out` filled, else the i18n key of the reason.
const char* find_build(const std::string& folder, Build& out);

// ---- this computer (files.cpp) --------------------------------------------------------------------------------

struct Prereq {
    const char* label;  // i18n key
    Status status;
    std::string detail;  // translated
};
std::vector<Prereq> check_prerequisites(const std::string& install_dir);
std::string install_command();  // the package command for this OS ("" on Windows)

// ---- the setup (pipeline.cpp) ---------------------------------------------------------------------------------

enum Stage { kStageTools, kStageImport, kStageMods, kStageRecompile, kStageShaders, kStageFonts, kStageBuild,
             kStageCount };

struct SetupArgs {
    std::string keys, title_keys, base, update, ptbr_zip, install_dir;
    std::vector<std::string> dlc;
};

struct SetupState {
    bool running = false, finished = false, cancelled = false;
    int exit_code = 0;
    int stage = -1;               // the stage running (or that failed), -1 before the first
    bool done[kStageCount] = {};  // stages passed
    float build_fraction = 0;     // ninja's [n/total] in the build stage
    double elapsed = 0;           // seconds since the start
    double eta = -1;              // seconds left in the build stage, -1 while unknown
    std::string last_line;
};

bool setup_supported();  // a bash pipeline: Linux and macOS
bool setup_start(const SetupArgs& args, std::string& error);
void setup_cancel();
SetupState setup_state();
float setup_progress(const SetupState& s);  // 0..1 over every stage
// The output lines from index `from` on, appended to `out`.
void setup_lines(size_t from, std::vector<std::string>& out);
std::string setup_log_path();  // the full output, also written to <install>/logs/launcher.log

// ---- the game (pipeline.cpp) ----------------------------------------------------------------------------------

bool launch_game(const Build& build, std::string& error);
void open_folder(const std::string& path);

}  // namespace launcher
