// nx_prepare_io on synthetic inputs only (no game data): NSOs built here (LZ4 literal runs and a match, right and
// wrong SHA-256, the real 3.0.3 build id from games/acnh/versions.txt), fake main.npdm, RomFS trees, the layouts the
// emulators write; and the packs, compared byte for byte with tools/pack_romfs.py's and tools/pack_spirv.py's and read
// back as runtime/src/hle_fs.cpp and nvn_shaders.cpp read them. Exit 0 when all pass.
// usage: prepare_io_test <source dir> [python]
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "inputs.h"
#include "nso.h"
#include "romfs_pak.h"
#include "sha256.h"
#include "spirv_pak.h"

using namespace nx::prepare;
namespace fs = std::filesystem;

static int g_failed = 0, g_checks = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        g_checks++;                                                  \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            g_failed++;                                              \
        }                                                            \
    } while (0)

using Bytes = std::vector<uint8_t>;

static fs::path g_tmp;
static std::string g_src, g_python;

#ifdef _WIN32
static const char* kNull = "NUL";
#else
static const char* kNull = "/dev/null";
#endif

// A command line run by the shell; cmd.exe wants the whole of a line that starts with a quote quoted once more.
static int sh(std::string cmd) {
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    return std::system(cmd.c_str());
}

static std::string s(const fs::path& p) { return p.string(); }

static void write_file(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
static void write_text(const fs::path& p, const std::string& t) { write_file(p, Bytes(t.begin(), t.end())); }
static Bytes read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static Bytes pattern(size_t n, uint32_t seed) {
    Bytes b(n);
    for (size_t i = 0; i < n; i++) seed = seed * 1103515245u + 12345u, b[i] = uint8_t(seed >> 16);
    return b;
}
static void put32(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b[at + i] = uint8_t(v >> (8 * i));
}
static void put64(Bytes& b, size_t at, uint64_t v) {
    for (int i = 0; i < 8; i++) b[at + i] = uint8_t(v >> (8 * i));
}

// ---- synthetic NSOs ---------------------------------------------------------------------------------------------

// An LZ4 block of literals only: one sequence, no match (valid for every decoder: the last sequence has no match).
static Bytes lz4_literals(const Bytes& d) {
    Bytes o;
    const size_t n = d.size();
    o.push_back(uint8_t((n >= 15 ? 15 : n) << 4));
    if (n >= 15) {
        size_t r = n - 15;
        while (r >= 255) o.push_back(255), r -= 255;
        o.push_back(uint8_t(r));
    }
    o.insert(o.end(), d.begin(), d.end());
    return o;
}

// 8 literals, a 32-byte match at offset 8 (overlapping, extended length), then 16 literals: what lz4_text() gives.
static Bytes text_plain() {
    Bytes t = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
    for (int k = 0; k < 32; k++) t.push_back(t[k % 8]);
    Bytes tail = pattern(16, 7);
    t.insert(t.end(), tail.begin(), tail.end());
    return t;
}
static Bytes lz4_text() {
    Bytes o = {uint8_t(8 << 4 | 15), 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 8, 0, uint8_t(32 - 4 - 15)};
    o.push_back(uint8_t(15 << 4));
    o.push_back(1);  // 15 + 1 literals
    Bytes tail = pattern(16, 7);
    o.insert(o.end(), tail.begin(), tail.end());
    return o;
}

struct NsoParts {
    Bytes text = text_plain(), ro = pattern(300, 11), data = pattern(100, 13);
};

static Bytes hex_id(const std::string& hex) {
    Bytes b(32, 0);
    for (size_t i = 0; i + 1 < hex.size() && i / 2 < 32; i += 2) b[i / 2] = uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16));
    return b;
}

// text and ro LZ4-compressed, data plain, the three hashed (flags 0x3B), at 0, 0x1000 and 0x2000.
static Bytes make_nso(const std::string& build_id_hex, const NsoParts& p = {}) {
    const Bytes* seg[3] = {&p.text, &p.ro, &p.data};
    const Bytes ctext = lz4_text(), cro = lz4_literals(p.ro);
    const Bytes* file[3] = {&ctext, &cro, &p.data};
    Bytes n(0x100, 0);
    std::memcpy(n.data(), "NSO0", 4);
    put32(n, 0xC, 0x3B);
    uint32_t mem = 0, off = 0x100;
    for (int i = 0; i < 3; i++) {
        put32(n, 0x10 + 0x10 * i, off);
        put32(n, 0x14 + 0x10 * i, mem);
        put32(n, 0x18 + 0x10 * i, uint32_t(seg[i]->size()));
        put32(n, 0x60 + 4 * i, uint32_t(file[i]->size()));
        uint8_t d[32];
        nx::Sha256 h;
        h.update(seg[i]->data(), seg[i]->size());
        h.final(d);
        std::memcpy(n.data() + 0xA0 + 0x20 * i, d, 32);
        off += uint32_t(file[i]->size());
        mem = (mem + uint32_t(seg[i]->size()) + 0xFFF) & ~0xFFFu;
    }
    put32(n, 0x3C, 0x40);  // bss
    const Bytes id = hex_id(build_id_hex);
    std::memcpy(n.data() + 0x40, id.data(), 32);
    for (int i = 0; i < 3; i++) n.insert(n.end(), file[i]->begin(), file[i]->end());
    return n;
}

static Bytes make_npdm(uint64_t program_id) {
    Bytes b(0xC0, 0);
    std::memcpy(b.data(), "META", 4);
    put32(b, 0x70, 0x80);
    put32(b, 0x74, 0x40);
    std::memcpy(b.data() + 0x80, "ACI0", 4);
    put64(b, 0x90, program_id);
    return b;
}

static std::map<std::string, std::string> g_ids;  // version -> build id, from versions.txt

