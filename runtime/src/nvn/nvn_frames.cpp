// Submits that are not waited for (the design, and what stays synchronous and why:
// private/wonder/patches/async_submit.md). The synchronous path waits for every submit's fence before returning to
// the game, so the submitting thread stalls for the whole GPU time of each frame. Here a submit or a present takes one
// of a few slots (command buffers, fences, descriptor pool, the stretch of the upload ring its replay filled, the
// compute results to copy back) and returns once queued. A slot is retired (its fences waited, 2 s at most, as the
// synchronous path) when it is reused, when the ring needs its memory back, or when something waits for it.
//
// Replays copy everything the GPU reads from guest memory into the ring at submit time, so the game may change its
// memory while a submit runs. Only compute results flow back (write-backs). Two modes use the slots:
//
// NX_PIPELINED (the default since DEBUG.md #61; docs/nvn/PERF-PLAN.md step 3): "pipelined fences". The
// replay and vkQueueSubmit stay on the submitting thread; the fence of the frame's work is not waited for. A submit
// with dispatches is split after its last DispatchCompute into two command buffers: the first part is queued at the
// split, while the rest is still being replayed, and only its fence is waited for, after the rest is queued. Its
// write-backs are copied before gpu_submit returns, so guest memory holds the compute results exactly when it does
// synchronously, and nothing the game waits on (nvnSyncWait, nvnQueueFinish, pool invalidation and finalization) has
// anything left to wait for: gpu_serial() is 0, as synchronously, and those calls never take the GPU lock. At most
// g_max_replays submits are in flight (the oldest is waited for before another is queued).
// NX_PIPELINED_NO_SPLIT=1: no split (a submit with write-backs is waited for whole).
//
// Chunks (docs/nvn/android-perf/PLAN.md R1): a submit's replay is also queued in parts while it is recorded, at the
// first point with no render pass open after NX_SUBMIT_CHUNK_DRAWS draws (at twice that, a pass is ended for it), so
// the GPU starts on a frame a few milliseconds into its replay instead of at its end (or at the split). At most
// kMaxParts command buffers per submit; each begins with the barrier that orders it after everything queued before.
// Default 150 on Android, 0 (off) elsewhere: MoltenVK encodes Metal inside each vkQueueSubmit.
//
// NX_ASYNC_GPU=<frames in flight> (the older experiment): a submit that has write-backs is retired before the next
// replay begins (it may read them) and before a wait on it returns; the game's waits retire slots under the GPU lock.
//
// Also the images of destroyed textures (freed once the submits that could use them are retired) are handled here, in
// every mode.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

VkQueue vk_queue();                // nvn_vk.cpp
std::mutex& vk_queue_mutex();      // nvn_vk.cpp: held around vkQueueSubmit (the presenter thread presents on the queue)
std::recursive_mutex& gpu_lock();  // nvn_vk.cpp

