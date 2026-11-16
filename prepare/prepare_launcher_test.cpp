// The game folders prepare_e2e_test wrote, checked by the launcher's own code (launcher/files.cpp) as a player's
// release checks them: the folder's parts and executables (check_file), that it was prepared and finished
// (prepared_folder: game.ini's first line names ACNHRecomp, .import/complete), the version and add-ons it holds
// (build_contents), and, with the player beside this test in the build folder, the game's library for this computer
// (find_build). A folder from ACNHRecomp-Importer passes the same way; one prepared by neither does not, and is told
// as one to prepare (FileCheck::unprepared). The synthetic extraction it was prepared from is found and checked as
// what to prepare from (Kind::Extract).
//   prepare_launcher_test <folder without a library> [<folder with this computer's library> [<extraction>]]
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "launcher.h"

namespace fs = std::filesystem;
using namespace launcher;

static int g_failed = 0, g_checks = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        g_checks++;                                                  \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            g_failed++;                                              \
        }                                                            \
    } while (0)

static void copy_tree(const fs::path& from, const fs::path& to) {
    fs::remove_all(to);
    fs::create_directories(to);
    fs::copy(from, to, fs::copy_options::recursive);
}

static void set_first_line(const fs::path& ini, const std::string& first) {
    std::ifstream in(ini);
    std::string text, line;
    std::getline(in, line);
    text = first + "\n";
    while (std::getline(in, line)) text += line + "\n";
    in.close();
    std::ofstream(ini) << text;
}

int main(int argc, char** argv) {
    if (argc < 2) return std::fputs("usage: prepare_launcher_test <folder> [<folder with library>]\n", stderr), 2;
    const std::string folder = argv[1];
    const std::string player = player_beside();
    std::printf("player beside: %s\n", player.empty() ? "none" : player.c_str());

    FileCheck c = check_file(Kind::Game, folder);
    std::printf("check_file: %d %s\n", int(c.status), c.text.c_str());
    CHECK(c.status == Status::Ok && c.text.find("3.0.3") != std::string::npos);
    Kind k = Kind::Mod;
    CHECK(classify_file(folder, k) && k == Kind::Game);
    BuildContents bc = build_contents(folder);
    CHECK(bc.version == "3.0.3" && bc.addons.size() == 1 && bc.addons[0].index == 1 && bc.addons[0].files == 2);
    Build b;
    const char* why = find_build(folder, b);
    if (!player.empty()) CHECK(why && std::string(why) == "launcher.reason.no_game_lib");  // no library for here

    if (argc > 2 && !fs::exists(fs::path(argv[2]) / ".import" / "complete")) {
        std::printf("no folder with a library at %s (prepare_e2e_zig skipped): find_build not checked on one\n", argv[2]);
    } else if (argc > 2) {
        const std::string with_lib = argv[2];
        CHECK(check_file(Kind::Game, with_lib).status == Status::Ok);
        why = find_build(with_lib, b);
        if (!player.empty()) {
            if (why && *why) std::printf("find_build: %s\n", why);
            CHECK(why && !*why && b.exe == player && fs::equivalent(b.data, with_lib));
        }
    }

    // The same folder written by ACNHRecomp-Importer: accepted as before. By neither, or unfinished: refused.
    const fs::path copy = fs::path(folder).parent_path() / "launcher_copy";
    copy_tree(folder, copy);
    set_first_line(copy / "game.ini", "# Written by ACNHRecomp-Importer: what this game folder holds.");
    CHECK(check_file(Kind::Game, copy.string()).status == Status::Ok);
    set_first_line(copy / "game.ini", "# some other tool");
    CHECK(check_file(Kind::Game, copy.string()).status == Status::Error);
    CHECK(check_file(Kind::Game, copy.string()).unprepared);
    if (!player.empty()) CHECK(std::string(find_build(copy.string(), b)) == "launcher.err.game_unprepared");
    set_first_line(copy / "game.ini", "# ACNHRecomp game folder");
    fs::remove(copy / ".import" / "complete");
    CHECK(check_file(Kind::Game, copy.string()).status == Status::Error);
    fs::remove_all(copy);
    CHECK(!check_file(Kind::Game, folder).unprepared && is_prepared_folder(folder));

    // The extraction the folders were prepared from: what to prepare from (its package beside is only a warning), a
    // game folder not prepared yet, and found by the search near it. A prepared folder is no extraction to find.
    if (argc > 3) {
        const std::string ex = argv[3];
        FileCheck e = check_file(Kind::Extract, ex);
        std::printf("check_file extract: %d %s\n", int(e.status), e.text.c_str());
        CHECK(e.status != Status::Error && e.text.find("3.0.3") != std::string::npos);
        CHECK(check_file(Kind::Game, ex).unprepared && !is_prepared_folder(ex));
        bool found = false;
        for (const Found& f : search_files(Kind::Extract, {fs::path(ex).parent_path().string()})) {
            found |= fs::equivalent(f.path, ex);
            CHECK(!is_prepared_folder(f.path));
        }
        CHECK(found);
        // its ExeFS alone: the RomFS beside it is found too; a folder of neither (prepare_e2e's busy/) is refused
        CHECK(check_file(Kind::Extract, (fs::path(ex) / "exefs").string()).status != Status::Error);
        const fs::path busy = fs::path(ex).parent_path().parent_path() / "busy";
        if (fs::is_directory(busy)) CHECK(check_file(Kind::Extract, busy.string()).status == Status::Error);
    }
    CHECK(translate("launcher.ok.extract", {"3.0.3"}, "x").find("3.0.3") != std::string::npos);
    CHECK(translate("prepare.no.such.key", {"a"}, "english a") == "english a");

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