static void make_exefs(const fs::path& d, const std::string& main_id) {
    write_file(d / "main", make_nso(main_id));
    write_file(d / "sdk", make_nso("5D4B0A6C3B2A19080706050403020100"));
    write_file(d / "subsdk0", make_nso("0123456789ABCDEF0123456789ABCDEF"));
    write_file(d / "rtld", make_nso("AA000000000000000000000000000000"));
    write_file(d / "main.npdm", make_npdm(kAcnhProgramId));
}

// A small RomFS with every folder the game needs and the files the probe looks for.
static const char* kDirs[] = {"Bcsv", "Effect", "Font", "Layout", "Message", "Model", "Movie", "Pack", "Shader", "System"};
static const char* kKeys[] = {"Shader/Park_UBER_All.Nin_NX_NVN.szs", "System/GSys/gsys_resource.Nin_NX_NVN.release.szs",
                              "Pack/StaticParam.pack", "Font/ScalableFont.sarc.zs", "Movie/OpeningMovie.webm"};
static int make_romfs(const fs::path& d, bool skeleton = false, const char* skip_dir = nullptr) {
    int n = 0;
    uint32_t seed = 1;
    for (const char* dir : kDirs) {
        if (skip_dir && std::strcmp(dir, skip_dir) == 0) continue;
        for (int k = 0; k < 3; k++, n++)
            write_file(d / dir / ("file" + std::to_string(k) + ".bin"), skeleton ? Bytes() : pattern(100 + k * 37, seed++));
    }
    for (const char* k : kKeys) {
        if (skip_dir && std::strncmp(k, skip_dir, std::strlen(skip_dir)) == 0) continue;
        write_file(d / k, skeleton ? Bytes() : pattern(64, seed++)), n++;
    }
    return n;
}

static ProbeOptions quiet() {  // no "fewer files than a whole RomFS" warning for these tiny trees
    ProbeOptions o;
    o.expect_files = 0;
    o.expect_bytes = 0;
    return o;
}

static void dump(const ProbeResult& r) {
    for (const Message& m : r.messages)
        std::printf("    [%s] %s: %s\n", m.severity == Severity::Error ? "E" : m.severity == Severity::Warning ? "W" : "i",
                    m.key.c_str(), m.text.c_str());
}

static bool args_are(const ProbeResult& r, const char* key, std::vector<std::string> args) {
    const Message* m = r.find(key);
    return m && m->args == args;
}
// Any of the messages of that key with these arguments.
static bool any_args(const ProbeResult& r, const char* key, std::vector<std::string> args) {
    for (const Message& m : r.messages)
        if (m.key == key && m.args == args) return true;
    return false;
}

// ---- the cases --------------------------------------------------------------------------------------------------

static void test_nso() {
    // LZ4: literals with extended lengths, an overlapping match; malformed blocks refused.
    const Bytes t = text_plain(), ct = lz4_text();
    Bytes out(t.size());
    CHECK(lz4_decompress(ct.data(), ct.size(), out.data(), out.size()) && out == t);
    CHECK(!lz4_decompress(ct.data(), ct.size(), out.data(), out.size() - 1));  // another size
    Bytes bad = ct;
    bad[9] = 9, bad[10] = 0;  // offset past the start
    CHECK(!lz4_decompress(bad.data(), bad.size(), out.data(), out.size()));
    const Bytes lit = pattern(300, 3), cl = lz4_literals(lit);
    Bytes o2(lit.size());
    CHECK(lz4_decompress(cl.data(), cl.size(), o2.data(), o2.size()) && o2 == lit);

    // The flat image: the segments at their offsets, zeros between, the size where data ends.
    const NsoParts p;
    const Bytes nso = make_nso(g_ids["3.0.3"]);
    Bytes img;
    NsoHeader h;
    NsoStatus st = nso_to_flat(nso, img, &h);
    CHECK(st.ok());
    CHECK(build_id_hex(h) == g_ids["3.0.3"]);
    CHECK(h.seg[0].compressed && h.seg[1].compressed && !h.seg[2].compressed && h.seg[2].hashed);
    CHECK(img.size() == 0x2000 + p.data.size() && h.image_size() == img.size());
    CHECK(std::memcmp(img.data(), p.text.data(), p.text.size()) == 0);
    CHECK(std::memcmp(img.data() + 0x1000, p.ro.data(), p.ro.size()) == 0);
    CHECK(std::memcmp(img.data() + 0x2000, p.data.data(), p.data.size()) == 0);
    CHECK(img[p.text.size()] == 0 && img[0xFFF] == 0);

    Bytes h2 = nso;
    h2[0xA0 + 0x20 + 5] ^= 1;  // ro's hash
    st = nso_to_flat(h2, img);
    CHECK(st.error == NsoError::Hash && st.segment == 1);
    Bytes tr(nso.begin(), nso.end() - 10);
    CHECK(nso_to_flat(tr, img).error == NsoError::Truncated);
    Bytes nn = nso;
    nn[0] = 'X';
    CHECK(nso_to_flat(nn, img).error == NsoError::NotNso);
    CHECK(nso_to_flat(Bytes(nso.begin(), nso.begin() + 0x80), img).error == NsoError::NotNso);

    // tools/nso2bin.py writes the same image (it needs Python's lz4).
    if (!g_python.empty() && sh("\"" + g_python + "\" -c \"import lz4.block\" 2>" + kNull) == 0) {
        const fs::path in = g_tmp / "nso2bin" / "main", outp = g_tmp / "nso2bin" / "main.bin";
        write_file(in, nso);
        CHECK(sh("\"" + g_python + "\" \"" + g_src + "/tools/nso2bin.py\" \"" + s(in) + "\" \"" + s(outp) + "\" >" + kNull) == 0);
        Bytes mine;
        CHECK(nso_to_flat(nso, mine).ok() && read_all(outp) == mine);
        std::printf("  nso2bin.py: same image (%zu bytes)\n", mine.size());
    } else {
        std::printf("  SKIP nso2bin.py comparison (no python or no lz4 module)\n");
    }
}

