// Parallel replay (docs/PARALLEL_REPLAY.md). The frame's commands flattened; the runs between commands with ordered
// effects (dispatches, copies, uniform updates) are regions; a large region is cut into chunks, each replayed into a
// command buffer of its own by a worker thread (the replay thread takes one too) from a copy of the state at its start,
// which the replay thread gets by walking the region's state commands. The buffers are queued in order. A chunk whose
// worker met something only the replay thread may do (worker_bail) is replayed by the replay thread in its place.
// NX_PARALLEL_REPLAY=1: on. NX_PARALLEL_PLAN=1: the plan's statistics only (nothing runs differently).
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#if !defined(_WIN32)
#include <sys/resource.h>
#include <unistd.h>
#endif
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "nvn_draw_state.h"
#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

PerThread<bool> t_parallel_worker;
PerThread<bool> t_bail;
PerThread<void*> t_worker_slot;
PerThread<int> t_worker_index;

namespace {

struct Flat {
    const Cmd* cmd;
    const CmdList* list;
    size_t top = 0;  // the submit's list it came from
};

// As replay_list walks them: a called list's commands in place of the call.
void flatten(const CmdList& list, int depth, size_t top, std::vector<Flat>& out) {
    for (const Cmd& c : list.cmds) {
        if (c.op == Cmd::CallList) {
            if (depth < 8)
                if (CmdList* l = submit_command_list(c.a[0])) flatten(*l, depth + 1, top, out);
            continue;
        }
        out.push_back({&c, &list, top});
    }
}

bool targets_differ(const Cmd& a, const Cmd& b) {
    if (a.n != b.n) return true;
    for (int k = 0; k < 9; k++)
        if (a.a[k] != b.a[k]) return true;
    return false;
}

struct PlanStats {
    uint64_t frames = 0, cmds = 0, draws = 0, segments = 0, big = 0, big_draws = 0, largest = 0;
    uint64_t breaks_targets = 0, breaks_dispatch = 0, breaks_copy = 0, breaks_uub = 0;
    uint64_t uub_waits = 0, makespan = 0;  // uniform updates that overlap what in-flight segments read; simulated time
} g_plan;

struct Range64 {
    uint64_t lo, hi;
};

}  // namespace

bool parallel_plan_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_PARALLEL_PLAN");
        return e && e[0] == '1';
    }();
    return on;
}

