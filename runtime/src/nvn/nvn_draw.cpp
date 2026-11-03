// Draws (docs/NVN.md, N6): replays the bind, state and draw commands of nvn_state.cpp into Vulkan. Everything a draw
// reads from guest memory (vertices, indices, uniform and storage buffers, textures) is copied at replay time into
// the submit's upload buffer: the replay runs right when the game submits and is waited for, so the memory holds
// what the GPU would have read. Render targets become a dynamic-rendering pass, begun at the first draw and ended
// before any transfer.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <map>
#include <set>

#include "nvn_draw_state.h"
#include "nvn_ultrawide.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

// The replay state shared with nvn_draw_trace.cpp (nvn_draw_state.h).
namespace replay {

// Per replaying thread: the replay thread's, and each parallel replay worker's own (docs/PARALLEL_REPLAY.md).
PerThread<State> g_state;
PerThread<Stats> g_stats;
PerThread<uint64_t> g_state_gen;

const uint8_t* guest_bytes(uint64_t gpu) {
    uint64_t off = 0;
    MemoryPool* p = pool_at_gpu(gpu, &off);
    uint64_t host = p ? pool_host(*p, off) : 0;
    return host ? reinterpret_cast<const uint8_t*>(host) : nullptr;
}

}  // namespace replay

using namespace replay;

