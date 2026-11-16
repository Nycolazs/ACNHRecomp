// prepare_job.h: the probe and the preparation for the launcher's pages, the tools' paths and the log.
#include "prepare_job.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "launcher.h"

#if defined(NX_LAUNCHER_PREPARE)
#include "gamelib.h"
#include "prepare.h"
#endif

namespace launcher {

namespace fs = std::filesystem;

namespace {

fs::path u8p(const std::string& p) { return fs::path(reinterpret_cast<const char8_t*>(p.c_str())); }
std::string s8(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

// The first of `rels` (relative to the launcher's folder) that is a file, else "".
std::string beside(std::initializer_list<const char*> rels) {
    const std::string dir = own_dir();
    if (dir.empty()) return "";
    std::error_code ec;
    for (const char* r : rels) {
        const fs::path p = u8p(dir) / u8p(r);
        if (fs::is_regular_file(p, ec)) return s8(fs::weakly_canonical(p, ec));
    }
    return "";
}

}  // namespace

#if defined(NX_LAUNCHER_PREPARE)
bool prepare_built() { return true; }
#else
bool prepare_built() { return false; }
#endif

const PrepareTools& prepare_tools(bool recheck) {
    static PrepareTools t;
    static bool checked = false;
    if (checked && !recheck) return t;
    checked = true;
    t = PrepareTools();
#if defined(_WIN32)
    t.zig = beside({"tools/zig/zig.exe", "zig/zig.exe"});
    t.shader_tool = beside({"tools/shaders/spirv_harness.exe", "shaders/spirv_harness.exe"});
#else
    t.zig = beside({"tools/zig/zig", "zig/zig", "../Resources/tools/zig/zig"});
    t.shader_tool = beside({"tools/shaders/spirv_harness", "shaders/spirv_harness", "../Resources/tools/shaders/spirv_harness"});
#endif
    if (const char* z = std::getenv("NX_LAUNCHER_ZIG")) t.zig = z;  // tests
    if (const char* s = std::getenv("NX_LAUNCHER_SHADER_TOOL")) t.shader_tool = s;
#if defined(NX_LAUNCHER_PREPARE)
    if (t.zig.empty())
        t.problem = "tools/zig (the compiler of the game's code) is not beside the launcher";
    else if (std::string why = gl::check_toolchain(t.zig, ""); !why.empty())
        t.problem = why;
    else if (std::error_code ec; t.shader_tool.empty() || !fs::is_regular_file(u8p(t.shader_tool), ec))
        t.problem = "tools/shaders (the shader converter) is not beside the launcher";
#else
    t.problem = "this launcher was built without the preparation";
#endif
    return t;
}

std::vector<std::string> prepare_platforms() {
    std::vector<std::string> out;
#if defined(NX_LAUNCHER_PREPARE)
    for (const gl::Target& t : gl::targets()) out.push_back(t.name);
#endif
    return out;
}

std::string prepare_host_platform() {
#if defined(NX_LAUNCHER_PREPARE)
    return gl::host_target();
#else
    return "";
#endif
}

PrepareSpace prepare_space(const nx::prepare::ProbeResult& r, size_t platforms, bool in_place) {
#if defined(NX_LAUNCHER_PREPARE)
    const prep::Space s = prep::space_needed(r, platforms, in_place);
    return {s.need, s.result};
#else
    (void)r, (void)platforms, (void)in_place;
    return {};
#endif
}

bool prepare_library_stale(const std::string& folder) {
#if defined(NX_LAUNCHER_PREPARE)
    const std::string host = gl::host_target();
    if (host.empty()) return false;
    std::ifstream f(u8p(folder) / "game" / host / "abi.txt");
    std::string abi;
    return (f >> abi) && abi != gl::abi();
#else
    (void)folder;
    return false;
#endif
}

// ---- the probe ------------------------------------------------------------------------------------------------

namespace {

struct ProbeJob {
    std::mutex m;
    std::thread th;
    bool thread_busy = false;
    bool pending = false;  // a request the thread has not taken yet
    std::vector<std::string> inputs, dlc;
    int requested = 0, answered = 0;
    nx::prepare::ProbeResult result;
};
ProbeJob& probe_job() {
    static ProbeJob j;
    return j;
}

void probe_loop() {
    ProbeJob& j = probe_job();
    for (;;) {
        std::vector<std::string> inputs, dlc;
        int serial = 0;
        {
            std::lock_guard lk(j.m);
            if (!j.pending) {
                j.thread_busy = false;
                return;
            }
            inputs = j.inputs, dlc = j.dlc, serial = j.requested;
            j.pending = false;
        }
        nx::prepare::ProbeOptions po;
        po.dlc = dlc;
        nx::prepare::ProbeResult r = nx::prepare::probe(inputs, po);
        std::lock_guard lk(j.m);
        if (serial == j.requested) j.result = std::move(r), j.answered = serial;
    }
}

}  // namespace

void probe_start(const std::vector<std::string>& inputs, const std::vector<std::string>& dlc) {
    ProbeJob& j = probe_job();
    std::lock_guard lk(j.m);
    j.inputs = inputs, j.dlc = dlc;
    j.requested++;
    j.pending = true;
    if (!j.thread_busy) {
        if (j.th.joinable()) j.th.join();  // ended: it cleared thread_busy as its last step
        j.thread_busy = true;
        j.th = std::thread(probe_loop);
    }
}

bool probe_busy() {
    ProbeJob& j = probe_job();
    std::lock_guard lk(j.m);
    return j.answered != j.requested;
}

bool probe_poll(int& serial, nx::prepare::ProbeResult& out) {
    ProbeJob& j = probe_job();
    std::lock_guard lk(j.m);
    if (j.answered == 0 || j.answered == serial || j.answered != j.requested) return false;
    serial = j.answered;
    out = j.result;
    return true;
}

// ---- the preparation ------------------------------------------------------------------------------------------

namespace {

std::mutex g_lock;
PrepareState g_state;
std::vector<std::string> g_lines;
std::chrono::steady_clock::time_point g_t0;
std::ofstream g_log;
bool g_started = false;

#if defined(NX_LAUNCHER_PREPARE)
static_assert(int(prep::Stage::Check) == kPrepCheck && int(prep::Stage::Finish) == kPrepFinish &&
              int(prep::Stage::Done) == kPrepDone);

void add_line(const std::string& l) {
    g_lines.push_back(l);
    if (g_log) g_log << l << "\n";
}
#endif

}  // namespace

std::string prepare_log_path() { return parent_dir(launcher_ini_path()) + "/prepare.log"; }

bool prepare_start(const PrepareArgs& a, std::string& error) {
#if defined(NX_LAUNCHER_PREPARE)
    const PrepareTools& t = prepare_tools(true);
    prep::Options o;
    o.inputs = a.inputs;
    o.dlc = a.dlc;
    o.out = a.out;
    o.platforms = a.platforms;
    o.variant_60fps = a.variant_60fps;
    o.in_place = a.in_place;
    o.zig = t.zig;
    o.shader_tool = t.shader_tool;
    std::lock_guard lk(g_lock);
    if (g_state.running) return error = "already running", false;
    if (!prep::start(o)) return error = "already running", false;
    g_state = PrepareState();
    g_state.running = true;
    g_lines.clear();
    g_t0 = std::chrono::steady_clock::now();
    g_started = true;
    if (g_log.is_open()) g_log.close();
    std::error_code ec;
    fs::create_directories(u8p(parent_dir(prepare_log_path())), ec);
    g_log.open(u8p(prepare_log_path()), std::ios::trunc);
    std::string what;
    for (const std::string& i : a.inputs) what += " \"" + i + "\"";
    for (const std::string& d : a.dlc) what += " --dlc \"" + d + "\"";
    add_line("ACNHRecomp " NX_VERSION_STRING ": preparing" + what +
             (a.in_place ? std::string(" --in-place") : " --out \"" + a.out + "\"") +
             (a.variant_60fps ? "" : " --no-60fps"));
    return true;
#else
    (void)a;
    error = "this launcher was built without the preparation";
    return false;
#endif
}

void prepare_cancel() {
#if defined(NX_LAUNCHER_PREPARE)
    prep::cancel();
#endif
}

void prepare_shutdown() {
#if defined(NX_LAUNCHER_PREPARE)
    if (prepare_running()) prep::cancel();
    prep::wait();
#endif
    ProbeJob& j = probe_job();
    std::thread t;
    {
        std::lock_guard lk(j.m);
        j.pending = false;
        t = std::move(j.th);
    }
    if (t.joinable()) t.join();
}

PrepareState prepare_state() {
    std::lock_guard lk(g_lock);
#if defined(NX_LAUNCHER_PREPARE)
    if (!g_started) return g_state;
    if (g_state.running) {
        prep::Status s = prep::status();
        for (const prep::LogLine& l : s.log) {
            if (l.text.empty())
                add_line(std::string("[") + prep::stage_name(l.stage) + "]");
            else
                add_line("  " + l.text);
        }
        PrepareState& g = g_state;
        g.running = s.running, g.finished = s.finished, g.rc = s.rc;
        if (s.stage != prep::Stage::Idle && s.stage != prep::Stage::Done) g.stage = int(s.stage);
        for (int k = kPrepCheck; k < g.stage; k++) g.done[k] = true;
        if (s.finished && s.rc == 0)
            for (int k = kPrepCheck; k <= kPrepDone; k++) g.done[k] = true;
        g.fraction = s.fraction, g.overall = s.overall;
        g.item = s.item;
        g.platform = s.platform, g.platforms = s.platforms;
        g.error = s.error, g.out = s.out, g.version = s.version;
        if (!s.messages.empty()) g.messages = s.messages;
        g.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
        if (s.finished) {
            prep::wait();
            add_line(s.rc == 0 ? "ready: " + s.out : s.rc == 4 ? std::string("cancelled") : "stopped: " + s.error);
            g_log.close();
        }
    }
#endif
    return g_state;
}

bool prepare_running() {
    std::lock_guard lk(g_lock);
    return g_state.running;
}

void prepare_lines(size_t from, std::vector<std::string>& out) {
    std::lock_guard lk(g_lock);
    for (size_t i = from; i < g_lines.size(); i++) out.push_back(g_lines[i]);
}

}  // namespace launcher
