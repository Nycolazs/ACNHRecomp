// The game folder's preparation end to end (prepare.h, acnh_prepare) on synthetic inputs only, no game data: an
// extracted ExeFS (NSOs made by io/test_inputs.h, main with 3.0.3's build id), a small RomFS, an add-on, a package
// lying beside them. The recompile is skipped (NX_PREPARE_SKIP_RECOMP: synthetic modules are no real program) and the
// shader tool is fake_shader_tool. The folder written is checked as the player reads it: analysis/<module>.bin's size
// (loader.cpp), romfs.pak's index and data (hle_fs.cpp), aoc/<index>/, spirv.pak (nvn_shaders.cpp), game.ini and
// .import/complete (launcher/files.cpp, checked by prepare_launcher_test).
//   prepare_e2e_test base <source dir> <work dir> <acnh_prepare> <fake shader tool>
//   prepare_e2e_test zig <source dir> <work dir> <fake shader tool> <zig folder> <python> <zig host>
// zig: the Compile stage with a real zig on tools/make_fake_gen.py's stand-in for the recompiled code (fetched with
// tools/fetch_zig.py into <zig folder> when it is not there; exit 77, skipped, when that cannot be done), the library
// loaded and its ABI checked against the player's (NX_GAME_ABI).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <nx/module.h>

#include "gamelib.h"
#include "nso.h"
#include "prepare.h"
#include "test_inputs.h"

using namespace nx::prepare;
using namespace nx::prepare::test;
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

static const char* kId303 = "FF1D1C05670DB6021C85B624A710B963";  // games/acnh/versions.txt
static const char* kId207 = "0948E48778171EE6D5D7A65813ED5FCC";

// A path as the preparation takes it (UTF-8 on every platform; path::string() is the ANSI code page on Windows).
static std::string s(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}
// "ção": a folder named with accents (escaped: the source file's encoding does not matter)
static const fs::path kAccents = fs::path(u8"\u00e7\u00e3o");

static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

// A command line (UTF-8) run by the shell: on Windows as UTF-16 (_wsystem), so a path named with accents arrives whole.
static int sh(std::string cmd) {
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
    int rc = _wsystem(fs::path(std::u8string(cmd.begin(), cmd.end())).wstring().c_str());
#else
    int rc = std::system(cmd.c_str());
#endif
#ifndef _WIN32
    if (rc != -1 && WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
    return rc;
}
static std::string q(const fs::path& p) { return "\"" + s(p) + "\""; }

struct Run {
    prep::Status last;
    std::set<prep::Stage> stages;
    std::string log;
    double max_overall = 0;
    bool overall_monotonic = true;
};

// Runs the preparation to its end, every stage it went through and its log kept.
static Run run_prep(const prep::Options& o, double cancel_at_shaders_after = -1) {
    Run r;
    CHECK(prep::start(o));
    CHECK(!prep::start(o));  // one at a time
    auto shaders_since = std::chrono::steady_clock::time_point{};
    for (;;) {
        prep::Status st = prep::status();
        r.stages.insert(st.stage);
        for (const auto& l : st.log) {
            if (l.text.empty()) r.stages.insert(l.stage);  // where each stage began
            r.log += l.text + "\n";
        }
        if (st.overall + 1e-9 < r.max_overall) r.overall_monotonic = false;
        r.max_overall = std::max(r.max_overall, st.overall);
        if (cancel_at_shaders_after >= 0 && st.stage == prep::Stage::Shaders) {
            if (shaders_since == std::chrono::steady_clock::time_point{}) shaders_since = std::chrono::steady_clock::now();
            if (std::chrono::steady_clock::now() - shaders_since > std::chrono::duration<double>(cancel_at_shaders_after))
                prep::cancel(), cancel_at_shaders_after = -1;
        }
        if (st.finished) {
            r.last = st;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    prep::wait();
    if (r.last.rc != 0) std::printf("  rc %d: %s\n", r.last.rc, r.last.error.c_str());
    return r;
}

static prep::Options base_options(const fs::path& in, const fs::path& out, const fs::path& tool) {
    prep::Options o;
    o.inputs = {s(in)};
    o.out = s(out);
    o.platforms = {gl::host_target().empty() ? "linux-x64" : gl::host_target()};
    o.shader_tool = s(tool);
    o.skip_recompile = true;
    o.probe.expect_files = 0;  // no "fewer files than a whole RomFS" warning for these tiny trees
    o.probe.expect_bytes = 0;
    return o;
}

// The game's files as the player extracted them: <dir>/exefs, <dir>/romfs, a package beside them.
static void make_inputs(const fs::path& dir, const std::string& main_id = kId303) {
    make_exefs(dir / "exefs", main_id);
    make_romfs(dir / "romfs");
    write_file(dir / "romfs" / "Message" / "deep" / "x.bin", pattern(70000, 3));  // 70 KB, two folders down
    write_text(dir / "Animal Crossing [01006F8002326000][v3.0.3].nsp", "PFS0 never read");
}

static std::map<std::string, uint64_t> tree(const fs::path& dir, bool skip_packages = true) {
    std::map<std::string, uint64_t> m;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (skip_packages && is_game_package(it->path().filename().string())) {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file()) m[it->path().lexically_relative(dir).generic_string()] = it->file_size();
    }
    return m;
}

static std::string first_line(const fs::path& p) {
    std::ifstream f(p);
    std::string l;
    std::getline(f, l);
    return l;
}
static std::string text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

#ifdef __linux__
// The processes whose command line holds `needle` (/proc/<pid>/cmdline); in *clang, how many of them run clang.
static size_t procs_with(const std::string& needle, size_t* clang = nullptr) {
    size_t n = 0, c = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/proc", ec)) {
        const std::string pid = e.path().filename().string();
        if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
        const std::string cmd = text(e.path() / "cmdline");  // empty once it has ended (a zombie)
        if (cmd.find(needle) == std::string::npos) continue;
        n++;
        if (cmd.find(std::string("\0clang\0", 7)) != std::string::npos) c++;
    }
    if (clang) *clang = c;
    return n;
}
#endif

// romfs.pak as hle_fs.cpp reads it: path -> (offset, size).
static bool read_pak(const fs::path& p, std::map<std::string, std::pair<uint64_t, uint64_t>>& out) {
    std::ifstream f(p, std::ios::binary);
    char h[12];
    if (!f.read(h, 12) || std::memcmp(h, "NXRP\x01\0\0\0", 8) != 0) return false;
    uint32_t n;
    std::memcpy(&n, h + 8, 4);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t len;
        if (!f.read(reinterpret_cast<char*>(&len), 2)) return false;
        std::string path(len, '\0');
        uint64_t off, size;
        if (!f.read(path.data(), len) || !f.read(reinterpret_cast<char*>(&off), 8) || !f.read(reinterpret_cast<char*>(&size), 8))
            return false;
        out[path] = {off, size};
    }
    return true;
}