namespace {

PerThread<bool> g_rendering;
PerThread<bool> g_render_pass;
PerThread<bool> g_vertex_pending;  // a narrow pass-end barrier since the last one that covers vertex stages (G4)
PerThread<uint64_t> g_pass_serial, g_vertex_visible_at;  // pass ends so far; the serial a full barrier covered
PerThread<uint64_t> g_vertex_barriers;

// Off by default (NX_PASS_BARRIER_NARROW=1 turns it on): the first S23 run with it showed a black band over the course
// background in 2 of 28 frames (phone_n6def, 15000 and 16200), a timing-dependent hazard of the DEBUG-2 #71 class.
bool narrow_pass_barrier() {
    static const bool on = std::getenv("NX_PASS_BARRIER_NARROW") != nullptr;
    return on;
}

// A vertex shader sampling a rendered image after a narrow pass-end barrier: the pass ends (it cannot take a barrier
// inside) and the writes so far are made visible to the vertex stages.
void vertex_read_barrier(VkCommandBuffer cb, const ShaderInfo* vs);  // the open pass is a render pass object (nvn_renderpass.cpp), not dynamic rendering
PerThread<Targets> g_pass;           // what the open pass renders to
PerThread<GpuImage* [9]> g_pass_images;  // their images (8: depth), resolved once per pass for gather_textures
PerThread<uint32_t> g_pass_written;  // targets the open pass's draws wrote: bit c colour c, bit 8 depth
PerThread<uint64_t[8]> g_pass_ends;  // passes ended, by draw_end_rendering reason
uint64_t g_submit_ns = 0;  // when the submit being replayed started
PerThread<VkExtent2D> g_pass_extent;

// What this submit's command buffer already has bound and set. Vulkan keeps the pipeline, vertex and index buffers
// and dynamic state of a command buffer across its dynamic-rendering passes, and most draws repeated what the draw
// before them set (the vertex streams and the viewport of a whole pass): only what differs is recorded. Forgotten
// at each submit, on another command buffer, and when something else binds a pipeline there (the masked clear's
// fullscreen draw, nvn_mask_clear.cpp). NX_DRAW_SLOW: forgotten before every draw.
struct Bound {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t vertex_set = 0;  // bit s: binding s holds vertex[s]
    Upload vertex[16];
    bool has_index = false, has_dynamic = false;
    Upload index;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    VkViewport viewport{};
    VkRect2D scissor{};
    float blend[4] = {};
    uint32_t raster[10] = {};  // raster_dynamic's inputs: stencil reference and masks per face, the depth bias
};
PerThread<Bound> g_bound;
PerThread<uint64_t> g_bound_gen;  // bumped with every reset of g_bound (bind_sets' repeat check, nvn_sets.cpp)
PerThread<uint64_t> g_state_set, g_state_kept;  // state commands recorded and left out, for the log

template <typename T>
bool same_bytes(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }

void bind_vertex(VkCommandBuffer cb, uint32_t binding, const Upload& u) {
    Upload& now = (*g_bound).vertex[binding];
    if (((*g_bound).vertex_set >> binding & 1) && now.buffer == u.buffer && now.offset == u.offset) {
        (*g_state_kept)++;
        return;
    }
    VkDeviceSize off = u.offset;
    p_vkCmdBindVertexBuffers(cb, binding, 1, &u.buffer, &off);
    now = u, (*g_bound).vertex_set |= 1u << binding, (*g_state_set)++;
}

// Viewport, scissor, blend constants and the rasterizer's dynamic state (every pipeline declares them dynamic).
void set_dynamic(VkCommandBuffer cb, const VkViewport& vp, const VkRect2D& sc) {
    Bound& b = (*g_bound);
    const State& s = (*g_state);
    uint32_t raster[10] = {s.stencil_ref[0],        s.stencil_ref[1],        s.stencil_compare_mask[0],
                          s.stencil_compare_mask[1], s.stencil_write_mask[0], s.stencil_write_mask[1]};
    std::memcpy(raster + 6, &s.bias_factor, 4), std::memcpy(raster + 7, &s.bias_units, 4);
    std::memcpy(raster + 8, &s.bias_clamp, 4);
    bool slack = s.depth.depth_test && equal_slack_pass(s.depth);
    raster[9] = slack;
    bool all = !b.has_dynamic;
    b.has_dynamic = true;
    auto differs = [&](bool d) { return (all || d) ? ((*g_state_set)++, true) : ((*g_state_kept)++, false); };
    if (differs(!same_bytes(b.viewport, vp))) p_vkCmdSetViewport(cb, 0, 1, &vp), b.viewport = vp;
    if (differs(!same_bytes(b.scissor, sc))) p_vkCmdSetScissor(cb, 0, 1, &sc), b.scissor = sc;
    if (differs(std::memcmp(b.blend, s.blend_color, sizeof b.blend) != 0))
        p_vkCmdSetBlendConstants(cb, s.blend_color), std::memcpy(b.blend, s.blend_color, sizeof b.blend);
    if (differs(std::memcmp(b.raster, raster, sizeof raster) != 0))  // stencil reference and masks, depth bias
        raster_dynamic(cb, slack), std::memcpy(b.raster, raster, sizeof raster);  // (nvn_raster.cpp)
}

// A 16:9 layer laid over a widened target (the UI's composite quad, squeezed into the middle): when nothing else covered
// the target this frame (no scene: a loading screen, the NookPhone's apps over a still world), it kept an older
// picture, which differed between the targets the game takes in turn: strips at the edges flickered. Before the first
// such layer of the frame the target is cleared black.
void layer_sides(VkCommandBuffer cb, const Targets& t, bool indexed, const Cmd& c) {
    if (!t.color_count || !t.colors[0] || !(*g_rendering) || (*t_parallel_worker)) return;
    GpuImage* img = image_of(t.colors[0]);
    const uint64_t frame = g_frame.load(std::memory_order_relaxed);
    if (!img || !img->uw || img->full_frame == frame || t.views.level[0] != 0) return;
    // The layer's quad: its first four positions span 0..1 (the whole layer).
    const auto& st = (*g_state);
    if (st.attribs.empty() || st.streams.empty()) return;
    const VertexAttrib& a = st.attribs[0];
    const uint32_t stride = a.stream < st.streams.size() ? st.streams[a.stream].stride : 0;
    const Range& vb = st.vertex_buffers[a.stream < 16 ? a.stream : 0];
    const uint64_t first = uint64_t(std::max(0, indexed ? c.i[1] : c.i[0]));
    if (stride < 8 || !vb.address) return;
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (uint32_t v = 0; v < 4; v++) {
        const uint64_t at = (first + v) * stride + a.offset;
        if (at + 8 > vb.size) return;
        float p[2];
        std::memcpy(p, guest_bytes(vb.address + at), 8);
        for (int k = 0; k < 2; k++) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
    }
    if (std::fabs(lo[0]) > 0.01f || std::fabs(lo[1]) > 0.01f || std::fabs(hi[0] - 1) > 0.01f || std::fabs(hi[1] - 1) > 0.01f)
        return;
    img->full_frame = frame;
    const VkExtent2D e = pass_extent();
    const uint32_t w = std::min(img->width, e.width), h = std::min(img->height, e.height);
    VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    ca.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkClearRect r{{{0, 0}, {w, h}}, 0, 1};
    p_vkCmdClearAttachments(cb, 1, &ca, 1, &r);
    static int logged = 0;
    if (logged++ < 4) log("nvn: a 16:9 layer over target %llx (%ux%u) with nothing else drawn this frame: cleared first",
                          (unsigned long long)t.colors[0], w, h);
}

// A window texture widened to the screen's shape (video.aspect = auto, nvn_ultrawide.cpp): what the game draws in
// 16:9 only (loading screens, the NookPhone's apps) leaves the sides as they were, and the window textures it takes
// in turn kept different leftovers there: strips at the edges flickered. The first pass on one after each present
// clears its sides black; with a 3D scene the composite covers them again.
void clear_window_sides(VkCommandBuffer cb, const Targets& t, VkExtent2D extent, uint32_t cleared) {
    if (!t.color_count || !t.colors[0] || (*t_parallel_worker)) return;
    Texture* tex = get<Texture>(t.colors[0]);
    if (!tex || !(tex->flags & 1) || !tex->uw || tex->sides_cleared == g_replay_presents + 1) return;
    tex->sides_cleared = g_replay_presents + 1;  // a pass that cleared all of it counts too (not cleared again later)
    GpuImage* img = image_of(t.colors[0]);
    if ((cleared & 1) || !img || t.views.level[0] != 0 || img->width <= img->height * 16 / 9 + 2) return;
    const uint32_t mid = std::min(img->width, img->height * 16 / 9), side = (img->width - mid) / 2;
    if (!side || extent.width < img->width || extent.height < img->height) return;
    VkClearAttachment a{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    a.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkClearRect r[2] = {{{{0, 0}, {side, img->height}}, 0, 1},
                        {{{int32_t(img->width - side), 0}, {side, img->height}}, 0, 1}};
    p_vkCmdClearAttachments(cb, 1, &a, 2, r);
}

void begin_rendering(VkCommandBuffer cb, const Targets& t) {
    if ((*t_bail)) return;
    if (clears_pending_other(t)) clears_flush(cb);  // other targets' clears first, in order
    // An image never written (layout UNDEFINED) has nothing to load: loadOp CLEAR to zero, not a LOAD of whatever
    // the memory held (G3, docs/nvn/DEBUG.md #50); a pending clear still wins (clears_apply). Env: bisecting.
    static const bool load_undefined = std::getenv("NX_UNDEFINED_LOAD") != nullptr;
    uint32_t fresh = 0;  // bit c colour c, bit 8 depth: begun cleared to 0 (depth 0, stencil 0)
    VkRenderingAttachmentInfo colors[8]{};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkExtent2D extent{UINT32_MAX, UINT32_MAX};
    VkFormat formats[8]{};  // the views' (render pass objects)
    for (uint32_t k = 0; k < t.color_count; k++) {
        colors[k].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        GpuImage* img = t.colors[k] ? image_of(t.colors[k]) : nullptr;
        (*g_pass_images)[k] = img;
        if (!img) continue;
        // A worker of the parallel replay renders only into images already rendered (GENERAL): the first time is the
        // replay thread's (docs/PARALLEL_REPLAY.md).
        if ((img->layout != VK_IMAGE_LAYOUT_GENERAL || !img->rendered) && worker_bail("target not rendered yet")) continue;
        if (img->layout == VK_IMAGE_LAYOUT_UNDEFINED && !load_undefined) fresh |= 1u << k;
        transition(cb, *img, VK_IMAGE_LAYOUT_GENERAL);  // rendered images stay GENERAL (sampled while rendered)
        if (!img->rendered) img->rendered = true;
        if (const uint64_t f = g_frame.load(std::memory_order_relaxed); img->drawn_frame != f)
            img->drawn_before = img->drawn_frame, img->drawn_frame = f;
        static int logged_3d = 0;
        static const bool trace_3d = std::getenv("NX_TRACE_3D") != nullptr;
        if (trace_3d && img->view_type == VK_IMAGE_VIEW_TYPE_3D && logged_3d++ < 16)
            log("nvn: render into 3D texture %llx (%ux%ux%u) slice %u level %u", (unsigned long long)t.colors[k],
                img->width, img->height, img->depth, t.views.layer[k], t.views.level[k]);
        uint32_t level = std::min<uint32_t>(t.views.level[k], img->levels - 1);
        formats[k] = view_format(*img, t.views.format[k]);
        colors[k].imageView = attachment_view(*img, level, t.views.layer[k], formats[k]);
        // NX_TRACE_CUBES=<first frame>: writes into cube images from that frame on. With 400 from the start, the log
        // ran out at the first 3D scene (frame ~171) and never reached a course load.
        static const char* trace_cubes = std::getenv("NX_TRACE_CUBES");
        static const uint64_t cube_from = trace_cubes ? std::strtoull(trace_cubes, nullptr, 10) : 0;
        static int cube_logs = 0;
        if (trace_cubes && (img->view_type == VK_IMAGE_VIEW_TYPE_CUBE || img->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) &&
            g_frame >= cube_from && cube_logs++ < 2000)
            log("cubes: render into %llx level %u layer %u (%ux%u, %u levels, %u layers) view %s",
                (unsigned long long)t.colors[k], level, t.views.layer[k], img->width, img->height, img->levels,
                img->layers, colors[k].imageView == img->view ? "WHOLE IMAGE (fallback)" : "ok");
        colors[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        colors[k].loadOp = fresh >> k & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent = {std::min(extent.width, std::max(1u, img->width >> level)),
                  std::min(extent.height, std::max(1u, img->height >> level))};
    }
    GpuImage* dimg = t.depth ? image_of(t.depth) : nullptr;
    (*g_pass_images)[8] = dimg;
    if (dimg && dimg->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
        if ((dimg->layout != VK_IMAGE_LAYOUT_GENERAL || !dimg->rendered) && worker_bail("depth not rendered yet")) return;
        if (dimg->layout == VK_IMAGE_LAYOUT_UNDEFINED && !load_undefined) fresh |= 1u << 8;
        transition(cb, *dimg, VK_IMAGE_LAYOUT_GENERAL);
        if (!dimg->rendered) dimg->rendered = true;
        if (const uint64_t f = g_frame.load(std::memory_order_relaxed); dimg->drawn_frame != f)
            dimg->drawn_before = dimg->drawn_frame, dimg->drawn_frame = f;
        uint32_t level = std::min<uint32_t>(t.views.level[8], dimg->levels - 1);
        depth.imageView = attachment_view(*dimg, level, t.views.layer[8]);
        depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth.loadOp = fresh >> 8 & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.clearValue.depthStencil = {1.0f, 0};  // far (a fresh depth buffer; the course passes test LESS)
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent = {std::min(extent.width, std::max(1u, dimg->width >> level)),
                  std::min(extent.height, std::max(1u, dimg->height >> level))};
    } else {
        dimg = nullptr;
    }
    if (extent.width == UINT32_MAX) extent = {1, 1};
    VkRenderingAttachmentInfo stencil = depth;  // same view, its own loadOp
    uint32_t cleared = clears_apply(t, colors, depth, stencil);  // pending clears of these targets: loadOp CLEAR
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = t.color_count;
    ri.pColorAttachments = colors;
    if (dimg) {
        if (dimg->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &depth;
        if (dimg->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &stencil;
    }
    if ((*t_bail)) return;  // a worker gave up on a target: no pass
    (*g_render_pass) = render_pass_objects();
    if ((*g_render_pass))
        begin_render_pass(cb, t.color_count, formats, colors, dimg ? dimg->format : VK_FORMAT_UNDEFINED, depth, stencil,
                          extent);
    else
        p_vkCmdBeginRendering(cb, &ri);
    (*g_rendering) = true;
    (*g_pass_written) = cleared | fresh;  // as a clear inside the pass: sampling it needs it stored first
    trace_pass("begin: cleared, fresh", t, cleared, fresh);
    (*g_pass) = t;
    (*g_pass_extent) = extent;
    clear_window_sides(cb, t, extent, cleared | fresh);
}

uint64_t g_dump_draws = 0, g_dump_request = 0;

// DispatchCompute: the bound program's compute stage over (x, y, z) groups, outside render passes, then a barrier
// so what it wrote is visible to what follows.
void dispatch(VkCommandBuffer cb, const Cmd& c) {
    static const bool off = std::getenv("NX_NO_COMPUTE") != nullptr;  // debug: skip compute dispatches
    if (off) return;
    sets_keep_texture_set(VK_NULL_HANDLE);  // the draw fast path's set 2 is a graphics layout's
    const auto* shaders = program_shaders((*g_state).program);
    const ShaderInfo* cs = nullptr;
    if (shaders)
        for (const ShaderInfo* s : *shaders)
            if (s->stage == ShaderStage::Compute) cs = s;
    if (!cs) return;
    uint32_t x = uint32_t(c.i[0]), y = uint32_t(c.i[1]), z = uint32_t(c.i[2]);
    const uint8_t* args = c.n == 1 ? guest_bytes(c.a[0]) : nullptr;  // indirect: {x, y, z}
    if (c.n == 1 && !args) return;
    // Written by an earlier dispatch of this submit: only the GPU knows them (vkCmdDispatchIndirect, below).
    bool gpu_args = args && !written_separate() && written_overlaps(args, 12);
    if (args && !gpu_args) std::memcpy(&x, args, 4), std::memcpy(&y, args + 4, 4), std::memcpy(&z, args + 8, 4);
    if (!gpu_args && (!x || !y || !z)) return;
    trace_compute_once(cs, x, y, z);  // 0, 0, 0 for GPU arguments
    trace_nan_dispatch(cs);
    std::vector<const ShaderInfo*> stages{cs};
    const ProgramLayout& layout = program_layout(stages);
    VkPipeline pipeline = compute_pipeline(cs, layout);
    if (!pipeline) return;
    draw_end_rendering(cb, 2);
    gather_textures(stages);
    build_sets(cb, stages, layout);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    bind_sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout);
    if (gpu_profile_on()) gpu_profile_mark(cb, "compute " + cs->hash);
    if (!gpu_args || !written_indirect(cb, args)) p_vkCmdDispatch(cb, x, y, z);
    if (gpu_profile_on()) gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    (*g_stats).dispatches++;
}

void vertex_read_barrier(VkCommandBuffer cb, const ShaderInfo* vs) {
    if (!(*g_vertex_pending) || !vs) return;
    bool reads = false;
    for (size_t k = 0; k < (*g_textures).bindings.size() && !reads; k++)
        if ((*g_textures).bindings[k].first == vs && k < (*g_textures).resolved.size() && (*g_textures).resolved[k].texture)
            if (GpuImage* img = image_of((*g_textures).resolved[k].texture))
                reads = img->rendered && img->rendered_at > (*g_vertex_visible_at);
    if (!reads) return;
    draw_end_rendering(cb, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    (*g_vertex_pending) = false, (*g_vertex_barriers)++, (*g_vertex_visible_at) = (*g_pass_serial);
}

// NX_DRAW_TIMING=1: where a draw's CPU time goes, by part (the timer register, ~40 ns ticks on Apple silicon; averaged
// over thousands of draws), logged every 300 submits as microseconds per submit.
const char* const kDrawParts[] = {"textures", "build_sets", "inputs", "pass+key+pipeline", "bind", "draw+after",
                                  "bs:buffers", "bs:textures", "bind:resolve", "bind:update", "tex:gather"};
constexpr int kDrawPartCount = 11;
uint64_t g_draw_part[kDrawPartCount] = {}, g_draw_mark = 0, g_draw_timed = 0;
std::map<std::string, uint64_t> g_draws_by_target;
bool draw_timing() {
    static const bool on = std::getenv("NX_DRAW_TIMING") != nullptr;
    return on;
}
inline uint64_t draw_ticks() {
#if defined(__aarch64__) && !defined(_MSC_VER)
    uint64_t v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return plat::monotonic_ns();
#endif
}
}  // namespace
// NX_AB=1 (with NX_DRAW_TIMING): a change under test runs every other 300 submits (ab_new() true), the old code in
// between, and the timing lines say which: both halves see the same heat (an M1 Air throttles within minutes).
bool g_ab_new = true;
bool ab_new() { return g_ab_new; }
void draw_part(int k) {
    if (!draw_timing()) return;
    const uint64_t now = draw_ticks();
    g_draw_part[k] += now - g_draw_mark, g_draw_mark = now;
}
namespace {

// The draw's viewport and scissor: the guest's, scaled to a screen target.
void draw_viewport(float ui_x, VkViewport& vp, VkRect2D& sc) {
    vp = VkViewport{0, 0, float((*g_pass_extent).width), float((*g_pass_extent).height), 0, 1};
    if ((*g_state).has_viewport)
        vp = {float((*g_state).viewport[0]), float((*g_state).viewport[1]), float((*g_state).viewport[2]),
              float((*g_state).viewport[3]), (*g_state).depth_near, (*g_state).depth_far};
    // NVN's upper-left window origin (what Wonder sets) maps NDC +Y to the top of the render target, like D3D;
    // Vulkan maps it to the bottom. A negative-height viewport gives NVN's mapping, and with it NVN's front faces.
    // (Before this, the front face was inverted instead: triangles were culled right, but every render target was
    // upside down, which the post chain undid in some scenes and not in others: the first course showed a
    // mirrored sky.)
    sc = VkRect2D{{0, 0}, (*g_pass_extent)};
    if ((*g_state).has_scissor)
        sc = {{std::max(0, (*g_state).scissor[0]), std::max(0, (*g_state).scissor[1])},
              {uint32_t(std::max(0, (*g_state).scissor[2])), uint32_t(std::max(0, (*g_state).scissor[3]))}};
    if ((*g_uw).on) {  // the guest's pixel spans, scaled
        if ((*g_state).has_viewport) {
            int32_t x = int32_t(vp.x), w = int32_t(vp.width), y = int32_t(vp.y), h = int32_t(vp.height);
            uw_span(x, w, (*g_pass_extent).width, (*g_uw).sx);
            uw_span(y, h, (*g_pass_extent).height, (*g_uw).sy);
            vp.x = float(x), vp.width = float(w), vp.y = float(y), vp.height = float(h);
        }
        if ((*g_state).has_scissor) {
            int32_t x = sc.offset.x, w = int32_t(sc.extent.width), y = sc.offset.y, h = int32_t(sc.extent.height);
            // A squeezed UI draw's scissor (a clipped list, the birthday wheel) follows its geometry: along the squeezed
            // axis the guest's span is taken about the middle of the target, at the resolution part of the scale.
            auto squeezed = [](int32_t& p, int32_t& n, uint32_t extent, float f, float s) {
                const double guest = double(extent) / f;
                if (p <= 0 && double(p) + n >= guest - 1.0) {  // all of it
                    p = 0, n = int32_t(extent);
                    return;
                }
                double p0 = (double(p) - guest * 0.5) * s + extent * 0.5, p1 = p0 + double(n) * s;
                p0 = std::clamp(p0, 0.0, double(extent)), p1 = std::clamp(p1, 0.0, double(extent));
                p = int32_t(std::lround(p0)), n = int32_t(std::lround(p1 - p0));
            };
            if (ui_x > 0) squeezed(x, w, (*g_pass_extent).width, (*g_uw).sx, (*g_uw).s());
            else uw_span(x, w, (*g_pass_extent).width, (*g_uw).sx);
            if (ui_x < 0) squeezed(y, h, (*g_pass_extent).height, (*g_uw).sy, (*g_uw).s());
            else uw_span(y, h, (*g_pass_extent).height, (*g_uw).sy);
            sc.offset.x = x, sc.extent.width = uint32_t(w), sc.offset.y = y, sc.extent.height = uint32_t(h);
        }
    }
    vp.y += vp.height, vp.height = -vp.height;
}

// The end of a draw, shared by the full path and the fast path: vertex buffers, viewport and scissor (scaled to a
// screen target), the draw itself (unless `emit` is false: draw merging records it later), and what it wrote.
void draw_issue(VkCommandBuffer cb, const Targets& t, bool indexed, const Cmd& c, const DrawInputs& in,
                const ShaderInfo* vs, const ShaderInfo* fs, float ui_x, bool emit = true) {
    // The vertex buffers resolved above, plus the zero stream (binding 15).
    for (uint32_t s = 0; s < 15; s++)
        if (in.used >> s & 1) bind_vertex(cb, s, in.vertex[s]);
    bind_vertex(cb, 15, zero_stream());

    VkViewport vp;
    VkRect2D sc;
    draw_viewport(ui_x, vp, sc);
    set_dynamic(cb, vp, sc);

    trace_skin(indexed, c, vs, fs);
    if (gpu_profile_draws()) {
        char label[128];
        std::snprintf(label, sizeof label, "  draw vs %s fs %s n%d x%d", vs->hash.c_str(), fs ? fs->hash.c_str() : "-",
                      indexed ? c.i[0] : c.i[1], std::max(1, c.i[3]));
        gpu_profile_mark(cb, label);
    }
    if (indexed) {
        uint32_t count = uint32_t(c.i[0]);
        if ((*g_bound).has_index && (*g_bound).index.buffer == in.index.buffer && (*g_bound).index.offset == in.index.offset &&
            (*g_bound).index_type == in.index_type) {
            (*g_state_kept)++;
        } else {
            p_vkCmdBindIndexBuffer(cb, in.index.buffer, in.index.offset, in.index_type);
            (*g_bound).has_index = true, (*g_bound).index = in.index, (*g_bound).index_type = in.index_type, (*g_state_set)++;
        }
        bool q = zero_draws_on() && zero_draws_begin(cb, vs->hash + " " + (fs ? fs->hash : std::string("-")));
        if (emit) p_vkCmdDrawIndexed(cb, count, uint32_t(std::max(1, c.i[3])), 0, c.i[1], uint32_t(c.i[2]));
        if (q) zero_draws_end(cb);
    } else {
        bool q = zero_draws_on() && zero_draws_begin(cb, vs->hash + " " + (fs ? fs->hash : std::string("-")));
        if (emit) p_vkCmdDraw(cb, uint32_t(c.i[1]), uint32_t(std::max(1, c.i[3])), uint32_t(c.i[0]), uint32_t(c.i[2]));
        if (q) zero_draws_end(cb);
    }
    (*g_stats).done++;
    (*g_pass_written) |= ((1u << t.color_count) - 1) | ((*g_state).depth.depth_write || (*g_state).depth.stencil_test ? 1u << 8 : 0);
}

// The draw fast path (docs/PARALLEL_REPLAY.md, after Wind Waker HD's port): a draw whose state generation
// (g_state_gen: nothing but data changed since), targets, command buffer and open pass are the last full draw's reuses
// what that draw resolved: program, layout, textures (their set 2), pipeline, screen-scale decisions. Only the data is
// done again: uniform and storage buffers (sets 0-1), vertex and index data, viewport and scissor. ~86% of the island's
// draws at noon. Checked on each use: the sampled textures unchanged (not written by the CPU, same image), the texture
// handles read from uniform buffers the same, the screen scale's generation. NX_FAST_DRAW=0: off.
struct Prepared {
    bool valid = false;
    uint64_t gen = 0, uw_gen = 0;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    Targets t{}, tt{};
    uint32_t mode = 0;
    const std::vector<const ShaderInfo*>* shaders = nullptr;
    const ShaderInfo *vs = nullptr, *fs = nullptr;
    const ProgramLayout* layout = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSet set2 = VK_NULL_HANDLE;
    float ui_x = 0;
    PipelineKey key{};      // the pipeline's key (draw merging builds the instanced variant from it)
    bool merge_ok = false;  // no per-instance vertex streams: its draws may be merged
    struct Tex {
        Texture* tex;
        GpuImage* img;
    };
    std::vector<Tex> textures;
    struct Handle {
        int stage, bank;
        uint32_t word;
        uint64_t value;
    };
    std::vector<Handle> handles;
};
PerThread<Prepared> g_prep;
PerThread<uint64_t> g_fast_hits, g_fast_misses;

// Draw merging (docs/PARALLEL_REPLAY.md): on the island ~95% of the fast path's draws repeat the previous draw's mesh,
// state and viewport and differ only in one or two uniform blocks of the vertex shader (an object's transform, its
// skinning or wind constants): ~12 draws a run. Turnip's fixed cost per draw (~3.5 us of GPU time) made those runs
// most of the frame's GPU time. The run becomes one instanced draw: the first draw binds everything and its
// vkCmdDraw is held back; each next draw that would bind the same things but those blocks only counts, and where its
// blocks are in guest memory is noted; at the end of the run (anything else recorded, see merge_flush's callers) the
// blocks are gathered one after another into the upload ring, bound once over the whole run, and the vertex module
// reads instance k's copies (nvn_spirv_instance.cpp). Instances are drawn in order, as the draws were. Guest memory
// stays as it was through the run: no upload was invalidated (an inline uniform update) since its first draw.
// NX_DRAW_MERGE=0: off (on by default on Android).
constexpr uint32_t kMergeMax = 256;  // instances a run
struct Merge {
    bool on = false;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    uint64_t bound_gen = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const Prepared* prep = nullptr;
    bool indexed = false;
    int32_t i[3] = {};
    DrawInputs in;
    VkViewport vp{};
    VkRect2D sc{};
    ResolvedSets sets;
    BufferWrite writes[64];
    uint32_t nwrites = 0;
    uint32_t n = 0;
    // The blocks that move (known from the run's second draw): their dynamic offset slot, binding, size bound (the
    // stride of the gathered copies), and each instance's bytes in guest memory.
    uint32_t nb = 0, slot[2] = {}, binding[2] = {}, range[2] = {};
    int bank[2] = {};  // their uniform bank in the vertex stage
    struct Src {
        const uint8_t* p;
        uint32_t size;
    } src[2][kMergeMax];
    // Run continuation (merge_continues): the data the run's last draw was built from. A next draw that binds the same
    // but the run's blocks is counted in without building its sets and inputs again (~80% of the island's draws).
    uint64_t epoch = 0, a0 = 0;
    Range uniforms[2][18], storage[2][16], vertex_buffers[16];  // stages 0 and 1 (nvn_sets.cpp's api_stage)
    int32_t viewport[4] = {}, scissor[4] = {};
    bool has_viewport = false, has_scissor = false;
};
PerThread<Merge> g_merge;
PerThread<uint64_t> g_merge_runs, g_merge_draws, g_merge_fallbacks, g_merge_quick;
PerThread<uint64_t[20]> g_merge_why;  // NX_MERGE_WHY: why fast draws did not extend a run

bool merge_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_DRAW_MERGE");
#if defined(__ANDROID__)
        if (!e) return !parallel_replay_on();
#endif
        return e && e[0] == '1' && !parallel_replay_on();
    }();
    return on;
}

void merge_emit(const Merge& m, uint32_t instances) {
    if (m.indexed) p_vkCmdDrawIndexed(m.cb, uint32_t(m.i[0]), instances, 0, m.i[1], 0);
    else p_vkCmdDraw(m.cb, uint32_t(m.i[1]), instances, uint32_t(m.i[0]), 0);
}

// Records the held-back draw (or run): before anything else is recorded into its command buffer.
void merge_flush() {
    Merge& m = *g_merge;
    if (!m.on) return;
    m.on = false;
    if (m.n == 1) return merge_emit(m, 1);
    (*g_merge_runs)++, (*g_merge_draws) += m.n;
    const ProgramLayout& layout = *m.prep->layout;
    BufferWrite w[64];
    std::memcpy(w, m.writes, m.nwrites * sizeof(BufferWrite));
    uint32_t offsets[64];
    std::memcpy(offsets, m.sets.offsets, m.sets.noff * sizeof(uint32_t));
    for (uint32_t j = 0; j < m.nb; j++) {  // instance k's copy at k * range, zero padded as dedup_upload pads
        const uint32_t stride = m.range[j];
        Upload u = upload_alloc(uint64_t(m.n) * stride, 256);
        for (uint32_t k = 0; k < m.n; k++) {
            uint8_t* to = u.ptr + uint64_t(k) * stride;
            const uint32_t len = std::min(m.src[j][k].size, stride);
            std::memcpy(to, m.src[j][k].p, len);
            if (len < stride) std::memset(to + len, 0, stride - len);
        }
        for (uint32_t k = 0; k < m.nwrites; k++)
            if (w[k].binding == m.binding[j]) w[k].info.buffer = u.buffer, w[k].info.range = uint64_t(m.n) * stride;
        offsets[m.slot[j]] = uint32_t(u.offset);
    }
    VkDescriptorSet s0 = sets_write_set(layout.sets[0], w, m.nwrites);
    if (!s0) {
        static int logged = 0;
        if (logged++ < 3) log("nvn: draw merging: no descriptor set for a run of %u draws (left out)", m.n);
        return;
    }
    PipelineKey key = m.prep->key;
    key.inst_binding = key.inst_step = 0;
    for (uint32_t j = 0; j < m.nb; j++)
        key.inst_binding |= (m.binding[j] + 1) << 8 * j, key.inst_step |= (m.range[j] / 16) << 16 * j;
    pipeline_skip_allowed(true);  // still compiling: the run is drawn draw by draw meanwhile
    if (VkPipeline p = graphics_pipeline(key, layout)) {
        p_vkCmdBindPipeline(m.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        (*g_bound).pipeline = p;
        p_vkCmdBindDescriptorSets(m.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.layout, 0, 1, &s0, m.sets.noff, offsets);
        merge_emit(m, m.n);
    } else {  // the plain pipeline (bound) reads instance k's copies at their own offsets
        (*g_merge_fallbacks)++;
        const uint32_t base[2] = {offsets[m.slot[0]], m.nb > 1 ? offsets[m.slot[1]] : 0};
        for (uint32_t k = 0; k < m.n; k++) {
            for (uint32_t j = 0; j < m.nb; j++) offsets[m.slot[j]] = base[j] + k * m.range[j];
            p_vkCmdBindDescriptorSets(m.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.layout, 0, 1, &s0, m.sets.noff, offsets);
            merge_emit(m, 1);
        }
    }
    (*g_bound_gen)++;  // set 0 is not what bind_sets' repeat check last bound
}

void merge_snapshot(Merge& m, const Cmd& c) {
    const State& s = *g_state;
    std::memcpy(m.uniforms, s.uniforms, sizeof m.uniforms);
    std::memcpy(m.storage, s.storage, sizeof m.storage);
    std::memcpy(m.vertex_buffers, s.vertex_buffers, sizeof m.vertex_buffers);
    std::memcpy(m.viewport, s.viewport, sizeof m.viewport), std::memcpy(m.scissor, s.scissor, sizeof m.scissor);
    m.has_viewport = s.has_viewport, m.has_scissor = s.has_scissor;
    m.epoch = *g_upload_epoch, m.a0 = c.a[0];
}

// Notes where instance m.n's block j is (the range bound for its bank); false: not gathered (no memory, or written by
// a dispatch of this submit: its copy is composed on the GPU).
bool merge_source(Merge& m, uint32_t j, const Range& r) {
    const uint8_t* p = r.address ? guest_bytes(r.address) : nullptr;
    const uint32_t size = uint32_t(std::min<uint64_t>(p ? r.size : 0, 0x10000));
    if (!p || written_overlaps(p, size)) return false;
    write_back_before_read(p, size, "merge");
    m.src[j][m.n] = {p, size};
    return true;
}

// Whether this fast draw continues the run with only the run's blocks rebound since its last draw: everything else it
// would build is what that draw built (same bindings, and no upload invalidated since: the same copies).
bool merge_continues(VkCommandBuffer cb, const Prepared& prep, bool indexed, const Cmd& c) {
    Merge& m = *g_merge;
    if (!m.on || m.n < 2 || m.n >= kMergeMax || m.cb != cb || m.prep != &prep || (*g_bound).pipeline != m.pipeline ||
        (*g_bound_gen) != m.bound_gen || m.indexed != indexed || c.i[0] != m.i[0] || c.i[1] != m.i[1] || c.i[2] != 0 ||
        c.i[3] > 1 || (indexed && c.a[0] != m.a0) || m.epoch != *g_upload_epoch)
        return false;
    const State& s = *g_state;
    for (uint32_t j = 0; j < m.nb; j++) {
        if (uint64_t(m.n + 1) * m.range[j] > 0x10000) return false;  // maxUniformBufferRange
        m.uniforms[0][m.bank[j]] = s.uniforms[0][m.bank[j]];
    }
    if (std::memcmp(m.uniforms, s.uniforms, sizeof m.uniforms) != 0 ||
        std::memcmp(m.storage, s.storage, sizeof m.storage) != 0 ||
        std::memcmp(m.vertex_buffers, s.vertex_buffers, sizeof m.vertex_buffers) != 0 ||
        std::memcmp(m.viewport, s.viewport, sizeof m.viewport) != 0 ||
        std::memcmp(m.scissor, s.scissor, sizeof m.scissor) != 0 || m.has_viewport != s.has_viewport ||
        m.has_scissor != s.has_scissor)
        return false;
    for (uint32_t j = 0; j < m.nb; j++)
        if (!merge_source(m, j, s.uniforms[0][m.bank[j]])) return false;
    if (m.epoch != *g_upload_epoch) return false;  // a late write-back
    m.n++;
    return true;
}

// Whether this fast draw only continues the held-back run (`rs`, `vp`, `sc`: what it would bind and set).
bool merge_extends(VkCommandBuffer cb, const Prepared& prep, bool indexed, const Cmd& c, const DrawInputs& in,
                   const ResolvedSets& rs, const VkViewport& vp, const VkRect2D& sc) {
    Merge& m = *g_merge;
    auto why = [](int k) { (*g_merge_why)[k]++; return false; };
    if (!m.on || m.cb != cb || m.prep != &prep || m.pipeline != prep.pipeline || (*g_bound).pipeline != m.pipeline ||
        (*g_bound_gen) != m.bound_gen || m.indexed != indexed || c.i[0] != m.i[0] || c.i[1] != m.i[1] ||
        c.i[2] != 0 || c.i[3] > 1 || in.used != m.in.used || rs.noff != m.sets.noff)
        return why(1);
    if (indexed && (in.index.buffer != m.in.index.buffer || in.index.offset != m.in.index.offset ||
                    in.index_type != m.in.index_type))
        return why(2);
    for (uint32_t s = 0; s < 15; s++)
        if ((in.used >> s & 1) && (in.vertex[s].buffer != m.in.vertex[s].buffer || in.vertex[s].offset != m.in.vertex[s].offset))
            return why(3);
    if (std::memcmp(&vp, &m.vp, sizeof vp) != 0 || std::memcmp(&sc, &m.sc, sizeof sc) != 0 ||
        std::memcmp(rs.sets, m.sets.sets, sizeof rs.sets) != 0)
        return why(4);
    if (m.epoch != *g_upload_epoch) return why(0);  // guest memory changed since the run's first draw
    if (m.n >= kMergeMax) return why(14);
    static const bool detail = std::getenv("NX_MERGE_WHY") && std::getenv("NX_MERGE_WHY")[0] == '2';
    static int shown = 0;
    const State& s = *g_state;
    if (m.n == 1) {  // the run's second draw: which blocks move, and may they be read by instance
        uint32_t nb = 0, slots[2] = {};
        for (uint32_t k = 0; k < rs.noff; k++)
            if (rs.offsets[k] != m.sets.offsets[k]) {
                if (nb == 2) {
                    if (detail && shown++ < 40) {
                        std::string o;
                        for (uint32_t j = 0; j < rs.noff; j++)
                            o += " " + std::to_string(int64_t(rs.offsets[j]) - int64_t(m.sets.offsets[j]));
                        log("merge why 5: vs %s offsets moved by:%s", prep.vs->hash.c_str(), o.c_str());
                    }
                    return why(5);  // three blocks move
                }
                slots[nb++] = k;
            }
        if (!nb) return why(6);
        const ShaderInfo* vs = prep.vs;
        for (uint32_t j = 0; j < nb; j++) {
            uint32_t b = 0;
            for (uint64_t d = prep.layout->dynamic, k = 0;; d &= d - 1, k++) {
                if (!d) return why(8);
                if (k == slots[j]) {
                    b = uint32_t(__builtin_ctzll(d));
                    break;
                }
            }
            if (!vs->masks_known || !vs->uniform_extent[b] || (dynamic_uniform_bindings(vs) >> b & 1)) return why(9);
            const DescriptorBinding* block = nullptr;
            for (const DescriptorBinding& db : vs->refl.bindings)
                if (db.set == 0 && db.binding == b) block = &db;
            if (!block) return why(10);
            if (block->nvn.source != NvnSource::UniformBlock || block->nvn.index < 0 || block->nvn.index >= 18)
                return why(7);
            if (prep.fs)
                for (const DescriptorBinding& db : prep.fs->refl.bindings)
                    if (db.set == 0 && db.binding == b) return why(11);
            uint32_t range = 0;
            for (uint32_t k = 0; k < m.nwrites; k++)
                if (m.writes[k].binding == b) range = uint32_t(m.writes[k].info.range);
            // Reads past the block return 0 (robust buffer access, as on Maxwell): only a block read within its range.
            if (!range || range % 16) return why(12);
            if (vs->uniform_extent[b] > range) return why(13);
            if (!instance_patchable(vs, b)) return why(19);
            if (uint64_t(2) * range > 0x10000) return why(14);
            m.slot[j] = slots[j], m.binding[j] = b, m.range[j] = range, m.bank[j] = block->nvn.index;
        }
        m.nb = nb;
        m.n = 0;  // the first draw's blocks, as the snapshot taken with it has them
        for (uint32_t j = 0; j < nb; j++)
            if (!merge_source(m, j, m.uniforms[0][m.bank[j]])) return m.n = 1, why(15);
        m.n = 1;
    } else {
        for (uint32_t k = 0; k < rs.noff; k++)
            if (rs.offsets[k] != m.sets.offsets[k] && k != m.slot[0] && (m.nb < 2 || k != m.slot[1])) {
                if (detail && shown++ < 40) log("merge why 5: vs %s slot %u moved too", prep.vs->hash.c_str(), k);
                return why(5);  // another block moves
            }
        for (uint32_t j = 0; j < m.nb; j++)
            if (uint64_t(m.n + 1) * m.range[j] > 0x10000) return why(14);  // maxUniformBufferRange
    }
    for (uint32_t j = 0; j < m.nb; j++)
        if (!merge_source(m, j, s.uniforms[0][m.bank[j]])) return why(15);
    if (m.epoch != *g_upload_epoch) return why(0);
    m.n++;
    return true;
}

bool fast_draw_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_FAST_DRAW");
        if (e && e[0] == '0') return false;
        // Debug features that work per draw (snapshots, traces, overrides) need the full path.
        for (const char* v : {"NX_SNAP_FS", "NX_SNAP_FS_BEFORE", "NX_SNAP_FOLLOW", "NX_DUMP_ON_FS", "NX_DUMP_ON_TARGET",
                              "NX_DUMP_DRAW_TEXTURES", "NX_TRACE_DRAWS", "NX_DRAW_SLOW", "NX_PASS_BARRIER_NARROW",
                              "NX_SWAP_FS", "NX_SKIP_FS", "NX_UW_TRACE", "NX_DRAW_TIMING", "NX_TRACE_SKIN"})
            if (const char* x = std::getenv(v); x && *x) return false;
        return sets_cache_enabled();
    }();
    return on && !gpu_profile_draws() && !zero_draws_on();
}

// Whether the prepared draw still holds (the generation and the targets compared by the caller).
bool prepared_holds(const Prepared& p) {
    if (p.uw_gen != uw_generation()) return false;
    for (const Prepared::Tex& x : p.textures)
        if (x.tex->dirty.load(std::memory_order_acquire) || x.tex->has_writes.load(std::memory_order_acquire) ||
            x.tex->image.get() != x.img)
            return false;
    for (const Prepared::Handle& h : p.handles) {
        const Range& r = (*g_state).uniforms[h.stage][h.bank];
        const uint8_t* q = r.address ? guest_bytes(r.address) : nullptr;
        uint64_t v = q && uint64_t(h.word) * 4 + 4 <= r.size ? *reinterpret_cast<const uint32_t*>(q + h.word * 4) : 0;
        if (v != h.value) return false;
    }
    return true;
}

bool draw(VkCommandBuffer cb, const Targets& t, uint32_t mode, bool indexed, const Cmd& c) {
    (*g_stats).total++;
    if ((*g_state).rasterizer_discard || !(*g_state).render_enable) return false;  // nothing reaches the targets
    Prepared& prep = *g_prep;
    if (prep.valid && prep.gen == *g_state_gen && prep.cb == cb && prep.mode == mode && *g_rendering &&
        targets_same(prep.t, t) && fast_draw_on() && prepared_holds(prep)) {
        (*g_fast_hits)++;
        if (merge_on() && merge_continues(cb, prep, indexed, c)) {
            (*g_merge_quick)++, (*g_stats).done++;
            return true;
        }
        // The kept set 2 is for this draw's bind_sets only: every way out before it drops it (left set, the next
        // build_sets, of another layout or a dispatch, would bind it: a GPU fault).
        auto give_up = [] {
            sets_keep_texture_set(VK_NULL_HANDLE);
            return false;
        };
        sets_keep_texture_set(prep.set2);
        build_sets(cb, *prep.shaders, *prep.layout);
        if (*t_bail) return give_up();
        DrawInputs in;
        if (!draw_inputs_shared(cb, prep.vs, indexed, c, in)) {
            (*g_stats).no_indices++;
            return give_up();
        }
        if (*t_bail) return give_up();
        if (!*g_rendering) begin_rendering(cb, prep.tt);  // a written range's copy ended the pass
        if (*t_bail) return give_up();
        if ((*g_bound).cb != cb) merge_flush(), (*g_bound) = Bound{}, (*g_bound_gen)++, (*g_bound).cb = cb;
        ResolvedSets rs;
        sets_resolve(*prep.layout, rs);
        const bool merging = prep.merge_ok && merge_on();
        if (!merging) (*g_merge_why)[prep.merge_ok ? 16 : 17]++;
        if (merging) {
            VkViewport vp;
            VkRect2D sc;
            draw_viewport(prep.ui_x, vp, sc);
            if (merge_extends(cb, prep, indexed, c, in, rs, vp, sc)) {
                merge_snapshot(*g_merge, c);
                (*g_stats).done++;
                return true;
            }
            merge_flush();
        }
        if ((*g_bound).pipeline != prep.pipeline) {
            p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, prep.pipeline);
            (*g_bound).pipeline = prep.pipeline, (*g_state_set)++;
        }
        sets_bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, *prep.layout, rs);
        const bool hold = merging && c.i[2] == 0 && c.i[3] <= 1;  // may start a run: its vkCmdDraw held back
        if (merging && !hold) (*g_merge_why)[18]++;
        draw_issue(cb, t, indexed, c, in, prep.vs, prep.fs, prep.ui_x, !hold);
        if (hold) {
            Merge& m = *g_merge;
            m.on = true, m.cb = cb, m.bound_gen = *g_bound_gen, m.pipeline = prep.pipeline, m.prep = &prep;
            m.indexed = indexed, m.i[0] = c.i[0], m.i[1] = c.i[1], m.i[2] = c.i[2];
            m.in = in, m.sets = rs, m.n = 1;
            m.nwrites = sets_set0_writes(m.writes, 64);
            m.nb = 0;
            merge_snapshot(m, c);
            draw_viewport(prep.ui_x, m.vp, m.sc);
            if (!m.nwrites) merge_flush();
        }
        return true;
    }
    (*g_fast_misses)++;
    merge_flush();
    prep.valid = false;
    sets_keep_texture_set(VK_NULL_HANDLE);  // never left over into a full build
    bool data_dep = false;  // the draw's resolution read vertex data (a UI quad's size): not prepared for the next
    const auto* shaders = program_shaders((*g_state).program);
    if (!shaders || shaders->empty()) {
        (*g_stats).no_program++;
        return false;
    }
    const ShaderInfo* vs = nullptr;
    const ShaderInfo* fs = nullptr;
    for (const ShaderInfo* s : *shaders) {
        if (s->stage == ShaderStage::Vertex) vs = s;
        if (s->stage == ShaderStage::Fragment) fs = s;
    }
    if (!vs) {
        (*g_stats).no_vertex++;
        return false;
    }
    // A stage without a translation ("no <hash>.spv" when the program was set): without its fragment shader the
    // draw writes depth only, and the deferred lighting shows a black silhouette (character faces, the Nook Stop).
    // Drawn anyway; the first draws say which, so a missing translation is not mistaken for a rendering bug.
    if (uint32_t missing = program_missing_stages((*g_state).program)) {
        static int logged = 0;
        (*g_stats).missing_stage++;
        if (logged < 20 && ++logged)
            log("nvn: draw %llu: program %llx has %u stage(s) without a translation (vs %s fs %s): run with "
                "NX_DUMP_SHADER_MISSES=1 and translate shader_misses/", (unsigned long long)(*g_stats).total,
                (unsigned long long)(*g_state).program, missing, vs->hash.c_str(), fs ? fs->hash.c_str() : "none");
    }
    std::vector<const ShaderInfo*> swapped;  // NX_SWAP_FS / NX_SKIP_FS (nvn_draw_trace.cpp)
    if (!debug_shader_overrides(shaders, fs, swapped)) return false;
    hang_note(vs, fs);  // which programs a GPU hang came with (nvn_vk.cpp)
    const ProgramLayout& layout = program_layout(*shaders);

