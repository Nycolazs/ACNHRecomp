// The Vulkan side of NVN, shared between nvn_vk.cpp (device, images, submit, present) and nvn_draw.cpp (pipelines,
// resources, draws). Everything here runs under the GPU lock taken by gpu_submit / gpu_present.
#pragma once

#include <atomic>
#include <ctime>
#include "nvn_per_thread.h"

#include <unordered_set>
#include <mutex>
#include <string>
#include <vector>

#include "nvn.h"
#include "vk_gen.h"

namespace nx::nvn {
// A cache array of each replaying thread (the replay thread's and the parallel replay workers', docs/PARALLEL_REPLAY.md),
// allocated on its first use: as a plain NX_REPLAY_LOCAL it would be in every thread's TLS block (~90 threads).
template <typename T, size_t N>
struct TlsArray {
    T* p = nullptr;
    T& operator[](size_t i) {
        if (!p) [[unlikely]] p = new T[N]();
        return p[i];
    }
    T* begin() { return &(*this)[0]; }
    T* end() { return &(*this)[0] + N; }
};
}  // namespace nx::nvn

namespace nx::nvn {

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;  // every level and layer
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0, height = 0, levels = 1, layers = 1, depth = 1;  // depth: 3D images only
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool rendered = false;  // written by the GPU: never re-uploaded from guest memory
    bool uploaded = false;  // filled from the texture's pool memory
    // Given texels by nvnTextureWriteTexels: those never reach guest memory here, so it is not uploaded from there
    // again (a pool flush over it wiped the light probes' direction map, docs/nvn/DEBUG.md #49).
    bool texels = false;
    VkComponentMapping swizzle{};  // for sampling (attachment views stay identity)
    bool storage = false;          // created with VK_IMAGE_USAGE_STORAGE_BIT (compute shaders write it)
    bool mutable_format = true;
    uint64_t rendered_at = 0;      // the pass-end serial of the last pass that rendered into it (nvn_draw.cpp, G4)    // views may use another format (MUTABLE_FORMAT; nvn_image.cpp, ubwc_flags)
    // An R8 texture always read as (1, 1, 1, R) (a glyph cache): stored as RGBA8 (255, 255, 255, R) and sampled
    // without swizzle. MoltenVK returned alpha 0 for that view swizzle on a 2D-array view, so all text was invisible.
    bool alpha_from_r = false;
    uint32_t nvn_format = 0;  // the texture's NVN format
    bool uw = false;          // a scaled screen-sized render target (nvn_ultrawide.cpp)
    float sx = 1, sy = 1;     // its size over the game's (width / base_w, height / base_h)
    // The last two frames (g_frame) it was rendered in: a target drawn in consecutive frames is redrawn every frame
    // (pipeline_skip_allowed may skip a draw into it while its pipeline compiles; one drawn once, a LUT, may not).
    uint64_t drawn_frame = 0, drawn_before = 0;
    uint64_t full_frame = 0;  // a widened target: the frame a draw last covered all of it (nvn_draw.cpp, layer_sides)
};

VkDevice vk_device();
bool vk_depth_clip_control();
// The core features the device was created with (all the GPU has): depthClamp, depthBiasClamp, depthBounds, ...
const VkPhysicalDeviceFeatures& vk_features();
bool gpu_require_subgroup32();  // compute pipelines pin the subgroup size to 32 (nvn_pipeline.cpp)
bool gpu_is_integrated();  // the device in use is an integrated GPU (set when the device is made)
VkPhysicalDevice vk_physical();
// nvn_gpu_pick.cpp: logs every device and returns the one NX_GPU asks for (default: the first discrete GPU).
VkPhysicalDevice pick_physical_device(const std::vector<VkPhysicalDevice>& devices);
uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want);

// The image behind an NVN texture (guest NVNtexture*), made on first use.
GpuImage* image_of(uint64_t texture);
// Presents replayed so far (the replay thread's frames, in queue order with the submits).
extern uint64_t g_replay_presents;
// NX_1D_IMAGES=1 (bisecting, nvn_pipeline.cpp): NVN 1D and 1D-array textures as Vulkan 1D images, the old way. By
// default they are 2D / 2D-array images of height 1, which the translated shaders (1D sampled as 2D) can view (G8).
bool vk_1d_images();
void transition(VkCommandBuffer cb, GpuImage& img, VkImageLayout to);
// nvn_present.cpp: the window texture drawn into the swapchain (rotated, fit, alpha 1) instead of blitted.
bool present_pass_wanted();
bool present_pass_targets(VkFormat format, const std::vector<VkImage>& images, VkExtent2D extent,
                          VkSurfaceTransformFlagBitsKHR transform);
