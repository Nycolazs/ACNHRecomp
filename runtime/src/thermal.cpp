// Thermal governor (Android; docs/ANDROID.md, "Thermal mode"). At the game's 30 fps a Galaxy S20 FE reached 78-84 °C
// within 2-3 minutes of daylight play: the GPU held at its top clock and the big cores at Samsung's cap whether a
// frame needed them or not. One setting, video.thermal (NX_THERMAL=performance|balanced|cool overrides it):
//  - Every phone: the ADPF session (perf_hint.cpp) gets the game's real frame period as its target, with a margin per
//    mode, and the longer of the game's and the replay's work per frame; cool also asks for power efficiency.
//  - Rooted (video.root_clocks, NX_ROOT_CLOCKS=1): every second the GPU's busy share and clock, the CPU temperature and
//    the frames since are looked at; every 2 s the GPU clock cap and the big cores' cap go to the lowest steps that
//    still hold the frame period with the mode's margin. A step down needs 6 s of evidence, a step up happens at once
//    (to the step the GPU's load needs, not one at a time). Hot (AThermal headroom or the CPU temperature over the
//    mode's limit) lowers the ceilings by a step or two, before Samsung's own throttling takes several at once.
//  - performance: as before this file: with root the GPU's top clock kept, the CPU caps left alone.
// Every value written is put back when the app goes to the background and when the process ends, however it ends:
// the root shell reads its commands from a socket and runs the restore ($R) when that socket closes.
#include "platform.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
extern char** environ;
#endif

#include <nx/runtime.h>

#include "config.h"

namespace nx::plat {

#if defined(__ANDROID__)
namespace {

// gpu_target: the GPU busy share wanted at the chosen clock; cpu_target: the 90th percentile of a frame's CPU work
// (the longer of the game's and the replay's) over the period; late_max: the share of late frames tolerated before
// the caps go up. Hot: AThermal headroom (1 = severe throttling) or the hottest CPU zone in °C; max_drop: how many
// steps the ceilings come down then. adpf: the ADPF target as a share of the period (the margin it keeps).
struct Mode {
    const char* name;
    double gpu_target, cpu_target, late_max;
    int gpu_floor_mhz, cpu_floor_mhz;
    float headroom_hot;
    int temp_hot, max_drop;
    double adpf;
    // The battery's temperature (C) over which the ceilings come down, and under which the system's own GPU limit
    // (SSRM: 400 MHz with the battery at ~37 C on the S20 FE) is not kept: the body stays under ~43 C either way.
    int battery_hot, battery_ssrm;
};
const Mode kModes[3] = {
    {"performance", 1.0, 1.0, 1.0, 0, 0, 99.0f, 999, 0, 0.0, 0, 0},
    {"balanced", 0.85, 0.80, 0.03, 441, 1200, 0.90f, 81, 1, 0.85, 42, 41},
    // Cool trades a little: the GPU nearly full, a few late frames, and the ceilings two steps down when hot.
    {"cool", 0.96, 0.88, 0.08, 305, 1000, 0.75f, 68, 2, 0.95, 39, 0},
};
// The big cores' caps, MHz (the S20 FE's SSRM keeps them at 1747 even cold; the replay is memory-bound: at 1.5 GHz it
// took only a little longer than at 1.75, and 2.5 GHz made it ~10% faster). The steps over 1600 are there so that a
// busy frame takes the cores up one step, not straight to the system's own cap (2841 MHz on the S20 FE, where they
// stayed for whole sessions: 76-80 C, the battery at 41 C, and Samsung then held the GPU at 400 MHz).
const int kCpuCaps[] = {2400, 2100, 1800, 1600, 1500, 1400, 1300, 1200, 1100, 1000};
constexpr int kCpuStartLevel = 3;  // 1800: about Samsung's own cap, where the island's replay keeps its frame
constexpr int kCpuLevels = int(sizeof kCpuCaps / sizeof kCpuCaps[0]);

// The environment (tests) wins over config.ini.
std::string setting_value(const char* env, const char* key, const char* fallback) {
    if (const char* e = std::getenv(env); e && *e) return e;
    return config_get(key, fallback);
}

int parse_mode(const char* s) { return std::strcmp(s, "performance") == 0 ? 0 : std::strcmp(s, "cool") == 0 ? 2 : 1; }

// Frames since the governor last looked (written by the presenting thread, taken by the governor).
constexpr int kBins = 80;  // a frame's CPU work over the period, 2.5% each; the last one 197.5% and more
std::atomic<int64_t> g_period{0}, g_replay{0}, g_main{0};
std::atomic<uint32_t> g_frames{0}, g_late{0};
std::atomic<float> g_dyn_scale{1.0f};  // dynamic resolution: the rows' share (Governor::dynamic_resolution)
std::atomic<uint32_t> g_bins[kBins];

std::mutex g_mx;
std::condition_variable g_cv;
bool g_paused = false;

// A root shell (su -c, Magisk) on a socket: one command line in, its output and "@@" back. The shell evaluates each
// line; when the socket ends (this process gone: exited, crashed or killed) it runs $R, the restore the governor
// keeps up to date. SIGPIPE ignored in it, so that output written after we are gone cannot end it before the restore.
struct RootShell {
    int fd = -1;
    pid_t pid = -1;
    std::string in;

