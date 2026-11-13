// The launcher on this computer's desktop, and new releases (launcher.h, "desktop.cpp").
//
// add_to_desktop(): a shortcut to the launcher, with the app's icon, where a player starts programs:
//  - Windows: ACNHRecomp.lnk on the desktop and in the Start menu (the icon is the executable's, assets/acnhrecomp.rc);
//  - macOS: ACNHRecomp.app copied into Applications (/Applications, else ~/Applications), freed of the download
//    quarantine, and an alias of it on the desktop;
//  - Linux: io.github.acnhrecomp.desktop in the applications menu and on the desktop, its icon in the user's theme
//    (acnhrecomp.png from the package). The game and the launcher take that app id, so Wayland finds the icon.
// Update check: GitHub's latest release of the project, asked once per start (launcher.update_check = 0: never),
// through curl (Windows 10+, macOS, Linux) on a thread of its own.
#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <iterator>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
extern char** environ;
#endif

#include "launcher.h"
#include "sha256.h"

namespace launcher {

namespace {

namespace fs = std::filesystem;

fs::path p8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string s8(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// Runs a program (looked up in PATH) without a console window and waits; its exit code, or -1 if it could not start.
// (SDL_CreateProcess either shows a console on Windows or, in the background, gives no exit code.)
int run(const std::vector<std::string>& argv) {
#if defined(_WIN32)
    std::wstring cmd;
    for (const std::string& a : argv) {
        int n = MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, w.data(), n);
        if (!cmd.empty()) cmd += L' ';
        cmd += L'"' + w + L'"';  // no argument here holds a quote
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    return int(code);
#else
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t child = 0;
    int rc = posix_spawnp(&child, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return -1;
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

bool run_ok(const std::vector<std::string>& argv) { return run(argv) == 0; }

std::string read_all(const fs::path& p) {
    std::string s;
#if defined(_WIN32)
    FILE* f = _wfopen(p.c_str(), L"rb");
#else
    FILE* f = std::fopen(p.c_str(), "rb");
#endif
    if (!f) return s;
    char b[16384];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
    return s;
}

#if defined(_WIN32)
fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR w = nullptr;
    fs::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &w))) p = w;
    CoTaskMemFree(w);
    return p;
}

bool make_link(const fs::path& lnk, const fs::path& target) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(target.parent_path().c_str());
    link->SetIconLocation(target.c_str(), 0);
    link->SetDescription(L"ACNHRecomp");
    IPersistFile* file = nullptr;
    bool ok = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Save(lnk.c_str(), TRUE));
    if (file) file->Release();
    link->Release();
    return ok;
}
#endif

#if defined(__linux__)
fs::path home() {
    const char* h = std::getenv("HOME");
    return h ? fs::path(h) : fs::path();
}
fs::path desktop_dir() {
    std::string d;
    if (FILE* f = popen("xdg-user-dir DESKTOP 2>/dev/null", "r")) {
        char b[4096];
        while (std::fgets(b, sizeof b, f)) d += b;
        pclose(f);
    }
    while (!d.empty() && (d.back() == '\n' || d.back() == '\r')) d.pop_back();
    return d.empty() ? home() / "Desktop" : p8(d);
}
#endif

}  // namespace

DesktopResult add_to_desktop() {
    std::error_code ec;
    const fs::path dir = p8(own_dir());
#if defined(_WIN32)
    wchar_t exe[MAX_PATH * 4];
    if (!GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)))) return {false, ""};
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const fs::path desktop = known_folder(FOLDERID_Desktop), programs = known_folder(FOLDERID_Programs);
    bool ok = !desktop.empty() && make_link(desktop / L"ACNHRecomp.lnk", exe);
    if (!programs.empty()) make_link(programs / L"ACNHRecomp.lnk", exe);
    if (SUCCEEDED(init)) CoUninitialize();
    return {ok, s8(desktop)};
