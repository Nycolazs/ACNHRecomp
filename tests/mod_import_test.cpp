// The launcher's mod import (runtime/src/mod_manager.cpp) on the layouts mods come in: a plain romfs/, Atmosphere's
// atmosphere/contents/<title id>/romfs (three folders deep) with NookKit's runtime beside it, a bare NookKit/ folder,
// loose NookKit .json patches (a folder, one file, a zip), each as a folder and as a zip.
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mod_manager.h"

namespace fs = std::filesystem;

namespace nx {
void log(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vprintf(fmt, a);
    va_end(a);
    std::printf("\n");
}
}  // namespace nx

static int failures = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), failures++; \
    } while (0)

static const char* kMessage = R"({"file":"Version","archive":"LayoutMsg","messages":[]})";
static const char* kBcsv = R"({"file":"ItemParam","rows":[]})";
static const char* kPack = R"({"file":"Param/a.byml","archive":"StaticParam","data":{}})";

static void put(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}

// A stored (uncompressed) zip of (name, data).
static void zip(const fs::path& p, const std::vector<std::pair<std::string, std::string>>& files) {
    auto crc = [](const std::string& d) {
        uint32_t c = 0xFFFFFFFF;
        for (unsigned char b : d) {
            c ^= b;
            for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320 & (0 - (c & 1)));
        }
        return ~c;
    };
    std::string out, cd;
    auto le = [](std::string& s, uint32_t v, int n) {
        for (int k = 0; k < n; k++) s += char(v >> (8 * k));
    };
    for (auto& [name, data] : files) {
        uint32_t at = uint32_t(out.size()), c = crc(data);
        le(out, 0x04034b50, 4), le(out, 20, 2), le(out, 0, 2), le(out, 0, 2), le(out, 0, 4), le(out, c, 4);
        le(out, uint32_t(data.size()), 4), le(out, uint32_t(data.size()), 4), le(out, uint32_t(name.size()), 2), le(out, 0, 2);
        out += name + data;
        le(cd, 0x02014b50, 4), le(cd, 20, 2), le(cd, 20, 2), le(cd, 0, 2), le(cd, 0, 2), le(cd, 0, 4), le(cd, c, 4);
        le(cd, uint32_t(data.size()), 4), le(cd, uint32_t(data.size()), 4), le(cd, uint32_t(name.size()), 2);
        le(cd, 0, 2), le(cd, 0, 2), le(cd, 0, 2), le(cd, 0, 2), le(cd, 0, 4), le(cd, at, 4);
        cd += name;
    }
    uint32_t cd_at = uint32_t(out.size());
    out += cd;
    le(out, 0x06054b50, 4), le(out, 0, 2), le(out, 0, 2), le(out, uint32_t(files.size()), 2);
    le(out, uint32_t(files.size()), 2), le(out, uint32_t(cd.size()), 4), le(out, cd_at, 4), le(out, 0, 2);
    put(p, out);
}

struct Result {
    bool ok;
    std::string id, error;
    bool exefs;
};

static Result import(const fs::path& data, const fs::path& src) {
    Result r{};
    r.ok = nx::mod_manager::import_mod(data.string(), src.string(), &r.id, &r.exefs, &r.error);
    if (!r.ok) std::printf("  import %s: %s\n", src.filename().string().c_str(), r.error.c_str());
    return r;
}

static bool has(const fs::path& data, const Result& r, const std::string& rel) {
    return fs::is_regular_file(data / "mods" / r.id / "romfs" / rel);
}