namespace {

constexpr uint32_t kMaxSlots = 8;
constexpr uint32_t kMaxParts = 64;
constexpr int kMaxWorkers = 4;  // parallel replay threads, the replay thread included (nvn_parallel.cpp)  // command buffers per submit: chunks, the split, the last part
constexpr uint64_t kFenceTimeout = 2'000'000'000ull;  // a frame's work takes milliseconds; longer is a hung shader

struct Slot {
    VkCommandPool pool = VK_NULL_HANDLE;  // its own (the parts reset together), unless NX_CB_RESET=1
    bool pool_used = false;               // parts were begun since the pool's last reset
    VkCommandBuffer cbs[kMaxParts] = {};  // the parts, queued in order
    VkFence fences[kMaxParts] = {};
    uint32_t part = 0;                    // the part being recorded
    uint64_t queued = 0;                  // bit k: part k was queued (its fence will signal)
    uint32_t waited = 0;                  // parts below this were waited for (the ring may reuse their data)
    int split_part = -1;                  // pipelined: the part that ends with the last dispatch
    bool ok = true;                       // the current part is recording (a failed begin leaves nothing to queue)
    VkSemaphore acquired = VK_NULL_HANDLE;          // a present's swapchain image
    VkDescriptorPool descriptors = VK_NULL_HANDLE;  // made on first use (presents need none)
    bool descriptors_used = false;
    uint32_t sets_used = 0, descs_used[16] = {};  // allocated from the pool since its reset (peak logged by frame_log)
    std::atomic<bool> reset_done{true};  // the janitor reset its pools (nvn_janitor.cpp); frame_begin waits for it
    uint64_t serial = 0;    // in flight until retired; 0: free
    uint64_t ring_end = 0;  // the ring position after this submit's data
    std::vector<WriteBack> write_backs;
    bool replay = false;  // a submit's replay (not a present)
    uint64_t binds_at_split = 0;  // replay::written_bind_count() at the split
    uint64_t begin_ns = 0;
    // The parallel replay's workers (docs/PARALLEL_REPLAY.md): a command pool and a descriptor pool each (both are
    // externally synchronized), their command buffers queued between this slot's parts, under the last part's fence.
    VkCommandPool wpool[kMaxWorkers] = {};
    std::vector<VkCommandBuffer> wcbs[kMaxWorkers];
    uint32_t wused[kMaxWorkers] = {};
    VkDescriptorPool wdesc[kMaxWorkers] = {};
    bool wdesc_used[kMaxWorkers] = {};
};

Slot g_slots[kMaxSlots];
uint32_t g_family = 0;  // the queue family, for the workers' command pools

// After the slot's submits completed: its workers' command buffers and sets are free again.
void reset_worker_pools(Slot& s) {
    for (int w = 0; w < kMaxWorkers; w++) {
        if (s.wused[w]) p_vkResetCommandPool(vk_device(), s.wpool[w], 0);
        if (s.wdesc_used[w]) p_vkResetDescriptorPool(vk_device(), s.wdesc[w], 0);
        s.wused[w] = 0, s.wdesc_used[w] = false;
    }
}
uint32_t g_slot_count = 0, g_next = 0;
Slot* g_current = nullptr;               // the slot being recorded
VkSemaphore g_rendered[kMaxSlots] = {};  // per swapchain image: signaled for its present
std::atomic<uint64_t> g_submitted{0}, g_completed{0};
std::atomic<uint64_t> g_barrier{0};  // NX_ASYNC_GPU: the last submit with write-backs, retired before the next replay
FrameTimes g_times;
uint32_t g_max_replays = 2;                       // pipelined: submits in flight
uint64_t g_plan_serial = 0;  // frame_plan_split's count: a list's dispatch total is counted once per plan
uint32_t g_plan_dispatches = 0, g_seen_dispatches = 0;  // pipelined: the submit's DispatchCompute commands, replayed
// Pipelined, per log period: submits, split ones, write-backs after the first part's fence / after the whole submit,
// waits for the oldest submit (two in flight), and how far into the replay the split came.
struct Stats {
    uint64_t submits = 0, splits = 0, waited_first = 0, waited_whole = 0, throttled = 0, first_ns = 0, replay_ns = 0;
    uint64_t late = 0;  // write-backs left to the slot's retirement
    uint64_t chunks = 0;
} g_stats;
// A command pool per slot, reset whole when the slot is taken again (PHASE3.md C2): resetting each part's command
// buffer was 6-9% of the replay thread on Adreno (it frees the driver's per-pass objects). NX_CB_RESET=1: as before.
bool g_slot_pools = false;
uint64_t g_chunk_draws = 0;           // NX_SUBMIT_CHUNK_DRAWS (frame_init); 0: no chunks
bool g_chunking = false;              // this submit is chunked (frame_plan_split)
uint64_t g_chunk_from = UINT64_MAX;   // the draw count at the last part's start (UINT64_MAX: the next call's)
std::atomic<uint64_t> g_game_waits{0}, g_game_wait_ns{0};
uint32_t g_peak_sets = 0, g_peak_descs[16] = {};  // the most one slot's descriptor pool held in a submit, per log period
uint64_t g_staging_count = 0, g_staging_bytes = 0;  // staging_alloc's buffers, per log period (PHASE4.md U1)  // the game's waits that blocked (NX_ASYNC_GPU only)

// The upload ring (the synchronous path's upload buffer). Positions only grow; position p is byte p % size. What
// lies before g_tail belongs to retired submits; g_begin is where the submit being recorded started.
VkBuffer g_ring = VK_NULL_HANDLE;
uint8_t* g_ring_ptr = nullptr;
uint64_t g_ring_size = 0, g_head = 0, g_tail = 0, g_begin = 0;

// Images of destroyed textures, with the serial of the last submit that may still use them.
std::mutex g_grave_lock;
std::vector<std::pair<uint64_t, std::shared_ptr<GpuImage>>> g_grave;
std::vector<std::tuple<uint64_t, VkBuffer, VkDeviceMemory>> g_staging;  // staging_alloc's, freed like the grave's

// GPU-written images of destroyed textures, kept a few submits for a new texture over the same memory with the same
// shape. Wonder re-creates ~180 NVNtexture objects per frame over the same render-target memory: each got a new
// image, and its first draw ended the render pass to set it up (most of the ~300 passes per frame, each reloading
// and storing its targets; docs/nvn/DEBUG.md #34).
struct ReuseKey {
    uint64_t memory = 0;
    uint32_t format = 0, width = 0, height = 0, depth = 0, levels = 0, target = 0, samples = 0, swizzle = 0;
    bool operator<(const ReuseKey& o) const {
        return std::tie(memory, format, width, height, depth, levels, target, samples, swizzle) <
               std::tie(o.memory, o.format, o.width, o.height, o.depth, o.levels, o.target, o.samples, o.swizzle);
    }
};
ReuseKey reuse_key(const Texture& t) {
    uint32_t swizzle = uint32_t(t.swizzle[0]) | uint32_t(t.swizzle[1]) << 8 | uint32_t(t.swizzle[2]) << 16 |
                  uint32_t(t.swizzle[3]) << 24;
    static const bool key_swizzle = std::getenv("NX_KEY_SWIZZLE") != nullptr;  // as image_of's key (#46)
    if (!key_swizzle) swizzle = 0;
    return {t.memory, t.format, t.width, t.height, t.depth, t.levels, t.target, t.samples, swizzle};
}
std::map<ReuseKey, std::pair<uint64_t, std::shared_ptr<GpuImage>>> g_reusable;  // key -> (kept at, image)
uint64_t g_reuse_clock = 0, g_reused = 0;

void wait_fences(const VkFence* fences, uint32_t n) {
    if (!n) return;
    uint64_t t0 = plat::monotonic_ns();
    VkResult r = p_vkWaitForFences(vk_device(), n, fences, VK_TRUE, kFenceTimeout);
    if (r == VK_TIMEOUT) gpu_hung("a submit did not finish in 2 s");
    if (r == VK_ERROR_DEVICE_LOST) gpu_hung("vkWaitForFences");
    if (r != VK_SUCCESS) log("nvn/vk: vkWaitForFences failed (%d)", int(r));
    g_times.fence_ns += plat::monotonic_ns() - t0;
}

// Late write-backs (pipelined; NX_WRITE_BACK_LATE=0: off). A submit's compute results used to be waited for right
// after it was queued, so the replay thread waited for the GPU to finish everything before the frame's last dispatch:
// ~6 ms a frame on the island (an M1 Air), for 144 bytes in 3 ranges. Now the slot keeps them and applies them when it
// is retired (the next submits' throttle, normally a frame later). The replay waits for them only where it reads those
// guest bytes before that (write_back_before_read: uploads and storage bindings); the game's CPU sees them a frame
// or two later, as a later sync would have shown them on the console.
struct LateRange {
    uint64_t serial;
    const uint8_t* p;     // guest bytes
    uint64_t size;
    const uint8_t* from;  // the GPU's copy (upload memory, kept until the slot is retired)
    std::shared_ptr<std::vector<uint8_t>> before;  // the guest bytes that copy started from (null: unknown)
};
std::vector<LateRange> g_late;  // under the GPU lock
const uint8_t* g_late_lo = nullptr;
const uint8_t* g_late_hi = nullptr;  // the ranges' bounds: most reads are rejected by them
uint64_t g_late_waits = 0, g_late_reads = 0, g_late_copies = 0;

bool write_back_late() {
    static const bool on = [] {
        const char* e = std::getenv("NX_WRITE_BACK_LATE");
        return !(e && e[0] == '0');
    }();
    return on;
}

void late_bounds() {
    (*replay::g_upload_epoch)++;
    g_late_lo = g_late_hi = nullptr;
    for (const LateRange& r : g_late) {
        if (!g_late_lo || r.p < g_late_lo) g_late_lo = r.p;
        if (!g_late_hi || r.p + r.size > g_late_hi) g_late_hi = r.p + r.size;
    }
}

void late_add(uint64_t serial, const uint8_t* p, uint64_t size, const uint8_t* from,
              std::shared_ptr<std::vector<uint8_t>> before) {
    g_late.push_back({serial, p, size, from, std::move(before)});
    late_bounds();
}

void late_retired(uint64_t serial) {
    if (g_late.empty()) return;
    g_late.erase(std::remove_if(g_late.begin(), g_late.end(), [&](const LateRange& r) { return r.serial <= serial; }),
                 g_late.end());
    late_bounds();
}

// Compute results to guest memory: after the fence of the part that holds the submit's dispatches.
void apply_write_backs(Slot& s) {
    for (const WriteBack& w : s.write_backs) {
        replay::trace_nan_write_back(w.from, w.to, w.size);
        apply_write_back(w);
    }
    s.write_backs.clear();
}

// Waits for the oldest submit in flight and finishes it: compute results to guest memory (if not there yet), its
// descriptor pool and ring stretch free again, images destroyed before it freed. False when nothing is in flight.
// Fences complete in submission order on the one queue, so retiring oldest first keeps write-backs in order.
// The fences of the queued parts below `upto`.
uint32_t queued_fences(const Slot& s, uint32_t upto, VkFence* out) {
    uint32_t n = 0;
    for (uint32_t k = 0; k < upto && k < kMaxParts; k++)
        if (s.queued >> k & 1) out[n++] = s.fences[k];
    return n;
}

bool retire_oldest() {
    Slot* s = nullptr;
    for (uint32_t k = 0; k < g_slot_count; k++)
        if (g_slots[k].serial && (!s || g_slots[k].serial < s->serial)) s = &g_slots[k];
    if (!s) return false;
    VkFence fences[kMaxParts];
    wait_fences(fences, queued_fences(*s, kMaxParts, fences));
    gpu_busy_retired(uint32_t(s - g_slots), s->queued);  // NX_GPU_PROFILE=3
    apply_write_backs(*s);
    late_retired(s->serial);
    // Its command and descriptor pools: reset on the janitor thread (PHASE4.md C2b), or here (NX_JANITOR=0).
    if (janitor_on()) {
        s->reset_done.store(false, std::memory_order_relaxed);
        janitor_run([s] {
            if (g_slot_pools && s->pool_used) p_vkResetCommandPool(vk_device(), s->pool, 0);
            if (s->descriptors_used) p_vkResetDescriptorPool(vk_device(), s->descriptors, 0);
            s->pool_used = s->descriptors_used = false;
            reset_worker_pools(*s);
            s->sets_used = 0, std::fill(std::begin(s->descs_used), std::end(s->descs_used), 0u);
            s->reset_done.store(true, std::memory_order_release);
        });
    } else {
        if (s->descriptors_used) p_vkResetDescriptorPool(vk_device(), s->descriptors, 0);
        s->descriptors_used = false;
        s->sets_used = 0, std::fill(std::begin(s->descs_used), std::end(s->descs_used), 0u);
        reset_worker_pools(*s);
    }
    g_tail = std::max(g_tail, s->ring_end);
    g_completed = s->serial;
    s->serial = 0;
    // Pipelined: gpu_submit and gpu_present free the images, where the synchronous path does (the reuse clock ticks
    // there, so a destroyed target's image is kept for the same number of submits and presents).
    if (!pipelined()) free_retired_images(g_completed);
    return true;
}

void retire_through(uint64_t serial) {
    while (g_completed < serial && retire_oldest()) {
    }
}

}  // namespace

