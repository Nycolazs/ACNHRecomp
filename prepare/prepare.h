// The game folder prepared by ACNHRecomp itself, from the ExeFS and RomFS the player extracted from their own copy of
// Animal Crossing: New Horizons 3.0.3 (no keys, no NSP or XCI, nothing decrypted): one job on a worker thread, which
// the launcher (or acnh_prepare, prepare_cli.cpp) starts, polls and may cancel.
//
//   Check      the inputs (io/inputs.h probe()), the game folder, the free space and the tools
//   ExeFS      exefs/main, sdk, subsdk0, main.npdm copied; analysis/<module>.bin, the flat images the loader maps
//   Recompile  gen/<module>/: the modules to C++ (recomp/), main with the native 60 FPS variant on 3.0.2 / 3.0.3
//   Shaders    spirv/: the game's shaders to SPIR-V (the shader tool: tools/maxwell/spirv_harness, built native)
//   Packs      romfs.pak from the player's RomFS (unless in place), aoc/<index>/ for add-ons, spirv.pak
//   Compile    game/<platform>/: the game's library for each platform chosen (gamelib/, with zig); gen/ goes after
//   Finish     game.ini (its first line names ACNHRecomp) and .import/complete: launcher/files.cpp's prepared_folder()
//
// The folder is the one ACNHRecomp-Importer wrote, played the same way (README "The game folder", prepare/README.md).
// Run again on the same folder, what is already right is kept: the libraries built from this main for this runtime,
// romfs.pak, the shaders. Plain C++ and the OS's process calls: it builds for Android as for the desktop (the tools
// are then the APK's libzig.so and libspirv_harness.so, run from the app's native library folder).
//
// Taken from ACNHRecomp-Importer v1.0.7 (src/nxcore_api.cpp, MIT, same author): its stages after the extraction.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "inputs.h"

namespace prep {

enum class Stage { Idle, Check, ExeFS, Recompile, Shaders, Packs, Compile, Finish, Done };
const char* stage_name(Stage s);  // "check", "exefs", "recompile", ...

struct Options {
    // What the player picked: one folder (holding exefs/ and romfs/, an emulator's dump folder) or two (the ExeFS and
    // the RomFS), as nx::prepare::probe() takes them.
    std::vector<std::string> inputs;
    std::vector<std::string> dlc;  // add-on content folders (one RomFS each)
    std::vector<int> dlc_index;    // the index chosen for dlc[i]; -1 or none: from its path, else 1 (said)
    // The game folder: empty, or a game folder prepared before. "" with in_place: the folder holding the RomFS.
    std::string out;
    std::vector<std::string> platforms;  // gl::targets() names; empty: this device's (gl::host_target())
    bool variant_60fps = true;           // main with tmouh's SpeedFix as a second body (3.0.2, 3.0.3)
    // The player's RomFS stays loose where it is, as the game folder's romfs/ (or extract/romfs/): no romfs.pak, no
    // copy. Taken on its own when the RomFS already is there.
    bool in_place = false;
    std::string zig, zig_lib;  // the toolchain (zig_lib: its lib folder when not beside it, Android)
    std::string shader_tool;   // spirv_harness, NativeAOT (tools/build_shader_tool.py)
    std::string cache;         // zig's caches; "": <out>/.import/zig-cache, removed as each library is built
    unsigned jobs = 0;         // compile jobs; 0: the cores, as many as the memory holds
    // Tests (or NX_PREPARE_SKIP_RECOMP=1): no recompile; the Compile stage builds <out>/gen as it is when there is one
    // (a stand-in, tools/make_fake_gen.py), else nothing (the folder then has no library). Synthetic modules are no
    // real program.
    bool skip_recompile = false;
    // The check's other settings (verify_nso, walk_romfs, expect_files / expect_bytes, versions_text); its dlc and
    // dlc_index are the ones above.
    nx::prepare::ProbeOptions probe;
};

// A line of the preparation's log, with the stage it was written in. One with no text marks where its stage began
// (every stage gets one, also those too short to be seen polling).
struct LogLine {
    Stage stage = Stage::Idle;
    std::string text;
};

struct Status {
    Stage stage = Stage::Idle;
    double fraction = 0;  // of the current stage, 0 to 1
    double overall = 0;   // of the whole preparation, 0 to 1
    std::string item;     // what is being done: "main", "romfs.pak", "main/code_003.cpp", the shader tool's last line
    int platform = 0, platforms = 0;  // Compile: the platform being built (1-based) of how many
    std::vector<LogLine> log;  // the lines written since the last status() (at most the last 2000)
    std::vector<nx::prepare::Message> messages;  // the check's findings, once it ran
    bool running = false, finished = false;
    // finished: 0 done; 1 a stage failed; 2 refused (the inputs, the game folder, the space, the tools); 4 cancelled
    int rc = 0;
    std::string error;    // English, for the log and the player
    std::string out;      // the game folder
    std::string build_id, version;  // main's, once checked
};

// Starts the preparation on its own thread: false when one is running already.
bool start(const Options& o);
// The job's state; takes the log lines written since the last call.
Status status();
// Asks the running job to stop: it ends with rc 4 soon after (a child process is stopped), keeping what is complete.
void cancel();
// Waits for the running job to end (at once when there is none).
void wait();

// The platforms of a list ("this", "all", or "linux-x64,android-arm64"): false (and the bad name in `bad`) when one
// is no platform.
bool parse_platforms(const std::string& list, std::vector<std::string>& out, std::string& bad);

// The free space the preparation needs on the game folder's drive, as the Check stage counts it (it refuses below
// `need`), and about what the folder holds once it is done (`result`): from the probe of the picks, for `platforms`
// platforms; `shaders` and `compile` false when the folder has them already. The launcher shows it before starting.
struct Space {
    uint64_t need = 0, result = 0;
};
Space space_needed(const nx::prepare::ProbeResult& r, size_t platforms, bool in_place, bool shaders = true,
                   bool compile = true);

}  // namespace prep