    bool start() {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) return false;
        static const char kScript[] = "trap '' PIPE; R=:; while IFS= read -r l; do eval \"$l\"; done; eval \"$R\"";
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, sv[1], 0);
        posix_spawn_file_actions_adddup2(&fa, sv[1], 1);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        char* argv[] = {const_cast<char*>("su"), const_cast<char*>("-c"), const_cast<char*>(kScript), nullptr};
        int rc = posix_spawnp(&pid, "su", &fa, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        close(sv[1]);
        if (rc != 0) {
            close(sv[0]);
            pid = -1;
            return false;
        }
        fd = sv[0];
        return true;
    }

    bool fail() {
        if (fd >= 0) close(fd);  // the shell sees the end of its input: it restores and ends
        fd = -1;
        if (pid > 0) waitpid(pid, nullptr, WNOHANG);
        return false;
    }

    // `cmd` must be one line of valid shell: a syntax error would leave the "@@" out and end the shell (timeout).
    bool run(const std::string& cmd, std::string* out, int timeout_ms) {
        if (fd < 0) return false;
        std::string line = cmd + "; echo @@\n";
        for (size_t at = 0; at < line.size();) {
            ssize_t w = send(fd, line.data() + at, line.size() - at, MSG_NOSIGNAL);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return fail();
            at += size_t(w);
        }
        const uint64_t until = monotonic_ns() + uint64_t(timeout_ms) * 1'000'000ull;
        for (;;) {
            size_t end = in.find("@@\n");
            if (end != std::string::npos) {
                if (out) *out = in.substr(0, end);
                in.erase(0, end + 3);
                return true;
            }
            uint64_t now = monotonic_ns();
            if (now >= until) return fail();
            pollfd pf{fd, POLLIN, 0};
            int r = poll(&pf, 1, int((until - now) / 1'000'000ull) + 1);
            if (r <= 0) continue;  // EINTR or the time is up (checked above)
            char buf[2048];
            ssize_t n = recv(fd, buf, sizeof buf, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return fail();
            in.append(buf, size_t(n));
        }
    }
};

std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> w;
    size_t a = 0;
    while ((a = line.find_first_not_of(" \t%", a)) != std::string::npos) {
        size_t b = line.find_first_of(" \t%", a);
        w.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
        a = b;
    }
    return w;
}
long num(const std::vector<std::string>& w, size_t k) { return k < w.size() ? std::atol(w[k].c_str()) : 0; }

struct Policy {
    int id = 0;
    std::string path;          // .../policyN/scaling_max_freq
    long restore = 0;          // kHz: the system's value, put back at the end
    long written = 0;          // what we last wrote (0: never)
    long read = 0;             // this tick's value
    std::vector<long> freqs;   // kHz, ascending
    int not_kept = 0;          // ticks the value read stayed over ours
};

struct Governor {
    RootShell sh;
    // GPU: Samsung's /sys/kernel/gpu/gpu_max_clock (MHz; SSRM writes it every few seconds) or kgsl's max_gpuclk (Hz).
    std::string gpu_path;
    bool gpu_in_mhz = false;
    std::vector<long> steps;   // MHz, ascending
    long gpu_usual = 0;        // MHz: the system's value at start (441 on the S20 FE, even cold)
    bool gpu_sys_seen = false;  // the system wrote its GPU cap since
    long gpu_restore = 0;      // MHz: the system's latest own value
    long gpu_written = 0, gpu_read = 0;
    long ssrm_low = 0;         // MHz: the system's own value when under its usual one (its thermal limit), else 0
    int gpu_idx = 0;
    // CPU: each big cluster's scaling_max_freq; Samsung's cpufreq_max_limit (one value, SSRM's) only if those do not
    // hold (some kernels put the policy's maximum back): then it is written too, and put back as found.
    std::vector<Policy> pol;
    bool use_limit = false;
    long limit_restore = 0, limit_written = 0, limit_read = 0;
    int cpu_level = kCpuStartLevel;  // 0: the system's caps; k: kCpuCaps[k - 1]
    std::vector<std::string> zones;  // thermal_zoneN/temp of the CPU's sensors
    int temp = -1;             // °C, the hottest of them
    // Samples since the last decision.
    double busy_sum = 0, clk_sum = 0;
    int busy_n = 0;
    // Decision state.
    int drop = 0, hot_n = 0, cool_n = 0;
    int gpu_hold = 0, cpu_hold = 0, gpu_votes = 0, cpu_votes = 0;
    bool turn_gpu = true, last_down_gpu = true;
    std::string reads, restore_sent;
    double busy = -1, clk = 0, cpu_p90 = 0, late = 0;
    int battery = -1;  // tenths of a degree C (-1: not read)
    uint32_t frames = 0, late_n = 0;
    uint64_t decided_ns = 0;  // the last decide()
    uint32_t dyn_starved = 0, dyn_roomy = 0;  // decisions in a row for dynamic_resolution
    uint64_t dyn_changed_ns = 0;
    float headroom = -1;