void write_back_before_read(const uint8_t* p, uint64_t size, const char* why) {
    if (!g_late_lo || p >= g_late_hi || p + size <= g_late_lo) return;
    uint64_t serial = 0;
    for (const LateRange& r : g_late)
        if (p < r.p + r.size && r.p < p + size) serial = std::max(serial, r.serial);
    if (!serial) return;
    if (g_completed >= serial) return;
    if (worker_bail("late write-back")) return;  // waiting for a submit is the replay thread's (docs/PARALLEL_REPLAY.md)
    g_late_reads++;
    g_late_waits++;
    static int traced = 0;
    if (traced++ < 4)
        log("nvn/vk: a late write-back waited for (%s read of %llu bytes)", why, (unsigned long long)size);
    retire_through(serial);
}

bool late_in_ring(const LateRange& r) {
    return r.from >= g_ring_ptr && r.from + r.size <= g_ring_ptr + g_ring_size;
}

// Whether [p, p + size) meets a late write-back still in flight.
bool late_pending(const uint8_t* p, uint64_t size) {
    if (!g_late_lo || p >= g_late_hi || p + size <= g_late_lo) return false;
    for (const LateRange& r : g_late)
        if (p < r.p + r.size && r.p < p + size && r.serial > g_completed) return true;
    return false;
}

// A storage binding's copy about to be made from guest bytes that late write-backs will change: those not in upload
// memory are waited for now, before the copy reads the guest bytes (late_copy then adds the rest on the GPU).
void late_prepare(const uint8_t* p, uint64_t size) {
    if (!g_late_lo || p >= g_late_hi || p + size <= g_late_lo) return;
    g_late_reads++;
    for (const LateRange& r : g_late)
        if (p < r.p + r.size && r.p < p + size && r.serial > g_completed && !late_in_ring(r)) {
            g_late_waits++;
            retire_through(r.serial);
            return late_prepare(p, size);  // g_late changed
        }
}

