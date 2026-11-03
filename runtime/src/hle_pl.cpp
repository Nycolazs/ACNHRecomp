// nn::pl (shared system fonts) at the API level.
//
// On the console the pl sysmodule loads the system fonts (system archives 0100000000000810-814) into shared memory
// in BFTTF form: an 8-byte header {magic, size} and the TrueType data, both XORed with a key. The game hands that
// to nn::fontll::ScalableFontEngineHelper::Decode (sdk+3093c0 in SDK 16), which accepts the data already decoded
// when the first word is the plain magic 0x18029A7F, and then only skips the header.
//
// So any TrueType font works: <data root>/fonts/shared_<type>.ttf (0 standard, 1 simplified Chinese, 2 its
// extension, 3 traditional Chinese, 4 Korean, 5 extension), else shared.ttf, else the host's own
// (NX_SHARED_FONT, or a system font on macOS, Android, Windows and Linux). The fonts on our own firmware dump can go there once decoded.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

namespace {

struct Font {
    uint64_t addr = 0, size = 0;  // header + data, in guest memory
};

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> v;
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return v;
    plat::fseek64(f, 0, SEEK_END);
    int64_t n = plat::ftell64(f);
    plat::fseek64(f, 0, SEEK_SET);
    if (n > 0) {
        v.resize(size_t(n));
        v.resize(std::fread(v.data(), 1, v.size(), f));
    }
    std::fclose(f);
    return v;
}

#if defined(__linux__) && !defined(__ANDROID__)
bool is_ttf(const std::string& p) {
    return p.size() > 4 && (p.compare(p.size() - 4, 4, ".ttf") == 0 || p.compare(p.size() - 4, 4, ".TTF") == 0);
}

// fontconfig's sans-serif (a .ttf only: the game's font engine reads one font, not a .ttc/.otf collection), then a
// scan of the font folders (Regular/Sans names first).
std::vector<std::string> linux_fonts() {
    std::vector<std::string> out;
    if (FILE* p = popen("fc-match -s -f '%{file}\\n' 'sans-serif:fontformat=TrueType' 2>/dev/null", "r")) {
        char line[1024];
        while (std::fgets(line, sizeof line, p) && out.size() < 8) {
            std::string f(line);
            while (!f.empty() && (f.back() == '\n' || f.back() == '\r')) f.pop_back();
            if (is_ttf(f)) out.push_back(f);
        }
        pclose(p);
    }
    std::vector<std::string> dirs = {"/usr/share/fonts", "/usr/local/share/fonts"};
    if (const char* home = std::getenv("HOME")) {
        dirs.push_back(std::string(home) + "/.local/share/fonts");
        dirs.push_back(std::string(home) + "/.fonts");
        dirs.push_back(std::string(home) + "/.local/share/Steam/clientui/fonts");
        dirs.push_back(std::string(home) + "/.steam/steam/clientui/fonts");
    }
    std::vector<std::string> good, other;
    std::error_code ec;
    for (const std::string& d : dirs) {
        std::filesystem::recursive_directory_iterator it(d, std::filesystem::directory_options::skip_permission_denied, ec), end;
        for (int n = 0; !ec && it != end && n < 20000; it.increment(ec), n++) {
            const std::string f = it->path().string();
            if (!is_ttf(f)) continue;
            const std::string name = it->path().filename().string();
            const bool regular = name.find("Regular") != std::string::npos || name.find("Sans.ttf") != std::string::npos;
            const bool styled = name.find("Bold") != std::string::npos || name.find("Italic") != std::string::npos ||
                                name.find("Mono") != std::string::npos || name.find("Emoji") != std::string::npos;
            (regular && !styled ? good : other).push_back(f);
        }
        ec.clear();
    }
    std::sort(good.begin(), good.end());
    std::sort(other.begin(), other.end());
    out.insert(out.end(), good.begin(), good.end());
    out.insert(out.end(), other.begin(), other.end());
    return out;
}
#endif