bool present_pass_record(VkCommandBuffer cb, GpuImage& src, const int32_t* crop, uint32_t index);
void present_pass_release();  // its views of the swapchain images, before the swapchain is destroyed
bool present_pass_failed();   // the pass could not be made: frames are blitted, upright
GpuImage* present_pass_test_target();  // NX_PRESENT_TEST: an offscreen "display" for the pass (headless dumps)
// nvn_framegen.cpp: built-in frame generation (NX_FRAMEGEN=1): an in-between frame for each real one, shown first.
bool framegen_wanted();
void framegen_update();  // nvn_framegen.cpp: the setting read again (once a present)
bool framegen_frame_begin(uint64_t now_ns);  // per real frame: whether to make one this time (the pace allows it)
uint64_t framegen_real_delay_ns();           // how long after the in-between frame the real one is shown
// `src` (window texture `texture`) noted as the last frame; with `generate` and the last frame still there, the frame
// halfway between that one and `src` is made and returned (else null). *prev (optional): the last frame's image.
GpuImage* framegen_record(VkCommandBuffer cb, uint64_t texture, GpuImage& src, bool generate, GpuImage** prev);
// nvn_bc.cpp: BC textures decoded on the CPU where the GPU has no textureCompressionBC (NX_FORCE_BC_DECODE=1 anyway).
bool bc_host_decode(uint32_t nvn_format);         // a BC format this device cannot sample (BC6H excepted)
VkFormat bc_decoded_format(uint32_t nvn_format);  // what it is stored as instead
uint32_t bc_decoded_bytes(uint32_t nvn_format);   // bytes per texel of that
void bc_decode(uint32_t nvn_format, const uint8_t* blocks, uint32_t width, uint32_t height, uint32_t slices,
               uint8_t* out);  // tightly packed blocks -> tightly packed texels
bool bc_view_format(const GpuImage& img, uint32_t nvn, VkFormat& out);  // a view of a decoded image

// Per-submit upload memory (host visible, coherent), reset at every submit: data the replayed commands read from
// guest memory is copied here at replay time (vertex, index, uniform, storage data, texture staging).
struct Upload {
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t offset = 0;
    uint8_t* ptr = nullptr;
};
// What it is bound as, and GPU copies within it and dispatch arguments read from it (nvn_written.cpp).
constexpr VkBufferUsageFlags kUploadUsage =
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
Upload upload_alloc(uint64_t size, uint64_t align);
// nvn_frames.cpp: a buffer of its own for one big texture upload, freed when the submit being recorded retires.
Upload staging_alloc(uint64_t size);
// nvn_pipeline_cache.cpp: the on-disk pipeline cache, and the cost of what the replay creates (logged per window).
VkPipelineCache pipeline_cache();
void pipeline_cache_save();
void note_pipeline_created(uint64_t ns, bool worker);  // worker: compiled off the replay thread
void note_pipeline_alias();               // a new raw key that maps to a pipeline already built (canonical_key)
void note_texture_uploaded(uint64_t ns, uint64_t bytes);  // a texture copied from guest memory (the hitch lines)
void note_module_created(uint64_t ns);    // a shader module built (SPIR-V patches + vkCreateShaderModule)
void note_pipeline_waited(uint64_t ns);   // the replay waited this long for a compile in flight
void note_pipeline_skipped();             // a draw skipped because its pipeline compiles (NX_ASYNC_PIPELINES)
void note_pipeline_prewarmed();           // a pipeline of an earlier session sent to a worker
void note_astc_decoded(uint64_t ns);
void creation_log(uint64_t frames);
void present_hitch_check(uint64_t replay_ns, uint64_t upload_bytes, uint64_t present_ns, uint64_t fence_ns);  // a frame 50 ms after the previous: logged
Upload upload(const void* data, uint64_t size, uint64_t align);

// Descriptor sets for this submit (freed when it completes).
VkDescriptorSet descriptor_set(VkDescriptorSetLayout layout);
VkDescriptorPool make_descriptor_pool();
bool descriptor_layout_counts(VkDescriptorSetLayout layout, uint32_t (&by_type)[16]);  // descriptors of a layout, by VkDescriptorType (nvn_pipeline.cpp)