static void test_good_and_versions() {
    // Parent folder, names in other cases (ExeFS/, RomFS/).
    const fs::path g = g_tmp / "good";
    make_exefs(g / "ExeFS", g_ids["3.0.3"]);
    const int files = make_romfs(g / "RomFS");
    ProbeResult r = probe({s(g)}, quiet());
    if (!r.ok) dump(r);
    CHECK(r.ok && r.layout == Layout::Parent);
    CHECK(r.version == "3.0.3" && r.build_id == g_ids["3.0.3"] && r.has("prepare.exefs.version"));
    CHECK(r.program_id == kAcnhProgramId && r.count(Severity::Error) == 0 && r.count(Severity::Warning) == 0);
    CHECK(r.modules.size() == 3 && r.modules[0].name == "main" && r.modules[0].valid && r.modules[2].name == "subsdk0");
    CHECK(r.modules[0].image_size == 0x2000 + NsoParts().data.size());
    CHECK(r.romfs.walked && r.romfs.files == uint64_t(files) && r.romfs.missing_dirs.empty() && !r.romfs.skeleton);
    CHECK(r.exefs == s(g / "ExeFS") && r.romfs.path == s(g / "RomFS"));
    const Message* sum = r.find("prepare.romfs.summary");
    CHECK(sum && sum->args.size() == 2 && sum->args[0] == std::to_string(files));
    CHECK(r.dlc.empty());

    // The same tree with the default expectations: "fewer files than a whole RomFS", a warning only.
    ProbeResult few = probe({s(g)});
    CHECK(few.ok && few.has("prepare.romfs.few_files"));
    // Headers only, the RomFS's top only.
    ProbeOptions fast = quiet();
    fast.verify_nso = false, fast.walk_romfs = false;
    ProbeResult q = probe({s(g)}, fast);
    CHECK(q.ok && q.version == "3.0.3" && !q.romfs.walked);

    // 3.0.2: accepted, with a note. 2.0.7 and unknown builds: refused, update 3.0.3 needed.
    const fs::path v2 = g_tmp / "v302";
    make_exefs(v2 / "exefs", g_ids["3.0.2"]);
    make_romfs(v2 / "romfs");
    r = probe({s(v2)}, quiet());
    CHECK(r.ok && r.version == "3.0.2" && r.has("prepare.exefs.version_302"));
    CHECK(r.find("prepare.exefs.version_302")->severity == Severity::Warning);

    make_exefs(v2 / "exefs", g_ids["2.0.7"]);
    r = probe({s(v2)}, quiet());
    CHECK(!r.ok && r.version == "2.0.7" && args_are(r, "prepare.exefs.needs_303", {"2.0.7"}));
    CHECK(r.find("prepare.exefs.needs_303")->text.find("3.0.3") != std::string::npos);

    make_exefs(v2 / "exefs", "00112233445566778899AABBCCDDEEFF");
    r = probe({s(v2)}, quiet());
    CHECK(!r.ok && r.version.empty() && args_are(r, "prepare.exefs.unknown_build", {"00112233445566778899AABBCCDDEEFF"}));
}

static void test_bad_exefs() {
    const fs::path d = g_tmp / "badexefs";
    make_romfs(d / "romfs");
    // A module whose ro segment fails its hash, another whose data segment does, one cut short.
    make_exefs(d / "exefs", g_ids["3.0.3"]);
    Bytes sdk = read_all(d / "exefs" / "sdk");
    sdk[0xA0 + 0x20 + 1] ^= 0x80;
    write_file(d / "exefs" / "sdk", sdk);
    Bytes mainb = read_all(d / "exefs" / "main");
    mainb[mainb.size() - 1] ^= 0xFF;  // the plain data segment's last byte
    write_file(d / "exefs" / "main", mainb);
    Bytes sub = read_all(d / "exefs" / "subsdk0");
    sub.resize(0x100 + 20);
    write_file(d / "exefs" / "subsdk0", sub);
    ProbeResult r = probe({s(d)}, quiet());
    CHECK(!r.ok);
    CHECK(args_are(r, "prepare.exefs.bad_hash", {"main", "data"}));
    bool sdk_ro = false;
    for (const Message& m : r.messages) sdk_ro |= m.key == "prepare.exefs.bad_hash" && m.args == std::vector<std::string>{"sdk", "ro"};
    CHECK(sdk_ro);
    CHECK(args_are(r, "prepare.exefs.truncated", {"subsdk0"}));
    CHECK(r.version == "3.0.3");  // the build id is read all the same
    // Headers only: the hashes are not checked, the cut file still is.
    ProbeOptions fast = quiet();
    fast.verify_nso = false;
    r = probe({s(d)}, fast);
    CHECK(!r.ok && !r.has("prepare.exefs.bad_hash") && r.has("prepare.exefs.truncated"));

    // Not an NSO, missing, a damaged optional module (a warning only).
    make_exefs(d / "exefs", g_ids["3.0.3"]);
    write_text(d / "exefs" / "sdk", "not an executable at all");
    fs::remove(d / "exefs" / "subsdk0");
    Bytes s1 = make_nso("11111111111111111111111111111111");
    s1[0xA0] ^= 1;
    write_file(d / "exefs" / "subsdk1", s1);
    r = probe({s(d)}, quiet());
    CHECK(!r.ok && args_are(r, "prepare.exefs.not_nso", {"sdk"}) && args_are(r, "prepare.exefs.missing", {"subsdk0"}));
    CHECK(r.has("prepare.exefs.optional_bad") && r.find("prepare.exefs.optional_bad")->severity == Severity::Warning);
    fs::remove(d / "exefs" / "subsdk1");

    // main.npdm: missing, not one, another program's.
    make_exefs(d / "exefs", g_ids["3.0.3"]);
    fs::remove(d / "exefs" / "main.npdm");
    r = probe({s(d)}, quiet());
    CHECK(!r.ok && r.has("prepare.exefs.npdm_missing"));
    write_text(d / "exefs" / "main.npdm", std::string(0x100, 'x'));
    r = probe({s(d)}, quiet());
    CHECK(!r.ok && r.has("prepare.exefs.npdm_invalid"));
    write_file(d / "exefs" / "main.npdm", make_npdm(0x0100000000010000ull));
    r = probe({s(d)}, quiet());
    CHECK(!r.ok && args_are(r, "prepare.exefs.other_game", {"0100000000010000"}));
}

