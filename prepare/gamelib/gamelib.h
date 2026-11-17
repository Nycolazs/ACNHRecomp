// The game's library (lib<game>_game) built from the recompiled code (<game folder>/gen/), with the zig the
// preparation carries (clang and lld for every target, from any host). The player of each platform loads it from the
// game folder:
//   game/windows-x64/acnh_game.dll   game/linux-x64/libacnh_game.so   game/macos-arm64/libacnh_game.dylib
//   game/android-arm64/libacnh_game.so
// It holds no C++ runtime of its own: the few C++ runtime calls (the longjmp exception) and the C library come from
// the player's process, so there is one unwinder and one C++ runtime in it. It is compiled against ACNHRecomp's own
// runtime headers (runtime/include/nx/*.h, runtime/src/game_abi.cpp), built in, and carries the ABI the player checks
// (NX_GAME_ABI, from the top CMakeLists.txt): a library and a player of the same checkout always agree.
//
// Taken from ACNHRecomp-Importer v1.0.7 (src/gamelib/, MIT, same author). The Importer's repair of its package's
// toolchain is left out: check_toolchain() compares the zig folder with its sizes.txt (tools/fetch_zig.py) instead.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gl {

struct Target {
    const char* name;      // "linux-x64": the folder under game/
    const char* triple;    // zig's -target
    const char* lib_name;  // the file the player loads
    std::vector<std::string> cflags;  // the generated code's flags (ACNHRecomp's CMakeLists.txt, game_<module>)
    std::vector<std::string> ldflags;
    std::vector<std::string> needed;  // libraries the player's process provides, as DT_NEEDED (Android)
};
const std::vector<Target>& targets();
const Target* find_target(const std::string& name);
// The target of the device this runs on ("" when it has none: Windows on ARM, an Intel Mac, ...)
std::string host_target();

struct Options {
    std::string zig;          // the zig executable
    std::string zig_lib;      // its lib folder when not beside it ("" otherwise; Android: unpacked from the APK)
    std::string cache;        // a writable folder for zig's caches
    std::string gen;          // <game folder>/gen
    std::string out;          // the game folder: the library goes to <out>/game/<target>/
    std::string work;         // where the objects go (deleted when done)
    std::string target;       // a Target name
    std::string opt = "-O2";  // the optimisation level: -O2, fast, -O0 (gamelib.cpp: opt_flags)
    // With a hot list for this game (prepare/gamelib/hot/<build id>.txt, by main's build id) only its functions are
    // built at opt, the rest at "fast" (about a third quicker to compile; the game spends almost no time there).
    std::string build_id;
    bool split = true;
    unsigned jobs = 0;        // 0: the cores, as many as the memory holds
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(uint64_t done, uint64_t total, const std::string& item)> progress;
    std::function<void(const std::string&)> log;
};

// The ABI of the runtime headers built in (the first 12 hex digits of the SHA-1 of cpu.h, module.h, runtime.h, as
// the top CMakeLists.txt computes it): the player refuses a library of another ABI.
const char* abi();

// The start of the error build() gives when the toolchain's own files are at fault (toolchain_broken): the preparation
// then does not try main again without the 60 FPS variant (it would fail the same way, after a second full cycle).
extern const char* const kToolchainFault;

// Runs argv (argv[0] the program) with its output into log and env_extra ("NAME=value") added: the exit code, -1
// when it could not start. posix_spawn (Android 9 and up, desktop Unix) or CreateProcessW. With `cancel`, the child is
// stopped once it is set, with every process it started (its process group; on Windows its Job Object): the exit code
// is then not 0.
int run(const std::vector<std::string>& argv, const std::string& log, const std::vector<std::string>& env_extra = {},
        const std::atomic<bool>* cancel = nullptr);

// 0 when the library is in place; else the reason is in error (cancelled: 4).
int build(const Options& o, std::string& error);

// zig's lib folder: zig_lib, or lib beside the zig executable ("/" separators, no trailing one).
std::string toolchain_lib_dir(const std::string& zig, const std::string& zig_lib);
// Whether a step's log shows the toolchain's own files at fault: one of its headers or sources not found, or an
// error reported in one (an antivirus emptying or removing a libc header: "use of undeclared identifier 'FP_NAN'").
bool toolchain_broken(const std::string& log, const std::string& lib_dir);

// The zig install against its sizes.txt (written by tools/fetch_zig.py beside lib/, or inside it): every file listed
// there with that size. Without sizes.txt, the executable and a few files every target needs. "" when it is whole,
// else what is wrong (the first files missing or changed, and how many).
std::string check_toolchain(const std::string& zig, const std::string& zig_lib);

// Whether <out>/game/<target>/ holds a library built right for its platform (its revision in rev.txt) against this
// runtime (abi.txt) from the main of this build id (build.txt; "" leaves the build unchecked): false when it must be
// built (again).
bool library_current(const std::string& out, const std::string& target, const std::string& build_id);
// The build id of the main a library was built from: "" when there is no library or it does not say (before the
// Importer's 1.0.7).
std::string library_build_id(const std::string& out, const std::string& target);

}  // namespace gl