// A lost device or a submit running past 2 s: the process ends at once (docs/nvn/DEBUG.md #22).
[[noreturn]] void gpu_hung(const char* why);

// A compute shader's storage buffer, copied back to guest memory once the submit completed: (upload copy, guest
// address, size).
struct WriteBack {
    uint8_t* from;
    uint8_t* to;
    uint64_t size;
    // The guest bytes as they were when the copy was made: only what the GPU changed goes back (nvn_written.cpp).
    std::shared_ptr<std::vector<uint8_t>> before;
};
void apply_write_back(const WriteBack& w);  // nvn_written.cpp
// nvn_zero_copy.cpp (Android): [gpu, + bytes) bound in place in its pool's GPU-visible storage; false: copy it.
bool zero_copy_range(uint64_t gpu, uint64_t bytes, uint64_t align, Upload& out);
uint64_t zero_copy_uniform_align();
std::vector<WriteBack> draw_take_write_backs();  // nvn_draw.cpp: this submit's, for an asynchronous submit
void debug_dump_textures(uint64_t frame);  // nvn_debug.cpp: NX_DUMP_TEXTURES and the requested ones
void debug_dump_requested(uint64_t n);      // nvn_debug.cpp: NX_DUMP_ON_TARGET / NX_DUMP_ON_FS
void debug_dump_add(uint64_t texture);  // nvn_debug.cpp: read back at the end of this submit

// nvn_frames.cpp: submits that are not waited for (NX_PIPELINED, NX_ASYNC_GPU). Slots of command buffers, fences,
// descriptor pool and ring stretch; retired (fences waited, write-backs copied if not yet) on reuse, when the ring is
// full, or when something waits for them.
bool async_gpu();  // the slots are in use (either mode)
bool pipelined();  // NX_PIPELINED: fences not waited for, write-backs copied before gpu_submit returns
// The extensions and memory types that the Android performance plan depends on, logged once (nvn_caps.cpp).
void log_device_caps(VkPhysicalDevice physical, const std::vector<VkExtensionProperties>& extensions);
uint32_t upload_memory_type(uint32_t bits);
bool native_r11g11b10();
bool ubwc_flags();  // MUTABLE_FORMAT only with a format list (nvn_caps.cpp; Android)  // NVN 0x3f as B10G11R11_UFLOAT (nvn_caps.cpp; Android)  // the upload ring's memory type (nvn_caps.cpp)
void frame_init(VkCommandPool pool, uint32_t family, VkBuffer ring, uint8_t* ring_ptr, uint64_t ring_size);
VkCommandBuffer frame_begin(bool replay);  // NX_ASYNC_GPU, replay: submits with write-backs retired first
VkSemaphore frame_acquire_semaphore();
VkSemaphore frame_rendered_semaphore(uint32_t image);
void frame_submit(VkSemaphore wait, VkSemaphore signal, std::vector<WriteBack> write_backs, bool wait_done);
void frame_wait_all();
uint64_t frame_upload_used();
uint64_t frame_ring_size();  // the slots' upload ring (0: synchronous submits, no ring)
void write_back_before_read(const uint8_t* p, uint64_t size, const char* why = "");  // nvn_frames.cpp: a late write-back's bytes
bool late_pending(const uint8_t* p, uint64_t size);
void late_prepare(const uint8_t* p, uint64_t size);  // nvn_frames.cpp: before a storage copy reads guest bytes
bool late_copy(VkCommandBuffer cb, const uint8_t* p, uint64_t size, VkBuffer to, uint64_t to_offset);
bool ab_b(const char* name);  // nvn_vk.cpp: NX_AB=<name>, the B half of an in-run A/B test
uint64_t frame_completed();  // the serial of the last retired submit (for free_retired_images)
uint64_t frame_submitted();  // the serial of the latest submit queued (nvn_frames.cpp)
Upload frame_upload_alloc(uint64_t size, uint64_t align);
VkDescriptorSet frame_descriptor_set(VkDescriptorSetLayout layout);
// Pipelined: a submit with dispatches is split after the last one; its first part is queued at once and carries the
// write-backs. frame_plan_split counts the submit's DispatchCompute commands and returns the last top-level list that
// has one (SIZE_MAX: no split). frame_split ends the first part and returns the command buffer of the rest (once per
// submit; outside the mode it returns `cb`). frame_after_dispatch is for replay_list, after each DispatchCompute: it
// splits right after the last one. frame_cb: the command buffer to go on with (after a call that may have split).
size_t frame_plan_split(const std::vector<std::shared_ptr<CmdList>>& lists);
VkCommandBuffer frame_split(VkCommandBuffer cb);
VkCommandBuffer frame_after_dispatch(VkCommandBuffer cb);
VkCommandBuffer frame_cb(VkCommandBuffer cb);
// Chunks (nvn_frames.cpp, NX_SUBMIT_CHUNK_DRAWS): for replay_list before each command, with the draws done so far;
// returns the command buffer to go on with (a new part once enough draws were recorded).
VkCommandBuffer frame_chunk(VkCommandBuffer cb, uint64_t draws, bool pass_open);
VkCommandBuffer frame_pass_part(VkCommandBuffer cb);  // NX_SUBMIT_CHUNK_DRAWS=-1: a new part before a pass begins
// Host time inside vkQueueSubmit (with vkEndCommandBuffer: Metal encoding) and inside vkWaitForFences, running
// totals in every mode (the synchronous path adds its own).
struct FrameTimes {
    uint64_t encode_ns = 0, fence_ns = 0;
};
FrameTimes& frame_times();
void frame_log(uint64_t frames);  // the mode's counters, with the timing log
void free_retired_images(uint64_t completed);
// A GPU-written image of a destroyed texture with the same memory and shape (nvn_frames.cpp), or null.
std::shared_ptr<GpuImage> reuse_image(const Texture& t);  // images of textures destroyed before submit `completed` + 1
void retire_image(std::shared_ptr<GpuImage> img);  // freed once no submit can use it (a resized screen target)