static void test_romfs_problems() {
    const fs::path d = g_tmp / "romfs_problems";
    make_exefs(d / "exefs", g_ids["3.0.3"]);
    // Every file empty ("most files 0 bytes").
    make_romfs(d / "romfs", true);
    ProbeResult r = probe({s(d)}, quiet());
    CHECK(!r.ok && r.romfs.skeleton && r.has("prepare.romfs.skeleton"));
    ProbeOptions fast = quiet();
    fast.walk_romfs = false;
    r = probe({s(d)}, fast);
    CHECK(!r.ok && r.romfs.skeleton && args_are(r, "prepare.romfs.skeleton", {"5", "5"}));
    // Folders only (yuzu's "Skeleton" dump mode).
    fs::remove_all(d / "romfs");
    for (const char* dir : kDirs) fs::create_directories(d / "romfs" / dir / "sub");
    r = probe({s(d)}, quiet());
    CHECK(!r.ok && r.has("prepare.romfs.skeleton"));
    // Empty.
    fs::remove_all(d / "romfs");
    fs::create_directories(d / "romfs");
    r = probe({s(d)}, quiet());
    CHECK(!r.ok && r.has("prepare.romfs.empty"));
    // A folder the game needs is missing.
    make_romfs(d / "romfs", false, "Movie");
    r = probe({s(d)}, quiet());
    CHECK(!r.ok && args_are(r, "prepare.romfs.incomplete", {"Movie"}));
    // A key file missing: a warning.
    make_romfs(d / "romfs");
    fs::remove(d / "romfs" / "Pack" / "StaticParam.pack");
    r = probe({s(d)}, quiet());
    CHECK(r.ok && args_are(r, "prepare.romfs.key_file", {"Pack/StaticParam.pack"}));
    // Another game's RomFS.
    fs::remove_all(d / "romfs");
    write_text(d / "romfs" / "Data" / "thing.bin", "something");
    r = probe({s(d)}, quiet());
    CHECK(!r.ok && r.has("prepare.romfs.not_acnh"));
    // No RomFS at all.
    fs::remove_all(d / "romfs");
    r = probe({s(d)}, quiet());
    CHECK(!r.ok && r.has("prepare.input.no_romfs"));
}

static void test_containers() {
    const fs::path d = g_tmp / "containers";
    make_exefs(d / "exefs", g_ids["3.0.3"]);
    make_romfs(d / "romfs");
    const uint64_t files = probe({s(d)}, quiet()).romfs.files;
    // A package beside the extracted ExeFS and RomFS: said, left alone, the preparation goes on.
    write_text(d / "Animal Crossing [01006F8002326000].NSP", "PFS0");
    ProbeResult r = probe({s(d)}, quiet());
    if (!r.ok) dump(r);
    CHECK(r.ok && args_are(r, "prepare.input.container_ignored", {"Animal Crossing [01006F8002326000].NSP"}));
    CHECK(r.find("prepare.input.container_ignored")->severity == Severity::Warning);
    CHECK(r.find("prepare.input.container_ignored")->text.find("never reads") != std::string::npos);
    CHECK(!r.has("prepare.input.container"));
    fs::remove(d / "Animal Crossing [01006F8002326000].NSP");
    CHECK(probe({s(d)}, quiet()).ok && !probe({s(d)}, quiet()).has("prepare.input.container_ignored"));
    // A package picked itself: refused, with the advice to extract it.
    write_text(g_tmp / "game.xci", "HEAD");
    r = probe({s(g_tmp / "game.xci")}, quiet());
    CHECK(!r.ok && args_are(r, "prepare.input.container", {"game.xci"}));
    CHECK(r.find("prepare.input.container")->text.find("extract the ExeFS and RomFS with your emulator") != std::string::npos);
    // One in the RomFS, a split XCI's first part in the ExeFS, an NCA folder (yuzu's registered/): said, never counted
    // nor packed.
    write_text(d / "romfs" / "Model" / "deep" / "x.nca", "NCA3");
    write_text(d / "romfs" / "Model" / "deep" / "kept.bin", "kept");
    write_text(d / "exefs" / "game.xc0", "");
    fs::create_directories(d / "0123abcd.nca");
    write_text(d / "romfs" / "Pack" / "y.nca" / "00", "inside a package folder");
    r = probe({s(d)}, quiet());
    if (!r.ok) dump(r);
    CHECK(r.ok && any_args(r, "prepare.input.container_ignored", {"x.nca"}));
    CHECK(r.ok && any_args(r, "prepare.input.container_ignored", {"game.xc0"}));
    CHECK(r.ok && any_args(r, "prepare.input.container_ignored", {"0123abcd.nca"}));
    CHECK(r.ok && any_args(r, "prepare.input.container_ignored", {"y.nca"}));
    CHECK(r.romfs.files == files + 1);  // kept.bin only
    RomfsPakLayout pl;
    std::string perr;
    CHECK(romfs_pak_layout(s(d / "romfs"), pl, perr, {}, true) && pl.files.size() == files + 1);
    for (const RomfsPakEntry& e : pl.files) CHECK(e.path.find(".nca") == std::string::npos);
    CHECK(romfs_pak_layout(s(d / "romfs"), pl, perr) && pl.files.size() == files + 3);  // pack_romfs.py's: everything
    CHECK(is_game_package("A.NSP") && is_game_package("b.xcz") && !is_game_package("nsp") && !is_game_package("x.bin"));
    fs::remove(d / "romfs" / "Model" / "deep" / "x.nca");
    fs::remove(d / "romfs" / "Model" / "deep" / "kept.bin");
    fs::remove(d / "exefs" / "game.xc0");
    fs::remove(d / "0123abcd.nca");
    fs::remove_all(d / "romfs" / "Pack" / "y.nca");
    // Not a folder, not there.
    write_text(g_tmp / "notes.txt", "x");
    CHECK(probe({s(g_tmp / "notes.txt")}, quiet()).has("prepare.input.not_folder"));
    CHECK(probe({s(g_tmp / "nowhere")}, quiet()).has("prepare.input.missing"));
    CHECK(probe({}, quiet()).has("prepare.input.no_exefs"));
}