#elif defined(__APPLE__)
    // dir is <bundle>/Contents/MacOS/: a release's ACNHRecomp.app.
    fs::path bundle = dir.parent_path().parent_path().parent_path();
    if (dir.parent_path().filename() != "MacOS" || bundle.extension() != ".app") return {false, ""};
    fs::path apps = "/Applications";
    if (access(apps.c_str(), W_OK) != 0) apps = fs::path(std::getenv("HOME") ? std::getenv("HOME") : "") / "Applications";
    fs::create_directories(apps, ec);
    const fs::path installed = apps / "ACNHRecomp.app";
    if (fs::weakly_canonical(installed, ec) != fs::weakly_canonical(bundle, ec)) {
        fs::remove_all(installed, ec);
        if (!run_ok({"/usr/bin/ditto", s8(bundle), s8(installed)})) return {false, ""};
    }
    run_ok({"/usr/bin/xattr", "-dr", "com.apple.quarantine", s8(installed)});
    if (const char* h = std::getenv("HOME")) {
        const fs::path alias = fs::path(h) / "Desktop" / "ACNHRecomp";
        fs::remove(alias, ec);
        fs::create_symlink(installed, alias, ec);
    }
    return {true, s8(installed)};
#else
    const fs::path launcher = dir / "ACNHRecomp Launcher", png = dir / "acnhrecomp.png";
    if (!fs::is_regular_file(launcher, ec)) return {false, ""};
    const fs::path share = home() / ".local" / "share";
    const fs::path icon_dir = share / "icons" / "hicolor" / "512x512" / "apps", apps = share / "applications";
    fs::create_directories(icon_dir, ec), fs::create_directories(apps, ec);
    if (fs::is_regular_file(png, ec)) fs::copy_file(png, icon_dir / "io.github.acnhrecomp.png", fs::copy_options::overwrite_existing, ec);
    std::string entry = "[Desktop Entry]\nType=Application\nName=ACNHRecomp\nComment=Animal Crossing: New Horizons, recompiled\n"
                        "Exec=\"" + s8(launcher) + "\"\nPath=" + s8(dir) + "\nIcon=io.github.acnhrecomp\nTerminal=false\n"
                        "Categories=Game;\nStartupWMClass=io.github.acnhrecomp\n";
    auto write = [&](const fs::path& p) {
        FILE* f = std::fopen(p.c_str(), "wb");
        if (!f) return false;
        bool ok = std::fwrite(entry.data(), 1, entry.size(), f) == entry.size();
        ok = std::fclose(f) == 0 && ok;
        chmod(p.c_str(), 0755);
        return ok;
    };
    bool ok = write(apps / "io.github.acnhrecomp.desktop");
    const fs::path desktop = desktop_dir();
    if (fs::is_directory(desktop, ec)) {
        const fs::path d = desktop / "io.github.acnhrecomp.desktop";
        if (write(d)) run_ok({"gio", "set", s8(d), "metadata::trusted", "true"});  // GNOME: launchable, not text
    }
    run_ok({"update-desktop-database", s8(apps)});
    return {ok, s8(apps)};
#endif
}

// ---- new releases ---------------------------------------------------------------------------------------------

namespace {

std::mutex g_update_lock;
UpdateInfo g_update;
std::atomic<bool> g_update_started{false};

// "v0.6.1", "0.6.1-3-gabc": the numbers; false if there are none.
bool version_numbers(const std::string& s, int out[3]) {
    size_t at = s.find_first_of("0123456789");
    if (at == std::string::npos) return false;
    return std::sscanf(s.c_str() + at, "%d.%d.%d", &out[0], &out[1], &out[2]) == 3;
}

// The value of "key": "..." in GitHub's JSON (flat strings only).
std::string json_string(const std::string& json, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t at = json.find(k);
    if (at == std::string::npos) return "";
    at = json.find('"', json.find(':', at + k.size()) + 1);
    size_t end = at == std::string::npos ? at : json.find('"', at + 1);
    return end == std::string::npos ? "" : json.substr(at + 1, end - at - 1);
}

// The value of "key": "..." after `from` (a release asset's fields follow its name).
std::string json_string_after(const std::string& json, size_t from, const char* key) {
    return from == std::string::npos ? "" : json_string(json.substr(from), key);
}

fs::path make_work_dir();  // below: a new folder of this user's for downloads

// This computer's package in a release (the names tools/package_release_*.sh give them).
std::string package_name(const std::string& tag) {
#if defined(_WIN32)
    return "ACNHRecomp-" + tag + "-windows-x64.zip";
#elif defined(__APPLE__)
    return "ACNHRecomp-" + tag + "-macOS-arm64.zip";
#else
    return "ACNHRecomp-" + tag + "-linux-x64.tar.gz";
#endif
}

// The launcher's version; NX_LAUNCHER_VERSION=<v> pretends another (tests of the notice).
std::string own_version() {
    const char* e = std::getenv("NX_LAUNCHER_VERSION");
    return e && *e ? e : NX_VERSION_STRING;
}

}  // namespace

