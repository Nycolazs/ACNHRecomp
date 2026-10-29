// Paths, launcher.ini, the checks of the player's files and build folders, and the prerequisites (launcher.h).
#include "launcher.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "ui/i18n.h"

namespace launcher {

namespace fs = std::filesystem;
using nx::ui::tr;
using nx::ui::trf;

namespace {

// UTF-8 strings <-> paths on every OS (Windows paths are UTF-16).
fs::path u8(const std::string& p) { return fs::path(reinterpret_cast<const char8_t*>(p.c_str())); }
std::string str(const fs::path& p) {
    std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

std::string home() {
#if defined(_WIN32)
    return env("USERPROFILE");
#else
    return env("HOME");
#endif
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string ext(const std::string& path) { return lower(str(u8(path).extension())); }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool read_at(std::ifstream& f, uint64_t off, void* buf, size_t size) {
    f.clear();
    f.seekg(std::streamoff(off));
    f.read(static_cast<char*>(buf), std::streamsize(size));
    return size_t(f.gcount()) == size;
}

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) | uint64_t(rd32(p + 4)) << 32; }

// ---- the game folder -----------------------------------------------------------------------------------------

// The builds of main the project knows (games/acnh/versions.txt: the first 16 bytes of main's ModuleId, at 0x40 of
// the NSO): the version players know them by. Any other build is set up too, without the fixes at known addresses.
struct KnownBuild {
    const char* id;
    const char* version;
};
constexpr KnownBuild kBuilds[] = {
    {"FCD2BB238ABE99E925B4E452DF1F41F1", "3.0.2"},
    {"FF1D1C05670DB6021C85B624A710B963", "3.0.3"},
    {"0948E48778171EE6D5D7A65813ED5FCC", "2.0.7"},
    {"7FC1BAFF976AECA414520CB89F4616CB", "1.0.0"},
};

// A child folder of `dir` named `name` in any case ("exefs", "ExeFS"), or "".
fs::path child_dir(const fs::path& dir, const char* name) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (lower(str(e.path().filename())) == name && e.is_directory(ec)) return e.path();
    return {};
}

struct GameFolder {
    fs::path exefs, romfs, aoc;
};

// The parts of a game folder: exefs/ and romfs/ (or extract/romfs/), and aoc/ when there is add-on content.
bool game_parts(const std::string& path, GameFolder& g) {
    std::error_code ec;
    fs::path d = u8(path);
    if (!fs::is_directory(d, ec)) return false;
    g.exefs = child_dir(d, "exefs");
    g.romfs = child_dir(d, "romfs");
    if (g.romfs.empty() && fs::is_directory(d / "extract" / "romfs", ec)) g.romfs = d / "extract" / "romfs";
    g.aoc = child_dir(d, "aoc");
    return !g.exefs.empty() && !g.romfs.empty();
}

// Whether ACNHRecomp-Importer prepared the folder (and finished): its game.ini says so and .import/complete is
// there. ACNHRecomp only takes such folders; the importer is on the project's Discord (launcher.files.importer).
bool from_importer(const std::string& path) {
    std::error_code ec;
    fs::path d = u8(path);
    std::ifstream f(d / "game.ini");
    std::string first;
    std::getline(f, first);
    return first.find("ACNHRecomp-Importer") != std::string::npos && fs::exists(d / ".import" / "complete", ec);
}

// main's build id (hex), "" when the file is not an NSO.
std::string build_id(const fs::path& nso) {
    std::ifstream f(nso, std::ios::binary);
    uint8_t h[0x50];
    if (!f || !read_at(f, 0, h, sizeof h) || std::memcmp(h, "NSO0", 4) != 0) return "";
    char b[33];
    for (int k = 0; k < 16; k++) std::snprintf(b + 2 * k, 3, "%02X", h[0x40 + k]);
    return b;
}


uint64_t file_size(const std::string& path) {
    std::error_code ec;
    uint64_t n = fs::file_size(u8(path), ec);
    return ec ? 0 : n;
}

FileCheck ok(Status s, std::string text) { return {s, std::move(text)}; }

}  // namespace