static void test_layouts() {
    // Ryujinx: "Extract Data > ExeFS / RomFS" into two folders the player chose; either order.
    const fs::path e = g_tmp / "ryu" / "my exefs", ro = g_tmp / "ryu" / "acnh files";
    make_exefs(e, g_ids["3.0.3"]);
    make_romfs(ro);
    ProbeResult r = probe({s(e), s(ro)}, quiet());
    if (!r.ok) dump(r);
    CHECK(r.ok && r.layout == Layout::TwoPicks && r.exefs == s(e) && r.romfs.path == s(ro));
    r = probe({s(ro), s(e)}, quiet());
    CHECK(r.ok && r.layout == Layout::TwoPicks && r.exefs == s(e) && r.romfs.path == s(ro));
    // Two of one kind; one alone.
    r = probe({s(e), s(e)}, quiet());
    CHECK(!r.ok && r.has("prepare.input.pick_pair"));
    r = probe({s(e)}, quiet());
    CHECK(!r.ok && r.has("prepare.input.no_romfs") && r.modules.size() == 3);
    r = probe({s(ro)}, quiet());
    CHECK(!r.ok && r.has("prepare.input.no_exefs") && r.romfs.files > 0);
    CHECK(!probe({s(e), s(ro), s(ro)}, quiet()).ok);

    // Both extracted into one folder: the ExeFS's files are left out of the RomFS.
    const fs::path m = g_tmp / "mixed";
    make_exefs(m, g_ids["3.0.3"]);
    const int files = make_romfs(m);
    r = probe({s(m)}, quiet());
    if (!r.ok) dump(r);
    CHECK(r.ok && r.layout == Layout::Mixed && r.romfs.files == uint64_t(files));
    CHECK(r.romfs.exclude_root.size() == 5);  // main main.npdm rtld sdk subsdk0
    RomfsPakOptions po;
    po.exclude_root = r.romfs.exclude_root;
    PakResult pr = write_romfs_pak(r.romfs.path, s(g_tmp / "mixed.pak"), po);
    CHECK(pr.ok && pr.files == uint64_t(files));

    // The ExeFS of a parent that holds the RomFS too (dump/<title>/exefs picked): the parent is used.
    const fs::path par = g_tmp / "parent2";
    make_exefs(par / "exefs", g_ids["3.0.3"]);
    make_romfs(par / "romfs");
    r = probe({s(par / "exefs")}, quiet());
    CHECK(r.ok && r.layout == Layout::Parent);
    r = probe({s(par / "romfs")}, quiet());
    CHECK(r.ok && r.layout == Layout::Parent);

    // A game folder prepared before: extract/romfs, or romfs.pak.
    const fs::path old = g_tmp / "oldfolder";
    make_exefs(old / "exefs", g_ids["3.0.2"]);
    make_romfs(old / "extract" / "romfs");
    r = probe({s(old)}, quiet());
    CHECK(r.ok && r.romfs.path == s(old / "extract" / "romfs"));
    CHECK(write_romfs_pak(s(old / "extract" / "romfs"), s(old / "romfs.pak")).ok);
    fs::remove_all(old / "extract");
    r = probe({s(old)}, quiet());
    if (!r.ok) dump(r);
    CHECK(r.ok && r.romfs.path == s(old / "romfs.pak") && r.romfs.files == uint64_t(make_romfs(g_tmp / "count")));
}