    int top() const { return int(steps.size()) - 1; }
    int floor_idx(const Mode& m) const {
        int k = 0;
        while (k < top() && steps[size_t(k)] < m.gpu_floor_mhz) k++;
        return k;
    }
    int max_level(const Mode& m) const {
        int l = 0;
        if (pol.empty()) return 0;
        while (l < kCpuLevels && kCpuCaps[l] >= m.cpu_floor_mhz) l++;
        return l;
    }
    long gpu_value(long mhz) const { return gpu_in_mhz ? mhz : mhz * 1'000'000; }
    long gpu_mhz(long value) const { return gpu_in_mhz ? value : value / 1'000'000; }
    // The cap of a policy at a level, kHz: the highest of its clocks at or under the level's, never over the system's.
    long policy_cap(const Policy& p, int level) const {
        if (level <= 0) return p.restore;
        long cap = std::min(long(kCpuCaps[level - 1]) * 1000, p.restore), best = 0;
        for (long f : p.freqs)
            if (f <= cap) best = f;
        return best ? best : p.freqs.empty() ? cap : std::min(p.freqs.front(), p.restore);
    }
    long cpu_cap_mhz(int level) const {
        long m = 0;
        for (const Policy& p : pol) m = std::max(m, policy_cap(p, level));
        return m / 1000;
    }
    // The next level with another clock (two caps can land on the same step of the cores' table), within [lo, hi].
    int cpu_step(int level, int dir, int lo, int hi) const {
        int l = level + dir;
        while (l > lo && l < hi && cpu_cap_mhz(l) == cpu_cap_mhz(level)) l += dir;
        return std::clamp(l, lo, hi);
    }

