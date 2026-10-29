// The first-run launcher: the program a player opens before there is a game build (README, "Installation").
//
// It asks for the player's game folder (the game's executables and files from the player's own dump: exefs/, romfs/,
// aoc/ for add-on content) and the optional pt-BR translation, checks what this computer has, runs
// tools/setup_acnh.py as a child process and follows its stages, then starts the game it built. Or it is pointed at a
// build folder made earlier. It holds no game code, no keys and reads no game container.
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

namespace nx::plat {
void set_window_icon(SDL_Window* window);  // runtime/src/app_icon.cpp
}

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
std::string repo_root();            // the checkout holding tools/setup_acnh.py, "" if not found
std::string human_size(uint64_t bytes);
uint64_t free_space(const std::string& dir);  // of the folder, or of its nearest existing parent
std::string parent_dir(const std::string& path);
std::string file_name(const std::string& path);
bool is_file(const std::string& path);
bool is_dir(const std::string& path);

// ---- the player's files (files.cpp) ---------------------------------------------------------------------------

// The game folder (exefs/ and romfs/ of the player's own dump, aoc/ for add-on content) and the optional pt-BR
// translation package.
enum class Kind { Game, Mod };
enum class Status { Empty, Ok, Warning, Error };  // Warning: usable, but not everything works

struct FileCheck {
    Status status = Status::Empty;
    std::string text;  // translated
};

// Cheap checks: for the game folder its layout, the executables' headers (main's build id gives the version) and the
// RomFS's Shader/ folder; for the package, a zip's magic. Nothing is decrypted or unpacked.
FileCheck check_file(Kind kind, const std::string& path);
// What a dropped file or folder is: false when it is none of the kinds.
bool classify_file(const std::string& path, Kind& kind);

struct Found {
    std::string path;
    Kind kind;
    uint64_t size;
    FileCheck check;
};
// The game folders or packages in the user's Downloads, Desktop and Documents (a few folders deep), the top of the
// home folder and the folders of `near` (files or folders); each checked, the usable ones first.
std::vector<Found> search_files(Kind kind, const std::vector<std::string>& near);
std::string short_dir(const std::string& path);  // the file's (folder's) parent, the home folder shown as "~"

// ---- builds (files.cpp) ---------------------------------------------------------------------------------------

struct Build {
    std::string exe;   // the game's executable
    std::string data;  // its data root (NX_DATA_ROOT): analysis/, extract/romfs, config.ini, user/
};
// The build in `folder` (a launcher install, private/acnh, the repository itself, a build/ folder or a portable
// folder with data/): "" and `out` filled, else the i18n key of the reason.
const char* find_build(const std::string& folder, Build& out);
// ---- the desktop and new releases (desktop.cpp) ---------------------------------------------------------------

struct DesktopResult {
    bool ok = false;
    std::string where;  // the folder the shortcut (or, on macOS, the app) went to
};
DesktopResult add_to_desktop();  // a shortcut with the app's icon: desktop + Start menu / Applications / app menu
struct UpdateInfo {
    bool done = false, available = false;
    std::string version, url;  // the latest release's tag and page
};
std::string current_version();  // "v0.6.1"
void update_check_start();      // once per start, unless launcher.update_check = 0
UpdateInfo update_info();

// The player beside the launcher (a release, whose game code comes from the game folder): its path, else "".
std::string own_dir();        // the launcher's own folder (Contents/MacOS inside ACNHRecomp.app)
std::string player_beside();

// What a build holds: its game version (version.ini, "" if unknown) and the add-on content imported with it (aoc/).
struct AddOn {
    int index = 0;  // the add-on's number (its title is the application's + 0x1000 + index)
    uint64_t bytes = 0, files = 0;
};
struct BuildContents {
    std::string version;
    std::vector<AddOn> addons;
};
BuildContents build_contents(const std::string& data);  // reads the disk: call when the build changes

// ---- this computer (files.cpp) --------------------------------------------------------------------------------

struct Prereq {
    const char* label;  // i18n key
    Status status;
    std::string detail;  // translated
};
std::vector<Prereq> check_prerequisites(const std::string& install_dir);
std::string install_command();  // the package command for this OS ("" on Windows)

// ---- the setup (pipeline.cpp) ---------------------------------------------------------------------------------

enum Stage { kStageTools, kStageGame, kStageMods, kStageRecompile, kStageShaders, kStageFonts, kStageBuild,
             kStageCount };

struct SetupArgs {
    std::string game, ptbr_zip, install_dir;  // game: "" on a rebuild (the install folder links to it already)
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

// Starts the game in its own process. Its window opens where `from` is, at its size (or maximized like it), so the
// launcher's window seems to turn into the game's; keep `from` up until game_window_shown().
bool launch_game(const Build& build, std::string& error, SDL_Window* from = nullptr);
bool game_window_shown();  // the game started by launch_game shows its window, or has exited
// start.wav next to the launcher, if there is one, played once when the game starts (a sound logo). Optional and
// not part of the repository: whoever builds the launcher can drop their own there.
void start_sound_play();
bool start_sound_playing();
void open_folder(const std::string& path);

}  // namespace launcher