static void test_dlc() {
    uint64_t id = 0;
    CHECK(dlc_index_from_path("/home/x/yuzu/dump/01006F8002327001/romfs", &id) == 1 && id == 0x01006F8002327001ull);
    CHECK(dlc_index_from_path("C:\\Games\\Happy Home Paradise [01006f800232700A]\\romfs") == 10);
    CHECK(dlc_index_from_path("/games/Happy Home Paradise [01006f800232700A]/romfs", &id) == 10);
    CHECK(dlc_index_from_path("/g/aoc/3") == 3);
    CHECK(dlc_index_from_path("/g/My DLC", &id) == -1 && id == 0);
    CHECK(dlc_index_from_path("/g/0100000000010000/romfs", &id) == -1 && id == 0x0100000000010000ull);
    CHECK(dlc_index_from_path("/g/ACNH [01006F8002326000]/DLC", &id) == -1 && id == 0);  // a title further up: none
    CHECK(dlc_index_from_path("/g/01006F8002327003/", &id) == 3);

    // yuzu's / Eden's dump folder: the game's title folder and the add-ons' beside it.
    const fs::path ydump = g_tmp / "yuzu" / "dump";
    make_exefs(ydump / "01006F8002326000" / "exefs", g_ids["3.0.3"]);
    make_romfs(ydump / "01006F8002326000" / "romfs");
    write_file(ydump / "01006F8002327001" / "romfs" / "Model" / "hhp.bin", pattern(500, 5));
    write_file(ydump / "0100000000010000" / "romfs" / "other.bin", pattern(50, 5));  // another game's dump
    ProbeResult r = probe({s(ydump)}, quiet());
    if (!r.ok) dump(r);
    CHECK(r.ok && r.layout == Layout::EmulatorDump && r.dlc.size() == 1);
    CHECK(r.dlc.size() == 1 && r.dlc[0].index == 1 && r.dlc[0].files == 1 && r.dlc[0].bytes == 500);
    CHECK(r.dlc.size() == 1 && r.dlc[0].path == s(ydump / "01006F8002327001" / "romfs"));
    // The game's title folder picked: the add-on beside it found too.
    r = probe({s(ydump / "01006F8002326000")}, quiet());
    CHECK(r.ok && r.layout == Layout::Parent && r.dlc.size() == 1);

    // Picked: an add-on's folder named after its title (in the picks or as DLC), one with no title in its path.
    const fs::path g = g_tmp / "dlcgame";
    make_exefs(g / "exefs", g_ids["3.0.3"]);
    make_romfs(g / "romfs");
    const fs::path hhp = g_tmp / "Happy Home Paradise", tid = g_tmp / "dl" / "01006F8002327002";
    write_file(hhp / "Layout" / "a.bin", pattern(10, 1));
    write_file(tid / "Layout" / "b.bin", pattern(20, 1));
    ProbeOptions o = quiet();
    o.dlc = {s(hhp)};
    r = probe({s(g), s(tid)}, o);
    if (!r.ok) dump(r);
    CHECK(r.ok && r.layout == Layout::Parent && r.dlc.size() == 2);
    CHECK(r.has("prepare.dlc.unknown_index") && r.find("prepare.dlc.unknown_index")->severity == Severity::Warning);
    CHECK(r.find("prepare.dlc.unknown_index")->text.find("add-on 1") != std::string::npos);
    bool idx2 = false, unknown = false;
    for (const DlcInfo& d : r.dlc) idx2 |= d.index == 2 && d.path == s(tid), unknown |= d.index == 1 && d.path == s(hhp);
    CHECK(idx2 && unknown);  // no index in its name: add-on 1, the game's one paid add-on
    // The index the player chose, over the path's; two with no index are both add-on 1.
    o.dlc = {s(hhp), s(tid)};
    o.dlc_index = {3, 4};
    r = probe({s(g)}, o);
    if (!r.ok) dump(r);
    CHECK(r.ok && r.dlc.size() == 2 && !r.has("prepare.dlc.unknown_index"));
    CHECK(r.dlc.size() == 2 && r.dlc[0].index == 3 && r.dlc[1].index == 4);
    const fs::path other = g_tmp / "Other DLC";
    write_file(other / "Layout" / "c.bin", pattern(30, 3));
    o.dlc = {s(hhp), s(other)};
    o.dlc_index = {};
    r = probe({s(g)}, o);
    CHECK(!r.ok && r.has("prepare.dlc.duplicate") && args_are(r, "prepare.dlc.duplicate", {s(hhp), s(other), "1"}));
    o.dlc_index = {-1, 2};
    CHECK(probe({s(g)}, o).ok);
    // A game folder's aoc/<index>/.
    write_file(g / "aoc" / "1" / "Model" / "x.bin", pattern(30, 2));
    r = probe({s(g)}, quiet());
    CHECK(r.ok && r.dlc.size() == 1 && r.dlc[0].index == 1);
    // The same index twice, another title's content, the game's own RomFS as an add-on, an empty one.
    o.dlc = {s(ydump / "01006F8002327001")};
    r = probe({s(g)}, o);
    CHECK(!r.ok && r.has("prepare.dlc.duplicate"));
    o.dlc = {s(ydump / "0100000000010000")};
    r = probe({s(g)}, o);
    CHECK(!r.ok && r.has("prepare.dlc.other_title"));
    o.dlc = {s(ydump / "01006F8002326000")};
    r = probe({s(g)}, o);
    CHECK(!r.ok && r.has("prepare.dlc.is_game"));
    o.dlc = {s(g / "romfs")};  // the game's own RomFS, by its path
    r = probe({s(g)}, o);
    CHECK(!r.ok && r.has("prepare.dlc.is_game"));
    fs::create_directories(g_tmp / "emptydlc" / "01006F8002327005");
    o.dlc = {s(g_tmp / "emptydlc" / "01006F8002327005")};
    r = probe({s(g)}, o);
    CHECK(!r.ok && r.has("prepare.dlc.empty"));
}

// ---- the packs ----------------------------------------------------------------------------------------------------

static bool run_python(const std::vector<std::string>& args, const fs::path& stdout_to = {}) {
    std::string cmd = "\"" + g_python + "\" -I";
    for (const std::string& a : args) cmd += " \"" + a + "\"";
    if (!stdout_to.empty()) cmd += " >\"" + s(stdout_to) + "\"";
    return sh(cmd) == 0;
}