// ---- Ini ------------------------------------------------------------------------------------------------------

void Ini::load(const std::string& path) {
    path_ = path;
    entries_.clear();
    std::ifstream f(u8(path));
    std::string line, section;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (!section.empty() && k.find('.') == std::string::npos) k = section + "." + k;
        set(k, v);
    }
}

bool Ini::save() const {
    std::error_code ec;
    fs::create_directories(u8(parent_dir(path_)), ec);
    std::vector<std::string> sections;
    for (const auto& [k, v] : entries_) {
        std::string s = k.substr(0, k.find('.'));
        if (std::find(sections.begin(), sections.end(), s) == sections.end()) sections.push_back(s);
    }
    std::ofstream f(u8(path_ + ".tmp"), std::ios::trunc);
    if (!f) return false;
    f << "# ACNHRecomp settings\n";
    for (const auto& s : sections) {
        f << "\n[" << s << "]\n";
        for (const auto& [k, v] : entries_)
            if (k.compare(0, s.size() + 1, s + ".") == 0) f << k.substr(s.size() + 1) << " = " << v << "\n";
    }
    f.close();
    if (!f) return false;
    fs::rename(u8(path_ + ".tmp"), u8(path_), ec);
    return !ec;
}

std::string Ini::get(const std::string& key, const std::string& fallback) const {
    for (const auto& [k, v] : entries_)
        if (k == key) return v;
    return fallback;
}

void Ini::set(const std::string& key, const std::string& value) {
    for (auto& [k, v] : entries_)
        if (k == key) {
            v = value;
            return;
        }
    entries_.emplace_back(key, value);
}

Ini& settings() {
    static Ini ini;
    return ini;
}

// ---- paths ----------------------------------------------------------------------------------------------------

std::string launcher_ini_path() {
    if (std::string p = env("NX_LAUNCHER_INI"); !p.empty()) return p;  // tests
    std::string dir;
    if (char* pref = SDL_GetPrefPath("", "ACNHRecomp")) {
        dir = pref;
        SDL_free(pref);
    }
    if (dir.empty()) dir = home() + "/.ACNHRecomp/";
    return dir + "launcher.ini";
}

std::string default_install_dir() {
#if defined(_WIN32)
    std::string base = env("LOCALAPPDATA");
    return (base.empty() ? home() + "\\AppData\\Local" : base) + "\\ACNHRecomp";
#elif defined(__APPLE__)
    return home() + "/Library/Application Support/ACNHRecomp";
#else
    std::string base = env("XDG_DATA_HOME");
    return (base.empty() ? home() + "/.local/share" : base) + "/ACNHRecomp";
#endif
}

std::string repo_root() {
    std::vector<std::string> starts;
    if (const char* base = SDL_GetBasePath()) starts.push_back(base);  // the launcher's own folder
#if defined(NX_LAUNCHER_SOURCE_DIR)
    starts.push_back(NX_LAUNCHER_SOURCE_DIR);
#endif
    std::error_code ec;
    for (const std::string& s : starts)
        for (fs::path p = u8(s); !p.empty(); p = p.parent_path()) {
            if (fs::is_regular_file(p / "tools" / "setup_acnh.py", ec)) return str(p);
            if (p == p.parent_path()) break;
        }
    return "";
}

std::string human_size(uint64_t bytes) {
    char b[32];
    if (bytes >= 1000000000ull)
        std::snprintf(b, sizeof b, "%.1f GB", double(bytes) / 1e9);
    else if (bytes >= 1000000ull)
        std::snprintf(b, sizeof b, "%.0f MB", double(bytes) / 1e6);
    else
        std::snprintf(b, sizeof b, "%.0f KB", double(bytes) / 1e3);
    return b;
}

uint64_t free_space(const std::string& dir) {
    std::error_code ec;
    for (fs::path p = u8(dir); !p.empty(); p = p.parent_path()) {
        if (fs::exists(p, ec)) {
            fs::space_info s = fs::space(p, ec);
            return ec ? 0 : s.available;
        }
        if (p == p.parent_path()) break;
    }
    return 0;
}