const Font& font(uint32_t type) {
    static std::mutex lock;
    static Font fonts[6];
    std::lock_guard lk(lock);
    Font& f = fonts[type < 6 ? type : 0];
    if (f.addr) return f;
    std::string root = plat::data_root() + "/fonts/";
    std::vector<std::string> candidates = {root + "shared_" + std::to_string(type) + ".ttf", root + "shared.ttf"};
    if (const char* e = std::getenv("NX_SHARED_FONT")) candidates.push_back(e);
#if defined(__APPLE__)
    candidates.push_back("/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
    candidates.push_back("/System/Library/Fonts/Supplemental/Arial.ttf");
#elif defined(__ANDROID__)
    candidates.push_back("/system/fonts/Roboto-Regular.ttf");
#elif defined(_WIN32)
    // Arial Unicode (Office installs it) covers the most; else Arial, as on the Mac.
    candidates.push_back("C:/Windows/Fonts/arialuni.ttf");
    candidates.push_back("C:/Windows/Fonts/arial.ttf");
#elif defined(__linux__)
    // Liberation Sans has Arial's metrics; else the common distribution fonts.
    candidates.push_back("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    // Arch / SteamOS and Fedora put them elsewhere.
    candidates.push_back("/usr/share/fonts/liberation/LiberationSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/noto/NotoSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/TTF/LiberationSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/TTF/DejaVuSans.ttf");
    candidates.push_back("/usr/share/fonts/google-noto/NotoSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf");
    // Then whatever fontconfig calls sans-serif, and at last any TrueType file in the usual folders (a Steam Deck
    // in game mode has few fonts): the game crashes on its first dialog without one (#40).
    static const std::vector<std::string> found = linux_fonts();
    for (const std::string& path : found) candidates.push_back(path);
#endif
    std::vector<uint8_t> ttf;
    for (const auto& path : candidates) {
        ttf = read_file(path);
        if (!ttf.empty()) {
            log("pl: shared font %u <- %s (%zu bytes)", type, path.c_str(), ttf.size());
            break;
        }
    }
    if (ttf.empty()) log("pl: no font for shared font %u (put one in %s)", type, root.c_str());
    f.size = ttf.size() + 8;
    f.addr = kernel_alloc(f.size);
    st<uint32_t>(f.addr, 0x18029A7F);  // plain magic: "already decoded"
    st<uint32_t>(f.addr + 4, uint32_t(ttf.size()));
    if (!ttf.empty()) std::memcpy(reinterpret_cast<void*>(f.addr + 8), ttf.data(), ttf.size());
    return f;
}

void GetSharedFontLoadState(CPU& c) { c.x[0] = 1; }  // SharedFontLoadState_Loaded
void GetSharedFontAddress(CPU& c) { c.x[0] = font(uint32_t(c.x[0])).addr; }
void GetSharedFontSize(CPU& c) { c.x[0] = font(uint32_t(c.x[0])).size; }

// Result GetSharedFont(int* count, SharedFontData* out, int max, settings::LanguageCode) (SDK 20, ACNH): every
// shared font in the order of priority for the language, as SharedFontData {SharedFontType type; const void*
// address; size_t size} (0x18 bytes). Western order: standard, the Chinese and Korean sets, then the
// extension (button icons).
void GetSharedFont(CPU& c) {
    static const uint32_t kOrder[] = {0, 1, 2, 3, 4, 5};
    uint64_t count = c.x[0], out = c.x[1];
    int max = int(c.x[2]), n = 0;
    for (uint32_t type : kOrder) {
        if (n >= max) break;
        const Font& f = font(type);
        st<uint32_t>(out + 0x18ull * n, type);
        st<uint32_t>(out + 0x18ull * n + 4, 0);
        st<uint64_t>(out + 0x18ull * n + 8, f.addr);
        st<uint64_t>(out + 0x18ull * n + 0x10, f.size);
        n++;
    }
    if (count) st<int32_t>(count, n);
    c.x[0] = 0;
}

void RequestSharedFontLoad(CPU& c) { c.x[0] = 0; }  // already loaded

}  // namespace

NX_HLE("_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE", GetSharedFont);
NX_HLE("_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE", RequestSharedFontLoad);
NX_HLE("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", GetSharedFontLoadState);
NX_HLE("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", GetSharedFontAddress);
NX_HLE("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", GetSharedFontSize);

}  // namespace nx