// romfs.pak read back as hle_fs.cpp's pak() reads it: path -> (offset, size).
static bool read_romfs_pak(const fs::path& p, std::map<std::string, std::pair<int64_t, int64_t>>& files) {
    FILE* f = std::fopen(s(p).c_str(), "rb");
    if (!f) return false;
    char magic[4];
    uint32_t version = 0, count = 0;
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "NXRP", 4) == 0 && std::fread(&version, 4, 1, f) == 1 &&
              version == 1 && std::fread(&count, 4, 1, f) == 1;
    for (uint32_t i = 0; ok && i < count; i++) {
        uint16_t len = 0;
        int64_t off = 0, size = 0;
        std::string path;
        ok = std::fread(&len, 2, 1, f) == 1;
        path.resize(len);
        ok = ok && std::fread(path.data(), 1, len, f) == len && std::fread(&off, 8, 1, f) == 1 && std::fread(&size, 8, 1, f) == 1;
        files[path] = {off, size};
    }
    std::fclose(f);
    return ok;
}

static void test_romfs_pak() {
    const fs::path t = g_tmp / "paktree";
    const size_t sizes[] = {0, 1, 15, 16, 17, 4096, (1u << 20) + 3, (5u << 19) + 1};
    std::map<std::string, Bytes> want;
    auto add = [&](const std::string& rel, const Bytes& b) {
        write_file(t / fs::path(std::u8string(rel.begin(), rel.end())), b);
        want[rel] = b;
    };
    int k = 0;
    for (size_t n : sizes) add("Model/m" + std::to_string(k++) + ".bin", pattern(n, uint32_t(n + 1)));
    add("Message/caf\xc3\xa9.msbt", pattern(33, 2));      // UTF-8 names sort by their bytes
    add("Message/cafe.msbt", pattern(34, 3));
    add("Message/\xe6\x97\xa5\xe6\x9c\xac.msbt", pattern(35, 4));
    add("a/B", pattern(5, 5));
    add("a/a", pattern(6, 6));
    add("a b/c", pattern(7, 7));
    add("Z", pattern(8, 8));
    add("z", pattern(9, 9));
    add("deep/er/and/deeper/file.zs", pattern(70, 10));
    fs::create_directories(t / "Empty" / "Folder");  // not in the pack

    RomfsPakLayout l;
    std::string err;
    CHECK(romfs_pak_layout(s(t), l, err) && l.files.size() == want.size());
    for (size_t i = 1; i < l.files.size(); i++) CHECK(l.files[i - 1].path < l.files[i].path && l.files[i].offset % 16 == 0);

    std::vector<PakProgress> seen;
    RomfsPakOptions opt;
    opt.progress = [&](const PakProgress& p) { seen.push_back(p); };
    const fs::path mine = g_tmp / "romfs.pak";
    PakResult r = write_romfs_pak(s(t), s(mine), opt);
    CHECK(r.ok && r.files == want.size() && r.bytes == l.size && fs::file_size(mine) == l.size);
    CHECK(!fs::exists(s(mine) + ".part"));
    CHECK(!seen.empty() && seen.back().bytes_done == seen.back().bytes_total && seen.back().files_done == want.size());

    // Read back as the runtime reads it.
    std::map<std::string, std::pair<int64_t, int64_t>> idx;
    CHECK(read_romfs_pak(mine, idx) && idx.size() == want.size());
    const Bytes pak = read_all(mine);
    for (auto& [path, b] : want) {
        auto it = idx.find(path);
        CHECK(it != idx.end());
        if (it == idx.end()) continue;
        CHECK(it->second.second == int64_t(b.size()) && it->second.first % 16 == 0);
        CHECK(Bytes(pak.begin() + it->second.first, pak.begin() + it->second.first + it->second.second) == b);
    }

    // tools/pack_romfs.py: the same bytes, the same --size.
    if (!g_python.empty()) {
        const fs::path theirs = g_tmp / "romfs_py.pak", size_out = g_tmp / "romfs_py.size";
        CHECK(run_python({g_src + "/tools/pack_romfs.py", s(t), s(theirs)}));
        CHECK(read_all(theirs) == pak);
        CHECK(run_python({g_src + "/tools/pack_romfs.py", s(t), "--size"}, size_out));
        const Bytes sz = read_all(size_out);
        CHECK(std::stoull(std::string(sz.begin(), sz.end())) == l.size);
        // An empty tree: the header alone.
        fs::create_directories(g_tmp / "emptytree" / "sub");
        CHECK(write_romfs_pak(s(g_tmp / "emptytree"), s(g_tmp / "empty.pak")).ok);
        CHECK(run_python({g_src + "/tools/pack_romfs.py", s(g_tmp / "emptytree"), s(g_tmp / "empty_py.pak")}));
        CHECK(read_all(g_tmp / "empty.pak") == read_all(g_tmp / "empty_py.pak") && fs::file_size(g_tmp / "empty.pak") == 12);
        std::printf("  pack_romfs.py: same %zu bytes\n", pak.size());
    } else {
        std::printf("  SKIP pack_romfs.py comparison (no python)\n");
    }

    // Cancelled before it starts, and half way through a big file (the first progress call, at 16 MiB): nothing
    // left, an older pack kept as it was.
    std::atomic<bool> cancel{true};
    RomfsPakOptions c;
    c.cancel = &cancel;
    write_text(g_tmp / "old.pak", "older");
    r = write_romfs_pak(s(t), s(g_tmp / "old.pak"), c);
    CHECK(!r.ok && r.cancelled && read_all(g_tmp / "old.pak") == Bytes({'o', 'l', 'd', 'e', 'r'}));
    CHECK(!fs::exists(g_tmp / "old.pak.part"));
    write_file(g_tmp / "bigtree" / "Movie" / "big.webm", Bytes(20u << 20, 0x5A));
    cancel = false;
    int calls = 0;
    c.progress = [&](const PakProgress& p) {
        calls++;
        CHECK(p.bytes_done >= (16u << 20) && p.bytes_done < p.bytes_total && p.files_done == 0 && p.files_total == 1);
        cancel = true;
    };
    r = write_romfs_pak(s(g_tmp / "bigtree"), s(g_tmp / "old.pak"), c);
    CHECK(!r.ok && r.cancelled && calls == 1 && read_all(g_tmp / "old.pak") == Bytes({'o', 'l', 'd', 'e', 'r'}));
    CHECK(!fs::exists(g_tmp / "old.pak.part"));
    // Replacing an older pack.
    CHECK(write_romfs_pak(s(t), s(g_tmp / "old.pak")).ok && read_all(g_tmp / "old.pak") == pak);
    CHECK(!write_romfs_pak(s(g_tmp / "nowhere"), s(g_tmp / "x.pak")).ok && !fs::exists(g_tmp / "x.pak"));
}