// nvn_debug.cpp: NX_DUMP_FRAMES, the presented texture read back (recorded into the present's command buffer, written
// after its fence).
uint32_t frame_dump_every();
bool frame_dump_due(uint64_t presents);  // NX_DUMP_FRAMES, or a dump_frames file in the data folder
bool frame_dump_record(VkCommandBuffer cb, GpuImage& img);
void frame_dump_write(const GpuImage& img, uint64_t frame, const char* suffix = "");  // NNNNN<suffix>.ppm
bool frame_readback_lit(const GpuImage& img);  // the frame read back has something on it (not the boot's black)
bool debug_snapshots_pending();  // NX_SNAP_*: this submit recorded snapshots (it is then waited for)

// nvn_draw.cpp: replays one draw-related command; ends the render pass it may have open when asked.
struct Targets {
    uint64_t colors[8] = {};
    uint32_t color_count = 0;
    uint64_t depth = 0;
    TargetViews views;  // level and layer of each (index 8: depth)
};
// Field by field: a memcmp also compared the padding after color_count, which a copy need not keep.
inline bool targets_same(const Targets& a, const Targets& b) {
    if (a.color_count != b.color_count || a.depth != b.depth) return false;
    for (uint32_t k = 0; k < 9; k++) {
        if (k < 8 && a.colors[k] != b.colors[k]) return false;
        if (a.views.level[k] != b.views.level[k] || a.views.layer[k] != b.views.layer[k] ||
            a.views.format[k] != b.views.format[k])
            return false;
    }
    return true;
}
void draw_begin_submit();
void draw_end_submit();  // after the GPU finished the submit
void draw_log_stats();
void draw_command(VkCommandBuffer cb, const Cmd& c, const CmdList& list, const Targets& targets);
// reason (counted in the `nvn passes ended` log): 0 submit end, 1 draw, 2 dispatch, 3 clear of other targets,
// 4 clear larger than the pass, 5 copy, 6 snapshot, 7 SetRenderTargets (NX_TARGETS_BREAK).
void draw_end_rendering(VkCommandBuffer cb, int reason = 0);  // before transfers (clears, copies, present) and at the end
// ClearColor / ClearDepthStencil inside the render pass of the current targets (nvn_draw.cpp).
void draw_clear(VkCommandBuffer cb, const Targets& targets, const Cmd& c);  // nvn_clears.cpp
// nvn_clears.cpp: clears pending as the next pass's loadOps; nvn_draw.cpp: the open pass, for it.
bool clears_pending_image(const GpuImage* img);
bool clears_pending_other(const Targets& t);
uint32_t clears_apply(const Targets& t, VkRenderingAttachmentInfo* colors, VkRenderingAttachmentInfo& depth,
                      VkRenderingAttachmentInfo& stencil);