    // NX_SNAP_FS_BEFORE=<hash>[@<first draw>]: snapshot colour target 0 before draws with that fragment shader
    // (what a feedback pass like a tonemap reads), 8 at most.
    {
        static const std::string before = std::getenv("NX_SNAP_FS_BEFORE") ? std::getenv("NX_SNAP_FS_BEFORE") : "";
        static const uint64_t from = before.find('@') != std::string::npos
                                         ? std::strtoull(before.c_str() + before.find('@') + 1, nullptr, 10) : 0;
        static int taken = 0;
        bool match = (fs && before.compare(0, 16, fs->hash) == 0) || before.compare(0, 16, vs->hash) == 0;
        if (!before.empty() && match && (*g_stats).total >= from && taken < 8 &&
            t.color_count && t.colors[0])
            if (GpuImage* img = image_of(t.colors[0])) {
                taken++;
                draw_end_rendering(cb, 6);
                debug_snapshot(cb, *img, 0xB0000000ull + (*g_stats).total);
            }
    }
    // Textures first: an upload or a layout change cannot happen inside the render pass.
    uint32_t gathered = gather_textures(*shaders);
    if ((*t_bail)) return false;  // a parallel replay worker gave up the chunk: nothing more is recorded
    draw_part(0);
    {   // NX_DUMP_DRAW_TEXTURES=<fs hash>@<draw>: the images the first such draw samples, snapshotted (frames/00000_c00000NN)
        static const char* env = std::getenv("NX_DUMP_DRAW_TEXTURES");
        static int done_count = 0;
        static bool said = false;
        if (env && !said && fs) said = true, log("nvn: draw-textures watch %s, first fs %s total %llu", env, fs->hash.c_str(), (unsigned long long)(*g_stats).total);
        if (env && done_count < 6 && fs && std::strncmp(env, fs->hash.c_str(), 16) == 0 && env[16] == '@' &&
            (*g_stats).total >= std::strtoull(env + 17, nullptr, 10)) {
            const uint64_t tag = uint64_t(done_count++) << 8;
            draw_end_rendering(cb, 6);
            for (size_t k = 0; k < (*g_textures).resolved.size(); k++) {
                const ResolvedTexture& r = (*g_textures).resolved[k];
                GpuImage* img = r.texture ? image_of(r.texture) : nullptr;
                log("draw textures %zu: texture %llx image %p %ux%u fmt %d levels %u layers %u uploaded %d rendered %d", k,
                    (unsigned long long)r.texture, (void*)img, img ? img->width : 0, img ? img->height : 0,
                    img ? int(img->format) : 0, img ? img->levels : 0, img ? img->layers : 0, img ? int(img->uploaded) : 0,
                    img ? int(img->rendered) : 0);
                if (img)
                    for (uint32_t l = 0; l < (img->layers > 1 ? std::min(img->layers, 12u) : 1u); l++) {
                        g_snap_layer = l, g_snap_level = 0;
                        debug_snapshot(cb, *img, 0xC0000000ull + tag * 4096 + k * 16 + l);
                    }
                g_snap_layer = 0;
            }
        }
    }
    // A pass that neither tests nor writes depth but keeps its depth image bound while sampling that same image (the
    // world map's depth-to-colour linearization, fs 0aa56d1b): on Metal the image is then attached and read in one
    // encoder, which is undefined, and the copy came out 0 everywhere; the composite then took the player for
    // occluded (a flat silhouette, issue #50; DEBUG-2.md #67). The depth is unused: the pass runs without it.
    // NX_KEEP_SAMPLED_DEPTH=1: as before.
    Targets detached;
    const Targets& tt = [&]() -> const Targets& {
        static const bool keep = std::getenv("NX_KEEP_SAMPLED_DEPTH") != nullptr;
        const DepthStencilInfo& d = (*g_state).depth;
        if (keep || !t.depth || d.depth_test || d.depth_write || d.stencil_test) return t;
        GpuImage* dimg = image_of(t.depth);
        if (!dimg) return t;
        for (const ResolvedTexture& r : (*g_textures).resolved)
            if (r.texture && image_of(r.texture) == dimg) {
                static uint64_t n = 0;
                if (n++ == 0) log("nvn: a pass samples its own unused depth image: drawn without it (#50)");
                detached = t, detached.depth = 0;
                return detached;
            }
        return t;
    }();
    vertex_read_barrier(cb, vs);  // G4
    // Is this draw's target a scaled screen target (viewport, scissor, render scale, UI squeeze; nvn_ultrawide.cpp)
    (*g_uw) = UwTarget{};
    GpuImage* wi = nullptr;
    for (uint32_t i = 0; i < t.color_count; i++) {
        if (t.colors[i]) {
            if (Texture* tex = get<Texture>(t.colors[i]); tex && tex->base_w && !tex->uw) {
                if (worker_bail("screen target resize")) return false;
                tex->uw = true;
                uw_resize(*tex);
                if (tex->image) retire_image(std::move(tex->image)), tex->image.reset();
            }
            if (auto* img = image_of(t.colors[i]); img && img->uw && (img->sx != 1.0f || img->sy != 1.0f)) {
                wi = img;
                break;
            }
        }
    }
    if (!wi && t.depth) {
        if (auto* img = image_of(t.depth); img && img->uw && (img->sx != 1.0f || img->sy != 1.0f)) {
            wi = img;
        }
    }
    if (wi) (*g_uw) = {true, wi->sx, wi->sy};
    bool need_break = gathered & kNeedBreak;
    if (gathered & kSampledPending) clears_flush(cb);  // cleared (and stored) before it is sampled
    static const bool bytes = std::getenv("NX_TARGETS_MEMCMP") != nullptr;  // bisecting
    bool targets_changed = !(*g_rendering) || (bytes ? std::memcmp(&(*g_pass), &tt, sizeof tt) != 0 : !targets_same((*g_pass), tt));
    if ((*g_rendering) && (need_break || targets_changed)) {
        (targets_changed ? (*g_stats).breaks_targets : (*g_stats).breaks_textures)++;
        draw_end_rendering(cb, 1);
    }