void parallel_plan_frame(const std::vector<std::shared_ptr<CmdList>>& lists) {
    static std::vector<Flat> flat;
    flat.clear();
    for (size_t k = 0; k < lists.size(); k++) flatten(*lists[k], 0, k, flat);
    constexpr uint64_t kBig = 64;  // draws: a segment worth a worker
    constexpr int kWorkers = 3;
    const Cmd* targets = nullptr;
    uint64_t seg_draws = 0;
    // Simulated schedule, in draws: A walks (small segments cost their draws on A, a big one goes to the worker free
    // first); an update that overlaps what in-flight segments read waits for them all.
    uint64_t a_time = 0, worker_free[kWorkers] = {};
    std::vector<Range64> bound, inflight_reads, seg_reads;
    auto read = [&](uint64_t lo, uint64_t size) {
        if (size) seg_reads.push_back({lo, lo + size});
    };
    auto end_segment = [&] {
        if (!seg_draws) return;
        g_plan.segments++;
        if (seg_draws >= kBig) {
            g_plan.big++, g_plan.big_draws += seg_draws;
            int w = 0;
            for (int k = 1; k < kWorkers; k++)
                if (worker_free[k] < worker_free[w]) w = k;
            worker_free[w] = std::max(worker_free[w], a_time) + seg_draws;
            inflight_reads.insert(inflight_reads.end(), bound.begin(), bound.end());
            inflight_reads.insert(inflight_reads.end(), seg_reads.begin(), seg_reads.end());
        } else {
            a_time += seg_draws;
        }
        if (seg_draws > g_plan.largest) g_plan.largest = seg_draws;
        seg_draws = 0;
        seg_reads.clear();
    };
    auto wait_all = [&] {
        for (uint64_t t : worker_free) a_time = std::max(a_time, t);
        inflight_reads.clear();
    };
    for (const Flat& f : flat) {
        const Cmd& c = *f.cmd;
        switch (c.op) {
            case Cmd::SetRenderTargets:
                if (targets && targets_differ(*targets, c)) end_segment(), g_plan.breaks_targets++;
                targets = &c;
                break;
            case Cmd::DispatchCompute: end_segment(), g_plan.breaks_dispatch++, wait_all(); break;
            case Cmd::CopyTextureToTexture:
            case Cmd::CopyBufferToTexture:
            case Cmd::CopyTextureToBuffer: end_segment(), g_plan.breaks_copy++, wait_all(); break;
            case Cmd::UpdateUniformBuffer: {
                end_segment(), g_plan.breaks_uub++;
                uint64_t lo = c.a[0] + c.a[2], hi = lo + (c.blob < f.list->blobs.size() ? f.list->blobs[c.blob].size() : 0);
                for (const Range64& r : inflight_reads)
                    if (lo < r.hi && r.lo < hi) {
                        g_plan.uub_waits++, wait_all();
                        break;
                    }
                break;
            }
            case Cmd::BindUniformBuffer:
            case Cmd::BindStorageBuffer:
            case Cmd::BindVertexBuffer:
                read(c.a[0], c.a[1]);
                bound.push_back({c.a[0], c.a[0] + c.a[1]});
                if (bound.size() > 256) bound.erase(bound.begin(), bound.begin() + 128);  // a bound: the recent binds
                break;
            case Cmd::DrawArrays:
            case Cmd::DrawElements: seg_draws++, g_plan.draws++; break;
            default: break;
        }
    }
    end_segment();
    wait_all();
    g_plan.makespan += a_time;
    g_plan.cmds += flat.size();
    if (++g_plan.frames % 300) return;
    const double n = double(g_plan.frames);
    log("parallel plan: per frame %.0f commands, %.0f draws, %.1f segments, %.1f of %llu+ draws holding %.0f%% of the "
        "draws (largest %llu); breaks: targets %.1f, dispatch %.1f, copy %.1f, uniform update %.1f",
        double(g_plan.cmds) / n, double(g_plan.draws) / n, double(g_plan.segments) / n, double(g_plan.big) / n,
        (unsigned long long)kBig, g_plan.draws ? 100.0 * double(g_plan.big_draws) / double(g_plan.draws) : 0.0,
        (unsigned long long)g_plan.largest, double(g_plan.breaks_targets) / n, double(g_plan.breaks_dispatch) / n,
        double(g_plan.breaks_copy) / n, double(g_plan.breaks_uub) / n);
    log("parallel plan: %.1f uniform updates a frame wait for in-flight segments; with %d workers the draws take %.0f%% "
        "of the single-thread time", double(g_plan.uub_waits) / n, kWorkers,
        g_plan.draws ? 100.0 * double(g_plan.makespan) / double(g_plan.draws) : 0.0);
    g_plan = {};
}


// ---- the parallel replay ------------------------------------------------------------------------------------------

