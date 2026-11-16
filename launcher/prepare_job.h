// The game folder prepared inside the launcher (prepare/README.md): the probe of the player's picks and the stages of
// prepare/prepare.h on their worker threads, and the tools a release carries for them. pages.cpp draws the pages; this
// keeps the jobs, the log and the tools' paths. Built with the preparation (prepare/prepare.cmake defines
// NX_LAUNCHER_PREPARE), else prepare_built() is false and nothing starts.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "inputs.h"

namespace launcher {

bool prepare_built();

// The tools beside the launcher, as a release carries them: tools/zig/zig and tools/shaders/spirv_harness (.exe on
// Windows; in ACNHRecomp.app, Contents/Resources/tools/), or zig/ and shaders/ (a build folder). "" when not found.
// NX_LAUNCHER_ZIG and NX_LAUNCHER_SHADER_TOOL (tests) name others.
struct PrepareTools {
    std::string zig, shader_tool;
    std::string problem;  // English: what is wrong with them ("" when both are there and zig is whole)
};
const PrepareTools& prepare_tools(bool recheck = false);

// The platforms a game folder's code can be built for ("windows-x64", "linux-x64", "macos-arm64", "android-arm64")
// and this computer's ("" when it has none).
std::vector<std::string> prepare_platforms();
std::string prepare_host_platform();

// The free space the preparation needs and about what the game folder holds once done (prep::space_needed).
struct PrepareSpace {
    uint64_t need = 0, result = 0;
};
PrepareSpace prepare_space(const nx::prepare::ProbeResult& r, size_t platforms, bool in_place);

// Whether the game folder's library for this computer was built against another runtime than this launcher's (the
// player refuses it): it must be prepared again. False when there is none or it does not say (abi.txt).
bool prepare_library_stale(const std::string& folder);

// ---- the probe of the picks, on its own thread ----------------------------------------------------------------

// A newer request replaces one not started yet; the result of an older one is dropped.
void probe_start(const std::vector<std::string>& inputs, const std::vector<std::string>& dlc);
bool probe_busy();  // a request not answered yet
// The answer to the last request, once there: true (and `out` filled) when it is newer than `serial`.
bool probe_poll(int& serial, nx::prepare::ProbeResult& out);

// ---- the preparation ------------------------------------------------------------------------------------------

struct PrepareArgs {
    std::vector<std::string> inputs, dlc;
    std::string out;                     // the game folder ("" with in_place: the folder holding the RomFS)
    std::vector<std::string> platforms;  // empty: this computer's
    bool variant_60fps = true, in_place = false;
};
bool prepare_start(const PrepareArgs& a, std::string& error);
void prepare_cancel();
void prepare_shutdown();  // at exit: the preparation cancelled and waited for, the probe too

enum PrepareStage { kPrepIdle, kPrepCheck, kPrepExeFS, kPrepRecompile, kPrepShaders, kPrepPacks, kPrepCompile,
                    kPrepFinish, kPrepDone };
struct PrepareState {
    bool running = false, finished = false;
    int rc = 0;                 // finished: 0 done, 1 a stage failed, 2 refused, 4 cancelled
    int stage = kPrepIdle;      // the stage running (or the one that failed)
    bool done[kPrepDone + 1] = {};  // the stages passed
    double fraction = 0, overall = 0;
    double elapsed = 0;         // seconds since the start
    std::string item;           // what is being done
    int platform = 0, platforms = 0;
    std::string error, out, version;
    std::vector<nx::prepare::Message> messages;  // the check's findings
};
PrepareState prepare_state();  // polls the job: call once a frame while it shows
bool prepare_running();
// The log lines from index `from` on, appended to `out` (also written to prepare_log_path()).
void prepare_lines(size_t from, std::vector<std::string>& out);
std::string prepare_log_path();  // prepare.log beside launcher.ini

}  // namespace launcher