// The late write-backs' bytes within [p, p + size), copied on the GPU from their submits' results into `to` (whose
// byte 0 is p's), oldest first: the queue orders them after those submits, so nothing waits. Outside a render pass.
bool late_copy(VkCommandBuffer cb, const uint8_t* p, uint64_t size, VkBuffer to, uint64_t to_offset) {
    if (!g_late_lo || p >= g_late_hi || p + size <= g_late_lo) return false;
    bool any = false;
    for (const LateRange& r : g_late) {
        if (r.serial <= g_completed || !late_in_ring(r)) continue;  // already in guest memory
        const uint8_t* lo = std::max(r.p, p);
        const uint8_t* hi = std::min(r.p + r.size, p + size);
        if (lo >= hi) continue;
        // Only the words the CPU did not write since that copy was made (the guest still holds its `before`): the
        // rest is newer than the GPU's result. A bound range is often larger than what the shader writes: the rain's
        // particles are moved by a compute shader while the game writes each new drop's birth time beside them, and
        // the whole earlier copy put the first frames' births back over every new one (every drop dead: no rain).
        // NX_LATE_COPY_WHOLE=1: the whole range, as before.
        static const bool whole = std::getenv("NX_LATE_COPY_WHOLE") != nullptr;
        std::vector<VkBufferCopy> runs;
        const uint64_t src0 = uint64_t(r.from - g_ring_ptr), dst0 = to_offset;
        if (whole || !r.before || r.before->size() < r.size || (lo - r.p) % 4 || (hi - lo) % 4) {
            runs.push_back({src0 + uint64_t(lo - r.p), dst0 + uint64_t(lo - p), uint64_t(hi - lo)});
        } else {
            const uint8_t* was = r.before->data() + (lo - r.p);
            const uint64_t n = uint64_t(hi - lo);
            for (uint64_t k = 0; k < n;) {
                if (std::memcmp(lo + k, was + k, 4) != 0) {
                    k += 4;
                    continue;
                }
                uint64_t e = k + 4;
                while (e < n && std::memcmp(lo + e, was + e, 4) == 0) e += 4;
                runs.push_back({src0 + uint64_t(lo - r.p) + k, dst0 + uint64_t(lo - p) + k, e - k});
                k = e;
            }
            if (runs.empty()) continue;
        }
        if (!any) {  // the earlier submits' compute writes, before the copies read them
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                                   nullptr, 0, nullptr);
        }
        p_vkCmdCopyBuffer(cb, g_ring, to, uint32_t(runs.size()), runs.data());
        any = true;
    }
    if (any) {
        g_late_copies++;
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr,
                               0, nullptr);
    }
    return any;
}

namespace {

uint32_t replays_in_flight() {
    uint32_t n = 0;
    for (uint32_t k = 0; k < g_slot_count; k++) n += g_slots[k].serial && g_slots[k].replay;
    return n;
}

bool begin_part(VkCommandBuffer cb) {
    if (!g_slot_pools) p_vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (p_vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        log("nvn/vk: vkBeginCommandBuffer failed");
        return false;
    }
    // Earlier submits (and this submit's first part) may still run: their writes come before anything here (image
    // layouts are tracked in submission order, so the images already are where these commands expect them).
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    return true;
}

// Ends `cb` and queues it (Metal encoding happens inside vkQueueSubmit: MoltenVK's synchronous submits). False:
// nothing was queued, `fence` will not signal.
bool submit_part(VkCommandBuffer cb, VkFence fence, VkSemaphore wait, VkSemaphore signal) {
    uint64_t t0 = plat::monotonic_ns();
    // The host reads compute results after the fence.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                           nullptr);
    p_vkEndCommandBuffer(cb);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (wait) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait;
        si.pWaitDstStageMask = &stage;
    }
    if (signal) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signal;
    }
    p_vkResetFences(vk_device(), 1, &fence);
    VkResult r;
    if (vk_defer_active()) {  // queued in the recorder's ring, which takes the lock itself (holding it here could
        r = p_vkQueueSubmit(vk_queue(), 1, &si, fence);  // deadlock with a full ring)
    } else {
        std::lock_guard q(vk_queue_mutex());
        r = p_vkQueueSubmit(vk_queue(), 1, &si, fence);
    }
    g_times.encode_ns += plat::monotonic_ns() - t0;
    if (r == VK_ERROR_DEVICE_LOST) gpu_hung("vkQueueSubmit");
    if (r != VK_SUCCESS) log("nvn/vk: vkQueueSubmit failed (%d)", int(r));
    return r == VK_SUCCESS;
}

// As replay_list walks it. A list's own commands are scanned once (a recorded list does not change): the walk over
// every command of the frame took ~4% of the replay thread on the S23.
uint32_t count_dispatches(const CmdList& list, int depth) {
    auto& l = const_cast<CmdList&>(list);
    if (l.scanned_size != l.cmds.size()) {
        l.own_dispatches = 0, l.calls.clear();
        for (const Cmd& c : l.cmds) {
            if (c.op == Cmd::DispatchCompute) l.own_dispatches++;
            if (c.op == Cmd::CallList) l.calls.push_back(c.a[0]);
        }
        l.scanned_size = l.cmds.size();
    }
    // Once per plan: the game calls the same lists from many places (~4000 draws a frame through a few hundred
    // lists), and the walk over every call path was 9% of the replay thread at 60 fps.
    if (l.counted_plan == g_plan_serial) return l.counted_total;
    uint32_t n = l.own_dispatches;
    if (depth < 8)
        for (uint64_t h : l.calls)
            if (auto called = submit_command_list(h)) n += count_dispatches(*called, depth + 1);
    l.counted_plan = g_plan_serial, l.counted_total = n;
    return n;
}

int env_int(const char* name) {
    const char* e = std::getenv(name);
    return e ? std::atoi(e) : 0;
}

// Ends the part being recorded (and its render pass; pending clears stay pending, as they are not tied to a command
// buffer: the next pass on their targets takes them as load operations), queues it, and begins the next one.
VkCommandBuffer next_part(Slot& s) {
    pass_end_only(s.cbs[s.part], 0);
    gpu_busy_part_end(uint32_t(&s - g_slots), s.part, s.cbs[s.part]);
    if (s.ok && submit_part(s.cbs[s.part], s.fences[s.part], VK_NULL_HANDLE, VK_NULL_HANDLE)) s.queued |= 1ull << s.part;
    s.part++;
    s.ok = begin_part(s.cbs[s.part]);
    if (s.ok) gpu_busy_part_begin(uint32_t(&s - g_slots), s.part, s.cbs[s.part]);
    if (!s.ok) log("nvn/vk: a part of a submit could not begin: it is lost");
    g_chunk_from = UINT64_MAX;
    return s.cbs[s.part];
}

}  // namespace

// On by default. The synchronous path waits for every frame's fence on the thread that replays it, so the CPU and the
// GPU take turns. ACNH 3.0.3 on the island (a laptop with a dedicated GPU, the same scripted walk): replay 19.2 ->
// 14.8 ms and fence wait 7.8 -> 4.0 ms per frame, and 16 -> 2 frames over 50 ms in play, with the picture unchanged.
// (Another game's course once showed exploded meshes pipelined on a native driver; ACNH has shown none.)
// NX_PIPELINED=0: synchronous; 1..4: pipelined, that many submits in flight (2 at least). NX_ASYNC_GPU wins.
bool pipelined() {
    static const bool on = [] {
        if (env_int("NX_ASYNC_GPU") > 0) return false;
        const char* e = std::getenv("NX_PIPELINED");
        return !e || std::atoi(e) > 0;
    }();
    return on;
}

bool async_gpu() {
    static const bool on = pipelined() || env_int("NX_ASYNC_GPU") > 0;
    return on;
}