namespace {

using replay::State;

struct Job {
    size_t from = 0, to = 0;  // flat commands
    State state;              // the NVN state at `from`
    Targets st;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    bool bailed = false;
};

std::vector<Flat> g_flat;
std::vector<Job> g_jobs;
std::unordered_set<const Cmd*> g_skip;  // the region's clear-on-bind clears to skip (clear_once_plan)
void* g_slot = nullptr;
uint64_t g_mark = 0, g_serial = 0;

// The workers: woken by a generation per region, they take jobs until none is left.
std::mutex g_mx;
std::condition_variable g_cv, g_done_cv;
uint64_t g_gen = 0;
std::atomic<size_t> g_next{0}, g_left{0};
std::atomic<size_t> g_published{0};  // jobs ready (their state taken) so far
std::atomic<bool> g_walked{false};    // all of the region's jobs are published
std::atomic<int> g_active{0};
size_t g_count = 0;
int g_workers = 0;
struct Stats {
    uint64_t submits = 0, regions = 0, jobs = 0, bails = 0, parallel_draws = 0, serial_draws = 0;
    uint64_t walk_ns = 0, wait_ns = 0, queue_ns = 0, bail_ns = 0, own_job_ns = 0;
    uint64_t flatten_ns = 0, region_ns = 0, serial_ns = 0, ordered_ns = 0, total_ns = 0;
    std::atomic<uint64_t> job_ns{0}, worker_job_ns{0}, worker_cpu_ns{0};
} g_pstats;
const char* g_bail_why = nullptr;  // the first reason of the last window, for the log

bool is_draw(const Cmd& c) { return c.op == Cmd::DrawArrays || c.op == Cmd::DrawElements; }

// Commands whose effects are in order with everything around them: run by the replay thread between regions.
bool ordered(const Cmd& c) {
    switch (c.op) {
        case Cmd::DispatchCompute:
        case Cmd::CopyTextureToTexture:
        case Cmd::CopyBufferToTexture:
        case Cmd::CopyTextureToBuffer:
        case Cmd::UpdateUniformBuffer: return true;
        default: return false;
    }
}

void run_job(Job& j, int w) {
    const uint64_t t0 = plat::monotonic_ns(), c0 = thread_cpu_ns();
    (*t_parallel_worker) = true, (*t_bail) = false;
    (*t_worker_slot) = g_slot, (*t_worker_index) = w;
    (*t_clear_once_skip) = &g_skip;
    const bool worker_thread = replay_slot() != 0;
    static PerThread<uint64_t> submit_seen_pt;
    auto& submit_seen = *submit_seen_pt;
    if (worker_thread) replay_tls().mark = g_mark, replay_tls().serial = g_serial;
    (*replay::g_state) = j.state;
    draw_begin_worker(worker_thread && submit_seen != g_serial);  // the replay thread's caches are this submit's
    submit_seen = g_serial;
    VkCommandBuffer cb = frame_worker_begin(g_slot, w);
    if (cb) {
        Targets st = j.st;
        for (size_t i = j.from; i < j.to && !(*t_bail); i++) replay_cmd(cb, st, *g_flat[i].cmd, *g_flat[i].list);
        if ((*t_bail))
            pass_end_only(cb, 0);  // a valid buffer to end (it is not queued)
        else
            draw_end_rendering(cb, 0);  // its pass ended, its pending clears done
        frame_worker_end(cb);
    }
    j.cb = cb;
    j.bailed = (*t_bail) || !cb;
    const uint64_t ns = plat::monotonic_ns() - t0;
    g_pstats.job_ns.fetch_add(ns, std::memory_order_relaxed);
    if (w != g_workers) {
        g_pstats.worker_job_ns.fetch_add(ns, std::memory_order_relaxed);
        g_pstats.worker_cpu_ns.fetch_add(thread_cpu_ns() - c0, std::memory_order_relaxed);
    }
    (*t_parallel_worker) = false, (*t_bail) = false;
    (*t_clear_once_skip) = nullptr;
}

void take_jobs(int w) {
    for (int idle = 0;;) {
        size_t k = g_next.load(std::memory_order_acquire);
        if (k >= g_published.load(std::memory_order_acquire)) {
            if (g_walked.load(std::memory_order_acquire) && k >= g_published.load(std::memory_order_acquire)) break;
            if (++idle > 64) std::this_thread::yield();
            continue;
        }
        if (!g_next.compare_exchange_weak(k, k + 1, std::memory_order_acq_rel)) continue;
        idle = 0;
        run_job(g_jobs[k], w);
        if (g_left.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard lk(g_mx);
            g_done_cv.notify_all();
        }
    }
}

void worker_main(int w) {
    plat::set_thread_name("nvn replay w");
#if defined(__aarch64__) && defined(__ANDROID__)
    g_replay_tp[w + 1] = thread_pointer();  // its own slot of the per-thread state (nvn_per_thread.h)
    // Ahead of the game's other threads on the big cores: at the default priority the workers spent ~40% of their
    // chunks' time preempted (S20 FE). NX_PARALLEL_NICE: the nice value (Android lets an app go down to -19).
    {
        const char* e = std::getenv("NX_PARALLEL_NICE");
        int nice = e ? std::atoi(e) : -19;
        int r = setpriority(PRIO_PROCESS, int(gettid()), nice);
        if (w == 0) log("nvn/vk: parallel replay workers at nice %d (%s)", nice, r == 0 ? "ok" : std::strerror(errno));
    }
#endif
    plat::perf_hint_thread();  // part of the frame: in the ADPF session (perf_hint.cpp)
    uint64_t seen = 0;
    for (;;) {
        {
            std::unique_lock lk(g_mx);
            g_cv.wait(lk, [&] { return g_gen != seen; });
            seen = g_gen;
            g_active.fetch_add(1, std::memory_order_acq_rel);
        }
        take_jobs(w);
        g_active.fetch_sub(1, std::memory_order_acq_rel);
        plat::frame_helper_tick();  // on the big cores (frame_stats.cpp)
    }
}

void start_workers() {
    static bool started = false;
    if (started) return;
    started = true;
    const char* e = std::getenv("NX_PARALLEL_WORKERS");
    g_workers = std::clamp(e ? std::atoi(e) : 3, 1, 3);  // and the replay thread: kMaxWorkers (nvn_frames.cpp) = 4
    for (int w = 0; w < g_workers; w++) std::thread(worker_main, w).detach();
    log("nvn/vk: parallel replay with %d worker threads (NX_PARALLEL_REPLAY)", g_workers);
}

// One flat command on the replay thread, as replay_list replays it.
void replay_flat(VkCommandBuffer& cb, Targets& st, size_t k) {
    cb = frame_chunk(frame_cb(cb), (*replay::g_stats).done, pass_open());
    replay_cmd(cb, st, *g_flat[k].cmd, *g_flat[k].list);
}

// The state commands of [from, to) applied without drawing; the clear-on-bind decisions taken; counters written.
void walk_state(Targets& st, size_t k) {
    const Cmd& c = *g_flat[k].cmd;
    VkCommandBuffer none = VK_NULL_HANDLE;
    switch (c.op) {
        case Cmd::DrawArrays:
        case Cmd::DrawElements: break;
        case Cmd::ClearColor:
        case Cmd::ClearDepthStencil:
            if ((c.mask & kClearOnce) && clear_once_plan(st, c)) g_skip.insert(&c);
            break;
        case Cmd::SetRenderTargets:
            replay_cmd(none, st, c, *g_flat[k].list);
            // Their images made now, in order (a worker may not make one; a target remade every frame bailed a chunk).
            for (uint32_t t = 0; t < st.color_count; t++)
                if (st.colors[t]) image_of(st.colors[t]);
            if (st.depth) image_of(st.depth);
            break;
        default: draw_command(none, c, *g_flat[k].list, st); break;  // state; ReportCounter writes its counter
    }
}

// A region of `draws` draws in [a, b) replayed by the workers. False: not possible here (the caller replays it).
bool run_region(VkCommandBuffer& cb, Targets& st, size_t a, size_t b, uint64_t draws) {
    cb = frame_cb(cb);
    draw_end_rendering(cb, 0);  // the replay thread's pass ended and its pending clears done, before the workers'
    VkCommandBuffer cut = frame_cut(cb);
    if (cut == cb) return false;  // no part left to cut: queued in order is not possible
    cb = cut;
    const int threads = g_workers + 1;
    // A chunk per thread and one more (the replay thread walks first); a large region in more, to even them out.
    // Several chunks per thread: the workers (on cores the game's threads use too) are slower than the replay thread,
    // and taking chunks as they free up evens that out. NX_PARALLEL_CHUNK: draws per chunk.
    static const uint64_t per_chunk = std::getenv("NX_PARALLEL_CHUNK") ? std::strtoull(std::getenv("NX_PARALLEL_CHUNK"), nullptr, 10) : 96;
    const uint64_t chunks = std::clamp<uint64_t>(draws / std::max<uint64_t>(per_chunk, 16), uint64_t(threads), 64);
    const uint64_t per = (draws + chunks - 1) / chunks;
    g_jobs.clear(), g_skip.clear();
    g_jobs.reserve(chunks + 1);  // never reallocated while the workers read it
    g_slot = frame_current_slot();
    g_mark = replay_tls().mark, g_serial = replay_tls().serial;
    g_next.store(0, std::memory_order_release);
    g_published.store(0, std::memory_order_release);
    g_walked.store(false, std::memory_order_release);
    g_left.store(SIZE_MAX / 2, std::memory_order_release);  // set to the count once known
    {
        std::lock_guard lk(g_mx);
        g_gen++;
    }
    g_cv.notify_all();
    // The walk: each job is handed out as soon as the next one starts (its end known), while the walk goes on.
    const uint64_t w0 = plat::monotonic_ns();
    uint64_t in_job = 0;
    for (size_t k = a; k < b; k++) {
        const Cmd& c = *g_flat[k].cmd;
        if (g_jobs.empty() || (is_draw(c) && in_job >= per && g_jobs.size() < chunks)) {
            if (!g_jobs.empty()) g_jobs.back().to = k, g_published.store(g_jobs.size(), std::memory_order_release);
            Job& j = g_jobs.emplace_back();
            j.from = k, j.state = (*replay::g_state), j.st = st;
            in_job = 0;
        }
        if (is_draw(c)) in_job++;
        walk_state(st, k);
    }
    g_jobs.back().to = b;
    g_count = g_jobs.size();
    g_left.fetch_sub(SIZE_MAX / 2 - g_count, std::memory_order_acq_rel);
    g_published.store(g_count, std::memory_order_release);
    g_walked.store(true, std::memory_order_release);
    const State after = (*replay::g_state);
    const uint64_t w1 = plat::monotonic_ns();
    take_jobs(g_workers);  // the replay thread is a worker too (its own command pool: index g_workers)
    const uint64_t w2 = plat::monotonic_ns();
    {
        std::unique_lock lk(g_mx);
        g_done_cv.wait(lk, [] { return g_left.load(std::memory_order_acquire) == 0; });
    }
    while (g_active.load(std::memory_order_acquire)) std::this_thread::yield();  // none still looking for a job
    const uint64_t w3 = plat::monotonic_ns();
    g_pstats.walk_ns += w1 - w0, g_pstats.own_job_ns += w2 - w1, g_pstats.wait_ns += w3 - w2;
    // In order: the workers' buffers, and the replay thread's own replay of the chunks that bailed.
    std::vector<VkCommandBuffer> ready;  // queued together, until a chunk the replay thread replays itself
    ready.reserve(g_jobs.size());
    for (Job& j : g_jobs) {
        if (!j.bailed) {
            ready.push_back(j.cb);
            continue;
        }
        frame_queue_worker_cbs(ready.data(), uint32_t(ready.size()));
        ready.clear();
        g_pstats.bails++;
        if (!g_bail_why) g_bail_why = g_bail_reason.load(std::memory_order_relaxed);
        const uint64_t b0 = plat::monotonic_ns();
        (*replay::g_state) = j.state;
        Targets jst = j.st;
        (*t_clear_once_skip) = &g_skip;  // the plan's clear-on-bind decisions
        cb = frame_cb(cb);
        draw_begin_worker(false);  // a new command buffer: nothing bound in it yet
        for (size_t k = j.from; k < j.to; k++) replay_flat(cb, jst, k);
        draw_end_rendering(cb, 0);
        (*t_clear_once_skip) = nullptr;
        cb = frame_cut(cb);
        g_pstats.bail_ns += plat::monotonic_ns() - b0;
    }
    frame_queue_worker_cbs(ready.data(), uint32_t(ready.size()));
    (*replay::g_state) = after;
    g_pstats.regions++, g_pstats.jobs += g_jobs.size(), g_pstats.parallel_draws += draws;
    return true;
}

}  // namespace