std::string current_version() {
    std::string v = own_version();
    int n[3];
    if (!version_numbers(v, n)) return v;
    char b[48];
    std::snprintf(b, sizeof b, "v%d.%d.%d", n[0], n[1], n[2]);
    return b;
}

void update_check_start() {
    if (settings().get("launcher.update_check", "1") == "0" || g_update_started.exchange(true)) return;
    std::thread([] {
        std::error_code ec;
        const fs::path dir = make_work_dir();  // its own folder: the answer decides which file the update downloads
        const fs::path out = dir / "latest-release.json";
        std::string json;
        if (!dir.empty() && run_ok({"curl", "-sSfL", "--connect-timeout", "10", "--max-time", "20", "-H",
                    "Accept: application/vnd.github+json", "-A", "ACNHRecomp-launcher", "-o", s8(out),
                    "https://api.github.com/repos/Nycolazs/ACNHRecomp/releases/latest"}))
            json = read_all(out);
        if (!dir.empty()) fs::remove_all(dir, ec);
        std::string tag = json_string(json, "tag_name"), url = json_string(json, "html_url");
        int latest[3], mine[3];
        bool newer = version_numbers(tag, latest) && version_numbers(own_version(), mine) &&
                     (latest[0] != mine[0] ? latest[0] > mine[0]
                      : latest[1] != mine[1] ? latest[1] > mine[1] : latest[2] > mine[2]);
        std::lock_guard lk(g_update_lock);
        g_update.done = true;
        g_update.available = newer;
        g_update.version = tag;
        g_update.url = url.rfind("https://github.com/Nycolazs/ACNHRecomp/", 0) == 0
                           ? url : "https://github.com/Nycolazs/ACNHRecomp/releases/latest";
        // The package for this computer, with GitHub's digest of it ("sha256:<hex>"): without both, no update in place.
        const size_t at = newer ? json.find("\"" + package_name(tag) + "\"") : std::string::npos;
        const std::string asset = json_string_after(json, at, "browser_download_url");
        const std::string digest = json_string_after(json, at, "digest");
        if (asset.rfind("https://github.com/Nycolazs/ACNHRecomp/releases/download/", 0) == 0 &&
            asset.find("..") == std::string::npos && asset.find('%') == std::string::npos &&
            digest.rfind("sha256:", 0) == 0 && digest.size() == 7 + 64) {
            g_update.asset_url = asset;
            g_update.asset_sha256 = digest.substr(7);
        }
    }).detach();
}

namespace {

std::mutex g_install_lock;
UpdateProgress g_install;
std::atomic<bool> g_install_started{false};
fs::path g_relaunch;  // what to start after the update, found before any file was renamed

void install_stage(UpdateStage s, const std::string& error = "") {
    std::lock_guard lk(g_install_lock);
    g_install.stage = s, g_install.error = error;
}

// What an update replaces: the files of the launcher's folder (Linux, Windows), from the package's ACNHRecomp/. A Mac
// keeps the release page (update_can_install): a running bundle replaced in place, and macOS's App Management guard
// on /Applications, are not tried yet.
fs::path install_root() {
    fs::path d = p8(own_dir());
    return d.has_filename() ? d : d.parent_path();  // own_dir ends with a separator
}

// The update's journal: "new <file>" for each file put in place (relative to the root), then "done" once all are.
// update_cleanup reads it at the next start: it finishes an update stopped half-way and removes what it set aside.
fs::path journal_path() { return install_root() / ".update-journal.txt"; }

// A path from the package or the journal that stays under the root: relative, no drive, no "..".
bool inside(const fs::path& rel) {
    if (rel.empty() || rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) return false;
    for (const fs::path& part : rel)
        if (part == "..") return false;
    return true;
}

fs::path own_exe() {
#if defined(_WIN32)
    wchar_t b[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, b, DWORD(std::size(b)));
    return n ? fs::path(std::wstring(b, n)) : fs::path();
#elif defined(__APPLE__)
    return {};  // no in-place update on a Mac
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}

std::string file_sha256(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    nx::Sha256 h;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), std::streamsize(buf.size()));
        if (f.gcount() > 0) h.update(buf.data(), size_t(f.gcount()));
    }
    return h.hex();
}