FrameTimes& frame_times() { return g_times; }
uint64_t frame_completed() { return g_completed; }
uint64_t frame_submitted() { return g_submitted; }

void frame_init(VkCommandPool pool, uint32_t family, VkBuffer ring, uint8_t* ring_ptr, uint64_t ring_size) {
    // NX_ASYNC_GPU=1 (or 2): two frames in flight; 3 or 4: that many. A frame is a submit and a present, a slot each.
    // NX_PIPELINED likewise (at most that many submits in flight, whatever the slots).
    uint32_t frames = uint32_t(std::clamp(env_int(pipelined() ? "NX_PIPELINED" : "NX_ASYNC_GPU"), 2, 4));
    g_max_replays = frames;
    g_slot_count = std::min(kMaxSlots, frames * 2);
    VkDevice d = vk_device();
    g_slot_pools = std::getenv("NX_CB_RESET") == nullptr;
    g_family = family;
    for (uint32_t k = 0; k < g_slot_count; k++) {
        if (g_slot_pools) {
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.queueFamilyIndex = family;
            g_slot_pools = p_vkCreateCommandPool(d, &pci, nullptr, &g_slots[k].pool) == VK_SUCCESS;
        }
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = g_slot_pools ? g_slots[k].pool : pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = kMaxParts;
        p_vkAllocateCommandBuffers(d, &cai, g_slots[k].cbs);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (VkFence& f : g_slots[k].fences) p_vkCreateFence(d, &fci, nullptr, &f);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        p_vkCreateSemaphore(d, &sci, nullptr, &g_slots[k].acquired);
    }
    g_ring = ring, g_ring_ptr = ring_ptr, g_ring_size = ring_size;
#if defined(__ANDROID__)
    g_chunk_draws = std::getenv("NX_SUBMIT_CHUNK_DRAWS") ? uint64_t(env_int("NX_SUBMIT_CHUNK_DRAWS")) : 150;
#else
    g_chunk_draws = uint64_t(env_int("NX_SUBMIT_CHUNK_DRAWS"));
#endif
    if (g_chunk_draws) log("nvn/vk: submits queued in parts every %llu draws (NX_SUBMIT_CHUNK_DRAWS)",
                           (unsigned long long)g_chunk_draws);
    if (pipelined())
        log("nvn/vk: pipelined fences (NX_PIPELINED): %u submits in flight, %u slots", frames, g_slot_count);
    else
        log("nvn/vk: asynchronous submits (NX_ASYNC_GPU): %u frames in flight, %u slots", frames, g_slot_count);
}

VkCommandBuffer frame_begin(bool replay) {
    // NX_ASYNC_GPU: this replay may read the results of the last submit that had write-backs. Pipelined: they are
    // in guest memory since that submit returned.
    if (!pipelined() && replay && g_barrier > g_completed) retire_through(g_barrier);
    Slot& s = g_slots[g_next];
    if (s.serial) retire_through(s.serial);
    janitor_wait(s.reset_done);
    if (g_slot_pools && s.pool_used) p_vkResetCommandPool(vk_device(), s.pool, 0);  // none of its parts is in flight
    s.pool_used = true;
    g_next = (g_next + 1) % g_slot_count;
    g_begin = g_head;
    s.replay = replay, s.part = s.queued = s.waited = 0, s.split_part = -1, s.ok = true;
    s.begin_ns = plat::monotonic_ns();
    g_plan_dispatches = g_seen_dispatches = 0;
    g_chunking = false;
    if (!begin_part(s.cbs[0])) {
        g_current = nullptr;
        return VK_NULL_HANDLE;
    }
    g_current = &s;
    gpu_busy_part_begin(uint32_t(&s - g_slots), 0, s.cbs[0]);  // NX_GPU_PROFILE=3
    return s.cbs[0];
}

// ---- pipelined: the split after the last dispatch ----------------------------------------------------------------

size_t frame_plan_split(const std::vector<std::shared_ptr<CmdList>>& lists) {
    static const bool off = std::getenv("NX_PIPELINED_NO_SPLIT") != nullptr;
    g_plan_dispatches = g_seen_dispatches = 0;
    g_chunking = g_chunk_draws && g_current, g_chunk_from = UINT64_MAX;  // not with the profile (not called then)
    // NX_COMPUTE_SEPARATE's write-backs are not counted by written_bind_count(): such a submit is waited for whole.
    if (!pipelined() || off || !g_current || replay::written_separate()) return SIZE_MAX;
    size_t last = SIZE_MAX;
    g_plan_serial++;
    for (size_t k = 0; k < lists.size(); k++)
        if (uint32_t n = count_dispatches(*lists[k], 0)) g_plan_dispatches += n, last = k;
    return last;
}

VkCommandBuffer frame_cb(VkCommandBuffer cb) { return g_current ? g_current->cbs[g_current->part] : cb; }

VkCommandBuffer frame_split(VkCommandBuffer cb) {
    Slot* s = g_current;
    if (!pipelined() || !s || s->split_part >= 0 || cb != s->cbs[s->part] || s->part + 1 >= kMaxParts)
        return frame_cb(cb);
    s->split_part = int(s->part);
    s->binds_at_split = replay::written_bind_count();
    g_stats.splits++, g_stats.first_ns += plat::monotonic_ns() - s->begin_ns;
    VkCommandBuffer next = next_part(*s);
    if (!(s->queued >> s->split_part & 1))
        log("nvn/vk: the part with a split submit's dispatches was not queued: its compute results are lost");
    return next;
}

// NX_SUBMIT_CHUNK_DRAWS=-1: a part per render pass, cut as a pass begins (with NX_GPU_PROFILE=3: the GPU's time per
// pass, nvn_gpu_busy.cpp).
VkCommandBuffer frame_pass_part(VkCommandBuffer cb) {
    Slot* s = g_current;
    if (g_chunk_draws != UINT64_MAX || !g_chunking || !s || cb != s->cbs[s->part] || s->part + 2 >= kMaxParts) return cb;
    g_stats.chunks++;
    return next_part(*s);
}

VkCommandBuffer frame_chunk(VkCommandBuffer cb, uint64_t draws, bool pass_open) {
    Slot* s = g_current;
    // Two parts are kept for the split and the last one.
    if (!g_chunking || !s || cb != s->cbs[s->part] || s->part + 2 >= kMaxParts) return cb;
    if (g_chunk_draws == UINT64_MAX) return cb;  // frame_pass_part cuts
    if (g_chunk_from == UINT64_MAX) g_chunk_from = draws;
    uint64_t since = draws - g_chunk_from;
    if (since < g_chunk_draws || (pass_open && since < 2 * g_chunk_draws)) return cb;
    g_stats.chunks++;
    return next_part(*s);
}