std::string parent_dir(const std::string& path) { return str(u8(path).parent_path()); }
std::string file_name(const std::string& path) { return str(u8(path).filename()); }

bool is_file(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(u8(path), ec);
}

bool is_dir(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::is_directory(u8(path), ec);
}

// ---- the player's files ---------------------------------------------------------------------------------------

FileCheck check_file(Kind kind, const std::string& path) {
    if (path.empty()) return {};
    if (kind == Kind::Mod) {
        std::ifstream f(u8(path), std::ios::binary);
        if (!f) return ok(Status::Error, tr("launcher.err.unreadable"));
        char m[4] = {};
        f.read(m, 4);
        return std::memcmp(m, "PK\x03\x04", 4) == 0 ? ok(Status::Ok, trf("launcher.ok.zip", human_size(file_size(path))))
                                                    : ok(Status::Error, tr("launcher.err.zip"));
    }
    // The game folder: only what is needed to tell it apart is read (the executables' headers, folder names).
    GameFolder g;
    if (!is_dir(path)) return ok(Status::Error, tr("launcher.err.not_folder"));
    if (!game_parts(path, g)) return ok(Status::Error, tr("launcher.err.game_layout"));
    if (!from_importer(path)) return ok(Status::Error, tr("launcher.err.game_importer"));
    std::error_code ec;
    std::string id;
    for (const char* m : {"main", "sdk", "subsdk0"}) {
        std::string b = build_id(g.exefs / m);
        if (b.empty()) return ok(Status::Error, trf("launcher.err.game_exefs", m));
        if (std::strcmp(m, "main") == 0) id = b;
    }
    if (!fs::is_directory(g.romfs / "Shader", ec)) return ok(Status::Error, tr("launcher.err.game_romfs"));
    size_t addons = 0;
    if (!g.aoc.empty())
        for (const auto& e : fs::directory_iterator(g.aoc, ec))
            if (e.is_directory(ec)) addons++;
    std::string extra = addons ? trf("launcher.game.addons", std::to_string(addons)) : std::string();
    for (const KnownBuild& k : kBuilds)
        if (id == k.id) return ok(Status::Ok, trf("launcher.ok.game", k.version) + extra);
    return ok(Status::Warning, tr("launcher.warn.game_version") + extra);
}

bool classify_file(const std::string& path, Kind& kind) {
    GameFolder g;
    if (is_dir(path) && game_parts(path, g)) return kind = Kind::Game, true;
    if (ext(path) == ".zip") return kind = Kind::Mod, true;
    return false;
}

std::string short_dir(const std::string& path) {
    std::string d = parent_dir(path), h = home();
    if (!h.empty() && d.compare(0, h.size(), h) == 0 && (d.size() == h.size() || d[h.size()] == '/' || d[h.size()] == '\\'))
        return "~" + d.substr(h.size());
    return d;
}

namespace {

// Whether a found entry is worth checking as `kind`: a folder holding exefs/ and romfs/, or the pt-BR package (a .zip
// named like a translation).
bool search_match(Kind kind, const std::string& path) {
    if (kind == Kind::Game) {
        GameFolder g;
        return game_parts(path, g);
    }
    std::string name = lower(file_name(path));
    if (ext(path) != ".zip") return false;
    for (const char* w : {"ptbr", "pt-br", "pt_br", "pt br", "tradu", "portug", "brasil", "brazil"})
        if (name.find(w) != std::string::npos) return true;
    return false;
}

}  // namespace

