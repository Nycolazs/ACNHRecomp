// Built-in frame generation (NX_FRAMEGEN=1, video.frame_gen = builtin): the game keeps its 30 fps logic (it advances
// per frame) and the window shows 60, a frame made halfway between each two real ones.
//
// When real frame N arrives (gpu_present), N-1 is still on the GPU: the motion between the two is estimated, the
// in-between frame is drawn and presented at once, and N follows half a game frame later (nvn_vk.cpp). The cost is
// that half frame of latency, plus the GPU time below. Everything is compute work on the presented image, before the
// present pass (so FSR or the plain scaling apply to both frames alike):
//   1. luma.comp: N's luma at 1/4, 1/8, 1/16 and 1/32 (one pass). N-1 is read from its own window texture (not
//      copied: see framegen_record), its luma kept from the last time.
//   2. search.comp: motion vectors by symmetric block matching (the vector v at x pairs N-1 at x - v/2 with N at
//      x + v/2: vectors of the in-between frame itself, no holes), coarse to fine: a full search at 1/32 (+-96 px),
//      then a vector per 16x16 pixels, refined on the 1/8 and 1/4 luma down to a pixel (each pass takes the last
//      one's vectors of the cell and its neighbours as candidates). The last frame's field is a candidate too. The
//      cost leaves out the patch's mean change, so fades still match. A cell whose match is already near perfect is
//      not refined further (still, flat or steadily moving areas: most of the search's cost on a phone).
//   3. smooth.comp: a 3x3 vector median; cells without a good match are counted.
//   4. synth.comp: per pixel, the best-agreeing of a few nearby vectors; unchanged pixels (HUD, still scenery) are
//      copied as they are; no good vector: the plain cross-fade; most cells unmatched (a scene cut): all of it.
// GPU time per generated frame is measured with timestamps and logged ("nvn/fg:"): ~1.2-1.8 ms at 1080p on an Intel
// Iris Xe.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

uint32_t vk_queue_family();  // nvn_vk.cpp