// ---- the parallel replay's command buffers (nvn_parallel.cpp) ----------------------------------------------------

void* frame_current_slot() { return g_current; }

// On a worker: a command buffer of its own for the slot, begun as a part is (after a full barrier).
VkCommandBuffer frame_worker_begin(void* slot, int w) {
    Slot& s = *static_cast<Slot*>(slot);
    if (!s.wpool[w]) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = g_family;
        if (p_vkCreateCommandPool(vk_device(), &pci, nullptr, &s.wpool[w]) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    if (s.wused[w] == s.wcbs[w].size()) {
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = s.wpool[w];
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (p_vkAllocateCommandBuffers(vk_device(), &cai, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
        s.wcbs[w].push_back(cb);
    }
    VkCommandBuffer cb = s.wcbs[w][s.wused[w]++];
    return begin_part(cb) ? cb : VK_NULL_HANDLE;
}

void frame_worker_end(VkCommandBuffer cb) { p_vkEndCommandBuffer(cb); }

// On the replay thread, in order: a worker's command buffer queued after what was queued so far (no fence: the
// submit's last part's fence covers it, as everything earlier in submission order).
bool frame_queue_worker_cbs(const VkCommandBuffer* cbs, uint32_t n) {
    if (!n) return true;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = n;  // one submit for a region's buffers: each vkQueueSubmit is a kernel call
    si.pCommandBuffers = cbs;
    std::lock_guard q(vk_queue_mutex());
    return p_vkQueueSubmit(vk_queue(), 1, &si, VK_NULL_HANDLE) == VK_SUCCESS;
}

// The replay thread's part ends here and is queued; the next begins (what follows the workers' buffers).
VkCommandBuffer frame_cut(VkCommandBuffer cb) {
    Slot* s = g_current;
    if (!s || cb != s->cbs[s->part] || s->part + 2 >= kMaxParts) return frame_cb(cb);
    return next_part(*s);
}

VkCommandBuffer frame_after_dispatch(VkCommandBuffer cb) {
    if (!g_plan_dispatches || ++g_seen_dispatches != g_plan_dispatches) return frame_cb(cb);
    return frame_split(cb);
}

VkSemaphore frame_acquire_semaphore() {  // the next slot's (frame_begin takes that slot)
    Slot& s = g_slots[g_next];
    if (s.serial) retire_through(s.serial);  // its last present waited on it
    return s.acquired;
}

VkSemaphore frame_rendered_semaphore(uint32_t image) {
    if (image >= kMaxSlots) image = kMaxSlots - 1;
    if (!g_rendered[image]) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        p_vkCreateSemaphore(vk_device(), &sci, nullptr, &g_rendered[image]);
    }
    return g_rendered[image];
}

void frame_submit(VkSemaphore wait, VkSemaphore signal, std::vector<WriteBack> write_backs, bool wait_done) {
    if (!g_current) return;
    Slot& s = *g_current;
    g_current = nullptr;
    if (pipelined() && s.replay) {
        g_stats.submits++;
        if (s.split_part >= 0) g_stats.replay_ns += plat::monotonic_ns() - s.begin_ns;
    }
    if (s.ok) gpu_busy_part_end(uint32_t(&s - g_slots), s.part, s.cbs[s.part]);
    const bool has_last = s.ok && submit_part(s.cbs[s.part], s.fences[s.part], wait, signal);
    if (has_last) s.queued |= 1ull << s.part;
    if (!s.queued) return;  // nothing ran: no results to copy back, the slot stays free
    s.serial = ++g_submitted;
    s.ring_end = g_head;
    s.write_backs = std::move(write_backs);
    // The dispatches were in a part that never ran: the buffers hold no results, and copying them back would put the
    // inputs (or older data) over guest memory.
    const bool has_first = s.split_part >= 0 && (s.queued >> s.split_part & 1);
    if (s.split_part >= 0 && !has_first) s.write_backs.clear();
    if (pipelined()) {
        // At most g_max_replays submits in flight, this one included: the oldest are waited for once this one is
        // queued (before, the rest waited behind them and the GPU idled between the two parts).
        struct Throttle {
            Slot& s;
            ~Throttle() {
                if (!s.replay) return;
                bool waited = false;
                while (replays_in_flight() > g_max_replays && retire_oldest()) waited = true;
                g_stats.throttled += waited;
            }
        } throttle{s};
        // Debug features read this submit's results right after it: all of it is waited for. Otherwise only compute
        // results are: they are in the first part when the split came after the last dispatch that wrote storage.
        if (wait_done) {
            retire_through(s.serial);
        } else if (!s.write_backs.empty() && write_back_late()) {
            for (const WriteBack& w : s.write_backs) late_add(s.serial, w.to, w.size, w.from, w.before);
            g_stats.late++;
        } else if (!s.write_backs.empty()) {
            if (has_first && has_last && replay::written_bind_count() == s.binds_at_split) {
                VkFence fences[kMaxParts];
                wait_fences(fences, queued_fences(s, uint32_t(s.split_part) + 1, fences));
                apply_write_backs(s);
                g_stats.waited_first++;
            } else {
                retire_through(s.serial);
                g_stats.waited_whole++;
            }
        }
        return;
    }
    if (!s.write_backs.empty()) g_barrier = s.serial;
    // NX_ASYNC_COMPUTE_WAIT=1: submits with compute results are waited for at once, as synchronously (the game then
    // finds them in its memory right after nvnQueueSubmitCommands, without waiting on a sync).
    static const bool compute_wait = std::getenv("NX_ASYNC_COMPUTE_WAIT") != nullptr;
    if (wait_done || (compute_wait && !s.write_backs.empty())) retire_through(s.serial);
}

void frame_wait_all() {
    retire_through(g_submitted);
    for (uint32_t k = 0; k < g_slot_count; k++) janitor_wait(g_slots[k].reset_done);
}

uint64_t frame_upload_used() { return g_head - g_begin; }
uint64_t frame_ring_size() { return async_gpu() ? g_ring_size : 0; }

void frame_log(uint64_t frames) {
    gpu_busy_log(frames);
    if (uint64_t waits = g_game_waits.exchange(0))
        log("nvn/vk: the game's waits (syncs, finish, pool invalidate/finalize): %llu blocked, %.2f ms per frame",
            (unsigned long long)waits, double(g_game_wait_ns.exchange(0)) / 1e6 / double(frames));
    if (g_peak_sets) {
        log("nvn/vk: descriptor pool peak per submit: %u sets, ubo %u, ubo dynamic %u, ssbo %u, sampler %u, storage image %u, "
            "texel %u+%u (pool: 32768 sets, 65536 / 65536 / 16384 / 65536 / 4096 / 1024+1024)",
            g_peak_sets, g_peak_descs[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER], g_peak_descs[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC],
            g_peak_descs[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER], g_peak_descs[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER],
            g_peak_descs[VK_DESCRIPTOR_TYPE_STORAGE_IMAGE], g_peak_descs[VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER],
            g_peak_descs[VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER]);
        g_peak_sets = 0, std::fill(std::begin(g_peak_descs), std::end(g_peak_descs), 0u);
    }
    if (g_staging_count)
        log("nvn/vk: staging: %llu buffers, %.1f MB over %llu frames", (unsigned long long)g_staging_count,
            double(g_staging_bytes) / 1e6, (unsigned long long)frames);
    g_staging_count = g_staging_bytes = 0;
    if (!pipelined() || !g_stats.submits) return;
    const Stats& t = g_stats;
    log("nvn/vk: pipelined: %llu submits, %llu split (first part: %.0f%% of the replay), write-backs after the first "
        "part %llu, after the whole submit %llu, %llu waited for the oldest in flight, %llu chunks queued early",
        (unsigned long long)t.submits, (unsigned long long)t.splits,
        t.replay_ns ? 100.0 * double(t.first_ns) / double(t.replay_ns) : 0.0, (unsigned long long)t.waited_first,
        (unsigned long long)t.waited_whole, (unsigned long long)t.throttled, (unsigned long long)t.chunks);
    if (t.late || g_late_reads)
        log("nvn/vk: late write-backs: %llu submits, %llu reads of their bytes before retirement, %llu copied on the GPU, "
            "%llu waited", (unsigned long long)t.late, (unsigned long long)g_late_reads,
            (unsigned long long)g_late_copies, (unsigned long long)g_late_waits);
    g_late_reads = g_late_waits = g_late_copies = 0;
    g_stats = {};
}

Upload upload_locked(uint64_t size, uint64_t align);

// A parallel replay worker takes the ring in chunks (a lock per chunk, not per upload).
Upload frame_upload_alloc(uint64_t size, uint64_t align) {
    if (!*t_parallel_worker) return upload_locked(size, align);
    struct Chunk {
        uint64_t begin = UINT64_MAX, off = 0, end = 0;
        uint8_t* base = nullptr;
        VkBuffer buffer = VK_NULL_HANDLE;
    };
    static PerThread<Chunk> chunk_pt;
    Chunk& c = *chunk_pt;
    uint64_t o = (c.off + align - 1) & ~(align - 1);
    if (c.begin != g_begin || o + size > c.end) {
        const uint64_t want = std::max<uint64_t>(256 << 10, size + align);
        Upload u = upload_locked(want, 256);
        if (*t_bail) return u;  // the ring is full: scratch (the chunk is replayed by the replay thread)
        c = {g_begin, u.offset, u.offset + want, u.ptr - u.offset, u.buffer};
        o = (c.off + align - 1) & ~(align - 1);
    }
    c.off = o + size;
    return {c.buffer, o, c.base + o};
}

Upload upload_locked(uint64_t size, uint64_t align) {
    // The parallel replay's workers allocate here too (docs/PARALLEL_REPLAY.md); a full ring is only waited for by the
    // replay thread: a worker gets scratch memory (its chunk is replayed again by the replay thread).
    static std::mutex mx;
    std::lock_guard lk(mx);
    uint64_t at = (g_head + align - 1) & ~(align - 1);
    if (at % g_ring_size + size > g_ring_size) at += g_ring_size - at % g_ring_size;  // never split: the next lap
    if (at + size > g_tail + g_ring_size && worker_bail("upload ring full")) {
        static PerThread<std::vector<uint8_t>> scratch_pt;
        auto& scratch = *scratch_pt;
        if (scratch.size() < size) scratch.resize(size);
        return {g_ring, 0, scratch.data()};
    }
    while (at + size > g_tail + g_ring_size)  // the oldest submits' data is still read: wait for them
        if (!retire_oldest()) {
            // The submit's parts queued so far may still read its data: wait for them first.
            if (g_current && g_current->waited < g_current->part) {
                VkFence fences[kMaxParts];
                wait_fences(fences, queued_fences(*g_current, g_current->part, fences));
                g_current->waited = g_current->part;
            }
            log("nvn/vk: upload buffer full (%llu bytes in this submit)", (unsigned long long)(at + size - g_begin));
            g_tail = at + size - g_ring_size;  // this submit wraps over its own first data, as the synchronous path
            replay::dedup_reset();
            break;
        }
    g_head = at + size;
    uint64_t off = at % g_ring_size;
    return {g_ring, off, g_ring_ptr + off};
}

VkDescriptorSet frame_descriptor_set(VkDescriptorSetLayout layout) {
    if ((*t_parallel_worker)) {  // the worker's own pool of the slot it records for
        Slot* ws = static_cast<Slot*>((*t_worker_slot));
        if (!ws) return VK_NULL_HANDLE;
        VkDescriptorPool& pool = ws->wdesc[(*t_worker_index)];
        if (!pool) pool = make_descriptor_pool();
        ws->wdesc_used[(*t_worker_index)] = true;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (p_vkAllocateDescriptorSets(vk_device(), &ai, &set) != VK_SUCCESS) {
            log("nvn/vk: a parallel replay worker's descriptor pool is exhausted");
            worker_bail("descriptor pool");
        }
        return set;
    }
    if (!g_current) return VK_NULL_HANDLE;
    Slot& s = *g_current;
    if (!s.descriptors) s.descriptors = make_descriptor_pool();
    s.descriptors_used = true;
    {
        uint32_t c[16] = {};
        descriptor_layout_counts(layout, c);
        s.sets_used++;
        g_peak_sets = std::max(g_peak_sets, s.sets_used);
        for (int t = 0; t < 16; t++) s.descs_used[t] += c[t], g_peak_descs[t] = std::max(g_peak_descs[t], s.descs_used[t]);
    }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s.descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (p_vkAllocateDescriptorSets(vk_device(), &ai, &set) != VK_SUCCESS) log("nvn/vk: descriptor pool exhausted");
    return set;
}

// nvn.h: what nvnQueueFenceSync records and nvnSyncWait / nvnQueueFinish wait for. The synchronous path has nothing
// in flight when the game gets control back: 0, and no wait. Pipelined submits are in flight, but nothing the game
// can observe depends on them: compute results are in guest memory when gpu_submit returns, the GPU reads its inputs
// from the ring (never from guest memory), and images, descriptor pools and ring stretches are kept until their
// submit is retired. So it is 0 there too, and those calls neither wait nor take the GPU lock.
uint64_t gpu_serial() { return async_gpu() && !pipelined() ? g_submitted.load() : 0; }
uint64_t gpu_write_back_serial() { return async_gpu() && !pipelined() ? g_barrier.load() : 0; }

bool gpu_wait(uint64_t serial) {
    if (!serial || pipelined() || g_completed >= serial) return false;
    uint64_t t0 = plat::monotonic_ns();
    std::lock_guard lk(gpu_lock());
    if (g_completed < serial) retire_through(serial);
    g_game_waits++, g_game_wait_ns += plat::monotonic_ns() - t0;
    return true;
}

void free_retired_images(uint64_t completed) {
    std::vector<std::shared_ptr<GpuImage>> dead;
    {
        std::lock_guard lk(g_grave_lock);
        g_reuse_clock++;  // once per submit and present
        for (auto it = g_reusable.begin(); it != g_reusable.end();)
            if (it->second.first + 8 < g_reuse_clock) {
                g_grave.push_back({g_submitted + 1, std::move(it->second.second)});
                it = g_reusable.erase(it);
            } else {
                ++it;
            }
        if (g_reuse_clock % 3600 == 0 && g_reused) log("nvn/vk: %llu images reused", (unsigned long long)g_reused);
        auto done = std::stable_partition(g_grave.begin(), g_grave.end(),
                                          [&](const auto& e) { return e.first > completed; });
        for (auto it = done; it != g_grave.end(); ++it) dead.push_back(std::move(it->second));
        g_grave.erase(done, g_grave.end());
    }
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> staging;
    {
        std::lock_guard lk(g_grave_lock);
        auto done = std::stable_partition(g_staging.begin(), g_staging.end(),
                                          [&](const auto& e) { return std::get<0>(e) > completed; });
        for (auto it = done; it != g_staging.end(); ++it) staging.push_back({std::get<1>(*it), std::get<2>(*it)});
        g_staging.erase(done, g_staging.end());
    }
    for (auto& [b, m] : staging) p_vkDestroyBuffer(vk_device(), b, nullptr), p_vkFreeMemory(vk_device(), m, nullptr);
    for (auto& img : dead) {
        if (!img || !img->image) continue;
        if (img.use_count() > 1) continue;  // another live texture shares it (image_of): freed with the last one
        forget_image_views(*img);
        if (img->view) p_vkDestroyImageView(vk_device(), img->view, nullptr);
        p_vkDestroyImage(vk_device(), img->image, nullptr);
        if (img->memory) p_vkFreeMemory(vk_device(), img->memory, nullptr);
        img->image = VK_NULL_HANDLE, img->memory = VK_NULL_HANDLE, img->view = VK_NULL_HANDLE;
    }
}

// Images of destroyed textures are freed once no submit can use them: the one being recorded now included (a replay
// on another thread may hold it), hence the serial after the last submitted. Before the graveyard no image was ever
// freed: the opening creates textures continuously and the process reached 85 GB (the machine froze).
Texture::~Texture() {
    if (watched) plat::write_watch_remove(memory);
    if (!image) return;
    std::lock_guard lk(g_grave_lock);
    if (image->rendered && image->image && memory) {
        auto& slot = g_reusable[reuse_key(*this)];
        if (slot.second) g_grave.push_back({g_submitted + 1, std::move(slot.second)});
        slot = {g_reuse_clock, std::move(image)};
        return;
    }
    g_grave.push_back({g_submitted + 1, std::move(image)});
}

void retire_image(std::shared_ptr<GpuImage> img) {
    if (!img) return;
    std::lock_guard lk(g_grave_lock);
    g_grave.push_back({g_submitted + 1, std::move(img)});
}

// A host-visible buffer of its own for one big texture upload: the ring is 256 MB, and a scene load with ASTC decoded
// to RGBA8 staged 312 MB in one submit, wrapping over its own data before the GPU had copied it (DEBUG-2 #72). Freed
// once the submit being recorded is retired (as images in the grave). Falls back to the ring if allocation fails.
Upload staging_alloc(uint64_t size) {
    g_staging_count++, g_staging_bytes += size;
    VkDevice d = vk_device();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (p_vkCreateBuffer(d, &bi, nullptr, &buffer) != VK_SUCCESS) return upload_alloc(size, 16);
    VkMemoryRequirements req{};
    p_vkGetBufferMemoryRequirements(d, buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* p = nullptr;
    if (p_vkAllocateMemory(d, &ai, nullptr, &memory) != VK_SUCCESS || p_vkBindBufferMemory(d, buffer, memory, 0) ||
        p_vkMapMemory(d, memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
        p_vkDestroyBuffer(d, buffer, nullptr);
        if (memory) p_vkFreeMemory(d, memory, nullptr);
        return upload_alloc(size, 16);
    }
    std::lock_guard lk(g_grave_lock);
    g_staging.push_back({g_submitted + 1, buffer, memory});
    return {buffer, 0, static_cast<uint8_t*>(p)};
}

std::shared_ptr<GpuImage> reuse_image(const Texture& t) {
    // Off unless NX_IMAGE_REUSE=1: a CPU-written texture placed over a freed render target's memory would sample the
    // old render, and swizzle/alpha_from_r come from the old texture (review of #34). The pass breaks it avoided came
    // mostly from destroyed pool textures (fixed separately).
    // On by default (NX_IMAGE_REUSE=0 turns it off): the course's player samples AO/ambient targets of a destroyed
    // texture object two frames later (DEBUG.md #39); without the reused image they were black (a dark Mario).
    static const bool off = [] {
        const char* e = std::getenv("NX_IMAGE_REUSE");
        return e && e[0] == '0';
    }();
    if (!t.memory || off) return nullptr;
    std::lock_guard lk(g_grave_lock);
    auto it = g_reusable.find(reuse_key(t));
    if (it == g_reusable.end()) return nullptr;
    std::shared_ptr<GpuImage> img = std::move(it->second.second);
    g_reusable.erase(it);
    g_reused++;
    return img;
}

}  // namespace nx::nvn
