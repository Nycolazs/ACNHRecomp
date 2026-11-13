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
        const fs::path out = fs::temp_directory_path(ec) / "acnhrecomp-latest-release.json";
        std::string json;
        if (run_ok({"curl", "-sSfL", "--connect-timeout", "10", "--max-time", "20", "-H",
                    "Accept: application/vnd.github+json", "-A", "ACNHRecomp-launcher", "-o", s8(out),
                    "https://api.github.com/repos/Nycolazs/ACNHRecomp/releases/latest"}))
            json = read_all(out);
        fs::remove(out, ec);
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
fs::path g_relaunch;  // the launcher to start after the update (its path from before any file was renamed)

void install_stage(UpdateStage s, const std::string& error = "") {
    std::lock_guard lk(g_install_lock);
    g_install.stage = s, g_install.error = error;
}

// The folder the package's ACNHRecomp/ goes over: the launcher's own, or on macOS the one holding ACNHRecomp.app.
fs::path install_root() {
    fs::path d = p8(own_dir());
#if defined(__APPLE__)
    d = d.parent_path().parent_path().parent_path();  // Contents/MacOS -> Contents -> ACNHRecomp.app -> its folder
#endif
    return d;
}

fs::path own_exe() {
#if defined(_WIN32)
    wchar_t b[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, b, DWORD(std::size(b)));
    return n ? fs::path(std::wstring(b, n)) : fs::path();
#elif defined(__APPLE__)
    return install_root() / "ACNHRecomp.app";  // started again through `open`
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}

std::string file_sha256(const fs::path& p) {
    std::FILE* f = nullptr;
#if defined(_WIN32)
    f = _wfopen(p.c_str(), L"rb");
#else
    f = std::fopen(p.c_str(), "rb");
#endif
    if (!f) return "";
    nx::Sha256 h;
    std::vector<char> buf(1 << 20);
    for (size_t n; (n = std::fread(buf.data(), 1, buf.size(), f)) > 0;) h.update(buf.data(), n);
    std::fclose(f);
    return h.hex();
}

// Every file of `from` put at the same place under `to`: copied beside as .update-new, the old one renamed to
// .update-old (a running executable or a loaded library can be renamed, even on Windows, but not overwritten), the new
// one renamed into place. On a failure everything renamed so far is put back. The .update-old names are listed in
// .update-old.txt, for update_cleanup at the next start.
bool put_in_place(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    std::vector<std::pair<fs::path, fs::path>> moved;  // {old aside, its place}
    std::vector<fs::path> added;
    auto undo = [&] {
        std::error_code e2;
        for (const fs::path& p : added) fs::remove(p, e2);
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) fs::rename(it->first, it->second, e2);
    };
    for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const fs::path src = it->path(), rel = fs::relative(src, from, ec), dst = to / rel;
        if (ec) break;
        if (it->is_directory(ec) && !it->is_symlink(ec)) {
            fs::create_directories(dst, ec);
            if (ec) break;
            continue;
        }
        const fs::path fresh = dst.string() + ".update-new", old = dst.string() + ".update-old";
        fs::remove(fresh, ec);
        if (it->is_symlink(ec)) fs::copy_symlink(src, fresh, ec);
        else fs::copy_file(src, fresh, fs::copy_options::overwrite_existing, ec);
        if (ec) break;
        if (!it->is_symlink()) fs::permissions(fresh, fs::status(src).permissions(), ec), ec.clear();
        if (fs::exists(fs::symlink_status(dst, ec))) {
            fs::remove(old, ec);
            fs::rename(dst, old, ec);
            if (ec) {
                fs::remove(fresh);
                break;
            }
            moved.push_back({old, dst});
        } else {
            added.push_back(dst);
        }
        fs::rename(fresh, dst, ec);
        if (ec) break;
    }
    if (ec) {
        error = ec.message();
        undo();
        return false;
    }
    std::ofstream list(to / ".update-old.txt", std::ios::app);
    for (const auto& [old, place] : moved) list << s8(fs::relative(old, to, ec)) << "\n";
    return true;
}

void install(UpdateInfo u) {
    std::error_code ec;
    const fs::path root = install_root(), work = fs::temp_directory_path(ec) / ("acnhrecomp-update-" + u.version);
    fs::remove_all(work, ec);
    fs::create_directories(work / "x", ec);
    const fs::path pkg = work / package_name(u.version);
    g_relaunch = own_exe();
    install_stage(UpdateStage::Downloading);
    if (!run_ok({"curl", "-sSfL", "--connect-timeout", "15", "-A", "ACNHRecomp-launcher", "-o", s8(pkg), u.asset_url}))
        return install_stage(UpdateStage::Failed, "download");
    install_stage(UpdateStage::Checking);
    if (file_sha256(pkg) != u.asset_sha256) return install_stage(UpdateStage::Failed, "checksum");
    install_stage(UpdateStage::Unpacking);
#if defined(__APPLE__)
    const bool unpacked = run_ok({"/usr/bin/ditto", "-x", "-k", s8(pkg), s8(work / "x")});
#else
    const bool unpacked = run_ok({"tar", "-xf", s8(pkg), "-C", s8(work / "x")});  // Windows 10+: bsdtar reads .zip
#endif
    const fs::path top = work / "x" / "ACNHRecomp";
    if (!unpacked || !fs::is_directory(top, ec)) return install_stage(UpdateStage::Failed, "unpack");
    install_stage(UpdateStage::Installing);
    std::string why;
    if (!put_in_place(top, root, why)) return install_stage(UpdateStage::Failed, why);
    fs::remove_all(work, ec);
    install_stage(UpdateStage::Done);
}

}  // namespace

bool update_can_install() {
    const UpdateInfo u = update_info();
    if (!u.available || u.asset_url.empty() || player_beside().empty()) return false;
    std::error_code ec;
    const fs::path probe = install_root() / ".update-probe";
    const bool writable = static_cast<bool>(std::ofstream(probe));  // this user may write the folder (not Program Files)
    fs::remove(probe, ec);
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
    if (!run_ok({"/usr/bin/open", "-n", s8(g_relaunch)})) return;
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
    const fs::path root = install_root(), list = root / ".update-old.txt";
    std::ifstream in(list);
    if (!in) return;
    for (std::string rel; std::getline(in, rel);) {
        while (!rel.empty() && rel.back() == '\r') rel.pop_back();
        // only names an update wrote: under the folder, ending in .update-old
        if (rel.empty() || rel.find("..") != std::string::npos || rel.size() < 11 ||
            rel.compare(rel.size() - 11, 11, ".update-old") != 0)
            continue;
        fs::remove(root / p8(rel), ec);
    }
    in.close();
    fs::remove(list, ec);
}

UpdateInfo update_info() {
    std::lock_guard lk(g_update_lock);
    return g_update;
}

}  // namespace launcher