static void test_spirv_pak() {
    const fs::path t = g_tmp / "spirv";
    std::map<std::string, Bytes> want;
    auto add = [&](const std::string& name, const Bytes& b) {
        write_file(t / name, b);
        want[name] = b;
    };
    add("0123456789abcdef.spv", pattern(400, 1));  // 16: the longest key
    add("00ff.spv", pattern(4, 2));
    add("dead.spv", Bytes());
    add("Beef.spv", pattern(77, 3));
    write_file(t / "0123456789abcdef0.spv", pattern(9, 4));  // 17: left out
    write_file(t / "notes.txt", pattern(9, 5));
    write_file(t / "x.SPV", pattern(9, 6));  // the extension in another case: left out
    const fs::path mine = g_tmp / "spirv_nores.pak";
    SpirvPakLayout l;
    std::string err;
    CHECK(spirv_pak_layout(s(t), l, err) && l.files.size() == 4 && !l.results_tsv);
    CHECK(write_spirv_pak(s(t), s(mine)).ok);
    add("results.tsv", Bytes{'h', '\t', '1', '\n'});
    const fs::path mine2 = g_tmp / "spirv.pak";
    std::vector<PakProgress> seen;
    PakOptions po;
    po.progress = [&](const PakProgress& p) { seen.push_back(p); };
    PakResult r = write_spirv_pak(s(t), s(mine2), po);
    CHECK(r.ok && r.files == 5 && !seen.empty() && seen.back().bytes_done == r.bytes);

    // Read back as nvn_shaders.cpp's spirv_pak() reads it.
    const Bytes pak = read_all(mine2);
    CHECK(pak.size() >= 8 && pak[0] == 'S' && pak[1] == 'P' && pak[2] == 'K' && pak[3] == '1');
    uint32_t count = 0;
    std::memcpy(&count, pak.data() + 4, 4);
    CHECK(count == 5);
    std::map<std::string, Bytes> got;
    for (uint32_t k = 0; k < count && 8 + 32 * (k + 1) <= pak.size(); k++) {
        const uint8_t* e = pak.data() + 8 + size_t(k) * 32;
        uint64_t off, size;
        std::memcpy(&off, e + 16, 8), std::memcpy(&size, e + 24, 8);
        got[std::string(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 16))] =
            Bytes(pak.begin() + off, pak.begin() + off + size);
    }
    CHECK(got.size() == 5 && got["results.tsv"] == want["results.tsv"] && got["0123456789abcdef"] == want["0123456789abcdef.spv"]);
    CHECK(got["dead"].empty() && got["Beef"] == want["Beef.spv"] && got["00ff"] == want["00ff.spv"]);

    if (!g_python.empty()) {
        fs::remove(t / "results.tsv");
        CHECK(run_python({g_src + "/tools/pack_spirv.py", s(t), s(g_tmp / "spirv_nores_py.pak")}, g_tmp / "py.out"));
        CHECK(read_all(g_tmp / "spirv_nores_py.pak") == read_all(mine));
        write_file(t / "results.tsv", want["results.tsv"]);
        CHECK(run_python({g_src + "/tools/pack_spirv.py", s(t), s(g_tmp / "spirv_py.pak")}, g_tmp / "py.out"));
        CHECK(read_all(g_tmp / "spirv_py.pak") == pak);
        std::printf("  pack_spirv.py: same %zu bytes\n", pak.size());
    } else {
        std::printf("  SKIP pack_spirv.py comparison (no python)\n");
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: prepare_io_test <source dir> [python]\n");
        return 2;
    }
    g_src = argv[1];
    g_python = argc > 2 ? argv[2] : "";
    g_tmp = fs::absolute("prepare_io_test.tmp");
    fs::remove_all(g_tmp);
    fs::create_directories(g_tmp);

    // The build ids the project knows, from the file the library carries a copy of.
    Bytes vt = read_all(fs::path(g_src) / "games" / "acnh" / "versions.txt");
    for (const KnownVersion& v : known_versions(std::string(vt.begin(), vt.end()))) g_ids[v.version] = v.build_id;
    CHECK(g_ids.count("3.0.3") && g_ids.count("3.0.2") && g_ids.count("2.0.7"));
    CHECK(known_versions().size() == known_versions(std::string(vt.begin(), vt.end())).size());

    struct {
        const char* name;
        void (*fn)();
    } tests[] = {{"nso", test_nso},           {"good and versions", test_good_and_versions},
                 {"bad exefs", test_bad_exefs}, {"romfs problems", test_romfs_problems},
                 {"containers", test_containers}, {"layouts", test_layouts},
                 {"dlc", test_dlc},             {"romfs.pak", test_romfs_pak},
                 {"spirv.pak", test_spirv_pak}};
    for (auto& t : tests) {
        const int before = g_failed;
        std::printf("%s\n", t.name);
        t.fn();
        if (g_failed != before) std::printf("  %d failed\n", g_failed - before);
    }
    fs::remove_all(g_tmp);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