void clears_flush(VkCommandBuffer cb);  // outside a pass: the pending clears in a pass of their own
void clears_begin_submit();
void clears_log();
bool pass_open();  // a render pass is being recorded
// Render pass and framebuffer objects instead of dynamic rendering (nvn_renderpass.cpp, NX_RENDER_PASS_OBJECTS; always
// on a device without dynamic rendering, such as the Adreno 6xx's own Vulkan 1.1 driver).
bool render_pass_objects();
bool vk_dynamic_rendering();
void hang_note(const ShaderInfo* vs, const ShaderInfo* fs);  // a draw's program, for the GPU hang report
VkFormat unused_slot_format();  // a pipeline's format for a colour slot with no image
VkRenderPass compatible_render_pass(uint32_t count, const VkFormat* colors, VkFormat depth);
void begin_render_pass(VkCommandBuffer cb, uint32_t count, const VkFormat* formats,
                       const VkRenderingAttachmentInfo* colors, VkFormat depth_format,
                       const VkRenderingAttachmentInfo& depth, const VkRenderingAttachmentInfo& stencil,
                       VkExtent2D extent);
void forget_framebuffers(VkImageView view);
void render_pass_log();
void pass_end_only(VkCommandBuffer cb, int reason);  // draw_end_rendering without the pending clears
bool pass_open_on(const Targets& t);
VkExtent2D pass_extent();
void pass_begin(VkCommandBuffer cb, const Targets& t);
void pass_mark_written(uint32_t bits);
void draw_merge_flush();  // the held-back merged draw, before anything else is recorded (nvn_draw.cpp)
// nvn_mask_clear.cpp: colour target `slot` of the open pass on `t` cleared to `rgba` in `rect`, only the channels in
// `mask` (bit 0 r .. 3 a) written, by a fullscreen draw. False (nothing recorded) for integer targets or no pipeline.
bool mask_clear(VkCommandBuffer cb, const Targets& t, uint32_t slot, uint32_t mask, const float rgba[4],
                const VkRect2D& rect);
// nvn_replay.cpp: a command list into `cb` (`depth`: CallList nesting).
void replay_list(VkCommandBuffer cb, Targets& st, const CmdList& list, int depth);
CmdList* submit_command_list(uint64_t handle);
bool parallel_plan_on();  // nvn_parallel.cpp (docs/PARALLEL_REPLAY.md)
void parallel_plan_frame(const std::vector<std::shared_ptr<CmdList>>& lists);  // command_list(), once per handle per submit (kept alive by it)
void submit_lists_reset();  // at each submit's start (draw_begin_submit)

void replay_copy(VkCommandBuffer cb, const Cmd& c, const CmdList& list);  // nvn_copy.cpp: texture copies
void replay_buffer_copy(VkCommandBuffer cb, const Cmd& c, const CmdList& list);  // nvn_copy.cpp: buffer -> texture
void forget_image_views(const GpuImage& img);
// nvn_debug.cpp: NX_SNAP_DRAWS mid-frame snapshots of colour target 0.
bool debug_snap_wanted(uint64_t draw);
// nvn_debug.cpp: NX_ZERO_DRAWS, shader pairs whose draws produce no pixel.
bool zero_draws_on();
void zero_draws_begin_submit(VkCommandBuffer cb);
bool zero_draws_begin(VkCommandBuffer cb, const std::string& label);
void zero_draws_end(VkCommandBuffer cb);
void zero_draws_collect();
void zero_draws_log();
void frame_sig_flush(uint64_t presents);  // NX_FRAME_SIG=1: the frame's draw signature, logged (nvn_draw_trace.cpp)
void debug_snapshot(VkCommandBuffer cb, GpuImage& img, uint64_t draw);
extern uint32_t g_snap_layer, g_snap_level;  // the layer and level debug_snapshot reads (default 0)
void debug_snapshots_write();  // nvn_pipeline.cpp: before an image is freed