    bool discover() {
        std::string out;
        // The first command waits for Magisk's prompt (granted once).
        const char* cmd =
            "K=/sys/class/kgsl/kgsl-3d0; G=/sys/kernel/gpu/gpu_max_clock; "
            "[ -f $G ] && echo \"sgpu $(cat $G)\"; echo \"kgpu $(cat $K/max_gpuclk)\"; "
            "echo \"freqs $(cat $K/gpu_available_frequencies)\"; echo \"limit $(cat /sys/power/cpufreq_max_limit)\"; "
            "for p in /sys/devices/system/cpu/cpufreq/policy*; do echo \"policy ${p##*policy} $(cat $p/cpuinfo_max_freq) "
            "$(cat $p/scaling_max_freq) $(cat $p/scaling_available_frequencies)\"; done; "
            "for z in /sys/class/thermal/thermal_zone*; do echo \"zone ${z##*thermal_zone} $(cat $z/type)\"; done; "
            "echo \"id $(id -u)\"";
        if (!sh.run(cmd, &out, 30'000)) return false;
        long sgpu = 0, kgpu = 0, uid = -1, little_max = 0;
        std::vector<std::pair<int, std::string>> zone_types;
        size_t a = 0;
        while (a < out.size()) {
            size_t b = out.find('\n', a);
            std::string line = out.substr(a, b == std::string::npos ? std::string::npos : b - a);
            a = b == std::string::npos ? out.size() : b + 1;
            std::vector<std::string> w = words(line);
            if (w.empty()) continue;
            if (w[0] == "sgpu") sgpu = num(w, 1);
            else if (w[0] == "kgpu") kgpu = num(w, 1);
            else if (w[0] == "limit") limit_restore = num(w, 1);
            else if (w[0] == "id") uid = num(w, 1);
            else if (w[0] == "freqs")
                for (size_t k = 1; k < w.size(); k++) {
                    long mhz = std::atol(w[k].c_str()) / 1'000'000;
                    if (mhz > 0 && std::find(steps.begin(), steps.end(), mhz) == steps.end()) steps.push_back(mhz);
                }
            else if (w[0] == "policy" && w.size() >= 4) {
                Policy p;
                p.id = int(num(w, 1));
                long hw_max = num(w, 2);
                p.restore = num(w, 3);
                for (size_t k = 4; k < w.size(); k++) p.freqs.push_back(std::atol(w[k].c_str()));
                std::sort(p.freqs.begin(), p.freqs.end());
                p.path = "/sys/devices/system/cpu/cpufreq/policy" + std::to_string(p.id) + "/scaling_max_freq";
                p.freqs.push_back(hw_max);  // its cpuinfo_max_freq rides in the last slot until the clusters are told
                if (p.restore > 0) pol.push_back(p);
            } else if (w[0] == "zone" && w.size() >= 3)
                zone_types.push_back({int(num(w, 1)), w[2]});
        }
        if (uid != 0) {
            log("thermal: the root shell is not root (uid %ld): clock control off", uid);
            return false;
        }
        std::sort(steps.begin(), steps.end());
        if (sgpu > 0) gpu_path = "/sys/kernel/gpu/gpu_max_clock", gpu_in_mhz = true, gpu_usual = sgpu;
        else if (kgpu > 0) gpu_path = "/sys/class/kgsl/kgsl-3d0/max_gpuclk", gpu_usual = kgpu / 1'000'000;
        if (steps.empty()) gpu_path.clear();
        gpu_restore = gpu_usual;
        gpu_idx = top();
        // The little cluster (the lowest top clock) is left alone: the frame's threads are placed off it (frame_stats.cpp).
        for (Policy& p : pol) little_max = little_max ? std::min(little_max, p.freqs.back()) : p.freqs.back();
        pol.erase(std::remove_if(pol.begin(), pol.end(), [&](const Policy& p) { return p.freqs.back() <= little_max; }),
                  pol.end());
        for (Policy& p : pol) p.freqs.pop_back();
        // The CPU's own sensors (Qualcomm: cpu-0-0-usr .. cpu-1-7-usr): the big cluster's when named, at most 8.
        for (int pass = 0; pass < 2 && zones.empty(); pass++)
            for (auto& [n, type] : zone_types)
                if (zones.size() < 8 && type.rfind(pass == 0 ? "cpu-1" : "cpu", 0) == 0)
                    zones.push_back("/sys/class/thermal/thermal_zone" + std::to_string(n) + "/temp");
        // Every second: the shell's own `read` (a `cat` per file was a process each, ~10 a second).
        // (not "r": Android's mksh has that as an alias of fc, and every read failed)
        if (!sh.run("nxrd() { v=; read -r v < $2; echo \"$1 $v\"; }", nullptr, 3000)) return false;
        reads = "K=/sys/class/kgsl/kgsl-3d0; nxrd b $K/gpubusy; nxrd bp $K/gpu_busy_percentage; nxrd c $K/gpuclk";
        if (!gpu_path.empty()) reads += "; nxrd g " + gpu_path;
        for (const Policy& p : pol) reads += "; nxrd \"p " + std::to_string(p.id) + "\" " + p.path;
        if (limit_restore > 0) reads += "; nxrd l /sys/power/cpufreq_max_limit";
        for (const std::string& z : zones) reads += "; nxrd t " + z;
        reads += "; nxrd bat /sys/class/power_supply/battery/temp";  // tenths of a degree: the phone's body
        std::string desc;
        for (long s : steps) desc += " " + std::to_string(s);
        std::string cl;
        for (const Policy& p : pol)
            cl += " policy" + std::to_string(p.id) + "=" + std::to_string(p.restore / 1000) + "MHz";
        log("thermal: root: GPU cap %s (now %ld MHz), steps:%s; big cores:%s; cpufreq_max_limit %ld; %zu CPU sensors",
            gpu_path.empty() ? "not found" : gpu_path.c_str(), gpu_usual, desc.c_str(), cl.c_str(), limit_restore,
            zones.size());
        return true;
    }

    // $R: what we wrote, put back (the GPU and the policies we touched, cpufreq_max_limit only if we wrote it).
    void send_restore() {
        std::string r;
        auto add = [&](const std::string& path, long v) { r += (r.empty() ? "" : "; ") + ("echo " + std::to_string(v) + " > " + path); };
        if (gpu_written) add(gpu_path, gpu_value(gpu_restore));
        for (const Policy& p : pol)
            if (p.written) add(p.path, p.restore);
        if (limit_written) add("/sys/power/cpufreq_max_limit", limit_restore);
        if (r.empty() || r == restore_sent) return;
        if (sh.run("R='" + r + "'", nullptr, 3000)) restore_sent = r;
    }

    // The background: everything put back now (the shell's $R), and nothing written until the app is back.
    void restore_now() {
        if (restore_sent.empty()) return;
        sh.run("eval \"$R\"", nullptr, 3000);
        gpu_written = 0;
        for (Policy& p : pol) p.written = 0;
        limit_written = 0;
        log("thermal: in the background: the system's clock caps put back");
    }

    bool sample() {
        std::string out;
        if (!sh.run(reads, &out, 3000)) return false;
        size_t a = 0;
        bool busy_got = false;  // gpubusy (busy and total time of kgsl's last window), else gpu_busy_percentage
        long t = -1;
        while (a < out.size()) {
            size_t b = out.find('\n', a);
            std::vector<std::string> w = words(out.substr(a, b == std::string::npos ? std::string::npos : b - a));
            a = b == std::string::npos ? out.size() : b + 1;
            if (w.empty()) continue;
            if (w[0] == "b" && num(w, 2) > 0) busy_sum += double(num(w, 1)) / double(num(w, 2)), busy_n++, busy_got = true;
            else if (w[0] == "bp" && w.size() > 1 && !busy_got) busy_sum += double(num(w, 1)) / 100.0, busy_n++;
            else if (w[0] == "c") clk_sum += double(num(w, 1)) / 1e6;
            else if (w[0] == "g") gpu_read = gpu_mhz(num(w, 1));
            else if (w[0] == "l") limit_read = num(w, 1);
            else if (w[0] == "t" && w.size() > 1) t = std::max(t, num(w, 1) > 1000 ? num(w, 1) / 1000 : num(w, 1));  // m°C mostly
            else if (w[0] == "bat" && w.size() > 1) battery = int(num(w, 1));
            else if (w[0] == "p")
                for (Policy& p : pol)
                    if (p.id == num(w, 1)) p.read = num(w, 2);
        }
        temp = int(t);
        // A GPU cap we did not write: the system's (SSRM). Its usual value is what is put back; under that, it is its
        // thermal limit, which balanced and cool keep to.
        if (gpu_written && gpu_read > 0 && gpu_read != gpu_written) {
            // The first value the system writes while the phone is cool is its usual one: the value at start can be
            // what an earlier session (or a test script) left, 587 on the S20 FE, and SSRM's everyday 441 then
            // looked like a thermal limit, kept for the whole session (GPU at 400 MHz, a quarter of the frames late).
            if (!gpu_sys_seen && temp > 0 && temp < 65) gpu_usual = gpu_restore = gpu_read;
            gpu_sys_seen = true;
            if (gpu_read >= gpu_usual) gpu_restore = gpu_read;
            long low = gpu_read < gpu_usual ? gpu_read : 0;
            if (low != ssrm_low) log("thermal: the system's GPU cap %ld MHz%s", gpu_read, low ? " (its thermal limit)" : "");
            ssrm_low = low;
        }
        // A policy maximum raised over ours: the system's new value, what is put back. Kept over ours: the kernel does
        // not hold scaling_max_freq here, cpufreq_max_limit is used instead.
        for (Policy& p : pol) {
            if (p.written && p.read > p.written && p.written == p.restore) p.restore = p.read;
            p.not_kept = p.written && p.written < p.restore && p.read > p.written ? p.not_kept + 1 : 0;
            if (p.not_kept >= 3 && !use_limit && limit_restore > 0) {
                use_limit = true;
                log("thermal: policy%d's scaling_max_freq is not kept (%ld over %ld): cpufreq_max_limit used", p.id,
                    p.read, p.written);
            }
        }
        if (limit_written && limit_read > 0 && limit_read != limit_written && limit_read > limit_restore)
            limit_restore = limit_read;
        return true;
    }

    // Dynamic resolution (video.dynamic_resolution, NX_DYNAMIC_RES; on by default, balanced and cool only): frames late
    // while the GPU can go no higher (the system's thermal limit, or our ceiling) take the rows to 5/6 (1080 -> 900,
    // the output size unchanged) for at least a minute; back at full rows once there is no system limit and the GPU's
    // work would fit the ceiling with a third to spare. Each change makes the screen targets again (one frame late).
    template <class ShareAt>
    void dynamic_resolution(const Mode& m, int hi, ShareAt&& share_at) {
        static const bool on = [] {
            const std::string v = setting_value("NX_DYNAMIC_RES", "video.dynamic_resolution", "on");
            return v == "on" || v == "1";
        }();
        if (!on || m.gpu_target >= 1.0) {  // performance: full rows always
            g_dyn_scale.store(1.0f, std::memory_order_relaxed);
            return;
        }
        const uint64_t now = monotonic_ns();
        const bool low = g_dyn_scale.load(std::memory_order_relaxed) < 1.0f;
        const bool ceiling = gpu_idx >= hi || (ssrm_low && ssrm_low <= steps[size_t(std::min(gpu_idx, hi))]);
        const bool starved = late > 0.04 && late_n >= 3 && busy >= 0.75 && ceiling;
        dyn_starved = starved ? dyn_starved + 1 : 0;
        const bool roomy = !ssrm_low && late_n == 0 && busy >= 0 && share_at(hi) * 1.35 < m.gpu_target;
        dyn_roomy = roomy ? dyn_roomy + 1 : 0;
        if (now - dyn_changed_ns < 60'000'000'000ull && dyn_changed_ns) return;
        if (!low && dyn_starved >= 2) {
            g_dyn_scale.store(5.0f / 6.0f, std::memory_order_relaxed);
            dyn_changed_ns = now, dyn_starved = dyn_roomy = 0;
            log("thermal: %s: frames late at the GPU's ceiling (%.0f%% busy at %.0f MHz, system cap %ld MHz): rows x5/6",
                m.name, busy * 100, clk, ssrm_low);
        } else if (low && dyn_roomy >= 10) {
            g_dyn_scale.store(1.0f, std::memory_order_relaxed);
            dyn_changed_ns = now, dyn_starved = dyn_roomy = 0;
            log("thermal: %s: room on the GPU again (%.0f%% busy at %.0f MHz): full rows", m.name, busy * 100, clk);
        }
    }

    void decide(int mode_idx) {
        const Mode& m = kModes[mode_idx];
        frames = g_frames.exchange(0, std::memory_order_relaxed);
        late_n = g_late.exchange(0, std::memory_order_relaxed);
        // Frames missing against the time passed count as late too: a frame shown for three refreshes instead of two
        // (50 ms at 30 fps) is under the 1.25-period mark when the next one comes early, and at 28 fps the governor
        // saw "0 of 59 late" and took the GPU cap further down. Only while the game ran most of the window.
        {
            const uint64_t now = monotonic_ns();
            const int64_t p = g_period.load(std::memory_order_relaxed);
            if (decided_ns && p > 0) {
                const double expected = double(now - decided_ns) / double(p);
                if (frames >= 20 && double(frames) >= expected * 0.6 && expected - frames > late_n)
                    late_n = uint32_t(expected - frames);
            }
            decided_ns = now;
        }
        uint32_t bins[kBins], seen = 0;
        for (int k = 0; k < kBins; k++) bins[k] = g_bins[k].exchange(0, std::memory_order_relaxed);
        int p90 = 0;
        for (; p90 < kBins - 1; p90++)
            if ((seen += bins[p90]) * 10 >= frames * 9) break;
        cpu_p90 = frames ? (p90 + 1) / 40.0 : 0;
        late = frames ? double(late_n) / frames : 0;
        busy = busy_n ? busy_sum / busy_n : -1;
        clk = busy_n ? clk_sum / busy_n : 0;
        busy_sum = clk_sum = 0, busy_n = 0;
        headroom = thermal_headroom();
        if (mode_idx == 0) {  // performance: the GPU's top clock, the CPU as the system sets it
            gpu_idx = top(), cpu_level = 0, drop = 0;
            return;
        }
        // Hot: the ceilings a step lower every 10 s (to the mode's limit); cooled down: a step back every 20 s.
        // The CPU sensors when they read; AThermal's headroom only without them: the S20 FE's ROM reports over 1.0 even
        // cold, which kept the ceilings down for good (the big cores never back at the system's 1.75 GHz).
        const bool have_temp = temp > 0;
        bool hot = have_temp ? temp >= m.temp_hot : headroom >= m.headroom_hot;
        bool cooled = have_temp ? temp < m.temp_hot - 4 : headroom < m.headroom_hot - 0.1f;
        // The phone's body (its battery): the mode's limit (balanced 42 C, cool 39 C) is what the hand feels.
        if (battery > 0 && m.battery_hot > 0) {
            hot = hot || battery >= m.battery_hot * 10;
            cooled = cooled && battery < m.battery_hot * 10 - 10;
        }
        hot_n = hot ? hot_n + 1 : 0, cool_n = cooled ? cool_n + 1 : 0;
        if (hot_n >= 5 && drop < m.max_drop) {
            drop++, hot_n = 0;
            log("thermal: hot (%d C, headroom %.2f): ceilings %d step(s) down", temp, double(headroom), drop);
        }
        if (cool_n >= 10 && drop > 0) {
            drop--, cool_n = 0;
            log("thermal: cooler (%d C, headroom %.2f): ceilings %d step(s) down", temp, double(headroom), drop);
        }
        gpu_hold = std::max(0, gpu_hold - 1), cpu_hold = std::max(0, cpu_hold - 1);
        if (frames < 20) return;  // loading, paused, a menu: no evidence either way
        const bool gpu = !gpu_path.empty();
        const int lo = gpu ? floor_idx(m) : 0, hi = gpu ? std::max(lo, top() - drop) : 0;
        // Hot (drop steps): the cores' ceiling from 1800 MHz down, a step per drop (the steps over it are for cool phones).
        const int max_l = max_level(m), min_l = std::min(drop ? kCpuStartLevel + drop - 1 : 0, max_l);
        const int gpu_was = gpu_idx, cpu_was = cpu_level;
        // The GPU's work in MHz-seconds per second: its busy share at the clock it ran at. Busy time grows less than the
        // clock falls (memory), so the share predicted at a lower clock is on the safe side.
        const double work = gpu && busy >= 0 && clk > 0 ? busy * clk : -1;
        auto share_at = [&](int idx) { return work / double(steps[size_t(idx)]); };
        auto cpu_cap = [&](int level) { return double(cpu_cap_mhz(level)); };
        if (late > m.late_max && late_n >= 2) {
            // Frames late: up at once. The GPU when it is near full, the CPU when its work is, else what came down last.
            bool gpu_full = work >= 0 && share_at(std::min(gpu_idx, hi)) > 0.9;
            bool cpu_full = cpu_p90 > m.cpu_target;
            // The big cores already at their top: only the GPU can help (frame generation off, the GPU's busy share
            // stays under 90% while frames are late: the replay and the GPU's work in turn make the frame).
            const bool cpu_top = cpu_level <= min_l;
            // The CPU only when its work fills it: with it at half the frame, faster cores change nothing but the heat
            // (the airport: late frames at 40% CPU took the cores back to 2.84 GHz every few seconds).
            bool raise_gpu = gpu && (gpu_full || cpu_top || !cpu_full),
                 raise_cpu = !cpu_top && (cpu_full || (!gpu && cpu_p90 > m.cpu_target * 0.75));
            if (raise_gpu) gpu_idx++, gpu_hold = 5;
            if (raise_cpu) cpu_level = cpu_step(cpu_level, -1, min_l, max_l), cpu_hold = 5;
            gpu_votes = 0;
            // Late because of the GPU (full, the CPU's work well within its cap one step down): the cores do not help
            // and only heat the phone, which brings the system's GPU limit sooner. They keep coming down.
            const int next = cpu_step(cpu_level, 1, min_l, max_l);
            const bool cpu_spare = next > cpu_level && cpu_p90 * cpu_cap(cpu_level) / cpu_cap(next) < m.cpu_target;
            cpu_votes = !raise_cpu && gpu_full && cpu_spare && !cpu_hold ? cpu_votes + 1 : 0;
            if (cpu_votes >= 3) cpu_level = next, cpu_votes = 0;
        } else {
            if (work >= 0) {
                int need = lo;
                while (need < top() && share_at(need) > m.gpu_target) need++;
                if (need > gpu_idx) gpu_idx = need, gpu_hold = 5, gpu_votes = 0;
                else gpu_votes = need < gpu_idx && !gpu_hold ? gpu_votes + 1 : 0;
            }
            if (cpu_level > 0 && cpu_p90 > m.cpu_target + 0.08)
                cpu_level = cpu_step(cpu_level, -1, min_l, max_l), cpu_hold = 5, cpu_votes = 0;
            else {
                // The work's share at the next cap down, as if it scaled with the clock (it does less: memory).
                const int next = cpu_step(cpu_level, 1, min_l, max_l);
                bool fits = next > cpu_level && cpu_p90 * cpu_cap(cpu_level) / cpu_cap(next) < m.cpu_target;
                cpu_votes = fits && !cpu_hold ? cpu_votes + 1 : 0;
            }
            // One step down at a time, the GPU and the CPU taking turns, after 3 decisions (6 s) that agree.
            bool gpu_ready = gpu_votes >= 3, cpu_ready = cpu_votes >= 3;
            if (gpu_ready && (turn_gpu || !cpu_ready)) gpu_idx--, gpu_votes = 0, turn_gpu = false, last_down_gpu = true;
            else if (cpu_ready)
                cpu_level = cpu_step(cpu_level, 1, min_l, max_l), cpu_votes = 0, turn_gpu = true, last_down_gpu = false;
        }
        gpu_idx = gpu ? std::clamp(gpu_idx, lo, top()) : 0;
        cpu_level = std::clamp(cpu_level, min_l, max_l);
        dynamic_resolution(m, hi, share_at);
        const int gpu_now = std::min(gpu_idx, hi), gpu_before = std::min(gpu_was, hi);
        if (gpu_now != gpu_before || cpu_cap_mhz(cpu_level) != cpu_cap_mhz(cpu_was))
            log("thermal: %s: GPU cap %ld -> %ld MHz, big cores %ld -> %ld MHz (GPU %.0f%% busy at %.0f MHz, CPU work "
                "p90 %.0f%% of the frame, %u of %u frames late, %d C, headroom %.2f)",
                m.name, gpu ? steps[size_t(gpu_before)] : 0L, gpu ? steps[size_t(gpu_now)] : 0L,
                cpu_cap_mhz(cpu_was), cpu_cap_mhz(cpu_level), busy * 100, clk, cpu_p90 * 100, late_n, frames, temp,
                double(headroom));
    }

    void apply(int mode_idx) {
        const Mode& m = kModes[mode_idx];
        std::string w;
        auto put = [&](const std::string& path, long v) { w += (w.empty() ? "" : "; ") + ("echo " + std::to_string(v) + " > " + path); };
        if (!gpu_path.empty()) {
            long want = steps[size_t(std::min(gpu_idx, std::max(floor_idx(m), top() - drop)))];
            // The system's thermal limit kept (balanced: once the battery is over battery_ssrm; unread: always).
            const bool keep_ssrm = m.battery_ssrm <= 0 || battery <= 0 || battery >= m.battery_ssrm * 10;
            if (mode_idx != 0 && ssrm_low && keep_ssrm) want = std::min(want, ssrm_low);
            if (gpu_read != want) put(gpu_path, gpu_value(want));
            gpu_written = want;
        }
        long limit_want = 0;
        for (Policy& p : pol) {
            long want = policy_cap(p, cpu_level);
            limit_want = std::max(limit_want, want);
            if (!p.written && want >= p.read) continue;  // never lowered by us: the system's
            if (p.written && want == p.restore && p.read == p.restore) {  // back at the system's value: let go
                p.written = 0;
                continue;
            }
            // Under ours by someone else (the system's thermal limit): left as it is.
            if (p.read != want && !(p.read < want && p.read != p.written)) put(p.path, want);
            p.written = want;
        }
        if (use_limit) {
            long want = std::min(limit_want, limit_restore);
            if (limit_read != want && !(limit_read < want && limit_read != limit_written))
                put("/sys/power/cpufreq_max_limit", want);
            limit_written = want;
        }
        send_restore();  // before the write, so that the shell can always put back what it is about to change
        if (!w.empty()) sh.run(w, nullptr, 3000);
    }

    void status(int mode_idx) {
        log("thermal: %s: GPU cap %ld MHz (%.0f%% busy at %.0f MHz), big cores %ld MHz, %u of %u frames late, CPU work "
            "p90 %.0f%%, %d C, battery %.1f C, headroom %.2f",
            kModes[mode_idx].name, gpu_written, busy * 100, clk, cpu_cap_mhz(cpu_level), late_n, frames,
            cpu_p90 * 100, temp, battery / 10.0, double(headroom));
    }
};

void governor_main() {
    set_thread_name("thermal");
    Governor g;
    log("thermal: asking for root (NX_ROOT_CLOCKS=1)");
    if (!g.sh.start()) {
        log("thermal: su not available: ADPF only");
        return;
    }
    if (!g.discover()) {
        g.sh.fail();
        log("thermal: root refused or the clock files not found: ADPF only");
        return;
    }
    bool restored = false;
    for (uint64_t tick = 1;; tick++) {
        {
            std::unique_lock lk(g_mx);
            g_cv.wait_for(lk, std::chrono::seconds(1), [&] { return g_paused != restored; });
            if (g_paused && restored) continue;
            if (g_paused) {
                lk.unlock();
                g.restore_now();
                restored = true;
                continue;
            }
        }
        if (restored) {  // back from the background: from the top again, the frames meanwhile not counted
            restored = false;
            g.gpu_idx = g.top(), g.cpu_level = kCpuStartLevel, g.drop = 0, g.gpu_votes = g.cpu_votes = 0;
            g_frames.store(0), g_late.store(0);
            for (auto& b : g_bins) b.store(0);
        }
        const int mode = thermal_mode();
        if (!g.sample()) {
            log("thermal: the root shell ended: clock control off (the shell put the caps back)");
            return;
        }
        if (tick % 2 == 0) g.decide(mode);
        if (tick % 30 == 0) g.status(mode);
        g.apply(mode);
    }
}

}  // namespace

int thermal_mode() {
    static const char* env = std::getenv("NX_THERMAL");  // tests: wins over the setting
    if (env && *env) {
        static const int m = parse_mode(env);
        return m;
    }
    static std::atomic<uint64_t> gen{0};
    static std::atomic<int> cached{1};
    uint64_t g = config_generation();
    if (gen.load(std::memory_order_relaxed) != g) {
        cached.store(parse_mode(config_get("video.thermal", "balanced").c_str()), std::memory_order_relaxed);
        gen.store(g, std::memory_order_relaxed);
    }
    return cached.load(std::memory_order_relaxed);
}

double thermal_adpf_share(int mode) { return kModes[std::clamp(mode, 0, 2)].adpf; }

// AThermal (Android 11+): 0 cool, 1 the point of severe throttling; -1 unknown. It answers NaN when asked more than
// once a second: one answer per 1.5 s, shared by its callers.
float thermal_headroom() {
    using Acquire = void* (*)();
    using Headroom = float (*)(void*, int);
    static std::mutex mx;
    static void* mgr = nullptr;
    static Headroom headroom = nullptr;
    static bool tried = false;
    static float last = -1.0f;
    static uint64_t at = 0;
    std::lock_guard lk(mx);
    if (!tried) {
        tried = true;
        if (void* lib = dlopen("libandroid.so", RTLD_NOW)) {
            auto acquire = reinterpret_cast<Acquire>(dlsym(lib, "AThermal_acquireManager"));
            headroom = reinterpret_cast<Headroom>(dlsym(lib, "AThermal_getThermalHeadroom"));
            if (acquire && headroom) mgr = acquire();
        }
    }
    uint64_t now = monotonic_ns();
    if (mgr && headroom && (!at || now - at >= 1'500'000'000ull)) {
        float h = headroom(mgr, 0);
        last = h == h ? h : -1.0f;  // NaN: unknown
        at = now;
    }
    return last;
}

void thermal_start() {
    log("thermal: mode %s%s", kModes[thermal_mode()].name, std::getenv("NX_THERMAL") ? " (NX_THERMAL)" : "");
    const char* e = std::getenv("NX_ROOT_CLOCKS");
    if (e && e[0] == '1') std::thread(governor_main).detach();
}

void thermal_pause(bool paused) {
    {
        std::lock_guard lk(g_mx);
        g_paused = paused;
    }
    g_cv.notify_all();
}

float dynamic_render_scale() { return g_dyn_scale.load(std::memory_order_relaxed); }

void frame_period(int64_t ns) {
    if (ns > 0) g_period.store(ns, std::memory_order_relaxed);
}
int64_t frame_period_ns() { return g_period.load(std::memory_order_relaxed); }
void frame_replay_work(int64_t ns) { g_replay.store(ns, std::memory_order_relaxed); }
int64_t frame_replay_work_ns() { return g_replay.load(std::memory_order_relaxed); }
void thermal_main_work(int64_t ns) { g_main.store(ns, std::memory_order_relaxed); }

void thermal_frame(uint64_t interval_ns) {
    const int64_t p = g_period.load(std::memory_order_relaxed);
    if (p <= 0 || interval_ns > 250'000'000ull) return;  // a pause, a load: not a frame's time
    g_frames.fetch_add(1, std::memory_order_relaxed);
    if (int64_t(interval_ns) > p + p / 4) g_late.fetch_add(1, std::memory_order_relaxed);
    const int64_t work = std::max(g_replay.load(std::memory_order_relaxed), g_main.load(std::memory_order_relaxed));
    g_bins[std::clamp(int(work * 40 / p), 0, kBins - 1)].fetch_add(1, std::memory_order_relaxed);
}
#else
int thermal_mode() { return 0; }
double thermal_adpf_share(int) { return 0.0; }
float thermal_headroom() { return -1.0f; }
void thermal_start() {}
void thermal_pause(bool) {}
float dynamic_render_scale() { return 1.0f; }
void frame_period(int64_t) {}
int64_t frame_period_ns() { return 0; }
void frame_replay_work(int64_t) {}
int64_t frame_replay_work_ns() { return 0; }
void thermal_main_work(int64_t) {}
void thermal_frame(uint64_t) {}
#endif

}  // namespace nx::plat