// spirv.pak as nvn_shaders.cpp reads it: the keys.
static std::set<std::string> spirv_keys(const fs::path& p) {
    std::set<std::string> keys;
    std::ifstream f(p, std::ios::binary);
    uint32_t h[2];
    if (!f.read(reinterpret_cast<char*>(h), 8) || h[0] != 0x314B5053) return keys;
    for (uint32_t i = 0; i < h[1]; i++) {
        char e[32];
        if (!f.read(e, 32)) break;
        keys.insert(std::string(e, strnlen(e, 16)));
    }
    return keys;
}

static bool any_part_files(const fs::path& dir) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".part") return std::printf("  left: %s\n", s(it->path()).c_str()), true;
    return false;
}

// What the player needs of the folder, whatever the platforms.
static void check_folder(const fs::path& in, const fs::path& out, bool in_place, int aoc_index, const fs::path& dlc) {
    // exefs/ and analysis/: the modules (no rtld), their flat images ending where the data segment ends
    for (const char* m : {"main", "sdk", "subsdk0"}) {
        const Bytes nso = read_all(in / "exefs" / m);
        CHECK(read_all(out / "exefs" / m) == nso);
        std::vector<uint8_t> image;
        NsoHeader h;
        CHECK(nso_to_flat(nso, image, &h).ok());
        const Bytes bin = read_all(out / "analysis" / (std::string(m) + ".bin"));
        CHECK(bin == image);
        CHECK(bin.size() == uint64_t(h.seg[2].mem_offset) + h.seg[2].size);  // loader.cpp: mi->data + mi->data_size
    }
    CHECK(!fs::exists(out / "analysis" / "rtld.bin"));
    if (!in_place) CHECK(!fs::exists(out / "exefs" / "rtld"));  // in place, exefs/ is the player's own
    CHECK(read_all(out / "exefs" / "main.npdm") == read_all(in / "exefs" / "main.npdm"));
    CHECK(read_all(out / "analysis" / "main.npdm") == read_all(in / "exefs" / "main.npdm"));
    // the RomFS: romfs.pak with every file of it (and no package), or the loose folder in place
    const auto romfs = tree(in / "romfs");
    if (in_place) {
        CHECK(!fs::exists(out / "romfs.pak"));
        CHECK(fs::is_directory(out / "romfs" / "Shader"));
    } else {
        std::map<std::string, std::pair<uint64_t, uint64_t>> pak;
        CHECK(read_pak(out / "romfs.pak", pak));
        CHECK(pak.size() == romfs.size());
        const std::string data = text(out / "romfs.pak");
        for (const auto& [path, size] : romfs) {
            auto it = pak.find(path);
            CHECK(it != pak.end() && it->second.second == size);
            if (it != pak.end() && it->second.first + size <= data.size())
                CHECK(data.compare(it->second.first, size, text(in / "romfs" / path)) == 0);
        }
        CHECK(!fs::exists(out / "romfs"));
    }
    // aoc/<index>/: the add-on's files, without the package in it
    if (aoc_index > 0) {
        CHECK(tree(out / "aoc" / std::to_string(aoc_index)) == tree(dlc));
        CHECK(tree(out / "aoc" / std::to_string(aoc_index), false) == tree(dlc));
    }
    // spirv.pak: the shaders and results.tsv; the loose .spv gone, the record of the conversion kept
    CHECK(spirv_keys(out / "spirv.pak") == std::set<std::string>({"0123456789abcdef", "fedcba9876543210", "results.tsv"}));
    CHECK(!fs::exists(out / "spirv" / "0123456789abcdef.spv") && fs::exists(out / "spirv" / "summary.txt"));
    // game.ini and .import/: what launcher/files.cpp's prepared_folder() and nextendo.cpp read
    CHECK(first_line(out / "game.ini").find("ACNHRecomp") != std::string::npos);
    CHECK(text(out / "game.ini").find(std::string("build_id = ") + kId303 + "\n") != std::string::npos);
    CHECK(text(out / "game.ini").find("version = 3.0.3\n") != std::string::npos);
    CHECK(fs::exists(out / ".import" / "complete"));
    const std::string index = text(out / ".import" / "index.tsv");
    if (aoc_index > 0) CHECK(index.find("\taoc/" + std::to_string(aoc_index) + "/Layout/a.bin\n") != std::string::npos);
    if (in_place) CHECK(index.find("\tromfs/Shader/Park_UBER_All.Nin_NX_NVN.szs\n") != std::string::npos);
    // nothing half written, no package, no recompiled code left
    CHECK(!any_part_files(out));
    if (!in_place)  // in place, the player's package beside stays where it was
        for (const auto& [path, size] : tree(out, false)) CHECK(!is_game_package(fs::path(path).filename().string()));
    CHECK(!fs::exists(out / "gen"));
}

