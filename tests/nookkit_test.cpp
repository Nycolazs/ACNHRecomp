// NookKit JSON patches (runtime/src/nookkit.cpp) on the game's own files:
//   nookkit_test <romfs> [<bcsv> <byml>]   (the RomFS with the update applied, e.g. <data root>/extract/romfs)
// Every msbt of the USen archives rebuilds to its own bytes; a message patch replaces a message and adds one; a bcsv
// patch replaces and adds rows; a pack patch replaces a BYML; bad patches fail. <bcsv> and <byml>, NookKit's own
// output for the same patches (its nookkit-formats crate), are compared byte for byte when given.
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>

#include "nookkit.h"

namespace nx {
void log(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vprintf(fmt, a);
    va_end(a);
    std::printf("\n");
}
// The glue's (install) dependencies, unused here.
std::string g_root;
std::string read_file(const std::string& path) {
    std::string s;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        std::fclose(f);
    }
    return s;
}
std::string read_game_file(const std::string& rel) { return read_file(g_root + "/" + rel); }
void add_virtual_file(const std::string&, std::string) {}
std::vector<std::string> romfs_mod_roots() { return {}; }
}  // namespace nx

using namespace nx;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), failures++; \
    } while (0)

// A mod converted to JSON (NookKit/Bcsv/*.json, NookKit/Message/*.json) against the binary mod it came from: every
// patched table must equal the mod's byte for byte, every patched message archive hold the mod's messages.
static int verify(const std::string& romfs, const std::string& nookkit, const std::string& mod) {
    g_root = romfs;
    namespace fs = std::filesystem;
    nookkit::BcsvPatches b;
    nookkit::Patches m;
    std::string err;
    for (auto& e : fs::directory_iterator(nookkit + "/Bcsv"))
        if (!b.add(e.path().filename().string(), read_file(e.path().string()), &err)) std::printf("%s\n", err.c_str()), failures++;
    if (fs::is_directory(nookkit + "/Message"))
        for (auto& e : fs::directory_iterator(nookkit + "/Message"))
            if (!m.add(e.path().filename().string(), read_file(e.path().string()), &err)) std::printf("%s\n", err.c_str()), failures++;
    for (auto& [file, src] : b.files) {
        int replaced = 0, added = 0;
        std::string out = b.apply(file, read_game_file("Bcsv/" + file + ".bcsv"), &replaced, &added);
        bool same = out == read_file(mod + "/Bcsv/" + file + ".bcsv");
        std::printf("Bcsv/%s.bcsv: %d replaced, %d added: %s\n", file.c_str(), replaced, added, same ? "same as the mod" : "DIFFERENT");
        failures += !same;
    }
    for (auto& [file, patch] : m.files) {
        std::map<std::string, const nookkit::FilePatch*> files{{file, &patch}};
        for (const char* locale : {"USen", "USes", "USfr", "EUen", "EUde", "EUes", "EUfr", "EUit", "EUnl", "EUru", "JPja", "KRko", "CNzh", "TWzh"}) {
            std::string path = "Message/" + patch.archive + "_" + locale + ".sarc.zs";
            int n = 0;
            size_t before = 0, after = 0;
            std::string out = nookkit::patch_archive(read_game_file(path), locale, files, &n, &before, &after);
            std::string want = read_file(mod + "/" + path);
            int bad = 0;
            for (auto& [label, msg] : patch.labels)
                bad += nookkit::message_text(out, file, label) != nookkit::message_text(want, file, label);
            if (bad) std::printf("%s %s: %d messages differ from the mod\n", path.c_str(), file.c_str(), bad);
            failures += bad;
        }
        std::printf("Message %s/%s: %zu messages checked in every locale\n", patch.archive.c_str(), file.c_str(), patch.labels.size());
    }
    std::printf(failures ? "%d failures\n" : "ok\n", failures);
    return failures != 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: nookkit_test <romfs> [<bcsv> <byml>]\n"
                    "       nookkit_test --verify <romfs> <NookKit folder> <binary mod romfs>\n");
        return 2;
    }
    if (std::string(argv[1]) == "--verify" && argc >= 5) return verify(argv[2], argv[3], argv[4]);
    g_root = argv[1];

    for (const char* a : {"Dialog", "LayoutMsg", "Mail", "String", "System", "TalkFtr", "TalkObj", "TalkSys", "Tutorials"}) {
        std::string zs = read_game_file(std::string("Message/") + a + "_USen.sarc.zs");
        int bad = nookkit::roundtrip_failures(zs);
        if (bad) std::printf("%s: %d msbt don't round-trip\n", a, bad);
        CHECK(bad == 0);
    }

    nookkit::Patches p;
    std::string err;
    CHECK(p.add("version.json", R"([{ "file": "Version", "archive": "LayoutMsg", "messages": [
        { "label": "0001", "text": { "default": "NookKit + Ver.", "EUru": "NookKit + Вер." } },
        { "label": "Brand_New", "text": { "default": "{{Color index=3}}Hi{{{{" }, "attributes": { "like": "0001" } } ] }])",
                &err));
    CHECK(err.empty());
    CHECK(!p.add("bad.json", R"({"file":"F","messages":[{"label":"L","text":{"USen":"{{Nope}}"}}]})", &err));
    CHECK(!p.add("bad.json", R"({"file":"F","extra":1,"messages":[]})", &err));
    CHECK(!p.add("bad.json", R"({"file":"F","messages":[{"label":"L","text":{},"attributes":{"hex":"zz"}}]})", &err));
    CHECK(p.count() == 2);

    std::map<std::string, const nookkit::FilePatch*> files{{"Version", &p.files["Version"]}};
    for (const char* locale : {"USen", "EUru"}) {
        std::string zs = read_game_file(std::string("Message/LayoutMsg_") + locale + ".sarc.zs");
        int n = 0;
        size_t before = 0, after = 0;
        std::string out = nookkit::patch_archive(zs, locale, files, &n, &before, &after);
        CHECK(!out.empty() && n == 2 && after >= before);
        std::u16string want = locale[0] == 'E' ? u"NookKit + Вер." : u"NookKit + Ver.";
        want += char16_t(0);
        CHECK(nookkit::message_text(out, "Version", "0001") == want);
        CHECK((nookkit::message_text(out, "Version", "Brand_New") == std::u16string{0x0E, 0, 3, 2, 3, u'H', u'i', u'{', u'{', 0}));
        CHECK(nookkit::roundtrip_failures(out) == 0);
    }

    // Bcsv: a row replaced by its UniqueID, one added and patched again; hashes, a negative number, a float.
    nookkit::BcsvPatches b;
    CHECK(b.add("bcsv.json", R"([{ "file": "NmlNpcParam", "rows": [
  { "UniqueID u16": 1, "Label string8": "zzz01", "0xA54F92FD": { "mmh3": "WS" } },
  { "UniqueID u16": 60000, "Label string8": "new01", "0xA54F92FD": { "crc32": "Normal" }, "0xE2EFE82D": -5 },
  { "UniqueID u16": 60000, "Label string8": "new02", "0x5EF86F1F": 1.5 }
] }])", &err));
    CHECK(!b.add("bad.json", R"({"file":"T","rows":[{"B":"q"}]})", &err));
    CHECK(!b.add("bad.json", R"({"file":"T","rows":[{"A":5000000000}]})", &err));
    int replaced = 0, added = 0;
    std::string table = b.apply("NmlNpcParam", read_game_file("Bcsv/NmlNpcParam.bcsv"), &replaced, &added);
    if (replaced != 2 || added != 1) std::printf("bcsv: %d replaced, %d added\n", replaced, added);
    CHECK(replaced == 2 && added == 1);
    // Pack: a whole BYML replaced, in the original's byte order and version.
    nookkit::PackPatches pk;
    CHECK(pk.add("pack.json", R"({ "file": "Param/NpcHouse/duk07.byml", "archive": "StaticParam",
  "data": { "b": [1, 2.5, "x", true, null, 5000000000, -3, { "z": "y", "a": [] }], "a": "hello", "c": 1e3, "s": "x" } })",
                 &err));
    CHECK(!pk.add("bad.json", R"({ "file": "a", "archive": "b", "data": {"k": 1, "k": 2} })", &err));
    int packed = 0;
    std::string pack = pk.apply("StaticParam", read_game_file("Pack/StaticParam.pack"), &packed);
    CHECK(packed == 1);
    std::string byml = nookkit::sarc_file(pack, "Param/NpcHouse/duk07.byml");
    CHECK(byml.compare(0, 4, std::string("YB\4\0", 4)) == 0);
    // NookKit's own output for the same patches (nookkit-formats), when given: byte for byte.
    if (argc >= 4) {
        CHECK(table == read_file(argv[2]));
        CHECK(byml == read_file(argv[3]));
    }

    std::printf(failures ? "%d failures\n" : "ok\n", failures);
    return failures != 0;
}