// A new, empty folder of this user's for the download (mkdtemp: in a shared /tmp no one else can have made it).
fs::path make_work_dir() {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec) return {};
#if defined(_WIN32)  // %TEMP% is the user's own
    std::random_device rd;
    for (int i = 0; i < 8; i++) {
        char name[48];
        std::snprintf(name, sizeof name, "acnhrecomp-update-%08x", unsigned(rd()));
        if (fs::create_directory(base / name, ec) && !ec) return base / name;
    }
    return {};
#else
    std::string t = s8(base / "acnhrecomp-update-XXXXXX");
    return mkdtemp(t.data()) ? p8(t) : fs::path();
#endif
}

bool present(const fs::path& p) {  // exists, even as a broken symlink; a missing path is no error here
    std::error_code e;
    return fs::exists(fs::symlink_status(p, e));
}

// The renames of a journal's "new" files into place: the old file to .update-old (a running executable or a loaded
// library can be renamed, even on Windows, but not overwritten), the .update-new one in its place. `undo` (not when
// finishing at start) puts back what was renamed if one fails; a file is never left missing.
bool swap_in(const fs::path& root, const std::vector<std::string>& files, bool undo, std::string& error) {
    std::vector<std::string> done;
    for (const std::string& rel : files) {
        const fs::path dst = root / p8(rel), fresh = root / p8(rel + ".update-new"), old = root / p8(rel + ".update-old");
        if (!present(fresh)) continue;  // already in place (an update finished at start)
        std::error_code ec;
        if (present(dst)) {
            std::error_code e2;
            fs::remove_all(old, e2);
            fs::rename(dst, old, ec);
        }
        if (!ec) fs::rename(fresh, dst, ec);
        if (ec) {
            error = rel + ": " + ec.message();
            std::error_code e2;
            if (!present(dst) && present(old)) fs::rename(old, dst, e2);  // this one's old file back
            if (undo)
                for (auto it = done.rbegin(); it != done.rend(); ++it)
                    fs::rename(root / p8(*it + ".update-old"), root / p8(*it), e2);
            return false;
        }
        done.push_back(rel);
    }
    return true;
}

void install(UpdateInfo u) {
    std::error_code ec;
    const fs::path root = install_root(), work = make_work_dir();
    if (work.empty()) return install_stage(UpdateStage::Failed, "temporary folder");
    auto fail = [&](const std::string& why) {
        fs::remove_all(work, ec);
        install_stage(UpdateStage::Failed, why);
    };
    const fs::path pkg = work / package_name(u.version), unpacked = work / "x";
    fs::create_directory(unpacked, ec);
    g_relaunch = own_exe();
    install_stage(UpdateStage::Downloading);
    if (!run_ok({"curl", "-sSfL", "--connect-timeout", "15", "--speed-limit", "1024", "--speed-time", "60", "-A",
                 "ACNHRecomp-launcher", "-o", s8(pkg), u.asset_url}))
        return fail("download");
    install_stage(UpdateStage::Checking);
    if (file_sha256(pkg) != u.asset_sha256) return fail("checksum");
    install_stage(UpdateStage::Unpacking);
    const bool unpacked_ok = run_ok({"tar", "-xf", s8(pkg), "-C", s8(unpacked)});  // Windows 10+: bsdtar reads .zip
    const fs::path top = unpacked / "ACNHRecomp";
    if (!unpacked_ok || !fs::is_directory(top, ec)) return fail("unpack");
    install_stage(UpdateStage::Installing);
    // 1. Every new file copied beside its place as .update-new: stopping here harms nothing.
    std::vector<std::string> files;
    for (auto it = fs::recursive_directory_iterator(top, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const fs::path rel = it->path().lexically_relative(top);  // not fs::relative: that one follows symlinks
        if (!inside(rel)) {
            ec = std::make_error_code(std::errc::invalid_argument);
            break;
        }
        const fs::path dst = root / rel;
        const bool link = it->is_symlink(ec);
        if (!link && it->is_directory(ec)) {
            fs::create_directories(dst, ec);
            if (ec) break;
            continue;
        }
        const fs::path fresh = p8(s8(dst) + ".update-new");
        fs::remove(fresh, ec);
        if (link) fs::copy_symlink(it->path(), fresh, ec);
        else fs::copy_file(it->path(), fresh, fs::copy_options::overwrite_existing, ec);
        if (ec) break;
        if (!link) {
            const fs::file_status st = fs::status(it->path(), ec);
            if (!ec) fs::permissions(fresh, st.permissions(), ec);
            ec.clear();
        }
        files.push_back(s8(rel));
    }
    auto drop_new = [&] {
        std::error_code e2;
        for (const std::string& rel : files) fs::remove_all(root / p8(rel + ".update-new"), e2);
    };
    if (ec) {
        drop_new();
        return fail(ec.message());
    }
    // 2. The journal, then the renames (a moment): an update stopped there is finished at the next start.
    {
        const fs::path tmp = p8(s8(journal_path()) + ".tmp");
        std::ofstream j(tmp, std::ios::trunc);
        for (const std::string& rel : files) j << "new " << rel << "\n";
        j.close();
        if (!j || (fs::rename(tmp, journal_path(), ec), ec)) {  // whole, or not at all
            fs::remove(tmp, ec);
            drop_new();
            return fail("journal");
        }
    }
    std::string why;
    if (!swap_in(root, files, true, why)) {
        drop_new();
        fs::remove(journal_path(), ec);
        return fail(why);
    }
    std::ofstream(journal_path(), std::ios::app) << "done\n";
    fs::remove_all(work, ec);
    install_stage(UpdateStage::Done);
}

}  // namespace