namespace {

#include "nvn_framegen_spirv.inc"

enum Pass { kLuma, kSearch, kSmooth, kSynth, kPassCount };

struct Push {
    int32_t size[4];
    float a[4], b[4];
};

constexpr uint32_t kStamps = 4, kTimingSlots = 8;

struct State {
    bool tried = false, ok = false;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipes[kPassCount] = {};
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    GpuImage gen;         // the in-between frame (RGBA8)
    GpuImage luma[2][4];  // per frame: 1/4, 1/8, 1/16, 1/32
    GpuImage field[2];    // the coarse-to-fine fields: 1/32, then 1/16 matched on the 1/8 luma
    GpuImage fine;        // the 1/16 field refined on the 1/4 luma, to a pixel
    GpuImage final_[2];   // smoothed; the other one is the last frame's (a candidate)
    GpuImage unmatched;   // 1x1 R32UI: cells without a good match (a scene cut: cross-fade)
    int cur = 0;
    bool have_prev = false, have_field = false, warned = false;
    // The last real frame: its window texture, the image behind it then, and its last render serial.
    uint64_t last_texture = 0, last_rendered = 0;
    GpuImage* last_image = nullptr;
    // Pacing (nvn_vk.cpp): when the real frames come.
    uint64_t last_ns = 0, period_ns = 33'333'333;
    // GPU timestamps.
    VkQueryPool queries = VK_NULL_HANDLE;
    double tick_ns = 0;
    uint64_t ts_mask = 0;
    bool slot_used[kTimingSlots] = {};
    uint32_t slot = 0;
    double sum_ms[kStamps - 1] = {}, min_ms[kStamps - 1] = {1e9, 1e9, 1e9}, worst_ms = 0, best_ms = 1e9;
    uint32_t timed = 0;
    uint64_t generated = 0;
} g;

void destroy_image(GpuImage& img) {
    VkDevice d = vk_device();
    if (img.view) p_vkDestroyImageView(d, img.view, nullptr);
    if (img.image) p_vkDestroyImage(d, img.image, nullptr);
    if (img.memory) p_vkFreeMemory(d, img.memory, nullptr);
    img = GpuImage{};
}

bool make_image(GpuImage& img, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    VkDevice d = vk_device();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = format, ci.extent = {std::max(w, 1u), std::max(h, 1u), 1};
    ci.mipLevels = 1, ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (p_vkCreateImage(d, &ci, nullptr, &img.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(d, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size, ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (p_vkAllocateMemory(d, &ai, nullptr, &img.memory) != VK_SUCCESS) return false;
    p_vkBindImageMemory(d, img.image, img.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(d, &vi, nullptr, &img.view) != VK_SUCCESS) return false;
    img.format = format, img.width = ci.extent.width, img.height = ci.extent.height;
    img.rendered = true;
    return true;
}

constexpr size_t kImageCount = 2 * (1 + 4) + 2 + 3;
void all_images(GpuImage* (&out)[kImageCount]) {
    size_t n = 0;
    for (int k = 0; k < 2; k++) {
        out[n++] = &g.final_[k];
        for (auto& l : g.luma[k]) out[n++] = &l;
    }
    for (auto& f : g.field) out[n++] = &f;
    out[n++] = &g.gen, out[n++] = &g.fine, out[n++] = &g.unmatched;
}

uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

// The images for a w x h picture (made again when the presented size changes).
bool make_images(uint32_t w, uint32_t h, VkFormat format) {
    GpuImage* imgs[kImageCount];
    all_images(imgs);
    if (g.w) {
        if (async_gpu()) frame_wait_all();
        p_vkDeviceWaitIdle(vk_device());
        for (GpuImage* i : imgs) destroy_image(*i);
    }
    g.w = g.h = 0;
    g.have_prev = g.have_field = false;
    g.last_texture = 0, g.last_image = nullptr;
    const VkImageUsageFlags dump = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // NX_DUMP_FRAMES, and the blit to the swapchain
    bool ok = true;
    for (int k = 0; k < 2; k++) {
        ok = ok && make_image(g.final_[k], VK_FORMAT_R16G16B16A16_SFLOAT, div_up(w, 16), div_up(h, 16), 0);
        for (uint32_t l = 0; l < 4; l++)
            ok = ok && make_image(g.luma[k][l], VK_FORMAT_R16_SFLOAT, div_up(w, 4u << l), div_up(h, 4u << l), 0);
    }
    for (uint32_t l = 0; l < 2; l++)
        ok = ok && make_image(g.field[l], VK_FORMAT_R16G16B16A16_SFLOAT, div_up(w, l ? 16 : 32), div_up(h, l ? 16 : 32), 0);
    ok = ok && make_image(g.fine, VK_FORMAT_R16G16B16A16_SFLOAT, div_up(w, 16), div_up(h, 16), 0);
    ok = ok && make_image(g.gen, VK_FORMAT_R8G8B8A8_UNORM, w, h, dump);
    ok = ok && make_image(g.unmatched, VK_FORMAT_R32_UINT, 1, 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!ok) {
        log("nvn/fg: images for %ux%u could not be made: frame generation off", w, h);
        return false;
    }
    g.w = w, g.h = h;
    log("nvn/fg: %ux%u (format %d), motion field %ux%u", w, h, int(format), div_up(w, 16), div_up(h, 16));
    return true;
}

bool init() {
    VkDevice d = vk_device();
    // The luma levels are R16F storage images, filtered (not a core requirement for storage, but desktop GPUs have it).
    VkFormatProperties fp{};
    p_vkGetPhysicalDeviceFormatProperties(vk_physical(), VK_FORMAT_R16_SFLOAT, &fp);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((fp.optimalTilingFeatures & need) != need) {
        log("nvn/fg: R16F is not a filtered storage format here: frame generation off");
        return false;
    }
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (p_vkCreateSampler(d, &si, nullptr, &g.sampler) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[8] = {};
    for (uint32_t k = 0; k < 8; k++) {
        b[k].binding = k, b[k].descriptorCount = 1, b[k].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[k].descriptorType = k < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        if (k < 4) b[k].pImmutableSamplers = &g.sampler;
    }
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 8, dli.pBindings = b;
    if (p_vkCreateDescriptorSetLayout(d, &dli, nullptr, &g.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1, pli.pSetLayouts = &g.set_layout;
    pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &push;
    if (p_vkCreatePipelineLayout(d, &pli, nullptr, &g.layout) != VK_SUCCESS) return false;
    struct Code {
        const uint32_t* words;
        size_t bytes;
    } code[kPassCount] = {{kFgLuma, sizeof kFgLuma}, {kFgSearch, sizeof kFgSearch}, {kFgSmooth, sizeof kFgSmooth},
                          {kFgSynth, sizeof kFgSynth}};
    for (int k = 0; k < kPassCount; k++) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = code[k].bytes, mi.pCode = code[k].words;
        VkShaderModule m = VK_NULL_HANDLE;
        if (p_vkCreateShaderModule(d, &mi, nullptr, &m) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT, ci.stage.module = m, ci.stage.pName = "main";
        ci.layout = g.layout;
        VkResult r = p_vkCreateComputePipelines(d, pipeline_cache(), 1, &ci, nullptr, &g.pipes[k]);
        p_vkDestroyShaderModule(d, m, nullptr);
        if (r != VK_SUCCESS) return false;
    }
    // Timestamps, where the queue has them.
    uint32_t n = 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties(vk_physical(), &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    p_vkGetPhysicalDeviceQueueFamilyProperties(vk_physical(), &n, families.data());
    uint32_t bits = vk_queue_family() < n ? families[vk_queue_family()].timestampValidBits : 0;
    if (bits) {
        VkPhysicalDeviceProperties props{};
        p_vkGetPhysicalDeviceProperties(vk_physical(), &props);
        g.tick_ns = props.limits.timestampPeriod;
        g.ts_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP, qi.queryCount = kStamps * kTimingSlots;
        if (p_vkCreateQueryPool(d, &qi, nullptr, &g.queries) != VK_SUCCESS) g.queries = VK_NULL_HANDLE;
    }
    log("nvn/fg: frame generation on (GPU timestamps %s)", g.queries ? "on" : "unavailable");
    return true;
}

bool ready() {
    if (!g.tried) {
        g.tried = true;
        g.ok = init();
        if (!g.ok) log("nvn/fg: frame generation unavailable: frames are presented as the game makes them");
    }
    return g.ok;
}

// One dispatch of `pass` over gx x gy groups; bindings 0-3 sampled, 4-7 storage (GENERAL).
void dispatch(VkCommandBuffer cb, Pass pass, std::initializer_list<std::pair<uint32_t, GpuImage*>> binds, const Push& push,
              uint32_t gx, uint32_t gy) {
    VkDescriptorSet set = descriptor_set(g.set_layout);
    VkDescriptorImageInfo ii[8];
    VkWriteDescriptorSet w[8];
    uint32_t n = 0;
    for (auto& [binding, img] : binds) {
        ii[n] = {g.sampler, img->view, img->layout};
        w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[n].dstSet = set, w[n].dstBinding = binding, w[n].descriptorCount = 1;
        w[n].descriptorType = binding < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[n].pImageInfo = &ii[n];
        n++;
    }
    p_vkUpdateDescriptorSets(vk_device(), n, w, 0, nullptr);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipes[pass]);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 1, &set, 0, nullptr);
    p_vkCmdPushConstants(cb, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    p_vkCmdDispatch(cb, std::max(gx, 1u), std::max(gy, 1u), 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
}

void stamp(VkCommandBuffer cb, uint32_t k, bool timed) {
    if (timed)
        // The first once the game's frame is done (TOP_OF_PIPE would count the end of its rendering).
        p_vkCmdWriteTimestamp(cb, k ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, g.queries,
                              g.slot * kStamps + k);
}

// The timestamps of the slot about to be reused (8 generated frames ago), if the GPU has written them.
void collect_slot() {
    if (!g.queries || !g.slot_used[g.slot]) return;
    uint64_t t[kStamps] = {};
    if (p_vkGetQueryPoolResults(vk_device(), g.queries, g.slot * kStamps, kStamps, sizeof t, t, 8,
                                VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    double total = 0;
    for (uint32_t k = 0; k + 1 < kStamps; k++) {
        double ms = double((t[k + 1] - t[k]) & g.ts_mask) * g.tick_ns / 1e6;
        g.sum_ms[k] += ms, g.min_ms[k] = std::min(g.min_ms[k], ms), total += ms;
    }
    g.worst_ms = std::max(g.worst_ms, total), g.best_ms = std::min(g.best_ms, total);
    if (++g.timed % 300) return;
    // The best frame (and each pass's best) is the one least shared with other work on the GPU: other processes', or
    // the game's next frame.
    log("nvn/fg: GPU %.2f ms per generated frame (luma %.2f, motion %.2f, in-between frame %.2f), best %.2f "
        "(%.2f + %.2f + %.2f), worst %.2f, over %u, %ux%u",
        (g.sum_ms[0] + g.sum_ms[1] + g.sum_ms[2]) / g.timed, g.sum_ms[0] / g.timed, g.sum_ms[1] / g.timed,
        g.sum_ms[2] / g.timed, g.best_ms, g.min_ms[0], g.min_ms[1], g.min_ms[2], g.worst_ms, g.timed, g.w, g.h);
    g.timed = 0, g.worst_ms = 0, g.best_ms = 1e9;
    for (double& m : g.min_ms) m = 1e9;
    for (double& s : g.sum_ms) s = 0;
}

}  // namespace

// Read again at each present (framegen_update, nvn_vk.cpp gpu_present): the settings screen turns it on and off while
// the game runs; one frame's presents all see the same answer. NX_FRAMEGEN set before the game wins.
std::atomic<bool> g_fg_on{false};

void framegen_update() {
    static uint64_t gen = ~0ull;
    const uint64_t now = config_generation();
    if (now == gen) return;
    gen = now;
    // Off by default everywhere: block matching without the game's motion vectors smears what moves against what
    // moves behind it (the player running past flowers and bushes, every other refresh: it looked like flicker),
    // and on a Snapdragon 865 it costs ~4 ms of GPU a frame and ~6 C. video.frame_gen = builtin turns it on.
    bool on;
    if (const char* e = config_env_override("NX_FRAMEGEN")) on = e[0] == '1';
    else on = config_get("video.frame_gen", "off") == "builtin";
    // 60 presents a second already (video.frame_rate = 60): nothing to fill in.
    const char* i = std::getenv("NX_PRESENT_INTERVAL");
    if (on && i && std::atoi(i) == 1) {
        static bool logged = false;
        if (!logged) log("nvn/fg: the game presents at 60 fps (NX_PRESENT_INTERVAL=1): frame generation off"), logged = true;
        on = false;
    }
    if (on != g_fg_on.load(std::memory_order_relaxed)) log("nvn/fg: frame generation %s", on ? "on" : "off");
    g_fg_on.store(on, std::memory_order_relaxed);
}

bool framegen_wanted() {  // not with AMD's (nvn_temporal.cpp): one frame generation at a time
    return g_fg_on.load(std::memory_order_relaxed) && (!g.tried || g.ok) && !ffx_fg_wanted();
}

bool framegen_frame_begin(uint64_t now_ns) {
    uint64_t dt = g.last_ns ? now_ns - g.last_ns : 0;
    g.last_ns = now_ns;
    if (!dt) return false;
    // The real frames' period, smoothed (a hitch counts for little): the real frame is shown half of it after the
    // generated one.
    uint64_t clamped = std::clamp<uint64_t>(dt, 20'000'000, 50'000'000);
    g.period_ns = (g.period_ns * 7 + clamped) / 8;
    // Faster than ~45 fps there is nothing to fill in. Slower than 20 fps (a heavy scene, loading, a hitch) the frames
    // are shown as they come: the two are far apart, and waiting between them would hold up the replay of the next.
    return dt > 22'000'000 && dt < 50'000'000;
}

uint64_t framegen_real_delay_ns() { return std::clamp<uint64_t>(g.period_ns / 2, 8'000'000, 25'000'000); }

GpuImage* framegen_record(VkCommandBuffer cb, uint64_t texture, GpuImage& src, bool generate, GpuImage** prev) {
    if (prev) *prev = nullptr;
    if (!ready() || !src.view || src.view_type != VK_IMAGE_VIEW_TYPE_2D || !(src.aspect & VK_IMAGE_ASPECT_COLOR_BIT))
        return nullptr;
    if ((src.width != g.w || src.height != g.h) && !make_images(src.width, src.height, src.format)) {
        g.ok = false;
        return nullptr;
    }
    // The last frame is read where the game left it: its window texture is not drawn into again before this one is
    // presented (the game rotates 3; the next frames are replayed after this present). Checked: another texture, the
    // same image, not rendered into since.
    GpuImage* last = g.last_texture && g.last_texture != texture ? image_of(g.last_texture) : nullptr;
    bool last_ok = last && last == g.last_image && last->rendered_at == g.last_rendered && last->width == src.width &&
                   last->height == src.height && last->layout != VK_IMAGE_LAYOUT_UNDEFINED;
    if (!last_ok && g.last_texture && g.have_prev && !g.warned) {
        g.warned = true;
        log("nvn/fg: the last frame's window texture was reused or redrawn (texture %llx after %llx): no in-between "
            "frame then", (unsigned long long)texture, (unsigned long long)g.last_texture);
    }
    g.last_texture = texture, g.last_image = &src, g.last_rendered = src.rendered_at;
    int cur = g.cur ^ 1, old = g.cur;
    generate = generate && g.have_prev && last_ok;
    const bool timed = generate && g.queries;
    if (timed) {
        collect_slot();
        p_vkCmdResetQueryPool(cb, g.queries, g.slot * kStamps, kStamps);
    }
    stamp(cb, 0, timed);
    GpuImage* imgs[kImageCount];
    all_images(imgs);
    for (GpuImage* i : imgs) transition(cb, *i, VK_IMAGE_LAYOUT_GENERAL);
    transition(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const uint32_t w = g.w, h = g.h;
    GpuImage(&L)[4] = g.luma[cur];
    GpuImage(&P)[4] = g.luma[old];
    Push p{};
    p.size[0] = int32_t(w), p.size[1] = int32_t(h);
    dispatch(cb, kLuma, {{0, &src}, {4, &L[3]}, {5, &L[0]}, {6, &L[1]}, {7, &L[2]}}, p, div_up(w, 32), div_up(h, 32));
    g.cur = cur;
    g.have_prev = true;
    if (!generate) {
        g.have_field = false;  // the next pair starts afresh
        return nullptr;
    }
    stamp(cb, 1, timed);
    // Coarse to fine. a: full pixels per field cell, per luma texel, per coarser cell, per temporal cell;
    // b: the refinement steps (luma texels), from x halving down to y. 1/32: +-3 texels (+-96 px; the last frame's
    // vector covers faster motion); then 16x16-pixel cells on the 1/8 luma (16, 8, 4 px) and the 1/4 (4, 2, 1 px).
    // b.z: the coarse passes compare 2x2 taps (means of 2x2 texels), the last one every texel. b.w: the cost below
    // which a cell's vector is not refined further, last pass only: a mean difference of 1/400 over its 16 texels
    // (about 0.6 of an 8-bit level; noise and sub-pixel resampling leave more than that on textured moving areas,
    // so only matches that refining could not visibly improve stop early). Not on the coarse levels: their box-
    // averaged luma can match that well a vector still 16 px off, which the last pass's +-7 px would not undo.
    constexpr float kGoodEnough = 0.04f;
    struct Level {
        int mode;
        GpuImage *lprev, *lcur, *coarse, *out;
        float a[4], b[4];
    } levels[] = {
        {0, &P[3], &L[3], nullptr, &g.field[0], {32, 32, 0, 16}, {0.5f, 0.5f, 1, 0}},
        {1, &P[1], &L[1], &g.field[0], &g.field[1], {16, 8, 32, 16}, {2, 0.5f, 1, 0}},
        {2, &P[0], &L[0], &g.field[1], &g.fine, {16, 4, 16, 16}, {1, 0.25f, 0, kGoodEnough}},
    };
    GpuImage& temporal = g.final_[old];
    for (const Level& l : levels) {
        Push q{};
        q.size[0] = l.mode, q.size[1] = 3, q.size[2] = g.have_field ? 1 : 0;
        std::memcpy(q.a, l.a, sizeof q.a);
        std::memcpy(q.b, l.b, sizeof q.b);
        GpuImage* coarse = l.coarse ? l.coarse : &temporal;  // mode 0 reads no coarser field
        dispatch(cb, kSearch, {{0, l.lprev}, {1, l.lcur}, {2, coarse}, {3, &temporal}, {4, l.out}}, q,
                 div_up(l.out->width, 8), div_up(l.out->height, 8));
    }
    // The median, and the count of cells whose best cost is above 1 (a mean luma difference of 1/16 over the patch) or
    // whose mean luma changed by more than 0.12 (more than a fade does from one frame to the next).
    VkClearColorValue zero{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdClearColorImage(cb, g.unmatched.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr,
                           0, nullptr);
    GpuImage& field = g.final_[cur];
    Push m = p;
    m.a[0] = 1.0f, m.a[1] = 0.12f;
    dispatch(cb, kSmooth, {{0, &g.fine}, {4, &field}, {5, &g.unmatched}}, m, div_up(field.width, 8),
             div_up(field.height, 8));
    g.have_field = true;
    stamp(cb, 2, timed);
    transition(cb, *last, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // In-between frame; a cross-fade when more than 60% of the cells are unmatched (a scene cut).
    Push s = p;
    s.size[2] = int32_t(field.width * field.height * 6 / 10);
    s.a[0] = 16;
    dispatch(cb, kSynth, {{0, last}, {1, &src}, {2, &field}, {4, &g.gen}, {5, &g.unmatched}}, s, div_up(w, 16),
             div_up(h, 16));
    stamp(cb, 3, timed);
    if (timed) g.slot_used[g.slot] = true, g.slot = (g.slot + 1) % kTimingSlots;
    if (++g.generated == 1) log("nvn/fg: first in-between frame (%ux%u)", w, h);
    if (prev) *prev = last;
    return &g.gen;
}

}  // namespace nx::nvn