bool parallel_replay_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_PARALLEL_REPLAY");
        if (!(e && e[0] == '1')) return false;
        // Debug features record into the replay thread's command buffer or keep order-dependent state.
        for (const char* v : {"NX_SNAP_FS", "NX_SNAP_FS_BEFORE", "NX_SNAP_FOLLOW", "NX_DUMP_ON_FS", "NX_DUMP_ON_TARGET",
                              "NX_DUMP_DRAW_TEXTURES", "NX_TRACE_DRAWS", "NX_DRAW_SLOW", "NX_PASS_BARRIER_NARROW",
                              "NX_COMPUTE_SEPARATE", "NX_DRAW_TIMING", "NX_TRACE_CMDS", "NX_VK_DEFER"})
            if (const char* x = std::getenv(v); x && *x && !(std::strcmp(v, "NX_VK_DEFER") == 0 && x[0] == '0')) {
                log("nvn/vk: parallel replay off: %s is set", v);
                return false;
            }
        return true;
    }();
#if defined(__ANDROID__)
    return on && !gpu_profile_on() && !zero_draws_on() && !render_pass_objects();
#else
    return false;  // its state is per thread only on Android (nvn_per_thread.h)
#endif
}

void parallel_replay(VkCommandBuffer& cb, Targets& st, const std::vector<std::shared_ptr<CmdList>>& lists,
                     size_t split_after) {
    start_workers();
    const uint64_t t0 = plat::monotonic_ns();
    g_flat.clear();
    for (size_t k = 0; k < lists.size(); k++) flatten(*lists[k], 0, k, g_flat);
    g_pstats.flatten_ns += plat::monotonic_ns() - t0;
    static const uint64_t kMinRegion = std::getenv("NX_PARALLEL_MIN") ? std::strtoull(std::getenv("NX_PARALLEL_MIN"), nullptr, 10) : 64;  // draws
    const size_t n = g_flat.size();
    size_t i = 0;
    while (i < n) {
        // After the submit's split list (frame_plan_split): the rest goes into the second part, as replay did.
        if (i > 0 && g_flat[i - 1].top == split_after && g_flat[i].top > split_after) cb = frame_split(cb);
        size_t r = i;
        uint64_t draws = 0;
        while (r < n && !ordered(*g_flat[r].cmd) && !(r > i && g_flat[r - 1].top == split_after && g_flat[r].top > split_after))
            draws += is_draw(*g_flat[r].cmd), r++;
        const uint64_t r0 = plat::monotonic_ns();
        bool parallel = draws >= kMinRegion && run_region(cb, st, i, r, draws);
        const uint64_t r1 = plat::monotonic_ns();
        if (!parallel) {
            for (size_t k = i; k < r; k++) replay_flat(cb, st, k);
            g_pstats.serial_draws += draws;
        }
        const uint64_t r2 = plat::monotonic_ns();
        if (r < n && ordered(*g_flat[r].cmd)) replay_flat(cb, st, r++);
        g_pstats.region_ns += r1 - r0, g_pstats.serial_ns += r2 - r1, g_pstats.ordered_ns += plat::monotonic_ns() - r2;
        i = r;
    }
    g_pstats.total_ns += plat::monotonic_ns() - t0;
    if (++g_pstats.submits % 300 == 0) {
        const double s = double(g_pstats.submits);
        auto ms = [&](uint64_t ns) { return double(ns) / 1e6 / s; };
        log("nvn/vk: parallel replay: per submit %.1f regions, %.1f chunks, %.2f bailed%s%s; %.0f draws in parallel, %.0f "
            "on the replay thread; ms: walk %.2f, own chunks %.2f, wait %.2f, bailed chunks %.2f, all chunks %.2f (workers "
            "%.2f, on the CPU %.2f)", double(g_pstats.regions) / s, double(g_pstats.jobs) / s, double(g_pstats.bails) / s,
            g_bail_why ? " first: " : "", g_bail_why ? g_bail_why : "", double(g_pstats.parallel_draws) / s,
            double(g_pstats.serial_draws) / s, ms(g_pstats.walk_ns), ms(g_pstats.own_job_ns), ms(g_pstats.wait_ns),
            ms(g_pstats.bail_ns), ms(g_pstats.job_ns.load()), ms(g_pstats.worker_job_ns.load()),
            ms(g_pstats.worker_cpu_ns.load()));
        g_pstats.submits = g_pstats.regions = g_pstats.jobs = g_pstats.bails = g_pstats.parallel_draws = 0;
        g_pstats.serial_draws = g_pstats.walk_ns = g_pstats.wait_ns = g_pstats.queue_ns = g_pstats.bail_ns = 0;
        g_pstats.own_job_ns = 0, g_pstats.job_ns = 0, g_pstats.worker_job_ns = 0, g_pstats.worker_cpu_ns = 0;
        log("nvn/vk: parallel replay ms per submit: total %.2f = flatten %.2f + regions %.2f + serial %.2f + ordered %.2f",
            ms(g_pstats.total_ns), ms(g_pstats.flatten_ns), ms(g_pstats.region_ns), ms(g_pstats.serial_ns),
            ms(g_pstats.ordered_ns));
        g_pstats.total_ns = g_pstats.flatten_ns = g_pstats.region_ns = g_pstats.serial_ns = g_pstats.ordered_ns = 0;
        g_bail_why = nullptr;
    }
}

}  // namespace nx::nvn