    build_sets(cb, *shaders, layout);
    if ((*t_bail)) return false;
    draw_part(1);
    // Vertex and index data before the pass begins: a range a dispatch of this submit wrote may need a GPU copy
    // (nvn_written.cpp), which cannot be recorded inside a pass.
    DrawInputs in;
    if (!draw_inputs_shared(cb, vs, indexed, c, in)) {
        (*g_stats).no_indices++;
        return false;
    }
    check_vertex_ranges(vs, fs, indexed, c, in);  // NX_CHECK_VERTS (nvn_written.cpp)
    if ((*t_bail)) return false;
    draw_part(2);
    if (!(*g_rendering)) {
        if (gpu_profile_on()) {
            GpuImage* img = t.color_count && t.colors[0] ? image_of(t.colors[0]) : nullptr;
            char label[128];
            std::snprintf(label, sizeof label, "%ux%u L%u fs %s", img ? img->width : 0, img ? img->height : 0,
                          t.views.level[0], fs ? fs->hash.c_str() : "-");
            gpu_profile_mark(cb, label);
        }
        if (gpu_busy_on()) cb = frame_pass_part(cb);
        begin_rendering(cb, tt);
        if ((*t_bail)) return false;
        if (gpu_busy_on()) {  // NX_GPU_PROFILE=3 with NX_SUBMIT_CHUNK_DRAWS: GPU time by part (nvn_gpu_busy.cpp)
            GpuImage* img = t.color_count && t.colors[0] ? image_of(t.colors[0]) : nullptr;
            GpuImage* dimg = t.depth ? image_of(t.depth) : nullptr;
            char label[128];
            std::snprintf(label, sizeof label, "%ux%u%s%s fs %s", img ? img->width : dimg ? dimg->width : 0,
                          img ? img->height : dimg ? dimg->height : 0, t.color_count > 1 ? " mrt" : "",
                          dimg ? " +depth" : "", fs ? fs->hash.c_str() : "-");
            gpu_busy_note_pass(label);
        }
    }