int main() {
    fs::path root = fs::temp_directory_path() / "nx_mod_import_test";
    fs::remove_all(root);
    fs::path in = root / "in", data = root / "data";
    fs::create_directories(data);

    // A plain romfs/ mod, as before.
    put(in / "plain" / "romfs" / "Bcsv" / "ItemParam.bcsv", "x");
    Result r = import(data, in / "plain");
    CHECK(r.ok && r.id == "plain" && has(data, r, "Bcsv/ItemParam.bcsv") && !r.exefs);

    // Atmosphere layout, three folders deep, with NookKit's runtime: no ExeFS warning, named after the folder picked.
    fs::path atmo = in / "nkmod" / "atmosphere" / "contents" / "01006F8002326000";
    put(atmo / "romfs" / "NookKit" / "Message" / "version.json", kMessage);
    put(atmo / "romfs" / "skyline" / "plugins" / "libnookkit_core.nro", "nro");
    put(atmo / "exefs" / "main.npdm", "npdm");
    put(atmo / "exefs" / "subsdk9", "skyline");
    r = import(data, in / "nkmod");
    CHECK(r.ok && r.id == "nkmod" && has(data, r, "NookKit/Message/version.json") && !r.exefs);

    // The same with another plugin: a code mod, warned about.
    put(in / "codemod" / "atmosphere" / "contents" / "01006F8002326000" / "romfs" / "NookKit" / "Bcsv" / "a.json", kBcsv);
    put(in / "codemod" / "atmosphere" / "contents" / "01006F8002326000" / "romfs" / "skyline" / "plugins" / "libother.nro", "nro");
    put(in / "codemod" / "atmosphere" / "contents" / "01006F8002326000" / "exefs" / "subsdk9", "skyline");
    r = import(data, in / "codemod");
    CHECK(r.ok && r.exefs);

    // A bare NookKit/ folder.
    put(in / "bare" / "NookKit" / "Bcsv" / "rows.json", kBcsv);
    r = import(data, in / "bare");
    CHECK(r.ok && r.id == "bare" && has(data, r, "NookKit/Bcsv/rows.json"));

    // Loose patches in a folder: sorted into their kinds, a JSON that is none of them left out.
    put(in / "loose" / "names.json", kMessage);
    put(in / "loose" / "items.json", kBcsv);
    put(in / "loose" / "house.json", kPack);
    put(in / "loose" / "manifest.json", R"({"name":"x"})");
    r = import(data, in / "loose");
    CHECK(r.ok && has(data, r, "NookKit/Message/names.json") && has(data, r, "NookKit/Bcsv/items.json") &&
          has(data, r, "NookKit/Pack/house.json") && !fs::exists(data / "mods" / r.id / "romfs" / "NookKit" / "manifest.json"));

    // One .json file.
    put(in / "version.json", kMessage);
    r = import(data, in / "version.json");
    CHECK(r.ok && r.id == "version" && has(data, r, "NookKit/Message/version.json"));
    put(in / "junk.json", "{}");
    CHECK(!import(data, in / "junk.json").ok);

    // Zips: Atmosphere layout with NookKit's runtime, a bare NookKit/, loose patches.
    zip(in / "zatmo.zip", {{"atmosphere/contents/01006F8002326000/romfs/NookKit/Pack/p.json", kPack},
                           {"atmosphere/contents/01006F8002326000/romfs/skyline/plugins/libnookkit_core.nro", "nro"},
                           {"atmosphere/contents/01006F8002326000/exefs/subsdk9", "skyline"}});
    r = import(data, in / "zatmo.zip");
    CHECK(r.ok && r.id == "zatmo" && has(data, r, "NookKit/Pack/p.json") && !r.exefs);
    zip(in / "zbare.zip", {{"wrap/NookKit/Message/m.json", kMessage}});
    r = import(data, in / "zbare.zip");
    CHECK(r.ok && has(data, r, "NookKit/Message/m.json"));
    zip(in / "zloose.zip", {{"patches/a.json", kBcsv}, {"patches/b.json", kMessage}, {"readme.txt", "hi"}});
    r = import(data, in / "zloose.zip");
    CHECK(r.ok && has(data, r, "NookKit/Bcsv/a.json") && has(data, r, "NookKit/Message/b.json"));
    zip(in / "zempty.zip", {{"readme.txt", "hi"}});
    CHECK(!import(data, in / "zempty.zip").ok);

    // The kinds the launcher shows as chips: RomFS files, NookKit patches, ExeFS the pack had (kept after import).
    auto info = [&](const std::string& id) {
        for (auto& m : nx::mod_manager::list_mods(data.string()))
            if (m.id == id) return m;
        return nx::mod_manager::ModInfo{};
    };
    auto plain = info("plain"), nk = info("nkmod"), code = info("codemod"), loose = info("loose");
    CHECK(plain.has_romfs && !plain.has_nookkit && !plain.has_exefs);
    CHECK(!nk.has_romfs && nk.has_nookkit && !nk.has_exefs);  // NookKit's own plugin is neither
    CHECK(code.has_exefs && code.has_nookkit);
    CHECK(!loose.has_romfs && loose.has_nookkit);

    // Rename: the folder moves, the mod keeps its place in game.mods; taken names and bundled mods refused.
    auto order = [&] {
        std::vector<std::string> ids;
        for (auto& m : nx::mod_manager::list_mods(data.string()))
            if (m.enabled) ids.push_back(m.id);
        return ids;
    };
    std::vector<std::string> before = order();
    std::string to, err;
    CHECK(nx::mod_manager::rename_mod(data.string(), "plain", "  Plain Mod ", &to, &err) && to == "Plain Mod");
    CHECK(fs::is_directory(data / "mods" / "Plain Mod" / "romfs") && !fs::exists(data / "mods" / "plain"));
    std::vector<std::string> after = order();
    CHECK(after.size() == before.size() && after[0] == "Plain Mod");  // "plain" was imported first
    CHECK(!nx::mod_manager::rename_mod(data.string(), "bare", "loose", &to, &err));
    CHECK(nx::mod_manager::rename_mod(data.string(), "nkmod", "NKMod", &to, &err) && to == "NKMod" &&
          info("NKMod").has_nookkit);  // only the case changes
    CHECK(nx::mod_manager::rename_mod(data.string(), "bare", "a/b:c", &to, &err) && to == "a-b-c");
    CHECK(!nx::mod_manager::rename_mod(data.string(), "loose", "   ", &to, &err));
    CHECK(!nx::mod_manager::rename_mod(data.string(), "loose", "ptbr", &to, &err));

    fs::remove_all(root);
    std::printf(failures ? "%d failures\n" : "ok\n", failures);
    return failures != 0;
}