std::vector<Found> search_files(Kind kind, const std::vector<std::string>& near) {
    // {folder, how deep}: a few levels under the user's folders (Downloads/ACNH/...), only the top of the home folder.
    std::vector<std::pair<fs::path, int>> roots;
    std::error_code ec;
    auto root = [&](const fs::path& p, int depth) {
        if (p.empty() || !fs::is_directory(p, ec)) return;
        for (const auto& [q, d] : roots)
            if (fs::equivalent(p, q, ec)) return;
        roots.emplace_back(p, depth);
    };
    for (SDL_Folder f : {SDL_FOLDER_DOWNLOADS, SDL_FOLDER_DESKTOP, SDL_FOLDER_DOCUMENTS})
        if (const char* p = SDL_GetUserFolder(f)) root(u8(p), 3);
    const fs::path h = u8(home());
    for (const char* n : {"Downloads", "Desktop", "Documents"}) root(h / n, 3);
    for (const std::string& n : near) root(is_dir(n) ? u8(n) : u8(n).parent_path(), 2);
    root(h, 1);
    std::vector<fs::path> files;  // the files (the pt-BR package) or folders (a game folder) to check
    for (const auto& [r, depth] : roots) {
        int budget = 20000;  // entries per folder: a huge Downloads folder costs a moment, never a hang
        for (auto it = fs::recursive_directory_iterator(r, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator() && budget-- > 0; it.increment(ec)) {
            if (ec) break;
            const fs::path& p = it->path();
            if (it->is_directory(ec)) {
                std::string n = str(p.filename());
                if (kind == Kind::Game && search_match(kind, str(p))) {
                    files.push_back(p);
                    it.disable_recursion_pending();  // its own exefs/ and romfs/ are not other games
                    continue;
                }
                if (it.depth() + 1 >= depth || n.empty() || n[0] == '.' || n == "node_modules") it.disable_recursion_pending();
                continue;
            }
            if (kind != Kind::Game) files.push_back(p);
        }
    }
    std::vector<Found> out;
    std::vector<fs::path> seen;
    for (const fs::path& p : files) {
        std::string s = str(p);
        if (kind != Kind::Game && (!fs::is_regular_file(p, ec) || !search_match(kind, s))) continue;
        fs::path c = fs::weakly_canonical(p, ec);
        if (std::find(seen.begin(), seen.end(), c) != seen.end()) continue;
        seen.push_back(c);
        out.push_back({s, kind, kind == Kind::Game ? 0 : file_size(s), check_file(kind, s)});
        if (out.size() >= 40) break;
    }
    // The usable ones first, then by name.
    std::stable_sort(out.begin(), out.end(), [](const Found& a, const Found& b) {
        if (a.check.status != b.check.status) return int(a.check.status) < int(b.check.status);
        return lower(file_name(a.path)) < lower(file_name(b.path));
    });
    return out;
}

// ---- builds ---------------------------------------------------------------------------------------------------

BuildContents build_contents(const std::string& data) {
    BuildContents c;
    // version.ini (tools/acnh_version.py): "version = 2.0.7"
    std::ifstream ini(u8(data + "/version.ini"));
    for (std::string l; std::getline(ini, l);) {
        size_t eq = l.find('=');
        if (eq != std::string::npos && trim(l.substr(0, eq)) == "version") c.version = trim(l.substr(eq + 1));
    }
    // The add-on content: aoc/<index>/, linked from the game folder (runtime/src/hle_fs.cpp mounts the same folders).
    std::error_code ec;
    for (auto& e : fs::directory_iterator(u8(data + "/aoc"), ec)) {
        if (!e.is_directory(ec)) continue;
        std::string name = str(e.path().filename());
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
        AddOn a;
        a.index = std::atoi(name.c_str());
        for (auto& f : fs::recursive_directory_iterator(e.path(), ec))
            if (f.is_regular_file(ec)) a.files++, a.bytes += f.file_size(ec);
        c.addons.push_back(a);
    }
    std::sort(c.addons.begin(), c.addons.end(), [](const AddOn& x, const AddOn& y) { return x.index < y.index; });
    return c;
}

const char* find_build(const std::string& folder, Build& out) {
#if defined(_WIN32)
    const char* exe = "acnh.exe";
#else
    const char* exe = "acnh";
#endif
    const fs::path f = u8(folder);
    // {executable, data root} in the layouts a build can have.
    const std::pair<fs::path, fs::path> layouts[] = {
        {f / "build" / exe, f},                                     // the launcher's install folder, private/acnh
        {f / "private" / "acnh" / "build" / exe, f / "private" / "acnh"},  // the repository
        {f / exe, f.parent_path()},                                 // its build/ folder
        {f / exe, f / "data"},                                      // a portable folder
    };
    std::error_code ec;
    bool any_exe = false, any_data = false;
    for (const auto& [x, d] : layouts) {
        bool has_exe = fs::is_regular_file(x, ec);
        bool has_data = fs::is_regular_file(d / "analysis" / "main.bin", ec) && fs::is_directory(d / "extract" / "romfs", ec);
        any_exe |= has_exe, any_data |= has_data;
        if (has_exe && has_data) {
            out = {str(fs::absolute(x, ec)), str(fs::absolute(d, ec))};
            return "";
        }
    }
    return any_data ? "launcher.reason.no_exe" : any_exe ? "launcher.reason.no_data" : "launcher.reason.none";
}

// ---- this computer --------------------------------------------------------------------------------------------

namespace {

bool in_path(const char* tool) {
    std::string path = env("PATH");
#if defined(_WIN32)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::stringstream ss(path);
    std::string dir;
    std::error_code ec;
    while (std::getline(ss, dir, sep))
        if (!dir.empty() && fs::is_regular_file(u8(dir) / tool, ec)) return true;
    return false;
}

bool sdl3_dev_files() {
    std::vector<fs::path> c = {"/usr/lib/cmake/SDL3", "/usr/lib64/cmake/SDL3", "/usr/local/lib/cmake/SDL3",
                               "/usr/local/lib64/cmake/SDL3", "/usr/share/cmake/SDL3", "/opt/homebrew/lib/cmake/SDL3",
                               "/usr/local/opt/sdl3/lib/cmake/SDL3"};
    std::error_code ec;
    for (const auto& d : fs::directory_iterator("/usr/lib", ec))  // multiarch: /usr/lib/x86_64-linux-gnu/cmake/SDL3
        c.push_back(d.path() / "cmake" / "SDL3");
    std::stringstream ss(env("CMAKE_PREFIX_PATH"));
    std::string p;
    while (std::getline(ss, p, ':'))
        if (!p.empty()) c.push_back(u8(p) / "lib" / "cmake" / "SDL3");
    for (const auto& d : c)
        if (fs::is_regular_file(d / "SDL3Config.cmake", ec)) return true;
    return false;
}

#if defined(_WIN32)
// The Visual Studio C++ build tools (Windows SDK, C runtime) the setup's clang-cl links against: vswhere lists them.
bool msvc_tools() {
    std::string pf = env("ProgramFiles(x86)");
    fs::path vswhere = u8(pf) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
    std::error_code ec;
    if (!fs::is_regular_file(vswhere, ec)) return false;
    std::string cmd = "\"\"" + str(vswhere) + "\" -latest -products * -requires "
                      "Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath\"";
    FILE* p = _popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[512];
    std::string out;
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    _pclose(p);
    return !trim(out).empty();
}
#endif

bool vulkan_loader() {
    if (!SDL_Vulkan_LoadLibrary(nullptr)) return false;
    SDL_Vulkan_UnloadLibrary();
    return true;
}

}  // namespace

std::vector<Prereq> check_prerequisites(const std::string& install_dir) {
    std::vector<Prereq> r;
    auto add = [&](const char* label, bool good, Status bad, std::string detail_good, std::string detail_bad) {
        r.push_back({label, good ? Status::Ok : bad, good ? std::move(detail_good) : std::move(detail_bad)});
    };
    const std::string found = tr("launcher.check.found"), missing = tr("launcher.check.missing");
#if defined(_WIN32)
    // tools/setup_acnh.py fetches LLVM (clang-cl), SDL3, CMake and Ninja itself: Python, Visual Studio's C++ tools and
    // a Vulkan driver are what it needs from the system.
    add("launcher.check.scripts", !repo_root().empty(), Status::Error, found, tr("launcher.err.no_repo"));
    add("launcher.check.python", in_path("py.exe") || in_path("python.exe"), Status::Error, found, missing);
    add("launcher.check.compiler", msvc_tools(), Status::Error, found, missing);
    add("launcher.check.vulkan", vulkan_loader(), Status::Error, found, missing);
#else
    add("launcher.check.scripts", !repo_root().empty(), Status::Error, found, tr("launcher.err.no_repo"));
    add("launcher.check.compiler", in_path("clang") && in_path("clang++"), Status::Error, found, missing);
    add("launcher.check.python", in_path("python3"), Status::Error, found, missing);
    add("launcher.check.tools", in_path("curl") && in_path("tar") && in_path("bash"), Status::Error, found, missing);
    add("launcher.check.sdl", sdl3_dev_files(), Status::Error, found, missing);
    add("launcher.check.vulkan", vulkan_loader(), Status::Error, found, missing);
#if defined(__APPLE__)
    std::error_code ec;
    bool movie = (std::system("pkg-config --exists vpx vorbis >/dev/null 2>&1") == 0) ||
                 (fs::exists("/opt/homebrew/include/vpx/vpx_decoder.h", ec) && fs::exists("/opt/homebrew/include/vorbis/codec.h", ec)) ||
                 (fs::exists("/usr/local/include/vpx/vpx_decoder.h", ec) && fs::exists("/usr/local/include/vorbis/codec.h", ec));
#else
    bool movie = std::system("pkg-config --exists vpx vorbis >/dev/null 2>&1") == 0;
#endif
    add("launcher.check.movie", movie, Status::Warning, found, tr("launcher.check.movie_missing"));
#endif
    uint64_t free = free_space(install_dir);
    const uint64_t GB = 1000000000ull;
    r.push_back({"launcher.check.disk", free >= 30 * GB ? Status::Ok : free >= 20 * GB ? Status::Warning : Status::Error,
                 free >= 30 * GB ? trf("launcher.install.free", human_size(free))
                                 : trf("launcher.check.disk_low", human_size(free))});
    // 4 GB is enough: the setup runs fewer compile jobs at once on less memory, so it only takes longer. A "4 GB"
    // computer reports a little less (memory kept for the firmware or the integrated GPU).
    int ram_mb = SDL_GetSystemRAM();
    char ram[32];
    std::snprintf(ram, sizeof ram, "%.0f GB", ram_mb / 1024.0);
    r.push_back({"launcher.check.ram", ram_mb >= 3500 ? Status::Ok : Status::Warning,
                 ram_mb >= 15 * 1024 ? std::string(ram)
                 : ram_mb >= 3500    ? trf("launcher.check.ram_small", ram)
                                     : trf("launcher.check.ram_low", ram)});
    return r;
}

std::string install_command() {
#if defined(_WIN32)
    return "winget install Python.Python.3.13\n"
           "winget install Microsoft.VisualStudio.2022.BuildTools --override "
           "\"--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended\"";
#elif defined(__APPLE__)
    return "xcode-select --install\nbrew install llvm cmake ninja sdl3 molten-vk vulkan-loader libvpx libvorbis";
#else
    std::ifstream f("/etc/os-release");
    std::string line, ids;
    while (std::getline(f, line))
        if (line.rfind("ID=", 0) == 0 || line.rfind("ID_LIKE=", 0) == 0) ids += " " + lower(line.substr(line.find('=') + 1));
    if (ids.find("fedora") != std::string::npos || ids.find("rhel") != std::string::npos)
        return "sudo dnf install clang lld cmake ninja-build patch curl python3 spirv-tools pkgconf-pkg-config \\\n"
               "     vulkan-loader-devel SDL3-devel libvpx-devel libvorbis-devel mesa-vulkan-drivers vulkan-tools";
    if (ids.find("arch") != std::string::npos)
        return "sudo pacman -S --needed base-devel clang lld cmake ninja patch curl python spirv-tools pkgconf \\\n"
               "     vulkan-icd-loader vulkan-headers sdl3 libvpx libvorbis vulkan-tools";
    return "sudo apt install build-essential clang lld cmake ninja-build patch curl python3-venv spirv-tools pkg-config \\\n"
           "     libvulkan-dev libsdl3-dev libvpx-dev libvorbis-dev mesa-vulkan-drivers vulkan-tools";
#endif
}

}  // namespace launcher