    // Pipeline.
    PipelineKey key;
    std::memset(&key, 0, sizeof key);  // the key is compared byte for byte
    key.vs = vs, key.fs = fs;
    key.topology = mode;
    key.attrib_count = uint32_t(std::min<size_t>((*g_state).attribs.size(), 16));
    std::copy_n((*g_state).attribs.begin(), key.attrib_count, key.attribs);
    key.stream_count = uint32_t(std::min<size_t>((*g_state).streams.size(), 16));
    std::copy_n((*g_state).streams.begin(), key.stream_count, key.streams);
    std::copy_n((*g_state).blend, 8, key.blend);
    key.color = (*g_state).color, key.mask = (*g_state).mask, key.depth = (*g_state).depth, key.polygon = (*g_state).polygon;
    key.color_count = t.color_count, key.depth_clamp = (*g_state).depth_clamp;
    key.alpha_to_coverage = (*g_state).alpha_to_coverage;
    static const bool alpha_off = std::getenv("NX_ALPHA_TEST") && std::getenv("NX_ALPHA_TEST")[0] == '0';
    if (!alpha_off && (*g_state).color.alpha_test >= 1 && (*g_state).color.alpha_test <= 7)
        key.alpha_func = (*g_state).color.alpha_test, key.alpha_ref = (*g_state).alpha_ref;
    for (uint32_t k = 0; k < t.color_count; k++) {
        GpuImage* img = t.colors[k] ? image_of(t.colors[k]) : nullptr;
        key.color_formats[k] = img ? view_format(*img, t.views.format[k]) : unused_slot_format();
        if (img && (img->nvn_format == 0x3f || t.views.format[k] == 0x3f)) key.ufloat_colors |= 1u << k;  // G4
        if (img && img->hdr_clamp && fs && !fs->hdr_replacement) key.clamp_colors |= 1u << k;  // HDR: as its UNORM
    }
    GpuImage* dimg = tt.depth ? image_of(tt.depth) : nullptr;
    key.depth_format = dimg && dimg->aspect != VK_IMAGE_ASPECT_COLOR_BIT ? dimg->format : VK_FORMAT_UNDEFINED;
    // Screen scale, aspect part (nvn_ultrawide.cpp). The world is widened (or made taller) by the camera patch, so
    // only the 16:9 UI is squeezed into the middle (by ax, ay). The UI is drawn into small BGRA8 layer targets (430x180
    // for the HUD, never scaled) and composited into the scene target by a quad: the vertex shaders seen drawing into
    // such layer targets are the "UI" ones, and a UI quad into a scaled target that does not sample a scaled texture
    // is squeezed. NX_UW_CAMERA=0 (no camera patch): every geometry draw is squeezed, a world-and-UI shrink.
    static const bool cam_patch = !(std::getenv("NX_UW_CAMERA") && std::getenv("NX_UW_CAMERA")[0] == '0');
    const bool aniso = (*g_uw).on && ((*g_uw).ax() != 1.0f || (*g_uw).ay() != 1.0f);
    if (uw_on() && cam_patch && !(*g_uw).on && t.color_count && t.colors[0]) {
        const Texture* lt = get<Texture>(t.colors[0]);
        if (lt && lt->format == 0x25 && (lt->flags & 8)) uw_ui_vertex_shader(vs);
    }
    // NX_UW_UI=learn: only the vertex shaders learned above; default: every draw without depth test (screen-space UI).
    static const bool ui_learn = std::getenv("NX_UW_UI") && std::string(std::getenv("NX_UW_UI")) == "learn";
    const bool ui_pick = !cam_patch || (ui_learn ? uw_is_ui_vertex_shader(vs) : !(*g_state).depth.depth_test);
    // With the UI's own projections widened (nvn_ultrawide.cpp, the default) its geometry is right already: only its
    // scissor (below) is still taken about the middle.
    static const bool ortho = uw_ortho_on();
    float ui_x = 0;  // what key.uw_x would be: the scissor of a UI draw
    if (aniso && (!ui_pick || !uw_vs_scaled(vs)) && t.color_count && t.colors[0])  // the scene, a screen pass: all of it
        if (GpuImage* ci = image_of(t.colors[0])) ci->full_frame = g_frame.load(std::memory_order_relaxed);
    if (aniso && uw_vs_scaled(vs) && ui_pick) {
        // A draw that samples a widened render target is a screen-space pass (post chain, composite) or a picture
        // of the screen (the blurred capture behind a menu): it already covers the whole target. (Only short draws
        // counted before: the capture behind the birthday screen was squeezed twice, a 1920 / k wide band.)
        bool samples_wide = false;
        for (const ResolvedTexture& r : (*g_textures).resolved)
            if (r.texture)
                if (GpuImage* si = image_of(r.texture); si && si->uw && (si->sx != si->sy)) samples_wide = true;
        if (!samples_wide) ui_x = (*g_uw).ax() < 1.0f ? (*g_uw).ax() : -(*g_uw).ay();
        if (samples_wide && t.color_count && t.colors[0])  // the capture of the screen behind a menu: all of it
            if (GpuImage* ci = image_of(t.colors[0])) ci->full_frame = g_frame.load(std::memory_order_relaxed);
        if (!samples_wide && (*g_uw).ax() < 1.0f) layer_sides(cb, t, indexed, c);
        static bool dbg_nosq = false;  // DEBUG: a file uw_nosqueeze turns the squeeze off while it exists
        static uint64_t dbg_next = 0;
        if ((*g_stats).total >= dbg_next && plat::debug_files()) {  // ~8 opens a frame: on a phone's FUSE, ms each
            dbg_next = (*g_stats).total + 3000;
            FILE* f = plat::fopen_utf8(plat::data_root() + "/uw_nosqueeze", "rb");
            dbg_nosq = f != nullptr;
            if (f) std::fclose(f);
        }
        if (!ortho && !dbg_nosq) key.uw_x = ui_x;
        // Widened projection: a quad of the screen (the 3D scene's composite, the blurred capture behind a menu) is
        // laid out over the 16:9 part like the rest of the layer; it is stretched back over the whole target.
        else if (samples_wide) key.uw_x = (*g_uw).ax() < 1.0f ? 1.0f / (*g_uw).ax() : -1.0f / (*g_uw).ay();
    }
    // A quad given in screen units by a vertex shader with no projection (position * 2; the post passes use it for the
    // whole screen) that covers only part of the screen is UI: the NookPhone's shadow mask, drawn unsqueezed, showed
    // as a dark band beside the phone. Its first vertices decide (positions within +-0.5).
    if (aniso && cam_patch && ui_pick && key.uw_x == 0 && !uw_vs_scaled(vs) && !(*g_state).attribs.empty() &&
        !(*g_state).streams.empty()) {
        data_dep = true;
        const VertexAttrib& a = (*g_state).attribs[0];
        const uint32_t stride = a.stream < (*g_state).streams.size() ? (*g_state).streams[a.stream].stride : 0;
        const Range& vb = (*g_state).vertex_buffers[a.stream < 16 ? a.stream : 0];
        const uint64_t first = uint64_t(std::max(0, indexed ? c.i[1] : c.i[0]));
        float mx = 0, my = 0;
        bool ok = stride >= 8 && vb.address;
        for (uint32_t v = 0; ok && v < 4; v++) {
            const uint64_t at = (first + v) * stride + a.offset;
            const auto* f = at + 8 <= vb.size ? reinterpret_cast<const float*>(guest_bytes(vb.address + at)) : nullptr;
            if (!f || !std::isfinite(f[0]) || !std::isfinite(f[1]) || std::fabs(f[0]) > 0.51f || std::fabs(f[1]) > 0.51f) {
                ok = false;
                break;
            }
            mx = std::max(mx, std::fabs(f[0])), my = std::max(my, std::fabs(f[1]));
        }
        if (ok && (mx < 0.49f || my < 0.49f)) key.uw_x = ui_x = (*g_uw).ax() < 1.0f ? (*g_uw).ax() : -(*g_uw).ay();
    }
    // A pass over the whole widened target (not the squeezed UI) sees gl_FragCoord in the game's 16:9 units.
    static const bool fc_off = std::getenv("NX_UW_FRAGCOORD") && std::getenv("NX_UW_FRAGCOORD")[0] == '0';
    if (aniso && !fc_off && fs_reads_fragcoord(fs)) {
        const bool wide = (*g_uw).ax() < 1.0f;
        if (ui_x != 0 && key.uw_x == ui_x) {  // the squeezed UI: its 16:9 middle starts this many pixels in
            const float extent = float(wide ? (*g_pass_extent).width : (*g_pass_extent).height);
            key.fc_x = wide ? 1.0f : -1.0f;
            key.fc_off = -extent * (1.0f - (wide ? (*g_uw).ax() : (*g_uw).ay())) * 0.5f;
        } else {
            key.fc_x = wide ? (*g_uw).ax() : -(*g_uw).ay();
        }
    }
    if ((*g_uw).on) {  // NX_UW_TRACE=<first draw>:<count>, or a file uw_trace holding <count>: draws (wide = a scaled target)
        static const char* e = std::getenv("NX_UW_TRACE");
        static uint64_t from = 0, n = 0, next_check = 0;
        static bool init = [&] { if (e) std::sscanf(e, "%lu:%lu", &from, &n); return true; }();
        (void)init;
        if ((*g_stats).total >= next_check && plat::debug_files()) {
            next_check = (*g_stats).total + 2000;
            std::string path = plat::data_root() + "/uw_trace";
            if (FILE* f = plat::fopen_utf8(path, "rb")) {
                unsigned long long c = 0;
                if (std::fscanf(f, "%llu", &c) == 1 && c) from = (*g_stats).total, n = c;
                std::fclose(f);
                std::remove(path.c_str());
            }
        }
        if (n && (*g_stats).total >= from && (*g_stats).total < from + n) {
            GpuImage* wi = t.color_count && t.colors[0] ? image_of(t.colors[0]) : nullptr;
            const Texture* tx = t.color_count && t.colors[0] ? get<Texture>(t.colors[0]) : nullptr;
            std::string smp;
            for (const ResolvedTexture& r : (*g_textures).resolved) {
                GpuImage* si = r.texture ? image_of(r.texture) : nullptr;
                char bb[48];
                const Texture* st = r.texture ? get<Texture>(r.texture) : nullptr;
                std::snprintf(bb, sizeof bb, " %ux%u%s(f%x,fmt%x,%llx)", si ? si->width : 0, si ? si->height : 0, si && si->uw ? "w" : "", st ? st->flags : 0, st ? st->format : 0, (unsigned long long)r.texture);
                smp += bb;
            }
            log("uwsamples:%s", smp.c_str());
            log("uwdraw %llu vs %s fs %s %s count %d tgt %ux%u fl %x fmt %x wide %d scaled %d dt %d dw %d bl %d vp %d %d %d %d", (unsigned long long)(*g_stats).total,
                vs->hash.c_str(), fs ? fs->hash.c_str() : "-", indexed ? "idx" : "arr", indexed ? c.i[0] : c.i[1],
                wi ? wi->width : 0, wi ? wi->height : 0, tx ? tx->flags : 0, wi ? wi->nvn_format : 0, (*g_uw).on ? 1 : 0, key.uw_x != 0 ? 1 : 0,
                (*g_state).depth.depth_test, (*g_state).depth.depth_write, (*g_state).color.blend_enable[0],
                (*g_state).viewport[0], (*g_state).viewport[1], (*g_state).viewport[2], (*g_state).viewport[3]);
        }
    }
    {
        GpuImage* ti = t.color_count && t.colors[0] ? image_of(t.colors[0]) : t.depth ? image_of(t.depth) : nullptr;
        // Screen targets, and other targets drawn in the frame before too (redrawn every frame: shadow maps, the
        // post chain): a skipped draw is back the next frame. A target drawn once (a room's colour LUT) waits.
        const uint64_t f = g_frame.load(std::memory_order_relaxed);
        pipeline_skip_allowed(ti && (ti->uw || (ti->drawn_before && ti->drawn_before + 1 >= f)));
    }
    VkPipeline pipeline = graphics_pipeline(key, layout);
    if ((*t_bail)) return false;
    if (!pipeline) {
        (*g_stats).no_pipeline++;
        return false;
    }
    draw_part(3);

