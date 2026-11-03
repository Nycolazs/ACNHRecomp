// Performance hints (docs/nvn/ANDROID-PORT.md A5, DEBUG-2.md #84): on Android the threads that make a frame (the
// game's render thread, which also replays NVN, and the presenting one) join an ADPF session (APerformanceHint,
// Android 13+) with the frame period as its target, and each frame's duration is reported. The system then raises the big cores'
// clocks when a frame runs late instead of guessing from load, which on phones keeps a steady 60 fps cooler and faster
// than the default governor. The API is looked up with dlsym, so older Android versions just go without.
// The target and the duration reported follow the thermal mode (thermal.cpp). NX_ADPF=0: off. Other platforms: nothing.
#include "platform.h"

#if defined(__ANDROID__)
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

#include <cerrno>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nx/runtime.h>
#endif

namespace nx::plat {

#if defined(__ANDROID__)
namespace {

using GetManager = void* (*)();
using CreateSession = void* (*)(void*, const int32_t*, size_t, int64_t);
using ReportActual = int (*)(void*, int64_t);
using CloseSession = void (*)(void*);
using UpdateTarget = int (*)(void*, int64_t);
using PreferEfficiency = int (*)(void*, bool);  // Android 15

struct Api {
    GetManager get_manager = nullptr;
    CreateSession create = nullptr;
    ReportActual report = nullptr;
    CloseSession close = nullptr;
    UpdateTarget update = nullptr;
    PreferEfficiency efficiency = nullptr;
    bool ok = false;
};

const Api& api() {
    static const Api a = [] {
        Api x;
        const char* e = std::getenv("NX_ADPF");
        if (e && e[0] == '0') return x;
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        if (!lib) return x;
        x.get_manager = reinterpret_cast<GetManager>(dlsym(lib, "APerformanceHint_getManager"));
        x.create = reinterpret_cast<CreateSession>(dlsym(lib, "APerformanceHint_createSession"));
        x.report = reinterpret_cast<ReportActual>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
        x.close = reinterpret_cast<CloseSession>(dlsym(lib, "APerformanceHint_closeSession"));
        x.update = reinterpret_cast<UpdateTarget>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
        x.efficiency = reinterpret_cast<PreferEfficiency>(dlsym(lib, "APerformanceHint_setPreferPowerEfficiency"));
        x.ok = x.get_manager && x.create && x.report && x.close;
        return x;
    }();
    return a;
}

std::mutex g_lock;
std::vector<int32_t> g_tids;
void* g_session = nullptr;
bool g_dirty = false;
int64_t g_target = 0;
uint64_t g_target_at = 0;
int g_efficiency = -1;  // what the session was told (-1: nothing yet)
// Clock keeper (NX_CLOCK_KEEPER, off by default). On the Galaxy Tab S10+ (Dimensity 9300+, Mali) a frame is a chain
// of many guest threads that each keep a core busy for a fraction of the time, so the governor kept every cluster
// at 0.6-2.2 GHz and the game ran at 17 fps with less than two cores busy; ADPF did not raise them. Busy loops at the
// lowest priority, one on a core of each cpufreq policy, held the clusters' clocks up and the same scene ran at 30
// fps (the game's own cap). A keeper thread runs at SCHED_IDLE, so any game thread on its core preempts it, and only
// while frames are presented (it sleeps when none came for a second: the app paused or loading).
// NX_CLOCK_KEEPER=1: the first core of each policy; or a list of cores ("4,7"). NX_CLOCK_KEEPER_DUTY: percent of
// each 2 ms the loop spins (default 100); less costs less power and holds the clocks less high.
std::atomic<uint64_t> g_last_frame{0};

std::vector<int> keeper_cpus(const char* e) {
    std::vector<int> cpus;
    if (std::string(e) == "1") {
        if (DIR* d = opendir("/sys/devices/system/cpu/cpufreq")) {
            while (dirent* de = readdir(d))
                if (std::strncmp(de->d_name, "policy", 6) == 0) cpus.push_back(std::atoi(de->d_name + 6));
            closedir(d);
        }
        std::sort(cpus.begin(), cpus.end());
        return cpus;
    }
    for (const char* p = e; *p;) {
        char* end = nullptr;
        long v = std::strtol(p, &end, 10);
        if (end == p) break;
        if (v >= 0 && v < CPU_SETSIZE) cpus.push_back(int(v));
        p = *end == ',' ? end + 1 : end;
    }
    return cpus;
}

void keeper_main(int cpu, int duty) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof set, &set);
    sched_param sp{};
    sched_setscheduler(0, SCHED_IDLE, &sp);
    char name[16];
    std::snprintf(name, sizeof name, "keeper cpu%d", cpu);
    set_thread_name(name);
    const uint64_t spin = 2'000'000ull * uint64_t(duty) / 100, rest_us = uint64_t(2'000 * (100 - duty) / 100);
    for (;;) {
        uint64_t now = monotonic_ns();
        if (now - g_last_frame.load(std::memory_order_relaxed) > 1'000'000'000ull) {
            usleep(200'000);
            continue;
        }
        for (uint64_t until = now + spin; monotonic_ns() < until;) {
        }
        if (rest_us) usleep(useconds_t(rest_us));
    }
}

void clock_keeper_start() {
    const char* e = std::getenv("NX_CLOCK_KEEPER");
    if (!e || !*e || std::string(e) == "0") return;
    const char* d = std::getenv("NX_CLOCK_KEEPER_DUTY");
    int duty = d ? std::clamp(std::atoi(d), 1, 100) : 100;
    std::string list;
    for (int cpu : keeper_cpus(e)) {
        std::thread(keeper_main, cpu, duty).detach();
        list += (list.empty() ? "" : ",") + std::to_string(cpu);
    }
    log("clock keeper: cores %s, %d%% of each 2 ms (NX_CLOCK_KEEPER)", list.empty() ? "none" : list.c_str(), duty);
}

}  // namespace

// NX_GPU_TURBO=1 (an experiment, off by default): the Adreno kernel driver's power control off, so the GPU runs at its
// highest clock (thermal limits still apply). The idea is libadrenotools' adrenotools_set_turbo (BSD-2-Clause); this
// is written from the kernel's msm_kgsl UAPI: IOCTL_KGSL_SETPROPERTY (type 0x09, nr 0x32) with KGSL_PROP_PWRCTRL (0xE)
// and the value 0. The 30-fps lock lowered the GPU clock and frames then took longer (MEASUREMENTS.md, phase 4).
// The setting outlives the process (the GPU stayed at its top clock with the game closed), so every start sets it:
// off with NX_GPU_TURBO=1, back on (the driver's normal clock control) otherwise.
void gpu_turbo() {
    const char* e = std::getenv("NX_GPU_TURBO");
    const bool turbo = e && e[0] == '1';
    struct Property {
        unsigned int type;
        void* value;
        size_t sizebytes;
    };
    int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        log("gpu turbo: /dev/kgsl-3d0 not opened (errno %d)", errno);
        return;
    }
    unsigned int enable = turbo ? 0 : 1;
    Property p{0xE, &enable, sizeof enable};
    int r = ioctl(fd, _IOW(0x09, 0x32, Property), &p);
    log("gpu turbo: KGSL power control %s: %s (errno %d)", turbo ? "off (NX_GPU_TURBO=1)" : "on (normal)",
        r == 0 ? "done" : "refused", r == 0 ? 0 : errno);
    if (!turbo) close(fd);
    // The descriptor stays open: the driver ties the setting to it.
}