// nvn_debug.cpp: GPU time per render pass (NX_GPU_PROFILE=1).
bool gpu_profile_on();  // NX_GPU_PROFILE=1: per-pass labels, every submit waited for (nvn_debug.cpp)
// NX_GPU_PROFILE=3: the GPU's busy time per frame without waits, from per-part timestamps (nvn_gpu_busy.cpp).
// The janitor thread (nvn_janitor.cpp, NX_JANITOR): pool resets of retired slots, off the replay thread.
bool janitor_on();
void janitor_run(std::function<void()> job);
void janitor_wait(const std::atomic<bool>& done);
bool gpu_busy_on();
void gpu_busy_part_begin(uint32_t slot, uint32_t part, VkCommandBuffer cb);
void gpu_busy_part_end(uint32_t slot, uint32_t part, VkCommandBuffer cb);
void gpu_busy_retired(uint32_t slot, uint64_t queued);
void gpu_busy_log(uint64_t frames);
// nvn_vk_defer.cpp: the replay's Vulkan recording on a thread of its own. vk_defer_install swaps the entry points it
// covers (after the device is made); inside a VkDeferScope the replay thread's calls go to the recorder; vk_defer_drain
// waits until the recorder ran everything queued.
void vk_defer_install();
void vk_defer_drain();
bool vk_defer_active();  // this thread's Vulkan calls go to the recorder now
class VkDeferScope {
public:
    VkDeferScope();
    ~VkDeferScope();
    VkDeferScope(const VkDeferScope&) = delete;
    VkDeferScope& operator=(const VkDeferScope&) = delete;
private:
    bool on_ = false;
};
void gpu_busy_note_pass(const char* label);  // the label of the part being recorded, if it has none yet
bool gpu_profile_draws();  // NX_GPU_PROFILE=2: and a mark per draw
void gpu_profile_begin(VkCommandBuffer cb);
void gpu_profile_mark(VkCommandBuffer cb, const std::string& label);
void gpu_profile_collect();
void gpu_profile_log(uint64_t frames);

// Parallel replay (docs/PARALLEL_REPLAY.md, nvn_parallel.cpp): set on a worker thread while it replays a chunk. A worker
// never creates an image or changes one's state (layout, rendered, uploads): where the replay would, worker_bail()
// marks the chunk, the worker stops, and the replay thread replays the chunk itself in order.
extern PerThread<bool> t_parallel_worker;
extern PerThread<bool> t_bail;
extern PerThread<void*> t_worker_slot;  // the frame slot the worker records for (nvn_frames.cpp)
extern PerThread<int> t_worker_index;
void* frame_current_slot();
VkCommandBuffer frame_worker_begin(void* slot, int worker);  // on the worker
void frame_worker_end(VkCommandBuffer cb);
bool frame_queue_worker_cbs(const VkCommandBuffer* cbs, uint32_t n);  // on the replay thread, in order
VkCommandBuffer frame_cut(VkCommandBuffer cb);   // the replay thread's part queued, the next begun
void draw_begin_worker(bool fresh);  // nvn_draw.cpp: a worker's own draw state for a chunk
// nvn_replay.cpp: one command as replay_list replays it (not CallList); cb may change (the submit's parts).
void replay_cmd(VkCommandBuffer& cb, Targets& st, const Cmd& c, const CmdList& list);
bool parallel_replay_on();  // nvn_parallel.cpp
inline uint64_t thread_cpu_ns() {  // the calling thread's CPU time; 0 where there is none to read (Windows)
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#else
    return 0;
#endif
}
void parallel_replay(VkCommandBuffer& cb, Targets& st, const std::vector<std::shared_ptr<CmdList>>& lists,
                     size_t split_after);
inline std::atomic<const char*> g_bail_reason{nullptr};  // the latest worker_bail's, for the log
inline bool worker_bail(const char* why = "other") {
    if (!*t_parallel_worker) return false;
    *t_bail = true;
    g_bail_reason.store(why, std::memory_order_relaxed);
    return true;
}
// nvn_clears.cpp: a worker's clear-on-bind decisions (the clears to skip), taken by the replay thread's plan.
extern PerThread<const std::unordered_set<const Cmd*>*> t_clear_once_skip;
bool clear_once_plan(const Targets& t, const Cmd& c);
std::recursive_mutex& cache_mutex();  // nvn_pipeline.cpp: views, layouts, samplers, pipelines

namespace replay {
void dedup_reset();  // nvn_dedup.cpp
// Bumped whenever what an upload of guest bytes would give may change: dedup reset or invalidated, compute-written
// ranges or late write-backs added or dropped (nvn_dedup.cpp, nvn_written.cpp, nvn_frames.cpp). The replay thread's.
extern PerThread<uint64_t> g_upload_epoch;
bool written_separate();         // nvn_written.cpp (nvn_draw_state.h)
uint64_t written_bind_count();   // nvn_written.cpp: written_bind calls so far (a dispatch bound storage it writes)
void trace_nan_write_back(const uint8_t* from, const uint8_t* to, uint64_t size);  // nvn_draw_trace.cpp
}

}  // namespace nx::nvn