    if ((*g_bound).cb != cb || draw_slow(kSlowState)) (*g_bound) = Bound{}, (*g_bound_gen)++, (*g_bound).cb = cb;
    if ((*g_bound).pipeline != pipeline) {
        p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        (*g_bound).pipeline = pipeline, (*g_state_set)++;
    } else {
        (*g_state_kept)++;
    }
    bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout);
    draw_part(4);

    draw_issue(cb, t, indexed, c, in, vs, fs, ui_x);
    // Prepared for the next draws of the same state (the fast path above). Not when anything here was pending
    // (uploads, layout changes, a sampled target of this pass, pending clears) or debug overrides changed the shaders.
    if (!gathered && !data_dep && swapped.empty() && *g_rendering && !*t_bail && fast_draw_on()) {
        prep.valid = true;
        prep.gen = *g_state_gen, prep.uw_gen = uw_generation(), prep.cb = cb, prep.mode = mode;
        prep.t = t, prep.tt = tt;
        prep.shaders = shaders, prep.vs = vs, prep.fs = fs, prep.layout = &layout, prep.pipeline = pipeline;
        prep.set2 = sets_texture_set();
        prep.ui_x = ui_x;
        prep.key = key;
        prep.merge_ok = true;
        for (uint32_t s = 0; s < 16; s++) prep.merge_ok &= key.streams[s].divisor == 0;
        prep.textures.clear(), prep.handles.clear();
        const DrawTextures& dt = *g_textures;
        for (size_t k = 0; k < dt.resolved.size(); k++) {
            const ResolvedTexture& r = dt.resolved[k];
            if (r.texture) {
                Texture* tx = get<Texture>(r.texture);
                GpuImage* img = tx ? image_of(r.texture) : nullptr;
                if (!tx || !img) {
                    prep.valid = false;
                    break;
                }
                // A sampled image this pass renders to: the next draw may need the pass broken (pass_wrote).
                for (uint32_t i = 0; i < 9; i++)
                    if ((*g_pass_images)[i] == img) prep.valid = false;
                prep.textures.push_back({tx, img});
            }
            if (k < dt.bindings.size()) {
                const DescriptorBinding* b = dt.bindings[k].second;
                if (b->nvn.source == NvnSource::HandleInUniform && b->nvn.bank >= 3) {
                    const ShaderStage sg = dt.bindings[k].first->stage;  // nvn_sets.cpp's api_stage
                    prep.handles.push_back({sg == ShaderStage::Fragment ? 1 : sg == ShaderStage::Compute ? 5 : 0,
                                            b->nvn.bank - 3, uint32_t(b->nvn.word), k < dt.handles.size() ? dt.handles[k] : 0});
                }
            }
        }
        if (!prep.set2 && !dt.bindings.empty()) prep.valid = false;
    }
    // NX_SNAP_FS=<hash>,<hash>[@<first draw>]: also snapshot after draws with those fragment shaders (8 at most, or
    // NX_SNAP_FS_MAX).
    static const std::string snap_fs = std::getenv("NX_SNAP_FS") ? std::getenv("NX_SNAP_FS") : "";
    static const uint64_t snap_from = snap_fs.find('@') != std::string::npos
                                          ? std::strtoull(snap_fs.c_str() + snap_fs.find('@') + 1, nullptr, 10) : 0;
    static int snap_count = 0;
    static const int snap_max = std::getenv("NX_SNAP_FS_MAX") ? std::atoi(std::getenv("NX_SNAP_FS_MAX")) : 8;
    bool snap_by_fs = !snap_fs.empty() && (*g_stats).done >= snap_from && snap_count < snap_max &&
                      ((fs && snap_fs.find(fs->hash) != std::string::npos) ||
                       snap_fs.find(vs->hash) != std::string::npos) &&
                      ++snap_count;  // the vertex shader's hash matches too
    if ((snap_by_fs || snap_follow(t, vs, fs, (*g_textures).handles) || debug_snap_wanted((*g_stats).done)) && t.color_count && t.colors[0])
        if (GpuImage* snap = image_of(t.colors[0])) draw_end_rendering(cb, 6), debug_snapshot(cb, *snap, (*g_stats).done);
    // NX_DUMP_ON_TARGET=<hex NVNtexture*>: every NX_DUMP_FRAMES-th draw into it asks for NX_DUMP_TEXTURES to be read
    // back right after this submit (before later frames change them).
    static const char* dump_env = std::getenv("NX_DUMP_ON_TARGET");
    static const uint64_t dump_target = dump_env ? std::strtoull(dump_env, nullptr, 16) : 0;
    if (dump_target && t.colors[0] == dump_target) g_dump_request = ++g_dump_draws;
    // NX_DUMP_ON_FS=<fs hash>[,...]: a draw with one of these fragment shaders dumps its target 0 and what it samples.
    static const std::string dump_fs = [] { const char* e = std::getenv("NX_DUMP_ON_FS"); return std::string(e ? e : ""); }();
    static const uint64_t dump_from = [] { const char* e = std::getenv("NX_DUMP_FROM"); return e ? std::strtoull(e, nullptr, 10) : 0; }();
    if (!dump_fs.empty() && fs && g_frame >= dump_from && dump_fs.find(fs->hash) != std::string::npos && g_dump_draws < 400 &&
        t.colors[0] && image_of(t.colors[0]) && (std::getenv("NX_DUMP_ANY") || image_of(t.colors[0])->view_type == VK_IMAGE_VIEW_TYPE_CUBE ||
                                                           image_of(t.colors[0])->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) {
        debug_dump_add(t.colors[0]);
        for (const ResolvedTexture& r : (*g_textures).resolved) debug_dump_add(r.texture);
        g_dump_request = ++g_dump_draws;
    }
    trace_draw(t, mode, indexed, c, vs, fs, (*g_textures).bindings, (*g_textures).handles);
    return true;
}

}  // namespace