void perf_hint_thread() {
    if (!api().ok) return;
    thread_local bool joined = false;
    if (joined) return;
    joined = true;
    std::lock_guard lk(g_lock);
    int32_t tid = int32_t(syscall(SYS_gettid));
    if (g_tids.size() < 8 && std::find(g_tids.begin(), g_tids.end(), tid) == g_tids.end())
        g_tids.push_back(tid), g_dirty = true;
}

void perf_hint_frame(int64_t ns) {
    g_last_frame.store(monotonic_ns(), std::memory_order_relaxed);
    static std::once_flag keeper;
    std::call_once(keeper, clock_keeper_start);
    const Api& a = api();
    if (!a.ok || ns <= 0) return;
    // Performance: as before: 33.3 ms when NX_FPS_CAP holds the game at 30 fps, else 16.7, and the game thread's work.
    // Balanced and cool: the game's own period (its present interval, nvn_queue.cpp) times the mode's share. Unset,
    // NX_FPS_CAP left the target at 16.7 ms while the game ran at 30 fps: every frame looked twice too long and the
    // system kept the clocks up. The duration is the longer of the game's and the replay's work: the replay is the
    // frame's other half, on other threads of the session.
    const int mode = thermal_mode();
    static const int64_t fixed = [] {
        const char* cap = std::getenv("NX_FPS_CAP");
        return cap && std::atoi(cap) > 0 && std::atoi(cap) <= 30 ? int64_t(33'333'333) : int64_t(16'666'667);
    }();
    int64_t target = fixed;
    if (int64_t period = frame_period_ns(); mode != 0 && period > 0) target = int64_t(double(period) * thermal_adpf_share(mode));
    std::lock_guard lk(g_lock);
    if (g_dirty && !g_tids.empty()) {  // a new thread made part of the frame: a session with all of them
        if (g_session) a.close(g_session);
        g_session = a.create(a.get_manager(), g_tids.data(), g_tids.size(), target);
        g_dirty = false, g_target = target, g_efficiency = -1;
        log("adpf: %s for %zu threads, target %.1f ms", g_session ? "session made" : "no session", g_tids.size(),
            double(target) / 1e6);
    }
    if (!g_session) return;
    // A new target at most once a second: the game can switch between 30 and 60 fps often.
    if (uint64_t now = monotonic_ns(); target != g_target && a.update && now - g_target_at > 1'000'000'000ull) {
        a.update(g_session, target);
        static int logged = 0;
        if (logged++ < 20) log("adpf: target %.1f -> %.1f ms", double(g_target) / 1e6, double(target) / 1e6);
        g_target = target, g_target_at = now;
    }
    // Cool: the system may pick efficient cores and clocks over meeting the target early (Android 15).
    if (int eff = mode == 2; a.efficiency && eff != g_efficiency) a.efficiency(g_session, eff != 0), g_efficiency = eff;
    a.report(g_session, mode == 0 ? ns : std::max(ns, frame_replay_work_ns()));
}
#else
void gpu_turbo() {}
void perf_hint_thread() {}
void perf_hint_frame(int64_t) {}
#endif

}  // namespace nx::plat
