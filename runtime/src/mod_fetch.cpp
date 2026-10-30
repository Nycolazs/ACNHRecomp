// The two mods every player gets without doing anything (config.h mods_fetch): the pt-BR fan translation (mods/ptbr)
// and the parameter files of tmouh's 60 FPS SpeedFix (mods/speedfix60, with the code the importer compiled into the
// game). They come with the port: tools/bundle_mods.sh puts them in every package (beside the player, in
// ACNHRecomp.app's Resources, in the APK's assets) with mods/bundle.txt, and the start copies them into the game
// folder's mods/, where the runtime reads them.
//
// bundle.txt: its first line is the bundle's version, the other lines the files (paths under mods/). The game folder's
// mods/.bundle holds the version last copied there: a package with another version copies its files again (a fixed
// translation reaches folders that have the old one). Downloads were tried first (0.6.1): MediaFire's page changed,
// and Linux without unzip or a blocked curl left many players without either mod. NX_MOD_FETCH=0 or
// game.mod_fetch = off: nothing is copied.
#include "platform.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif !defined(__ANDROID__)
#include <unistd.h>
#endif

#include <nx/runtime.h>

#include "config.h"

namespace nx {

namespace {

namespace fs = std::filesystem;

std::string u8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

#if !defined(__ANDROID__)
// Where the package keeps its mods/: beside the player, or Contents/Resources when it runs from ACNHRecomp.app.
fs::path bundle_dir() {
    fs::path exe;
#if defined(_WIN32)
    wchar_t w[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, w, DWORD(std::size(w)));
    if (n && n < std::size(w)) exe = fs::path(std::wstring(w, n));
#elif defined(__APPLE__)
    char b[4096];
    uint32_t size = sizeof b;
    if (_NSGetExecutablePath(b, &size) == 0) exe = b;
#else
    char b[4096];
    ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
    if (n > 0) exe = std::string(b, size_t(n));
#endif
    std::error_code ec;
    fs::path dir = fs::weakly_canonical(exe, ec).parent_path();
    if (dir.filename() == "MacOS" && dir.parent_path().filename() == "Contents")
        return dir.parent_path() / "Resources";
    return dir;
}
#endif

// A file of the package's mods/ (rel under it); false if the package has none.
bool bundled(const std::string& rel, std::string& out) {
#if defined(__ANDROID__)
    return plat::read_asset("mods/" + rel, out);
#else
    out.clear();
    FILE* f = plat::fopen_utf8(u8(bundle_dir() / "mods" / fs::u8path(rel)), "rb");
    if (!f) return false;
    char b[65536];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
    std::fclose(f);
    return true;
#endif
}

std::string first_line(const std::string& s) {
    std::string l = s.substr(0, s.find('\n'));
    while (!l.empty() && l.back() == '\r') l.pop_back();
    return l;
}

std::string read_small(const fs::path& p) {
    std::string s;
    if (FILE* f = plat::fopen_utf8(u8(p), "rb")) {
        char b[256];
        s.assign(b, std::fread(b, 1, sizeof b, f));
        std::fclose(f);
    }
    return first_line(s);
}

bool write_file(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    FILE* f = plat::fopen_utf8(u8(p), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace

void mods_fetch() {
    if (const char* e = std::getenv("NX_MOD_FETCH"); e && e[0] == '0') return;
    if (config_get("game.mod_fetch", "on") == "off") return;
    std::string list;
    if (!bundled("bundle.txt", list)) return;  // a build without them (development)
    const std::string version = first_line(list);
    const fs::path mods = fs::u8path(plat::data_root()) / "mods";
    if (version.empty() || read_small(mods / ".bundle") == version) return;
    std::vector<std::string> files;
    std::istringstream lines(list.substr(std::min(list.size(), list.find('\n') + 1)));
    for (std::string rel; std::getline(lines, rel);) {
        while (!rel.empty() && rel.back() == '\r') rel.pop_back();
        if (!rel.empty() && rel.find("..") == std::string::npos && rel.find('/') != std::string::npos)
            files.push_back(rel);
    }
    // The folders these files go to are replaced whole: a file an older copy had and this one dropped goes too.
    std::error_code ec;
    for (const std::string& rel : files) fs::remove_all(mods / fs::u8path(rel.substr(0, rel.find('/'))), ec);
    size_t copied = 0, bytes = 0;
    std::string data;
    for (const std::string& rel : files) {
        if (!bundled(rel, data) || !write_file(mods / fs::u8path(rel), data)) {
            log("mods: %s could not be copied into %s", rel.c_str(), u8(mods).c_str());
            continue;
        }
        copied++, bytes += data.size();
    }
    const bool ok = copied == files.size();
    if (ok) write_file(mods / ".bundle", version + "\n");
    log("mods: the package's mods (%s) copied into %s: %zu of %zu files, %zu KB%s", version.c_str(), u8(mods).c_str(),
        copied, files.size(), bytes >> 10, ok ? "" : " (the rest: at the next start)");
}

}  // namespace nx