void draw_end_submit() {  // after the submit's fence: compute results to guest memory, debug snapshots
    debug_snapshots_write();
    for (const WriteBack& w : draw_take_write_backs()) {
        trace_nan_write_back(w.from, w.to, w.size);
        apply_write_back(w);
    }
}

std::vector<WriteBack> draw_take_write_backs() {  // an asynchronous submit copies them when it is retired
    std::vector<WriteBack> v = written_take();  // one per written range (disjoint), then NX_COMPUTE_SEPARATE's
    v.insert(v.end(), separate_write_backs().begin(), separate_write_backs().end());
    separate_write_backs().clear();
    return v;
}

void draw_timing_submit();
// A parallel replay worker's chunk (docs/PARALLEL_REPLAY.md): its own command buffer, caches and copies, fresh; the
// submit-wide state (written ranges, pipelines, lists) stays the replay thread's.
// `fresh`: a new submit for this thread (its caches of sets, copies and handles start over); else a chunk of the
// same submit, which keeps them (they are valid for the whole submit).
void draw_begin_worker(bool fresh) {
    (*g_rendering) = false;
    (*g_vertex_pending) = false, (*g_vertex_visible_at) = (*g_pass_serial);
    (*g_bound) = Bound{}, (*g_bound_gen)++;
    (*g_pass_written) = 0;
    (*g_uw) = UwTarget{};
    clears_begin_submit();
    if (fresh) sets_begin_submit(), dedup_reset();
}