bool update_can_install() {
    // Worked out once the check is done (each frame would write a probe file: antivirus scans, a Mac's /Applications)
    static int answer = -1;
    if (answer >= 0) return answer == 1;
    const UpdateInfo u = update_info();
    if (!u.done) return false;
    answer = 0;
    if (!u.available || u.asset_url.empty() || player_beside().empty()) return false;
#if defined(__APPLE__)
    return false;  // the release page (install_root)
#endif
    std::error_code ec;
    const fs::path probe = install_root() / ".update-probe";
    const bool writable = static_cast<bool>(std::ofstream(probe));  // this user may write it (not Program Files)
    fs::remove(probe, ec);
    answer = writable ? 1 : 0;
    return writable;
}

void update_install_start() {
    if (!update_can_install() || g_install_started.exchange(true)) return;
    std::thread(install, update_info()).detach();
}

UpdateProgress update_progress() {
    std::lock_guard lk(g_install_lock);
    return g_install;
}

void update_restart() {
    if (update_progress().stage != UpdateStage::Done || g_relaunch.empty()) return;
#if defined(__APPLE__)
    return;
#elif defined(_WIN32)
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + g_relaunch.wstring() + L"\"";
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return;
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
#else
    const std::string exe = s8(g_relaunch);
    char* args[] = {const_cast<char*>(exe.c_str()), nullptr};
    pid_t child = 0;
    if (posix_spawn(&child, exe.c_str(), nullptr, nullptr, args, environ) != 0) return;
#endif
    std::exit(0);
}

void update_cleanup() {
    std::error_code ec;
    const fs::path root = install_root(), jp = journal_path();
    std::ifstream in(jp);
    if (!in) return;
    std::vector<std::string> files;
    bool done = false;
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "done") done = true;
        else if (line.rfind("new ", 0) == 0 && inside(p8(line.substr(4)))) files.push_back(line.substr(4));
    }
    in.close();
    std::string why;
    // Stopped between the renames: the rest go in now; if one can't (a file held), all stays for the next start.
    if (!done && !swap_in(root, files, false, why)) return;
    // What was set aside: removed, and the names that are still in use (Windows: an old launcher still closing, the
    // game running) kept in the journal for the next start.
    std::vector<std::string> left;
    for (const std::string& rel : files) {
        const fs::path dst = root / p8(rel), old = root / p8(rel + ".update-old"), fresh = root / p8(rel + ".update-new");
        std::error_code e;
        if (!present(dst)) {  // never removed while its place is empty: the old file back, else the new one
            if (present(old)) fs::rename(old, dst, e);
            else if (present(fresh)) fs::rename(fresh, dst, e);
            if (e) left.push_back(rel);
            continue;
        }
        if (present(old)) {  // a file, or on macOS the old bundle
            fs::remove_all(old, e);
            if (e || present(old)) left.push_back(rel);
        }
        if (present(fresh)) fs::remove_all(fresh, e);  // a copy an interrupted update left
    }
    if (left.empty()) {
        fs::remove(jp, ec);
        return;
    }
    std::ofstream j(jp, std::ios::trunc);
    for (const std::string& rel : left) j << "new " << rel << "\n";
    j << "done\n";
}

UpdateInfo update_info() {
    std::lock_guard lk(g_update_lock);
    return g_update;
}

}  // namespace launcher