static int base(const fs::path& src, const fs::path& work, const fs::path& cli, const fs::path& tool) {
    (void)src;
    const fs::path in = work / "in" / "ACNH", hhp = work / "in" / "Happy Home Paradise";
    make_inputs(in);
    write_file(hhp / "Layout" / "a.bin", pattern(3000, 9));
    write_file(hhp / "Model" / "b.bin", pattern(500, 10));
    write_text(hhp / "Model" / "leftover.nca", "NCA3");  // never copied

    std::puts("prepare (API): parent folder, an add-on, a package beside");
    const fs::path out1 = work / "out1";
    prep::Options o = base_options(in, out1, tool);
    o.dlc = {s(hhp)};
    Run r = run_prep(o);
    CHECK(r.last.rc == 0 && r.last.stage == prep::Stage::Done && r.last.overall == 1.0);
    CHECK(r.overall_monotonic);
    // every stage began (its mark in the log) and said what it did; the recompile and compile skipped (no gen/)
    for (prep::Stage st : {prep::Stage::Check, prep::Stage::ExeFS, prep::Stage::Shaders, prep::Stage::Packs, prep::Stage::Finish})
        CHECK(r.stages.count(st));
    for (const char* said : {"platforms: ", "exefs: analysis/main.bin (", "recompile skipped", "shaders: 2 converted",
                             "romfs.pak: ", "aoc/1: 2 files", "spirv.pak: 2 shaders", "done: "})
        CHECK(r.log.find(said) != std::string::npos);
    CHECK(!r.stages.count(prep::Stage::Recompile) && !r.stages.count(prep::Stage::Compile));
    CHECK(r.last.build_id == kId303 && r.last.version == "3.0.3" && r.last.out == s(fs::weakly_canonical(out1)));
    bool pkg = false, unknown = false;
    for (const Message& m : r.last.messages) {
        pkg |= m.key == "prepare.input.container_ignored" && m.args[0].find(".nsp") != std::string::npos;
        unknown |= m.key == "prepare.dlc.unknown_index";
    }
    CHECK(pkg && unknown);
    CHECK(r.log.find("warning: ") != std::string::npos && r.log.find("romfs.pak: ") != std::string::npos);
    check_folder(in, out1, false, 1, hhp);

    std::puts("prepare again on the same folder: what is right is kept");
    const auto pak_time = fs::last_write_time(out1 / "romfs.pak");
    r = run_prep(o);
    CHECK(r.last.rc == 0);
    CHECK(r.log.find("romfs.pak: written before from this RomFS") != std::string::npos);
    CHECK(r.log.find("aoc/1: copied before") != std::string::npos);
    CHECK(r.log.find("shaders: converted before, kept") != std::string::npos);
    CHECK(r.log.find("spirv.pak: written before, kept") != std::string::npos);
    CHECK(fs::last_write_time(out1 / "romfs.pak") == pak_time);
    check_folder(in, out1, false, 1, hhp);

    std::puts("a stage replacing what a complete folder holds takes its .import/complete away first");
    // the shaders converted again (spirv.pak gone), cancelled: not complete; then to the end: complete again
    fs::remove(out1 / "spirv.pak");
    fs::remove(out1 / "spirv" / "summary.txt");
    set_env("NX_FAKE_SHADER_SLEEP", "30");
    r = run_prep(o, 0.3);
    set_env("NX_FAKE_SHADER_SLEEP", nullptr);
    CHECK(r.last.rc == 4 && !fs::exists(out1 / ".import" / "complete"));
    r = run_prep(o);
    CHECK(r.last.rc == 0 && fs::exists(out1 / ".import" / "complete"));
    check_folder(in, out1, false, 1, hhp);
#ifndef _WIN32
    if (geteuid() != 0) {  // permissions bind (root's do not)
        // the add-on changed, its old copy cannot be removed whole (a folder of it read-only): the stage fails, and
        // the folder, aoc/1 half gone, is not taken for complete
        write_file(hhp / "Model" / "c.bin", pattern(100, 11));
        fs::permissions(out1 / "aoc" / "1" / "Layout", fs::perms::owner_read | fs::perms::owner_exec);
        r = run_prep(o);
        fs::permissions(out1 / "aoc" / "1" / "Layout", fs::perms::owner_all);
        CHECK(r.last.rc == 1 && r.last.error.find("aoc") != std::string::npos);
        CHECK(!fs::exists(out1 / ".import" / "complete"));
        fs::remove(hhp / "Model" / "c.bin");
        r = run_prep(o);
        CHECK(r.last.rc == 0 && fs::exists(out1 / ".import" / "complete"));
        check_folder(in, out1, false, 1, hhp);
    }
#endif

    std::puts("a folder of another version: its shaders and romfs.pak made again; its romfs.pak refused as the RomFS");
    // as ACNHRecomp-Importer leaves a folder of another update: no records, game.ini's build id is its last main's
    const std::string other = "11111111111111111111111111111111", ini = text(out1 / "game.ini");
    CHECK(first_line(out1 / ".import" / "shaders.build_id") == kId303 &&
          first_line(out1 / ".import" / "romfs.pak.build_id") == kId303);
    write_text(out1 / "game.ini", std::string(ini).replace(ini.find(kId303), 32, other));
    fs::remove(out1 / ".import" / "shaders.build_id");
    fs::remove(out1 / ".import" / "romfs.pak.build_id");
    r = run_prep(o);
    CHECK(r.last.rc == 0);
    CHECK(r.log.find("the shaders were converted for another version of the game (" + other + "): converted again") !=
          std::string::npos);
    CHECK(r.log.find("shaders: 2 converted") != std::string::npos);
    CHECK(r.log.find("romfs.pak: written before") == std::string::npos && r.log.find("romfs.pak: ") != std::string::npos);
    CHECK(first_line(out1 / ".import" / "shaders.build_id") == kId303 &&
          first_line(out1 / ".import" / "romfs.pak.build_id") == kId303);
    check_folder(in, out1, false, 1, hhp);
    // a change of version cut short leaves the new ExeFS beside the old romfs.pak: refused as the RomFS, in place
    write_text(out1 / ".import" / "romfs.pak.build_id", other + "\n");
    prep::Options own = base_options(out1, {}, tool);
    own.out.clear(), own.in_place = true;
    r = run_prep(own);
    CHECK(r.last.rc == 2 &&
          r.last.error.find("romfs.pak was packed from the RomFS of another version of the game (" + other) != std::string::npos);
    write_text(out1 / ".import" / "romfs.pak.build_id", std::string(kId303) + "\n");
    r = run_prep(own);  // of this version: the folder prepared again from it
    CHECK(r.last.rc == 0 && r.log.find("shaders: converted before, kept") != std::string::npos);
    check_folder(in, out1, false, 1, hhp);

    std::puts("cancel while the shader tool runs");
    set_env("NX_FAKE_SHADER_SLEEP", "30");
    const auto t0 = std::chrono::steady_clock::now();
    r = run_prep(base_options(in, work / "out_cancel", tool), 0.3);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    set_env("NX_FAKE_SHADER_SLEEP", nullptr);
    CHECK(r.last.rc == 4 && took < 10);
    CHECK(!fs::exists(work / "out_cancel" / ".import" / "complete") && !fs::exists(work / "out_cancel" / "spirv" / "summary.txt"));
    std::printf("  stopped in %.1f s\n", took);

    std::puts("refused: a game folder inside the RomFS, one holding other things, a version other than 3.0.3");
    r = run_prep(base_options(in, in / "romfs" / "inside", tool));
    CHECK(r.last.rc == 2 && r.last.error.find("inside the RomFS") != std::string::npos);
    write_text(work / "busy" / "notes.txt", "mine");
    r = run_prep(base_options(in, work / "busy", tool));
    CHECK(r.last.rc == 2 && r.last.error.find("holds other things") != std::string::npos);
    make_inputs(work / "in" / "old", kId207);
    r = run_prep(base_options(work / "in" / "old", work / "out_old", tool));
    CHECK(r.last.rc == 2 && r.last.error.find("3.0.3") != std::string::npos);
    CHECK(!fs::exists(work / "out_old" / "exefs"));
    prep::Options no_tool = base_options(in, work / "out_tool", tool);
    no_tool.shader_tool = s(work / "nowhere" / "spirv_harness");
    r = run_prep(no_tool);
    CHECK(r.last.rc == 2 && r.last.error.find("shader tool") != std::string::npos);

    std::puts("refused: a gen/ or spirv/ the preparation did not make (tools/setup_acnh.py's data folder), in place or not");
    const fs::path dev = work / "dev";
    make_exefs(dev / "exefs", kId303);
    make_romfs(dev / "extract" / "romfs");
    write_text(dev / "gen" / "main" / "table.cpp", "// a developer's NX_GEN_DIR\n");
    write_text(dev / "spirv" / "0123456789abcdef.spv", "a developer's shader");
    prep::Options od = base_options(dev, {}, tool);
    od.out.clear(), od.in_place = true;
    r = run_prep(od);
    CHECK(r.last.rc == 2 && r.last.error.find(s(fs::path("dev") / "gen") + " was not made by ACNHRecomp's preparation") !=
                                std::string::npos);
    fs::remove_all(dev / "gen");
    r = run_prep(od);
    CHECK(r.last.rc == 2 && r.last.error.find(s(fs::path("dev") / "spirv") + " was not made") != std::string::npos);
    CHECK(read_all(dev / "spirv" / "0123456789abcdef.spv").size() == 20 && !fs::exists(dev / "analysis"));
    fs::remove_all(dev / "spirv");
    write_text(dev / "gen" / "main" / "table.cpp", "// a developer's NX_GEN_DIR\n");
    r = run_prep(base_options(in, dev, tool));  // its exefs/ lets it be the game folder; its gen/ does not
    CHECK(r.last.rc == 2 && r.last.error.find("was not made") != std::string::npos);
    CHECK(text(dev / "gen" / "main" / "table.cpp") == "// a developer's NX_GEN_DIR\n");
    // a folder prepared before keeps its spirv/ (the mark, or game.ini and .import/ for one from before it)
    CHECK(fs::exists(out1 / "spirv" / ".acnhrecomp_prepare"));
    fs::remove(out1 / "spirv" / ".acnhrecomp_prepare");
    r = run_prep(o);
    CHECK(r.last.rc == 0 && r.log.find("shaders: converted before, kept") != std::string::npos);

    std::puts("refused: no toolchain, a damaged one");
    prep::Options compile = base_options(in, work / "out_zig", tool);
    compile.skip_recompile = false;
    r = run_prep(compile);
    CHECK(r.last.rc == 2 && r.last.error.find("toolchain (zig)") != std::string::npos);
    const fs::path fz = work / "fakezig";
    write_file(fz / "zig", Bytes(2 << 20, 0x7f));
    write_text(fz / "lib" / "std" / "std.zig", "pub const x = 1;\n");
    write_text(fz / "sizes.txt", "# zig, trimmed\n2097152\tzig\n999\tlib/std/std.zig\n5\tlib/libc/gone.h\n10\tREADME.md\n");
    const std::string why = gl::check_toolchain(s(fz / "zig"), "");
    std::printf("  %s\n", why.c_str());
    CHECK(why.find("2 of its 3 files missing or changed") != std::string::npos &&
          why.find("lib/std/std.zig changed") != std::string::npos && why.find("lib/libc/gone.h missing") != std::string::npos);
    write_text(fz / "sizes.txt", "2097152\tzig\n17\tlib/std/std.zig\n");
    CHECK(gl::check_toolchain(s(fz / "zig"), "").empty());
    CHECK(gl::check_toolchain(s(fz / "zig"), s(work / "nolib")).find("lib folder is missing") != std::string::npos);
    write_text(fz / "sizes.txt", "2097152\tzig\n18\tlib/std/std.zig\n");
    compile.zig = s(fz / "zig");
    r = run_prep(compile);
    CHECK(r.last.rc == 2 && r.last.error.find("is damaged") != std::string::npos);

    std::puts("acnh_prepare: --exefs, --romfs, --dlc DIR:index, --out");
    set_env("NX_PREPARE_SKIP_RECOMP", "1");
    const fs::path out2 = work / "out2";
    int rc = sh(q(cli) + " --exefs " + q(in / "exefs") + " --romfs " + q(in / "romfs") + " --dlc " + q(s(hhp) + ":2") +
                " --out " + q(out2) + " --platforms this --shader-tool " + q(tool));
    CHECK(rc == 0);
    check_folder(in, out2, false, 2, hhp);
    CHECK(!fs::exists(out2 / "aoc" / "1"));

    std::puts("acnh_prepare GAME --in-place: the RomFS stays loose where it is");
    const fs::path in2 = work / "in2" / "ACNH";
    make_inputs(in2);
    write_file(in2 / "aoc" / "1" / "Layout" / "a.bin", pattern(3000, 9));
    rc = sh(q(cli) + " " + q(in2) + " --in-place --platforms this --shader-tool " + q(tool));
    CHECK(rc == 0);
    check_folder(in2, in2, true, 1, in2 / "aoc" / "1");
    CHECK(fs::exists(in2 / "Animal Crossing [01006F8002326000][v3.0.3].nsp"));  // left where it was

    std::puts("folders named with accents (UTF-8 paths, Windows too): the API, then acnh_prepare GAME --in-place");
    const fs::path acc = work / kAccents, in3 = acc / "ACNH", hhp3 = acc / fs::path(u8"Cora\u00e7\u00e3o");
    make_inputs(in3);
    write_file(hhp3 / "Layout" / "a.bin", pattern(3000, 9));
    write_file(hhp3 / "Model" / "b.bin", pattern(500, 10));
    prep::Options oa = base_options(in3, acc / "jogo", tool);
    oa.dlc = {s(hhp3)};
    oa.dlc_index = {1};
    r = run_prep(oa);
    CHECK(r.last.rc == 0 && r.last.out == s(fs::weakly_canonical(acc / "jogo")));
    check_folder(in3, acc / "jogo", false, 1, hhp3);
    rc = sh(q(cli) + " " + q(in3) + " --in-place --platforms this --shader-tool " + q(tool));
    CHECK(rc == 0);
    check_folder(in3, in3, true, 0, {});

    std::puts("acnh_prepare: refused and wrong command lines");
    write_text(work / "game.nsp", "PFS0");
    CHECK(sh(q(cli) + " --exefs " + q(work / "game.nsp") + " --romfs " + q(in / "romfs") + " --out " + q(work / "out3") +
             " --shader-tool " + q(tool)) == 2);
    CHECK(sh(q(cli) + " " + q(in) + " --out " + q(work / "out3") + " --platforms dreamcast") == 64);
    CHECK(sh(q(cli) + " " + q(in) + " --out " + q(work / "out3") + " --frobnicate") == 64);
    CHECK(sh(q(cli) + " " + q(in)) == 64);  // no --out, not in place
    CHECK(sh(q(cli) + " " + q(in) + " --dlc " + q(s(hhp) + ":0") + " --out " + q(work / "out3")) == 64);
    set_env("NX_PREPARE_SKIP_RECOMP", nullptr);

    std::vector<std::string> p;
    std::string bad;
    CHECK(prep::parse_platforms("linux-x64, android-arm64,linux-x64", p, bad) && p.size() == 2 && p[1] == "android-arm64");
    CHECK(prep::parse_platforms("all", p, bad) && p.size() == 4);
    CHECK(!prep::parse_platforms("linux-x64,ps5", p, bad) && bad == "ps5");
    return 0;
}