void draw_begin_submit() {
    draw_timing_submit();
    (*g_rendering) = false;
    (*g_vertex_pending) = false, (*g_vertex_visible_at) = (*g_pass_serial);  // each command buffer begins with a full barrier
    (*g_bound) = Bound{}, (*g_bound_gen)++;  // a new command buffer
    submit_lists_reset();
    sets_begin_submit();
    clears_begin_submit();
    g_submit_ns = plat::monotonic_ns();
    dedup_reset();
    written_reset();
    pipeline_begin_submit();
}

uint64_t recorded_draws();

void draw_log_stats() {
    if (*g_fast_hits + *g_fast_misses)
        log("nvn: draw fast path: %llu of %llu draws (%.1f%%)", (unsigned long long)*g_fast_hits,
            (unsigned long long)(*g_fast_hits + *g_fast_misses),
            100.0 * double(*g_fast_hits) / double(*g_fast_hits + *g_fast_misses));
    *g_fast_hits = *g_fast_misses = 0;
    if (*g_merge_runs)
        log("nvn: draw merging: %llu draws in %llu instanced draws (%.1f a draw), %llu runs drawn one by one (pipeline "
            "compiling), %llu counted in as continuations", (unsigned long long)*g_merge_draws,
            (unsigned long long)*g_merge_runs, double(*g_merge_draws) / double(*g_merge_runs),
            (unsigned long long)*g_merge_fallbacks, (unsigned long long)*g_merge_quick);
    *g_merge_runs = *g_merge_draws = *g_merge_fallbacks = *g_merge_quick = 0;
    if (std::getenv("NX_MERGE_WHY")) {
        std::string w;
        for (int k = 0; k < 20; k++) w += " " + std::to_string((*g_merge_why)[k]), (*g_merge_why)[k] = 0;
        log("nvn: draw merging refused by reason:%s", w.c_str());
    }
    if ((*g_vertex_barriers)) log("nvn passes: %llu vertex-read barriers (G4)", (unsigned long long)(*g_vertex_barriers));
    dedup_log();
    render_pass_log();
    written_log();
    sampled_ready_log();
    sets_log();
    if ((*g_state_set) + (*g_state_kept))
        log("nvn: draw state: %llu binds and sets recorded, %llu already in place", (unsigned long long)(*g_state_set),
            (unsigned long long)(*g_state_kept));
    (*g_state_set) = (*g_state_kept) = 0;
    clears_log();
    log("nvn passes ended: %llu submit end, %llu draw, %llu dispatch, %llu clear of other targets, %llu clear larger than "
        "the pass, %llu copy, %llu snapshot, %llu SetRenderTargets", (unsigned long long)(*g_pass_ends)[0],
        (unsigned long long)(*g_pass_ends)[1], (unsigned long long)(*g_pass_ends)[2], (unsigned long long)(*g_pass_ends)[3],
        (unsigned long long)(*g_pass_ends)[4], (unsigned long long)(*g_pass_ends)[5], (unsigned long long)(*g_pass_ends)[6],
        (unsigned long long)(*g_pass_ends)[7]);
    Stats& t = (*g_stats);
    log("nvn draws: %llu recorded, %llu replayed:", (unsigned long long)recorded_draws(), (unsigned long long)t.total);
    log("nvn draws: %llu total, %llu done, %llu no program, %llu no vertex shader, %llu no pipeline, %llu no indices, "
        "%llu missing a stage", (unsigned long long)t.total, (unsigned long long)t.done, (unsigned long long)t.no_program,
        (unsigned long long)t.no_vertex, (unsigned long long)t.no_pipeline, (unsigned long long)t.no_indices,
        (unsigned long long)t.missing_stage);
    log("nvn passes: %llu ended by a draw's texture uploads or layout changes, %llu by new targets, %llu dispatches",
        (unsigned long long)t.breaks_textures, (unsigned long long)t.breaks_targets, (unsigned long long)t.dispatches);
}

void pass_end_only(VkCommandBuffer cb, int reason) {
    if (!(*g_rendering)) return;
    merge_flush();
    (*g_pass_ends)[reason & 7]++, trace_pass("end: reason", (*g_pass), uint32_t(reason), 0);
    if ((*g_render_pass))
        p_vkCmdEndRenderPass(cb);
    else
        p_vkCmdEndRendering(cb);
    (*g_rendering) = false;
    // What the pass wrote is read by what follows (sampled, copied, loaded by the next pass), and what it sampled may
    // be rendered over next. The images stay GENERAL, so transition() records no barrier. Metal (MoltenVK) and the
    // Intel driver ordered passes anyway; NVIDIA overlaps them, and later passes read half-written tiles: the course
    // HUD in blocks (DEBUG-2 #71). NX_NO_PASS_BARRIER=1: none, as before (bisecting).
    // Narrow (PHASE4.md G4; NX_PASS_BARRIER_NARROW=1): the later stages only, so the next pass's vertex work and
    // binning may start while this one finishes. A draw whose vertex shader samples a rendered image gets a barrier
    // of its own first (vertex_read_barrier). By default ALL_COMMANDS, as before.
    static const bool no_barrier = std::getenv("NX_NO_PASS_BARRIER") != nullptr;
    if (!no_barrier) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        VkPipelineStageFlags dst = narrow_pass_barrier()
                                       ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                       : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
        (*g_vertex_pending) |= narrow_pass_barrier();
    }
    (*g_pass_serial)++;
    for (uint32_t k = 0; k < 9; k++)  // this pass's targets only (the other slots may name freed images)
        if ((k < (*g_pass).color_count || (k == 8 && (*g_pass).depth)) && (*g_pass_images)[k])
            (*g_pass_images)[k]->rendered_at = (*g_pass_serial);
    gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");
}
void draw_end_rendering(VkCommandBuffer cb, int reason) {  // and the pending clears (nvn_clears.cpp)
    pass_end_only(cb, reason);
    clears_flush(cb);
}
bool pass_open() { return (*g_rendering); }
bool pass_open_on(const Targets& t) { return (*g_rendering) && targets_same((*g_pass), t); }
VkExtent2D pass_extent() { return (*g_pass_extent); }
void pass_begin(VkCommandBuffer cb, const Targets& t) { begin_rendering(cb, t); }
// After a clear inside the pass (nvn_clears.cpp). A masked one is a fullscreen draw with its own pipeline, viewport
// and scissor (nvn_mask_clear.cpp): the next draw sets everything again.
uint64_t bound_generation() { return *g_bound_gen; }
void draw_merge_flush() { merge_flush(); }
void pass_mark_written(uint32_t bits) { (*g_pass_written) |= bits, (*g_bound) = Bound{}, (*g_bound_gen)++; }

bool replay::pass_wrote(const GpuImage* img) {
    if (!(*g_rendering) || !img) return false;
    uint32_t hit = (*g_pass).depth && (*g_pass_images)[8] == img ? 1u << 8 : 0;
    for (uint32_t c = 0; c < (*g_pass).color_count; c++)
        if ((*g_pass).colors[c] && (*g_pass_images)[c] == img) hit |= 1u << c;
    return (hit & (*g_pass_written)) != 0;
}

uint64_t submit_start_ns() { return g_submit_ns; }

// Entry points of the draws and dispatches for draw_command (nvn_draw_state.cpp).
bool draw_call(VkCommandBuffer cb, const Targets& t, uint32_t mode, bool indexed, const Cmd& c) {
    if (!draw_timing()) return draw(cb, t, mode, indexed, c);
    g_draw_mark = draw_ticks();
    const bool r = draw(cb, t, mode, indexed, c);
    draw_part(5);
    g_draw_timed++;
    {   // draws per target size (and depth-only), logged with the timing
        GpuImage* img = t.color_count && t.colors[0] ? image_of(t.colors[0]) : nullptr;
        GpuImage* dimg = !img && t.depth ? image_of(t.depth) : nullptr;
        char k[48];
        std::snprintf(k, sizeof k, "%ux%u%s", img ? img->width : dimg ? dimg->width : 0,
                      img ? img->height : dimg ? dimg->height : 0, dimg ? "d" : "");
        g_draws_by_target[k]++;
    }
    return r;
}

void draw_timing_submit() {
    static uint64_t submits = 0;
    if (!draw_timing() || ++submits % 300) return;
    uint64_t freq = 1000000000;
#if defined(__aarch64__) && !defined(_MSC_VER)
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
#endif
    std::string line;
    double total = 0;
    for (int k = 0; k < kDrawPartCount; k++) {
        const double us = double(g_draw_part[k]) * 1e6 / double(freq) / 300.0;
        total += us;
        char b[64];
        std::snprintf(b, sizeof b, " %s %.0f", kDrawParts[k], us);
        line += b;
        g_draw_part[k] = 0;
    }
    static const bool ab = std::getenv("NX_AB") != nullptr;
    log("draw timing%s: %.0f draws per submit, %.2f ms per submit in draws (us:%s)", !ab ? "" : g_ab_new ? " [new]" : " [old]",
        double(g_draw_timed) / 300.0, total / 1000.0, line.c_str());
    g_draw_timed = 0;
    if (ab) g_ab_new = !g_ab_new;
    std::vector<std::pair<uint64_t, std::string>> v;
    for (auto& [k, n] : g_draws_by_target) v.push_back({n, k});
    std::sort(v.rbegin(), v.rend());
    std::string by;
    for (size_t i = 0; i < v.size() && i < 12; i++) by += " " + v[i].second + ":" + std::to_string(v[i].first / 300);
    log("draw timing: draws per submit by target:%s", by.c_str());
    g_draws_by_target.clear();
}
void dispatch_call(VkCommandBuffer cb, const Cmd& c) { dispatch(cb, c); }

uint64_t draw_take_dump_request() {
    uint64_t r = g_dump_request;
    g_dump_request = 0;
    return r;
}

}  // namespace nx::nvn