static int zig(const fs::path& src, const fs::path& work, const fs::path& tool, const fs::path& zig_dir,
               const std::string& python, const std::string& host) {
#ifdef _WIN32
    const fs::path zig_exe = zig_dir / "zig.exe";
#else
    const fs::path zig_exe = zig_dir / "zig";
#endif
    if (!fs::exists(zig_exe)) {
        if (python.empty() || host.empty()) {
            std::puts("SKIP: no zig, and no Python or zig host to fetch it with");
            return 77;
        }
        std::printf("fetching zig into %s (tools/fetch_zig.py)\n", s(zig_dir).c_str());
        std::fflush(stdout);
        if (sh("\"" + python + "\" -I " + q(src / "tools" / "fetch_zig.py") + " " + host + " " + q(zig_dir)) != 0 ||
            !fs::exists(zig_exe)) {
            std::puts("SKIP: zig could not be downloaded (no network?): the compile step is not tested");
            return 77;
        }
    }
    const std::string target = gl::host_target().empty() ? "linux-x64" : gl::host_target();
    const fs::path in = work / "in" / "ACNH", out = work / "out_zig";
    make_inputs(in);
    // the stand-in for the recompiled code, in the recompiler's layout (and a game.ini of its own, replaced)
    if (sh("\"" + python + "\" -I " + q(src / "tools" / "make_fake_gen.py") + " " + q(out) + " 24") != 0) {
        std::puts("FAIL: tools/make_fake_gen.py");
        return 1;
    }
    // a library of another platform built from another main: removed, its player would crash
    write_text(out / "game" / "windows-x64" / "acnh_game.dll", "MZ");
    write_text(out / "game" / "windows-x64" / "build.txt", "00000000000000000000000000000000\n");

    std::printf("prepare with zig for %s\n", target.c_str());
    prep::Options o = base_options(in, out, tool);
    o.platforms = {target};
    o.zig = s(zig_exe);
    o.cache = s(work / "zig-cache");  // kept between runs of the test
    Run r = run_prep(o);
    std::fputs(r.log.c_str(), stdout);
    CHECK(r.last.rc == 0 && r.stages.count(prep::Stage::Compile) && !r.stages.count(prep::Stage::Recompile));
    CHECK(r.last.platform == 1 && r.last.platforms == 1);
    const gl::Target* t = gl::find_target(target);
    const fs::path dir = out / "game" / t->name, lib = dir / t->lib_name;
    CHECK(fs::is_regular_file(lib) && fs::file_size(lib) > 1000);
    CHECK(first_line(dir / "abi.txt") == NX_GAME_ABI && std::string(gl::abi()) == NX_GAME_ABI);
    CHECK(first_line(dir / "build.txt") == kId303 && first_line(dir / "rev.txt") == "4");
    CHECK(gl::library_current(s(out), target, kId303) && !gl::library_current(s(out), target, kId207));
    CHECK(!fs::exists(out / "gen") && !fs::exists(out / ".import" / ("build-" + target)));
    CHECK(!fs::exists(out / "game" / "windows-x64"));
    CHECK(r.log.find("windows-x64: its library was built from another version") != std::string::npos);
    check_folder(in, out, false, 0, {});
#ifndef _WIN32
    if (target == gl::host_target()) {  // loaded as the player loads it (game_folder.cpp)
        void* h = dlopen(s(lib).c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) std::printf("  dlopen: %s\n", dlerror());
        CHECK(h != nullptr);
        if (h) {
            auto abi = reinterpret_cast<const char* (*)()>(dlsym(h, "nx_game_abi"));
            CHECK(abi && std::strcmp(abi(), NX_GAME_ABI) == 0);
            CHECK(dlsym(h, "nx_game_longjmp") != nullptr);
            for (const char* m : {"main", "sdk", "subsdk0"}) {
                auto mi = static_cast<const nx::ModuleInfo*>(dlsym(h, (std::string("nx_module_") + m).c_str()));
                CHECK(mi && std::strcmp(mi->name, m) == 0 && mi->func_count > 0);
            }
        }
    }
#endif

    std::puts("prepare again: the library is kept");
    const auto lib_time = fs::last_write_time(lib);
    r = run_prep(o);
    CHECK(r.last.rc == 0 && !r.stages.count(prep::Stage::Compile));
    CHECK(r.log.find("its library built from this main for this ACNHRecomp: kept") != std::string::npos);
    CHECK(fs::last_write_time(lib) == lib_time);

    std::puts("cancel while compiling: zig stops with every compiler it started; prepared again, it ends (in a folder "
              "named with accents)");
    const fs::path outc = work / kAccents / "out_cancel";
    if (sh("\"" + python + "\" -I " + q(src / "tools" / "make_fake_gen.py") + " " + q(outc) + " 24") != 0) {
        std::puts("FAIL: tools/make_fake_gen.py");
        return 1;
    }
    // and files that take seconds each (compiled first, the biggest): a compiler left running would outlive the cancel
    for (int k = 0; k < 8; k++) {
        std::string code = "#include \"funcs.h\"\n\n";
        for (int n = 0; n < 1000; n++) {
            const std::string i = std::to_string(n % 8), j = std::to_string((n + 1) % 8), id = std::to_string(k * 1000 + n);
            code += "void heavy_" + id + "(nx::CPU& NX_RESTRICT c) {\n    uint64_t x = c.x[" + i + "], y = c.x[" + j +
                    "];\n    for (int i = 0; i < 48; i++) {\n        x = x * 0x9E3779B97F4A7C15ull + (y >> (i & 7));\n" +
                    "        y ^= x + uint64_t(i) * " + id + "u;\n    }\n    c.x[0] = x, c.x[1] = y;\n}\n";
        }
        write_text(outc / "gen" / "main" / ("code_9" + std::to_string(k) + "0.cpp"), code);
    }
    prep::Options oc = base_options(in, outc, tool);
    oc.platforms = {target};
    oc.zig = s(zig_exe);  // the folder's own zig cache: nothing of it compiled before
    CHECK(prep::start(oc));
    bool cancelled = false;
    size_t compilers = 0;
    const auto t0 = std::chrono::steady_clock::now();
    prep::Status st;
    for (;;) {
        st = prep::status();
        if (st.finished) break;
        if (!cancelled && st.stage == prep::Stage::Compile) {
#ifdef __linux__
            procs_with(s(outc), &compilers);
            if (compilers >= 2) prep::cancel(), cancelled = true;  // objects being compiled, side by side
#else
            if (st.fraction > 0.05) prep::cancel(), cancelled = true;
#endif
        }
        if (!cancelled && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(300)) prep::cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    prep::wait();
    CHECK(cancelled && st.rc == 4);
    CHECK(!fs::exists(outc / ".import" / "complete"));
#ifdef __linux__
    std::printf("  cancelled with %zu compilers running\n", compilers);
    size_t left = procs_with(s(outc));
    for (int k = 0; k < 30 && left; k++) {  // the processes killed: gone within a few ms, the compiles take seconds
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        left = procs_with(s(outc));
    }
    if (left) std::printf("  %zu processes of the cancelled compile still running\n", left);
    CHECK(left == 0);
#endif
    r = run_prep(oc);
    CHECK(r.last.rc == 0 && r.stages.count(prep::Stage::Compile));
    CHECK(gl::library_current(s(outc), target, kId303));
    check_folder(in, outc, false, 0, {});

    std::puts("recompiling synthetic modules fails (no MOD0), tried again without the 60 FPS variant first");
    // in a folder named with accents: the recompiler opens exefs/main from its UTF-8 path (not "cannot open")
    const fs::path out_recomp = work / kAccents / "out_recomp";
    prep::Options rec = o;
    rec.out = s(out_recomp);
    rec.skip_recompile = false;
    r = run_prep(rec);
    CHECK(r.last.rc == 1 && r.last.error.find("recompile: ") != std::string::npos);
    CHECK(r.last.error.find("cannot open") == std::string::npos);
    CHECK(r.log.find("the 60 FPS variant could not be recompiled") != std::string::npos);
    CHECK(r.stages.count(prep::Stage::Recompile) && !r.stages.count(prep::Stage::Compile));
    CHECK(!fs::exists(out_recomp / ".import" / "complete") && !fs::exists(out_recomp / "game.ini"));

    std::puts("a library of another runtime is built again");
    { std::ofstream(dir / "abi.txt") << "000000000000\n"; }
    CHECK(!gl::library_current(s(out), target, kId303));
    { std::ofstream(dir / "abi.txt") << NX_GAME_ABI << "\n"; }  // as built, for prepare_launcher_test
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return std::fputs("usage: see prepare_e2e_test.cpp\n", stderr), 2;
    const std::string mode = argv[1];
    int rc = 2;
    if (mode == "base" && argc == 6) {
        const fs::path work = argv[3];
        fs::remove_all(work);
        fs::create_directories(work);
        rc = base(argv[2], fs::absolute(work), fs::absolute(argv[4]), fs::absolute(argv[5]));
    } else if (mode == "zig" && argc == 8) {
        const fs::path work = argv[3];
        std::error_code ec;
        for (const fs::path d : {fs::path("in"), fs::path("out_zig"), kAccents}) fs::remove_all(work / d, ec);
        fs::create_directories(work);
        rc = zig(argv[2], fs::absolute(work), fs::absolute(argv[4]), fs::absolute(argv[5]), argv[6], argv[7]);
    } else {
        std::fputs("usage: see prepare_e2e_test.cpp\n", stderr);
        return 2;
    }
    if (rc == 77) return 77;
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed || rc ? 1 : 0;
}
