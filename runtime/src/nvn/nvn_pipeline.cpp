// Pipelines and sampled resources (nvn_pipeline.h). Everything is cached for the life of the process: layouts by
// their bindings, pipelines by their full state, samplers by their state, views by (image, type).
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

#include "nvn_pipeline.h"
#include "nvn_ultrawide.h"
#include "platform.h"

namespace nx::nvn {

// The caches of views, layouts, samplers and pipelines, shared by the replay thread and the parallel replay workers
// (docs/PARALLEL_REPLAY.md): held on their slow paths (a lookup past the thread's own front entries, a creation).
// Recursive: a layout's creation asks for set layouts, a pipeline's for its layout.
std::recursive_mutex& cache_mutex() {
    static std::recursive_mutex m;
    return m;
}

using namespace nx::vk;

// NVN views carry a format: the game renders into RGBA8 textures through sRGB views (the GPU encodes on write and
// blends in linear) and samples them the same way. Ignoring it lost one conversion per draw: far too much contrast
// and saturation (docs/nvn/DEBUG.md #29).
VkFormat view_format(const GpuImage& img, uint32_t nvn) {
    if (VkFormat f; bc_view_format(img, nvn, f)) return f;  // decoded BC (nvn_bc.cpp)
    if (!nvn || nvn == img.nvn_format || img.alpha_from_r || img.aspect != VK_IMAGE_ASPECT_COLOR_BIT) return img.format;
    VkFormat f = vk_format(nvn);
    uint32_t bw = 1, bh = 1, bytes = 0, iw = 1, ih = 1, ibytes = 0;
    format_block(nvn, bw, bh, bytes);
    format_block(img.nvn_format, iw, ih, ibytes);
    // The sizes of the host formats: R11G11B10F is 4 bytes in NVN but RGBA16F here, and a 4-byte view (RGBA8, R32F)
    // of an 8-byte image is no valid view (G4, docs/nvn/DEBUG.md #50). NX_VIEW_NVN_SIZES=1: the NVN sizes (bisecting).
    static const bool nvn_sizes = std::getenv("NX_VIEW_NVN_SIZES") != nullptr;
    if (!nvn_sizes && vk_block_bytes(f) && vk_block_bytes(img.format))
        bytes = vk_block_bytes(f), ibytes = vk_block_bytes(img.format);
    bool ok = f != VK_FORMAT_UNDEFINED && format_aspect(nvn) == 0 && bw == iw && bh == ih && bytes == ibytes;
    if (ok && f != img.format && !img.mutable_format) {  // ubwc_flags: an image made without MUTABLE_FORMAT
        static std::set<std::pair<uint32_t, uint32_t>> not_mutable;
        if (not_mutable.insert({img.nvn_format, nvn}).second)
            log("nvn/vk: texture format %#x viewed as %#x, but the image is not mutable: its own format", img.nvn_format,
                nvn);
        return img.format;
    }
    static std::set<std::pair<uint32_t, uint32_t>> logged;
    if (logged.insert({img.nvn_format, nvn}).second && logged.size() <= 40)
        log("nvn/vk: texture format %#x viewed as %#x%s", img.nvn_format, nvn, ok ? "" : " (not compatible: ignored)");
    return ok ? f : img.format;
}

VkImageViewType view_type(const ImageShape& s) {  // SPIR-V Dim: 0 1D, 1 2D, 2 3D, 3 Cube
    switch (s.dim) {
        case 0: return s.arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case 2: return VK_IMAGE_VIEW_TYPE_3D;
        case 3: return s.arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        default: return s.arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

namespace {

int process_id() {
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

VkShaderStageFlags stage_bit(ShaderStage s) {
    return s == ShaderStage::Vertex     ? VK_SHADER_STAGE_VERTEX_BIT
           : s == ShaderStage::Fragment ? VK_SHADER_STAGE_FRAGMENT_BIT
                                        : VK_SHADER_STAGE_COMPUTE_BIT;
}

// `kinds`: per vertex input location, 0 float or the integer kind of its attribute format (vertex_int_kind).
// `unwritten`: fragment input locations the vertex stage does not write (default_unwritten_inputs).
// `scaled`: the vertex module with the screen scale's specialization constants (gl_Position.xy).
// Thread-safe: graphics modules are made inside compile(), on the pipeline workers when the compile is asynchronous
// (the SPIR-V patches and vkCreateShaderModule were 1 ms a module on the replay thread: 5-10 ms when a scene showed
// new shaders, every session, the disk cache keeping pipelines but not modules).
std::recursive_mutex g_module_lock;
// `alpha_func`, `alpha_ref`: NVN's alpha test in the fragment module (nvn_spirv_alpha.cpp; func 0 none).
VkShaderModule module_of(const ShaderInfo* s, const uint8_t* kinds = nullptr, uint32_t unwritten = 0, bool scaled = false,
                         uint32_t alpha_func = 0, float alpha_ref = 0, uint32_t inst_binding = 0, uint32_t inst_step = 0) {
    std::lock_guard module_lock(g_module_lock);
    static std::map<std::tuple<const ShaderInfo*, std::string, uint32_t, bool, uint32_t, float, uint64_t>, VkShaderModule> cache;
    // The integer-vertex-input patch (skinned models' bone indices; without it Metal refuses every character's
    // pipeline, DEBUG.md #25/#26). NX_INT_ATTRIBS=0 turns it off.
    static const char* int_env = std::getenv("NX_INT_ATTRIBS");
    static const bool off = int_env && int_env[0] == '0';
    std::string k = kinds && !off ? std::string(reinterpret_cast<const char*>(kinds), 16) : std::string(16, '\0');
    auto& m = cache[{s, k, unwritten, scaled, alpha_func, alpha_func ? alpha_ref : 0.0f, uint64_t(inst_binding) << 32 | inst_step}];
    if (!m) {
        const uint64_t module_t0 = plat::monotonic_ns();
        bool is_patched = false;
        const uint8_t* int_kinds = k != std::string(16, '\0') ? reinterpret_cast<const uint8_t*>(k.data()) : nullptr;
        std::vector<uint32_t> code = patched_spirv(s, int_kinds, unwritten, &is_patched);
        if (scaled && s->stage == ShaderStage::Fragment) {  // gl_FragCoord.xy * spec constants 2, 3
            std::vector<uint32_t> patched;
            if (patch_fragcoord_scale(code, patched)) code.swap(patched);
        } else if (scaled) {  // screen scale: gl_Position.xy * spec constants 0, 1
            std::vector<uint32_t> patched;
            if (patch_x_scale(code, patched)) code.swap(patched);
            else log("nvn/vk: shader %s: screen scale not patched", s->hash.c_str());
        }
        if (alpha_func && s->stage == ShaderStage::Fragment) {
            std::vector<uint32_t> patched;
            if (patch_alpha_test(code, patched, alpha_func, alpha_ref)) code.swap(patched);
            else log("nvn/vk: shader %s: alpha test %u not patched", s->hash.c_str(), alpha_func);
        }
        if (inst_binding && s->stage == ShaderStage::Vertex) {  // draw merging: the block of instance k
            std::vector<uint32_t> patched;
            uint32_t bindings[2], steps[2], nb = 0;
            for (uint32_t j = 0; j < 2 && (inst_binding >> 8 * j & 0xff); j++)
                bindings[nb] = (inst_binding >> 8 * j & 0xff) - 1, steps[nb++] = inst_step >> 16 * j & 0xffff;
            if (patch_instance_blocks(code, bindings, steps, nb, patched)) code.swap(patched);
            else log("nvn/vk: shader %s: instance blocks %x not patched", s->hash.c_str(), inst_binding);
        }
        if (s->stage == ShaderStage::Vertex) clip_z_module(s, code);  // NX_CLIP_Z_PATCH (nvn_spirv_clip.cpp)
        nan_coords_module(code);  // nvn_spirv_nan.cpp
        if (const char* opt = std::getenv("NX_SPIRV_OPT"); opt || std::getenv("NX_SPIRV_TOOL")) {  // diagnostic: each module through `spirv-opt <flags>` (NX_SPIRV_OPT) or `<tool> in out` (NX_SPIRV_TOOL) first
            static int n = 0;
            std::string in = "/tmp/nx_spv_" + std::to_string(process_id()) + "_" + std::to_string(n++) + ".spv", out = in + ".out";
            if (FILE* f = std::fopen(in.c_str(), "wb")) {
                std::fwrite(code.data(), 4, code.size(), f);
                std::fclose(f);
                std::string cmd = std::getenv("NX_SPIRV_TOOL") ? std::string(std::getenv("NX_SPIRV_TOOL")) + " " + in + " " + out + " " + s->hash
                                                                : std::string("spirv-opt ") + opt + " " + in + " -o " + out + " 2>/dev/null";
                if (std::system(cmd.c_str()) == 0)
                    if (FILE* g = std::fopen(out.c_str(), "rb")) {
                        std::vector<uint32_t> o(1 << 20);
                        size_t words = std::fread(o.data(), 4, o.size(), g);
                        std::fclose(g);
                        if (words > 5) o.resize(words), code.swap(o);
                    }
                std::remove(in.c_str()), std::remove(out.c_str());
            }
        }
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size() * 4;
        ci.pCode = code.data();
        if (p_vkCreateShaderModule(vk_device(), &ci, nullptr, &m) != VK_SUCCESS) log("nvn/vk: shader %s rejected", s->hash.c_str());
        note_module_created(plat::monotonic_ns() - module_t0);
        static int logged = 0;
        if (is_patched && logged++ < 20) log("nvn/vk: shader %s: integer vertex inputs", s->hash.c_str());
    }
    return m;
}

std::unordered_map<VkDescriptorSetLayout, std::array<uint32_t, 16>>& layout_counts() {
    static std::unordered_map<VkDescriptorSetLayout, std::array<uint32_t, 16>> m;
    return m;
}

VkDescriptorSetLayout set_layout(const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
    static std::map<std::string, VkDescriptorSetLayout> cache;
    std::string key(reinterpret_cast<const char*>(bindings.data()), bindings.size() * sizeof(bindings[0]));
    auto& l = cache[key];
    if (!l) {
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = uint32_t(bindings.size());
        ci.pBindings = bindings.data();
        p_vkCreateDescriptorSetLayout(vk_device(), &ci, nullptr, &l);
        auto& c = layout_counts()[l];  // for the descriptor pool's peak use (nvn_frames.cpp)
        for (const auto& b : bindings)
            if (b.descriptorType < 16) c[b.descriptorType] += b.descriptorCount;
    }
    return l;
}

// ---- images ---------------------------------------------------------------------------------------------------------

// Views made for sampling and for rendering, by image; forget_image_views() drops an image's before it is freed.
using SampledKey = std::tuple<VkImage, VkImageViewType, uint32_t, uint64_t, uint64_t>;
std::map<SampledKey, VkImageView> g_sampled_views;
std::map<std::tuple<VkImage, uint32_t, uint32_t, VkFormat>, VkImageView> g_attachment_views;
// In front of g_sampled_views (a tree walk per sampled texture of every draw): copies of entries it holds, by a hash
// of the key (view null: an empty slot). forget_image_views, the only place entries go, empties the image's slots.
struct ViewHit {
    SampledKey key{};
    VkImageView view = VK_NULL_HANDLE;
};
// Per thread; emptied when forget_image_views bumps the generation (an image's views are about to be destroyed).
PerThread<TlsArray<ViewHit, 256>> g_view_hits;
PerThread<uint64_t> g_view_hits_gen;
std::atomic<uint64_t> g_views_gen{1};

bool identity(const VkComponentMapping& m) {
    auto id = [](VkComponentSwizzle s, VkComponentSwizzle own) { return s == VK_COMPONENT_SWIZZLE_IDENTITY || s == own; };
    return id(m.r, VK_COMPONENT_SWIZZLE_R) && id(m.g, VK_COMPONENT_SWIZZLE_G) && id(m.b, VK_COMPONENT_SWIZZLE_B) &&
           id(m.a, VK_COMPONENT_SWIZZLE_A);
}


// Views Metal accepts of an image: cubes only of cube images with 6 layers per cube, 3D only of 3D images (a 2D
// view of a 3D image is for rendering a slice), 1D of 1D (only with NX_1D_IMAGES: NVN 1D textures are 2D images of
// height 1, since the translator samples them as 2D, G8). A shader declaring another shape than the texture it gets
// (a samplerCube given a 2D texture) reads the dummy instead: Metal aborts on the view.
bool view_compatible(const GpuImage& img, VkImageViewType type, uint32_t layers) {
    bool cube_img = img.view_type == VK_IMAGE_VIEW_TYPE_CUBE || img.view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    bool img3d = img.view_type == VK_IMAGE_VIEW_TYPE_3D;
    bool img1d = img.view_type == VK_IMAGE_VIEW_TYPE_1D || img.view_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    switch (type) {
        case VK_IMAGE_VIEW_TYPE_CUBE:
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return cube_img && layers >= 6;
        case VK_IMAGE_VIEW_TYPE_3D: return img3d;
        case VK_IMAGE_VIEW_TYPE_1D:
        case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return img1d;
        default: return !img3d && !img1d;
    }
}

// A view for sampling: the image's own when it fits, else one of the view type with the texture's swizzle.
VkImageView view_of(GpuImage& img, VkImageViewType type, const ViewInfo& view) {
    uint32_t swizzle = img.alpha_from_r ? 0 : view.swizzle;  // materialized at upload
    VkComponentMapping m = img.swizzle;
    if (swizzle) {
        constexpr VkComponentSwizzle kSwizzle[] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE,
                                                   VK_COMPONENT_SWIZZLE_R,    VK_COMPONENT_SWIZZLE_G,
                                                   VK_COMPONENT_SWIZZLE_B,    VK_COMPONENT_SWIZZLE_A};
        m = {kSwizzle[swizzle & 0xFF], kSwizzle[(swizzle >> 8) & 0xFF], kSwizzle[(swizzle >> 16) & 0xFF],
             kSwizzle[swizzle >> 24]};
    }
    uint32_t base = std::min(view.base_level, img.levels - 1);
    uint32_t levels = view.levels ? std::min(view.levels, img.levels - base) : img.levels - base;
    uint32_t min_layer = std::min(view.min_layer, img.layers - 1);
    bool whole = base == 0 && levels == img.levels && min_layer == 0 && (!view.layers || view.layers == img.layers);
    VkFormat format = view_format(img, view.format);
    if (type == img.view_type && identity(m) && whole && format == img.format) return img.view;
    auto& cache = g_sampled_views;
    uint64_t range = uint64_t(base) | uint64_t(levels) << 16 | uint64_t(min_layer) << 32 | uint64_t(view.layers) << 48;
    uint32_t mapping = uint32_t(m.r) | uint32_t(m.g) << 8 | uint32_t(m.b) << 16 | uint32_t(m.a) << 24;
    SampledKey key{img.image, type, mapping, range, uint64_t(format)};
    uint64_t h = uintptr_t(img.image) >> 4 ^ range * 31 ^ mapping ^ uint64_t(type) << 5 ^ uint64_t(format);
    if (uint64_t gen = g_views_gen.load(std::memory_order_acquire); (*g_view_hits_gen) != gen) [[unlikely]] {
        for (ViewHit& h : (*g_view_hits)) h = {};
        (*g_view_hits_gen) = gen;
    }
    ViewHit& hit = (*g_view_hits)[(h ^ h >> 8) & 255];
    if (hit.view && hit.key == key) return hit.view;
    std::lock_guard lk(cache_mutex());
    auto& v = cache[key];
    if (!v) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = img.image;
        ci.viewType = type;
        ci.format = format;
        ci.components = m;
        uint32_t layers = std::min(view.layers ? view.layers : img.layers - min_layer, img.layers - min_layer);
        if (type == VK_IMAGE_VIEW_TYPE_2D || type == VK_IMAGE_VIEW_TYPE_1D) layers = 1;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE) layers = 6;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) layers -= layers % 6;
        if (!view_compatible(img, type, layers)) return VK_NULL_HANDLE;
        ci.subresourceRange = {img.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                   ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                   : img.aspect,
                               base, levels, min_layer, layers};
        if (p_vkCreateImageView(vk_device(), &ci, nullptr, &v) != VK_SUCCESS) v = img.view;
    }
    if (v) hit = {key, v};  // never a null one: those are tried again
    return v;
}

// R11G11B10F (0x3f) has no alpha, stored here as RGBA16F: sampling reads alpha 1 and blending takes destination
// alpha as 1, as Maxwell does (G4, docs/nvn/DEBUG.md #50). NX_R11G11B10_RAW=1: the stored alpha (bisecting).
bool ufloat_raw() {
    static const bool raw = std::getenv("NX_R11G11B10_RAW") != nullptr;
    return raw;
}

}  // namespace

bool descriptor_layout_counts(VkDescriptorSetLayout layout, uint32_t (&out)[16]) {
    std::lock_guard lk(cache_mutex());
    auto it = layout_counts().find(layout);
    if (it == layout_counts().end()) return false;
    std::copy(it->second.begin(), it->second.end(), out);
    return true;
}

// ---- layouts ----------------------------------------------------------------------------------------------------------

const ProgramLayout& program_layout(const std::vector<const ShaderInfo*>& stages) {
    static std::map<std::vector<const ShaderInfo*>, ProgramLayout> cache;
    static PerThread<const std::pair<const std::vector<const ShaderInfo*>, ProgramLayout>*> last_pt;  // entries stay
    auto& last = *last_pt;
    if (last && last->first == stages) return last->second;  // runs of draws use one program
    static PerThread<std::map<std::vector<const ShaderInfo*>, const std::pair<const std::vector<const ShaderInfo*>, ProgramLayout>*>*> front_pt;
    auto& front = *front_pt;
    if (!front) front = new std::map<std::vector<const ShaderInfo*>, const std::pair<const std::vector<const ShaderInfo*>, ProgramLayout>*>();
    if (auto f = front->find(stages); f != front->end()) return (last = f->second)->second;
    std::lock_guard lk(cache_mutex());
    auto it = cache.find(stages);
    if (it != cache.end()) return (*front)[stages] = last = &*it, last->second;
    std::map<uint32_t, VkDescriptorSetLayoutBinding> sets[4];
    for (const ShaderInfo* s : stages)
        for (const DescriptorBinding& b : s->refl.bindings) {
            if (b.set >= 4) continue;
            auto& vb = sets[b.set][b.binding];
            vb.binding = b.binding;
            vb.descriptorType = VkDescriptorType(b.type);
            vb.descriptorCount = std::max(1u, b.count);
            vb.stageFlags |= stage_bit(s->stage);
        }
    ProgramLayout pl;
    // On by default on Android and macOS (NX_DYNAMIC_UBO=0/1 overrides). On the Mac every frame once came out black
    // and the GPU hung a few thousand frames in (2026-10-01), with MoltenVK's Metal argument buffers on; with them off
    // (platform_mac.cpp) the island, menus, day and night drew right over ~40,000 frames (2026-10-26), and the sets
    // made per submit fell from ~27,000 to ~1,100 in a heavy island scene (replay -1 ms a frame).
    static const uint32_t max_dynamic = [] {
        const char* e = std::getenv("NX_DYNAMIC_UBO");
#if defined(__ANDROID__) || defined(__APPLE__)
        if (e && e[0] == '0') return 0u;
#else
        if (!e || e[0] != '1') return 0u;
#endif
        VkPhysicalDeviceProperties p{};
        p_vkGetPhysicalDeviceProperties(vk_physical(), &p);
        log("nvn/vk: uniform buffers bound with dynamic offsets (up to %u per layout)",
            p.limits.maxDescriptorSetUniformBuffersDynamic);
        return p.limits.maxDescriptorSetUniformBuffersDynamic;
    }();
    uint64_t dynamic = 0;
    for (auto& [n, b] : sets[0])
        if (b.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b.descriptorCount == 1 && n < 64) dynamic |= 1ull << n;
    if (dynamic && uint32_t(__builtin_popcountll(dynamic)) <= max_dynamic) {
        for (auto& [n, b] : sets[0])
            if (n < 64 && dynamic >> n & 1) b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        pl.dynamic = dynamic;
    }
    for (int k = 0; k < 4; k++) {
        std::vector<VkDescriptorSetLayoutBinding> v;
        for (auto& [n, b] : sets[k]) v.push_back(b);
        pl.sets[k] = set_layout(v);
    }
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 4;
    ci.pSetLayouts = pl.sets;
    p_vkCreatePipelineLayout(vk_device(), &ci, nullptr, &pl.layout);
    return cache[stages] = pl;
}

// ---- graphics pipelines ---------------------------------------------------------------------------------------------

namespace {

using PKey = ByteKey<sizeof(PipelineKey)>;

PipelineKey canonical_key(const PipelineKey& in);

void dyn_init(VkDynamicState (&d)[7]) {
    const VkDynamicState v[7] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                 VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                 VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_DEPTH_BIAS};
    std::memcpy(d, v, sizeof v);
}

// Everything vkCreateGraphicsPipelines reads, owned (the create info points into this object), so that the compile
// can run on another thread than the one that prepared it. Never moved after prepare_graphics_pipeline.
struct PipeBuild {
    VkPipelineShaderStageCreateInfo stages[2]{};
    VkSpecializationInfo spec_info{};
    float uw_values[2] = {1.0f, 1.0f};  // the screen scale's constants 0 and 1 (vertex stage)
    float fc_values[4] = {1.0f, 1.0f, 0.0f, 0.0f};  // gl_FragCoord's scale and offset, constants 2..5 (fragment)
    VkSpecializationInfo fc_spec{};
    uint32_t nstages = 0;
    std::vector<VkVertexInputAttributeDescription> attrs;
    std::vector<VkVertexInputBindingDescription> binds;
    VkPipelineVertexInputStateCreateInfo vi{};
    VkPipelineInputAssemblyStateCreateInfo ia{};
    VkPipelineViewportDepthClipControlCreateInfoEXT clip{};
    VkPipelineViewportStateCreateInfo vp{};
    VkPipelineRasterizationStateCreateInfo rs{};
    VkPipelineMultisampleStateCreateInfo ms{};
    VkPipelineDepthStencilStateCreateInfo ds{};
    VkPipelineColorBlendAttachmentState att[8]{};
    VkPipelineColorBlendStateCreateInfo cb{};
    VkDynamicState dyn[7]{};
    VkPipelineDynamicStateCreateInfo dy{};
    VkFormat color_formats[8]{};
    VkPipelineRenderingCreateInfo ri{};
    VkGraphicsPipelineCreateInfo ci{};
    VkPipelineCache cache = VK_NULL_HANDLE;
    std::string vs_hash, fs_hash;
    // The modules, made in compile() (module_of): per stage the shader and what its module is patched with.
    struct ModuleReq {
        const ShaderInfo* s = nullptr;
        uint8_t kinds[16] = {};
        bool has_kinds = false;
        uint32_t unwritten = 0;
        bool scaled = false;
        uint32_t alpha_func = 0;
        float alpha_ref = 0;
        uint32_t inst_binding = 0, inst_step = 0;
    } modules[2];
};

std::unique_ptr<PipeBuild> prepare_graphics_pipeline(const PipelineKey& key, const ProgramLayout& layout);

// vkCreateGraphicsPipelines on a prepared build. Thread-safe: the device functions are, and the VkPipelineCache is
// internally synchronized (it is not created EXTERNALLY_SYNCHRONIZED).
thread_local bool t_worker = false;

// NX_PIPE_STATS=<fragment shader hash>,...: the driver's statistics (instructions, registers, spills, ...) for the
// pipelines with those fragment shaders, logged, and its internal representations (the GPU's disassembly) written to
// pipe_<vs>_<fs>.txt in the data folder. Needs VK_KHR_pipeline_executable_properties (nvn_vk.cpp enables it).
bool pipe_stats_wanted(const std::string& fs) {
    static const std::string list = std::getenv("NX_PIPE_STATS") ? std::getenv("NX_PIPE_STATS") : "";
    return !list.empty() && p_vkGetPipelineExecutableStatisticsKHR && list.find(fs) != std::string::npos;
}

void pipe_stats_log(const PipeBuild& b, VkPipeline p) {
    VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pi.pipeline = p;
    uint32_t n = 0;
    p_vkGetPipelineExecutablePropertiesKHR(vk_device(), &pi, &n, nullptr);
    std::vector<VkPipelineExecutablePropertiesKHR> execs(n, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    p_vkGetPipelineExecutablePropertiesKHR(vk_device(), &pi, &n, execs.data());
    FILE* f = plat::fopen_utf8(plat::data_root() + "/pipe_" + b.vs_hash + "_" + b.fs_hash + ".txt", "w");
    for (uint32_t e = 0; e < n; e++) {
        VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        ei.pipeline = p, ei.executableIndex = e;
        uint32_t ns = 0;
        p_vkGetPipelineExecutableStatisticsKHR(vk_device(), &ei, &ns, nullptr);
        std::vector<VkPipelineExecutableStatisticKHR> st(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        p_vkGetPipelineExecutableStatisticsKHR(vk_device(), &ei, &ns, st.data());
        std::string line;
        for (auto& x : st) {
            char v[64];
            if (x.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR) std::snprintf(v, sizeof v, "%.1f", x.value.f64);
            else if (x.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR) std::snprintf(v, sizeof v, "%lld", (long long)x.value.i64);
            else if (x.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR) std::snprintf(v, sizeof v, "%llu", (unsigned long long)x.value.u64);
            else std::snprintf(v, sizeof v, "%d", int(x.value.b32));
            line += std::string(x.name) + "=" + v + " ";
        }
        log("pipe stats: %s + %s [%s] %s", b.vs_hash.c_str(), b.fs_hash.c_str(), execs[e].name, line.c_str());
        if (!f) continue;
        uint32_t nr = 0;
        p_vkGetPipelineExecutableInternalRepresentationsKHR(vk_device(), &ei, &nr, nullptr);
        std::vector<VkPipelineExecutableInternalRepresentationKHR> ir(nr, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
        p_vkGetPipelineExecutableInternalRepresentationsKHR(vk_device(), &ei, &nr, ir.data());
        std::vector<std::vector<char>> text(nr);
        for (uint32_t r = 0; r < nr; r++) text[r].resize(ir[r].dataSize), ir[r].pData = text[r].data();
        p_vkGetPipelineExecutableInternalRepresentationsKHR(vk_device(), &ei, &nr, ir.data());
        std::fprintf(f, "==== %s: %s\n%s\n", execs[e].name, line.c_str(), execs[e].description);
        for (uint32_t r = 0; r < nr; r++)
            std::fprintf(f, "---- %s\n%.*s\n", ir[r].name, int(text[r].size()), text[r].data());
    }
    if (f) std::fclose(f);
}

VkPipeline compile(PipeBuild& b) {
    VkPipeline p = VK_NULL_HANDLE;
    for (uint32_t k = 0; k < b.nstages; k++)
        if (!b.stages[k].module && b.modules[k].s) {
            const auto& mr = b.modules[k];
            b.stages[k].module = module_of(mr.s, mr.has_kinds ? mr.kinds : nullptr, mr.unwritten, mr.scaled, mr.alpha_func, mr.alpha_ref,
                                         mr.inst_binding, mr.inst_step);
        }
    const bool stats = pipe_stats_wanted(b.fs_hash);
    if (stats)
        b.ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
    const uint64_t t0 = plat::monotonic_ns();
    VkResult made = p_vkCreateGraphicsPipelines(vk_device(), b.cache, 1, &b.ci, nullptr, &p);
    note_pipeline_created(plat::monotonic_ns() - t0, t_worker);
    if (made != VK_SUCCESS) {
        log("nvn/vk: pipeline for %s + %s failed", b.vs_hash.c_str(), b.fs_hash.c_str());
        p = VK_NULL_HANDLE;
    } else if (g_trace) {
        log("nvn/vk: pipeline: %s + %s", b.vs_hash.c_str(), b.fs_hash.c_str());
    }
    if (stats && p) pipe_stats_log(b, p);
    return p;
}

// A compile that runs on a worker thread (prewarm, NX_ASYNC_PIPELINES) or is claimed by the replay thread that
// needs the result first: whoever moves `state` 0 -> 1 runs it.
struct PipeTask {
    std::unique_ptr<PipeBuild> build;
    std::atomic<int> state{0};  // 0 queued, 1 running, 2 done
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint64_t wanted_ns = 0;  // when a draw first asked for it (replay thread only): 0 = only prewarmed so far
    std::mutex m;
    std::condition_variable cv;
    void run() {
        int expect = 0;
        if (!state.compare_exchange_strong(expect, 1)) return;
        VkPipeline p = compile(*build);
        {
            std::lock_guard lk(m);
            pipeline = p;
            state.store(2);
        }
        cv.notify_all();
        build.reset();
    }
    VkPipeline wait() {
        run();  // not started: do it here
        std::unique_lock lk(m);
        cv.wait(lk, [&] { return state.load() == 2; });
        return pipeline;
    }
};

struct Pool {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::shared_ptr<PipeTask>> q;
    unsigned threads = 0;
};

Pool& pool() {
    // Leaked on purpose: the workers are detached and may still sit in a driver call at exit.
    static Pool* p = [] {
        auto* pl = new Pool;
        const char* e = std::getenv("NX_PIPE_THREADS");
        unsigned hw = std::thread::hardware_concurrency();
        unsigned n = e ? unsigned(std::atoi(e)) : std::min(2u, hw > 1 ? hw - 1 : 1u);
        pl->threads = std::min(n, 8u);
        for (unsigned i = 0; i < pl->threads; i++)
            std::thread([pl] {
                t_worker = true;
                for (;;) {
                    std::shared_ptr<PipeTask> t;
                    {
                        std::unique_lock lk(pl->m);
                        pl->cv.wait(lk, [&] { return !pl->q.empty(); });
                        t = std::move(pl->q.front());
                        pl->q.pop_front();
                    }
                    t->run();
                }
            }).detach();
        return pl;
    }();
    return *p;
}

// front: a pipeline a draw is waiting for goes before the prewarm ones (a task queued twice runs once).
void enqueue(const std::shared_ptr<PipeTask>& t, bool front = false) {
    Pool& pl = pool();
    {
        std::lock_guard lk(pl.m);
        if (front)
            pl.q.push_front(t);
        else
            pl.q.push_back(t);
    }
    pl.cv.notify_one();
}

// NX_PIPE_VERIFY=1: what the pipeline create info says, without pointers, for comparing the raw key's build with the
// canonical key's (canonical_key must change nothing Vulkan reads).
std::string serialize_build(const PipeBuild& b) {
    std::string o;
    auto mark = [&o](int id) { o.push_back(char(0xFF)), o.push_back(char(id)); };
    auto add = [&o](const void* p, size_t n) { o.append(static_cast<const char*>(p), n); };
    mark(0);
    for (uint32_t i = 0; i < b.nstages; i++) {
    mark(1);
        add(&b.stages[i].stage, sizeof b.stages[i].stage), add(&b.stages[i].module, sizeof b.stages[i].module);
        if (b.stages[i].pSpecializationInfo) add(b.stages[i].pSpecializationInfo->pData, sizeof(float));
    }
    mark(2);
    add(b.attrs.data(), b.attrs.size() * sizeof b.attrs[0]);
    mark(3);
    add(b.binds.data(), b.binds.size() * sizeof b.binds[0]);
    mark(4);
    add(&b.ia.topology, sizeof b.ia.topology);
    mark(5);
    add(&b.vp.viewportCount, sizeof b.vp.viewportCount);
    mark(6);
    add(&b.clip.negativeOneToOne, sizeof b.clip.negativeOneToOne);
    mark(7);
    { char has_clip = b.vp.pNext != nullptr; add(&has_clip, 1); }
    mark(8);
    {
        VkBool32 bias = b.rs.depthBiasEnable && b.ds.depthTestEnable;  // a bias with no depth test changes nothing
        add(&b.rs.depthClampEnable, sizeof(VkBool32)), add(&b.rs.rasterizerDiscardEnable, sizeof(VkBool32));
        add(&bias, sizeof bias);
    }
    mark(9);
    add(&b.rs.polygonMode, sizeof b.rs.polygonMode), add(&b.rs.cullMode, sizeof b.rs.cullMode);
    mark(10);
    add(&b.rs.frontFace, sizeof b.rs.frontFace);
    mark(11);
    add(&b.ms.rasterizationSamples, sizeof b.ms.rasterizationSamples);
    mark(12);
    add(&b.ms.alphaToCoverageEnable, sizeof b.ms.alphaToCoverageEnable);
    const auto& d = b.ds;
    mark(13);
    for (const auto* v : {&d.depthTestEnable, &d.depthWriteEnable, &d.stencilTestEnable}) add(v, sizeof *v);
    mark(14);
    if (d.depthTestEnable) add(&d.depthCompareOp, sizeof d.depthCompareOp);  // ignored with the test off
    mark(15);
    if (d.stencilTestEnable) add(&d.front, sizeof d.front), add(&d.back, sizeof d.back);
    mark(16);
    for (uint32_t i = 0; i < b.cb.attachmentCount; i++) {
        const auto& a = b.att[i];
        add(&a.blendEnable, sizeof a.blendEnable), add(&a.colorWriteMask, sizeof a.colorWriteMask);
        if (a.blendEnable) add(&a, sizeof a);  // factors and ops are ignored with the blend off
    }
    mark(17);
    add(&b.cb.attachmentCount, sizeof b.cb.attachmentCount);
    mark(18);
    add(b.dyn, sizeof b.dyn);
    mark(19);
    add(&b.ri.colorAttachmentCount, sizeof b.ri.colorAttachmentCount);
    mark(20);
    add(b.color_formats, b.ri.colorAttachmentCount * sizeof(VkFormat));
    mark(21);
    add(&b.ri.depthAttachmentFormat, sizeof b.ri.depthAttachmentFormat);
    mark(22);
    add(&b.ri.stencilAttachmentFormat, sizeof b.ri.stencilAttachmentFormat);
    mark(23);
    add(&b.ci.renderPass, sizeof b.ci.renderPass), add(&b.ci.layout, sizeof b.ci.layout);
    return o;
}

// Replay-thread state: pipelines by canonical key, and compiles in flight.
std::unordered_map<PKey, VkPipeline, ByteKeyHash> g_canon;
std::unordered_map<PKey, std::shared_ptr<PipeTask>, ByteKeyHash> g_inflight;
size_t g_prewarm_inflight = 0;

void adopt_finished() {
    for (auto it = g_inflight.begin(); it != g_inflight.end();) {
        if (it->second->state.load() == 2) {
            g_canon.emplace(it->first, it->second->pipeline);
            it = g_inflight.erase(it);
        } else {
            ++it;
        }
    }
}

PKey bytes_of(const PipelineKey& k) {
    PKey out;
    std::memcpy(out.b, &k, sizeof k);
    return out;
}

// NX_ASYNC_PIPELINES=1 (video.async_shaders, off by default): a draw whose pipeline is not built yet is skipped (the
// pipeline compiles on a worker thread and the next frames draw it) instead of stalling the replay: arriving on the
// island went from a 755 ms freeze to 98 ms on the M1. Off by default: a draw the game makes once (a scene's
// precomputed lighting) stays missing, and the Nook Inc. counter scene drew only its UI over black.
// Only draws into screen targets are skipped (pipeline_skip_allowed): what the game draws once into a texture of
// its own (the colour-grading LUT of a room, rendered slice by slice when it loads) was lost for good, and the tent's
// picture came out black, or white over stale memory.
PerThread<bool> g_skip_allowed{true};

bool async_skip() {
    static const bool on = [] {
        const char* e = std::getenv("NX_ASYNC_PIPELINES");
#if defined(__ANDROID__)
        // On by default on Android: Turnip takes 50-300 ms per new pipeline on a Snapdragon 865, and a walk over the
        // island from an empty cache froze 27 times (13 with it on; objects show a few frames late instead).
        if (!e) return pool().threads > 0;
#endif
        return e && e[0] == '1' && pool().threads > 0;
    }();
    return on && (*g_skip_allowed);
}

// The pipeline for a canonical key: built, in flight (waited for, or null under async_skip), or compiled now.
VkPipeline pipeline_for(const PipelineKey& canon, const ProgramLayout& layout, bool* pending) {
    PKey ck = bytes_of(canon);
    if (auto it = g_canon.find(ck); it != g_canon.end()) {
        note_pipeline_alias();
        return it->second;
    }
    if (auto it = g_inflight.find(ck); it != g_inflight.end()) {
        std::shared_ptr<PipeTask> t = it->second;
        if (t->state.load() != 2) {
            // Skipped while a worker builds it, but never for long: behind hundreds of prewarmed pipelines a draw's
            // own waited for seconds and the 3D scene stayed black (only the UI drawn). It is moved to the front of
            // the queue, and after 150 ms the replay builds it itself if no worker took it yet. One a worker is
            // building is skipped for up to a second: a single pipeline took ~450-600 ms on the M1 (Metal compiling a
            // large shader), and waiting for it froze the island that long when leaving the house.
            if (async_skip()) {
                const uint64_t now = plat::monotonic_ns();
                if (!t->wanted_ns) {
                    t->wanted_ns = now;
                    if (t->state.load() == 0) enqueue(t, true);
                }
                const uint64_t limit = t->state.load() == 1 ? 1'000'000'000ull : 150'000'000ull;
                if (now - t->wanted_ns < limit) return *pending = true, VK_NULL_HANDLE;
            }
            const uint64_t t0 = plat::monotonic_ns();
            t->wait();
            note_pipeline_waited(plat::monotonic_ns() - t0);
        }
        g_canon.emplace(ck, t->pipeline);
        g_inflight.erase(ck);
        return t->pipeline;
    }
    warm_record(canon);
    auto build = prepare_graphics_pipeline(canon, layout);
    if (async_skip()) {
        auto t = std::make_shared<PipeTask>();
        t->build = std::move(build);
        t->wanted_ns = plat::monotonic_ns();
        g_inflight.emplace(ck, t);
        enqueue(t, true);
        return *pending = true, VK_NULL_HANDLE;
    }
    VkPipeline p = compile(*build);
    g_canon.emplace(ck, p);
    return p;
}

}  // namespace

void pipeline_skip_allowed(bool allowed) { (*g_skip_allowed) = allowed; }

bool fs_reads_fragcoord(const ShaderInfo* fs) {
    static std::map<const ShaderInfo*, bool> memo;
    if (!fs) return false;
    static PerThread<std::unordered_map<const ShaderInfo*, bool>*> front_pt;  // the thread's own answers, no lock
    auto*& front = *front_pt;
    if (!front) front = new std::unordered_map<const ShaderInfo*, bool>();
    if (auto f = front->find(fs); f != front->end()) return f->second;
    std::lock_guard lk(cache_mutex());
    auto it = memo.find(fs);
    if (it != memo.end()) return (*front)[fs] = it->second;
    std::lock_guard module_lock(g_module_lock);
    std::vector<uint32_t> code = patched_spirv(fs, nullptr, 0, nullptr), unused;
    return memo[fs] = patch_fragcoord_scale(code, unused);
}

bool instance_patchable(const ShaderInfo* vs, uint32_t binding) {
    static PerThread<std::map<std::pair<const ShaderInfo*, uint32_t>, bool>*> front_pt;
    auto*& front = *front_pt;
    if (!front) front = new std::map<std::pair<const ShaderInfo*, uint32_t>, bool>();
    if (auto f = front->find({vs, binding}); f != front->end()) return f->second;
    static std::map<std::pair<const ShaderInfo*, uint32_t>, bool> memo;
    std::lock_guard lk(cache_mutex());
    if (auto it = memo.find({vs, binding}); it != memo.end()) return (*front)[{vs, binding}] = it->second;
    std::lock_guard module_lock(g_module_lock);
    std::vector<uint32_t> code = vs ? patched_spirv(vs, nullptr, 0, nullptr) : std::vector<uint32_t>(), unused;
    return (*front)[{vs, binding}] = memo[{vs, binding}] = vs && patch_instance_block(code, binding, 1, unused);
}

VkPipeline graphics_pipeline(const PipelineKey& key, const ProgramLayout& layout) {
    // By the key's bytes, as before, but hashed (nvn_pipeline.h) instead of a ~700-byte std::string built and
    // compared down a tree per draw; and the last entry first: consecutive draws mostly share their state.
    static std::unordered_map<PKey, VkPipeline, ByteKeyHash> cache;
    static PerThread<const std::pair<const PKey, VkPipeline>*> last_pt;  // entries are never erased
    auto& last = *last_pt;
    if (last && std::memcmp(&key, last->first.b, sizeof key) == 0) return last->second;
    PKey k = bytes_of(key);
    // The thread's own copy of the entries it met (the parallel replay's workers: no lock per draw).
    static PerThread<std::unordered_map<PKey, const std::pair<const PKey, VkPipeline>*, ByteKeyHash>*> front_pt;
    auto& front = *front_pt;
    if (!front) front = new std::unordered_map<PKey, const std::pair<const PKey, VkPipeline>*, ByteKeyHash>();
    if (auto it = front->find(k); it != front->end()) return (last = it->second)->second;
    std::lock_guard lk(cache_mutex());
    if (auto it = cache.find(k); it != cache.end()) return (*front)[k] = last = &*it, last->second;

    // A new raw key. The same Vulkan pipeline may already exist under another raw key that differs only in state
    // Vulkan ignores (see canonical_key): look it up by its canonical key before compiling.
    static const bool canon_off = std::getenv("NX_PIPE_CANON") && std::getenv("NX_PIPE_CANON")[0] == '0';  // A/B
    PipelineKey canon = canon_off ? key : canonical_key(key);
    static const bool verify = std::getenv("NX_PIPE_VERIFY") != nullptr;
    if (verify) {
        static uint64_t checked = 0, bad = 0;
        std::string a = serialize_build(*prepare_graphics_pipeline(key, layout));
        std::string b = serialize_build(*prepare_graphics_pipeline(canon, layout));
        checked++;
        if (a != b && ++bad <= 20) {
            size_t i = 0;
            while (i < a.size() && i < b.size() && a[i] == b[i]) i++;
            int sec = -1;
            for (size_t j = i; j-- > 0;) if (uint8_t(a[j]) == 0xFF && j + 1 < a.size()) { sec = uint8_t(a[j + 1]); break; }
            log("nvn/vk: PIPE_VERIFY first difference after marker %d, offset %zu", sec, i);
        }
        if (a != b && bad <= 20)
            log("nvn/vk: PIPE_VERIFY mismatch %llu of %llu: vs %s fs %s", (unsigned long long)bad, (unsigned long long)checked,
                key.vs ? key.vs->hash.c_str() : "-", key.fs ? key.fs->hash.c_str() : "-");
        if (checked % 100 == 0 || a != b) log("nvn/vk: PIPE_VERIFY checked %llu raw keys, %llu differ", (unsigned long long)checked, (unsigned long long)bad);
    }
    bool pending = false;
    VkPipeline p = pipeline_for(canon, layout, &pending);
    if (pending) {
        note_pipeline_skipped();
        return VK_NULL_HANDLE;  // not remembered: asked again by the next draw
    }
    last = &*cache.emplace(k, p).first;
    (*front)[k] = last;
    return last->second;
}

// Once per submit (replay thread): collect finished compiles, and start the pipelines of earlier sessions
// (pipelines_*.keys) whose shaders the game has loaded by now.
void pipeline_warm_step() {
    std::lock_guard lk(cache_mutex());
    adopt_finished();
    g_prewarm_inflight = g_inflight.size();
    static const bool on = [] {
        const char* e = std::getenv("NX_PIPE_PREWARM");
        return !(e && e[0] == '0') && pool().threads > 0;
    }();
    if (!on) return;
    static size_t cursor = 0;
    static bool loaded = false;
    static std::vector<WarmRec> recs;
    static std::vector<bool> done;
    static size_t remaining = 0;
    if (!loaded) {
        loaded = true;
        warm_load(recs);
        done.assign(recs.size(), false);
        remaining = recs.size();
        if (remaining) log("nvn/vk: prewarm: %zu pipelines from earlier sessions", remaining);
    }
    if (!remaining) return;
    const uint64_t t0 = plat::monotonic_ns();
    constexpr uint64_t kBudgetNs = 1000000;  // replay-thread time per submit spent preparing builds
    for (size_t scanned = 0; scanned < std::min<size_t>(recs.size(), 64) && g_prewarm_inflight < 32; scanned++) {  // 64 lookups per submit: the shaders of other courses never load
        if (plat::monotonic_ns() - t0 > kBudgetNs) break;
        size_t i = cursor++ % recs.size();
        if (done[i]) continue;
        WarmRec& r = recs[i];
        const ShaderInfo* vs = shader_by_hash(r.vs);
        const ShaderInfo* fs = r.fs[0] ? shader_by_hash(r.fs) : nullptr;
        if (!vs || (r.fs[0] && !fs)) continue;  // the game has not loaded them yet
        done[i] = true, remaining--;
        PipelineKey key = r.key;
        key.vs = vs, key.fs = fs;
        PKey ck = bytes_of(key);
        if (g_canon.count(ck) || g_inflight.count(ck)) continue;
        std::vector<const ShaderInfo*> stages{vs};
        if (fs) stages.push_back(fs);
        auto t = std::make_shared<PipeTask>();
        t->build = prepare_graphics_pipeline(key, program_layout(stages));
        g_inflight.emplace(ck, t);
        enqueue(t);
        g_prewarm_inflight++;
        note_pipeline_prewarmed();
    }
    // Count the ones that finished (adopt_finished erased them from g_inflight): recompute the in-flight count.
    g_prewarm_inflight = g_inflight.size();
}


namespace {

// The key reduced to what prepare_graphics_pipeline reads: state Vulkan ignores is zeroed, so draws whose raw keys
// differ only there (an attribute no vertex input reads, factors of a disabled blend, stencil ops with the test off,
// everything depth on a pass without a depth attachment...) share one pipeline. Output is unchanged: every field
// zeroed here is one the pipeline creation never reads, or reads only where it has no effect.
PipelineKey canonical_key(const PipelineKey& in) {
    PipelineKey k;
    std::memset(&k, 0, sizeof k);  // compared byte for byte
    k.vs = in.vs, k.fs = in.fs, k.topology = in.topology;
    uint32_t used_streams = 0, max_loc = 0;
    if (in.vs)
        for (const ShaderIo& io : in.vs->refl.inputs) {
            if (io.location >= 16 || io.location >= in.attrib_count) continue;
            const VertexAttrib& va = in.attribs[io.location];
            k.attribs[io.location].format = va.format;  // also the integer-input kind of the vertex module
            max_loc = std::max(max_loc, io.location + 1);
            if (vk_vertex_format(va.format) != VK_FORMAT_UNDEFINED && va.stream < 15) {
                k.attribs[io.location] = va;
                used_streams |= 1u << va.stream;
            }
        }
    k.attrib_count = max_loc;
    for (uint32_t s = 0; s < 15; s++)
        if (used_streams >> s & 1) k.streams[s] = in.streams[s], k.stream_count = s + 1;
    const uint32_t n = std::min<uint32_t>(in.color_count, 8);
    k.color_count = in.color_count;
    for (uint32_t i = 0; i < n; i++) {
        k.color_formats[i] = in.color_formats[i];
        k.mask.mask[i] = in.mask.mask[i] & 0xF;
        if (in.color.blend_enable[i]) {
            k.color.blend_enable[i] = 1;
            k.blend[i] = in.blend[i];
            k.blend[i].target = 0;
        }
    }
    k.ufloat_colors = in.ufloat_colors & (n >= 32 ? ~0u : (1u << n) - 1);
    k.depth_format = in.depth_format;
    if (in.depth_format != VK_FORMAT_UNDEFINED) {
        k.depth.depth_test = in.depth.depth_test ? 1 : 0, k.depth.depth_write = in.depth.depth_write ? 1 : 0;
        if (in.depth.depth_test) k.depth.depth_func = in.depth.depth_func;
        if (in.depth.stencil_test) {  // field by field: the struct has padding bytes the key compares
            k.depth.stencil_test = 1;
            for (int f = 0; f < 2; f++) {
                k.depth.stencil_func[f] = in.depth.stencil_func[f], k.depth.stencil_fail[f] = in.depth.stencil_fail[f];
                k.depth.depth_fail[f] = in.depth.depth_fail[f], k.depth.depth_pass[f] = in.depth.depth_pass[f];
            }
        }
    }
    k.polygon.cull = in.polygon.cull;
    k.polygon.front = in.polygon.front == 0 ? 0 : 1;
    k.polygon.mode = in.polygon.mode <= 1 ? in.polygon.mode : 2;
    uint32_t offset_bit = in.polygon.mode <= 2 ? 1u << in.polygon.mode : 4u;
    k.polygon.offset_enables = in.polygon.offset_enables & offset_bit ? 7 : 0;
    k.depth_clamp = in.depth_clamp && vk_features().depthClamp ? 1 : 0;
    k.alpha_to_coverage = in.alpha_to_coverage ? 1 : 0;
    k.uw_x = in.uw_x;
    k.fc_x = in.fc_x;
    k.fc_off = in.fc_off;
    if (in.fs && in.alpha_func >= 1 && in.alpha_func <= 7) k.alpha_func = in.alpha_func, k.alpha_ref = in.alpha_ref;
    k.inst_binding = in.inst_binding, k.inst_step = in.inst_binding ? in.inst_step : 0;
    return k;
}

std::unique_ptr<PipeBuild> prepare_graphics_pipeline(const PipelineKey& key, const ProgramLayout& layout) {
    auto build = std::make_unique<PipeBuild>();
    PipeBuild& B = *build;
    B.cache = pipeline_cache();  // created here, on the replay thread, before any worker needs it
    B.vs_hash = key.vs ? key.vs->hash : "-", B.fs_hash = key.fs ? key.fs->hash : "-";
    std::memcpy(B.color_formats, key.color_formats, sizeof B.color_formats);
    VkPipelineShaderStageCreateInfo(&stages)[2] = B.stages;
    VkSpecializationInfo& spec_info = B.spec_info;
    uint32_t& nstages = B.nstages;
    uint8_t int_kinds[16] = {};
    for (uint32_t a = 0; a < key.attrib_count && a < 16; a++) int_kinds[a] = uint8_t(vertex_int_kind(key.attribs[a].format));
    // NX_TRACE_PIPELINES=1: every new pipeline's shaders, and each vertex shader's inputs once.
    static std::set<const ShaderInfo*> logged_vs;
    static const bool trace = std::getenv("NX_TRACE_PIPELINES") != nullptr;
    if (trace) log("nvn/vk: new pipeline vs %s fs %s", key.vs ? key.vs->hash.c_str() : "-", key.fs ? key.fs->hash.c_str() : "-");
    if (trace && key.vs && logged_vs.insert(key.vs).second) {
        std::string line;
        for (const ShaderIo& in : key.vs->refl.inputs) {
            char b[64];
            const VertexAttrib* va = in.location < key.attrib_count ? &key.attribs[in.location] : nullptr;
            std::snprintf(b, sizeof b, " %u:%x/s%u+%u", in.location, va ? va->format : 0, va ? va->stream : 0,
                          va ? va->offset : 0);
            line += b;
        }
        log("nvn/vk: vs %s inputs (location:format/stream+offset)%s", key.vs->hash.c_str(), line.c_str());
    }
    uint32_t unwritten = unwritten_inputs(key.vs, key.fs);
    for (const ShaderInfo* s : {key.vs, key.fs}) {
        if (!s) continue;
        auto& st = stages[nstages++];
        st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st.stage = VkShaderStageFlagBits(stage_bit(s->stage));
        const bool scaled = (s == key.vs && key.uw_x != 0) || (s == key.fs && key.fc_x != 0);
        auto& mr = B.modules[nstages - 1];  // made in compile(), off the replay thread when that is asynchronous
        mr.s = s, mr.unwritten = s == key.fs ? unwritten : 0, mr.scaled = scaled;
        if (s == key.fs) mr.alpha_func = key.alpha_func, mr.alpha_ref = key.alpha_ref;
        if (s == key.vs) mr.inst_binding = key.inst_binding, mr.inst_step = key.inst_step;
        if (s == key.vs && int_kinds) std::memcpy(mr.kinds, int_kinds, sizeof mr.kinds), mr.has_kinds = true;
        st.pName = "main";
        if (scaled) {  // screen scale: constants 0 and 1 of the patched vertex module (nvn_ultrawide.cpp)
            static const VkSpecializationMapEntry entries[2] = {{0, 0, sizeof(float)}, {1, sizeof(float), sizeof(float)}};
            B.uw_values[0] = key.uw_x > 0 ? key.uw_x : 1.0f, B.uw_values[1] = key.uw_x < 0 ? -key.uw_x : 1.0f;
            spec_info = {2, entries, 2 * sizeof(float), B.uw_values};
            st.pSpecializationInfo = &spec_info;
        }
        if (scaled && s == key.fs) {  // gl_FragCoord in 16:9 units: constants 2 and 3 (nvn_spirv_fragcoord.cpp)
            static const VkSpecializationMapEntry entries[4] = {
                {2, 0, sizeof(float)}, {3, 4, sizeof(float)}, {4, 8, sizeof(float)}, {5, 12, sizeof(float)}};
            B.fc_values[0] = key.fc_x > 0 ? key.fc_x : 1.0f, B.fc_values[1] = key.fc_x < 0 ? -key.fc_x : 1.0f;
            B.fc_values[2] = key.fc_x > 0 ? key.fc_off : 0.0f, B.fc_values[3] = key.fc_x < 0 ? key.fc_off : 0.0f;
            B.fc_spec = {4, entries, 4 * sizeof(float), B.fc_values};
            st.pSpecializationInfo = &B.fc_spec;
        }
    }

    // Vertex input: each shader input location reads the attribute of the same index. Locations with no attribute
    // (or an unknown format) read a zero vec4 from binding 15.
    std::vector<VkVertexInputAttributeDescription>& attrs = B.attrs;
    std::vector<VkVertexInputBindingDescription>& binds = B.binds;
    bool streams_used[16] = {}, zero_used = false;
    if (key.vs)
        for (const ShaderIo& in : key.vs->refl.inputs) {
            VkVertexInputAttributeDescription a{in.location, 15, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
            if (in.location < key.attrib_count) {
                const VertexAttrib& va = key.attribs[in.location];
                VkFormat f = vk_vertex_format(va.format);
                if (f != VK_FORMAT_UNDEFINED && va.stream < 15) {
                    a.binding = va.stream, a.format = f, a.offset = va.offset;
                    streams_used[va.stream] = true;
                }
            }
            if (a.binding == 15) zero_used = true;
            attrs.push_back(a);
        }
    for (uint32_t s = 0; s < 15; s++)
        if (streams_used[s]) {
            const VertexStream& vs = key.streams[s];
            binds.push_back({s, vs.stride, vs.divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
        }
    if (zero_used) binds.push_back({15, 0, VK_VERTEX_INPUT_RATE_INSTANCE});
    VkPipelineVertexInputStateCreateInfo& vi = B.vi;
    vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = uint32_t(binds.size());
    vi.pVertexBindingDescriptions = binds.data();
    vi.vertexAttributeDescriptionCount = uint32_t(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo& ia = B.ia;
    ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = vk_topology(key.topology);

    VkPipelineViewportDepthClipControlCreateInfoEXT& clip = B.clip;
    clip = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    clip.negativeOneToOne = VK_TRUE;  // NVN depth mode 0 (Wonder): clip-space z in -1..1
    VkPipelineViewportStateCreateInfo& vp = B.vp;
    vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    if (vk_depth_clip_control() && !depth_remap()) vp.pNext = &clip;

    VkPipelineRasterizationStateCreateInfo& rs = B.rs;
    rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = key.polygon.mode == 0 ? VK_POLYGON_MODE_POINT : key.polygon.mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = vk_cull(key.polygon.cull);
    // NVNfrontFace CW 0 / CCW 1, taken as is: the draws use a negative-height viewport (nvn_draw.cpp) that gives NVN's
    // upper-left-origin mapping, so windings match NVN's.
    rs.frontFace = key.polygon.front == 0 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    // SetDepthClamp: clamps instead of clipping at the near and far planes, like depthClampEnable (a device feature;
    // nvn_vk.cpp enables every core feature the GPU has, and MoltenVK has it through Metal's depth clip mode).
    rs.depthClampEnable = key.depth_clamp && vk_features().depthClamp ? VK_TRUE : VK_FALSE;
    // Polygon offset: NVNpolygonOffsetEnable has a bit per polygon mode (POINT 1, LINE 2, FILL 4; modes are POINT 0,
    // LINE 1, FILL 2), and the bit of the mode drawn with decides. Factor, units and clamp are dynamic (SetDepthBias
    // at each draw, nvn_raster.cpp): the game sets them on the command buffer, not in the state object.
    uint32_t offset_bit = key.polygon.mode <= 2 ? 1u << key.polygon.mode : 4u;
    rs.depthBiasEnable = key.polygon.offset_enables & offset_bit || equal_slack_pass(key.depth) ? VK_TRUE : VK_FALSE;

    VkPipelineMultisampleStateCreateInfo& ms = B.ms;
    ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // BindMultisampleState's alpha to coverage (G7; single-sampled targets: alpha under 0.5 drops the fragment),
    // logged once per pipeline that has it. NX_NO_ALPHA_TO_COVERAGE=1: off, as when it was a stub.
    static const bool no_a2c = std::getenv("NX_NO_ALPHA_TO_COVERAGE") != nullptr;
    ms.alphaToCoverageEnable = key.alpha_to_coverage && !no_a2c ? VK_TRUE : VK_FALSE;
    if (key.alpha_to_coverage)
        log("nvn/vk: alpha to coverage%s: fs %s", no_a2c ? " (off)" : "", key.fs ? key.fs->hash.c_str() : "-");

    VkPipelineDepthStencilStateCreateInfo& ds = B.ds;
    ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    bool has_depth = key.depth_format != VK_FORMAT_UNDEFINED;
    ds.depthTestEnable = has_depth && key.depth.depth_test;
    ds.depthWriteEnable = has_depth && key.depth.depth_write;
    ds.depthCompareOp = vk_compare(key.depth.depth_func);
    // EQUAL after a depth pre-pass (the colour pass does not write depth) needs both passes to compute bit-identical
    // depth; two translated shaders compiled separately by Metal do not, and Wonder's whole course colour pass was
    // discarded (only the sky showed). LESS_OR_EQUAL keeps what EQUAL keeps for opaque geometry after a pre-pass.
    if (key.depth.depth_func == 3 && !key.depth.depth_write) {
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        static const bool always = std::getenv("NX_EQUAL_ALWAYS") != nullptr;  // diagnostic: EQUAL passes test nothing
        if (always) ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    }
    ds.stencilTestEnable = has_depth && key.depth.stencil_test && !std::getenv("NX_NO_STENCIL");
    VkStencilOpState* faces[2] = {&ds.front, &ds.back};
    for (int f = 0; f < 2; f++) {
        faces[f]->failOp = vk_stencil_op(key.depth.stencil_fail[f]);
        faces[f]->depthFailOp = vk_stencil_op(key.depth.depth_fail[f]);
        faces[f]->passOp = vk_stencil_op(key.depth.depth_pass[f]);
        faces[f]->compareOp = vk_compare(key.depth.stencil_func[f]);
        faces[f]->compareMask = faces[f]->writeMask = 0xFF;  // dynamic: SetStencilValueMask / SetStencilMask
    }

    VkPipelineColorBlendAttachmentState(&att)[8] = B.att;
    for (uint32_t k = 0; k < key.color_count; k++) {
        const BlendTarget& b = key.blend[k];
        // An R11G11B10F target's destination alpha is 1 on Maxwell; the RGBA16F image holds what the shader wrote.
        bool ufloat = (key.ufloat_colors >> k & 1) && !ufloat_raw();
        auto factor = [ufloat](uint32_t f) {
            VkBlendFactor v = vk_blend_factor(f);
            if (!ufloat) return v;
            return v == VK_BLEND_FACTOR_DST_ALPHA             ? VK_BLEND_FACTOR_ONE
                   : v == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA ? VK_BLEND_FACTOR_ZERO
                   : v == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE  ? VK_BLEND_FACTOR_ZERO  // min(As, 1 - 1)
                                                              : v;
        };
        att[k].blendEnable = key.color.blend_enable[k] ? VK_TRUE : VK_FALSE;
        att[k].srcColorBlendFactor = factor(b.src_rgb);
        att[k].dstColorBlendFactor = factor(b.dst_rgb);
        att[k].colorBlendOp = vk_blend_op(b.eq_rgb);
        att[k].srcAlphaBlendFactor = factor(b.src_a);
        att[k].dstAlphaBlendFactor = factor(b.dst_a);
        att[k].alphaBlendOp = vk_blend_op(b.eq_a);
        att[k].colorWriteMask = key.fs && key.fs->depth_pass ? 0 : key.mask.mask[k] & 0xF;  // its yellow is never seen
    }
    VkPipelineColorBlendStateCreateInfo& cb = B.cb;
    cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = key.color_count;
    cb.pAttachments = att;

    // Stencil reference and masks and depth bias values come from command-buffer calls (nvn_raster.cpp), so they are
    // dynamic: one pipeline for every value the game uses.
    VkDynamicState(&dyn)[7] = B.dyn;
    dyn_init(dyn);
    VkPipelineDynamicStateCreateInfo& dy = B.dy;
    dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = uint32_t(sizeof dyn / sizeof dyn[0]);
    dy.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo& ri = B.ri;
    ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = key.color_count;
    ri.pColorAttachmentFormats = B.color_formats;
    bool stencil = key.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT || key.depth_format == VK_FORMAT_S8_UINT;
    ri.depthAttachmentFormat = key.depth_format == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : key.depth_format;
    ri.stencilAttachmentFormat = stencil ? key.depth_format : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo& ci = B.ci;
    ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &ri;
    if (render_pass_objects())  // nvn_renderpass.cpp: compatible with every pass of these formats
        ci.pNext = nullptr, ci.renderPass = compatible_render_pass(key.color_count, key.color_formats, key.depth_format);
    ci.stageCount = nstages;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = layout.layout;
    return build;
}

}  // namespace

// ---- sampled textures -------------------------------------------------------------------------------------------------

// Pending nvnTextureWriteTexels regions go to the image (a transfer: outside render passes).
void apply_texel_writes(VkCommandBuffer cb, Texture& t, GpuImage& img) {
    // Nothing queued (nearly always): no lock. Every sampled texture of every draw comes here, and taking the
    // registry lock alone also invalidated every thread's get<T> cache each time (docs/nvn/PERF-PLAN.md).
    if (!t.has_writes.load(std::memory_order_acquire)) return;
    std::vector<Texture::TexelWrite> writes;
    {
        std::lock_guard lk(objects_lock());
        writes.swap(t.writes);
        t.has_writes.store(false, std::memory_order_release);
    }
    if (writes.empty()) return;
    img.texels = true;
    VkImageLayout back = img.layout;
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    bool is3d = img.view_type == VK_IMAGE_VIEW_TYPE_3D;
    for (const auto& w : writes) {
        if (w.level >= img.levels) continue;
        Upload u = upload(w.data.data(), w.data.size(), 16);
        VkBufferImageCopy r{};
        r.bufferOffset = u.offset;
        r.imageSubresource = {img.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                  ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                  : img.aspect,
                              w.level, is3d ? 0 : std::min(w.z, img.layers - 1), is3d ? 1 : std::min(w.d, img.layers)};
        r.imageOffset = {int32_t(w.x), int32_t(w.y), is3d ? int32_t(w.z) : 0};
        r.imageExtent = {w.w, w.h, is3d ? w.d : 1};
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
    if (back != VK_IMAGE_LAYOUT_UNDEFINED) transition(cb, img, back);
}

uint64_t g_submit_serial = 0;

void pipeline_begin_submit() {
    g_submit_serial++;
    pipeline_warm_step();
}

PerThread<uint64_t[6]> g_not_ready;  // why sampled_ready said no: no image, dirty, texel writes, rendered, not uploaded, layout

bool sampled_ready(uint64_t texture) {
    Texture* t = get<Texture>(texture);
    if (!t || !t->image) return (*g_not_ready)[0]++, false;  // no image yet (missing textures are the caller's)
    check_linear_content(*t);
    if (t->dirty && !t->image->rendered) return (*g_not_ready)[1]++, false;
    if (t->has_writes.load(std::memory_order_acquire)) return (*g_not_ready)[2]++, false;  // no lock (nvn.h)
    const GpuImage& img = *t->image;
    bool ok = img.rendered ? img.layout == VK_IMAGE_LAYOUT_GENERAL
                           : img.uploaded && img.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!ok) (*g_not_ready)[img.rendered ? 3 : !img.uploaded ? 4 : 5]++;
    return ok;
}

void sampled_ready_log() {
    log("nvn passes: textures not ready: %llu no image, %llu dirty, %llu texel writes, %llu rendered not GENERAL, "
        "%llu not uploaded, %llu uploaded in another layout",
        (unsigned long long)(*g_not_ready)[0], (unsigned long long)(*g_not_ready)[1], (unsigned long long)(*g_not_ready)[2],
        (unsigned long long)(*g_not_ready)[3], (unsigned long long)(*g_not_ready)[4], (unsigned long long)(*g_not_ready)[5]);
}

VkImageView attachment_view(GpuImage& img, uint32_t level, uint32_t layer, VkFormat format) {
    if (format == VK_FORMAT_UNDEFINED) format = img.format;
    level = std::min(level, img.levels - 1);
    // A 3D image's slice is a layer of its 2D-array-compatible views, counted at this level.
    layer = std::min(layer, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? std::max(1u, img.depth >> level) - 1 : img.layers - 1);
    if (level == 0 && layer == 0 && img.levels == 1 && img.layers == 1 && img.view_type == VK_IMAGE_VIEW_TYPE_2D &&
        format == img.format)
        return img.view;
    std::lock_guard lk(cache_mutex());
    auto& cache = g_attachment_views;
    auto& v = cache[{img.image, level, layer, format}];
    if (!v) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = img.image;
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = format;
        ci.subresourceRange = {img.aspect, level, 1, layer, 1};
        if (p_vkCreateImageView(vk_device(), &ci, nullptr, &v) != VK_SUCCESS) v = img.view;
    }
    return v;
}

GpuImage* ensure_contents(VkCommandBuffer cb, uint64_t texture) {
    Texture* t = get<Texture>(texture);
    GpuImage* img = t ? image_of(texture) : nullptr;
    if (!img) return nullptr;
    // A parallel replay worker (docs/PARALLEL_REPLAY.md) only samples what needs nothing done: an upload, texel
    // writes or a first clear are the replay thread's.
    if ((*t_parallel_worker)) {
        bool clean = !t->dirty.load(std::memory_order_acquire) && !t->has_writes.load(std::memory_order_acquire) &&
                     (img->rendered || img->uploaded) && img->layout != VK_IMAGE_LAYOUT_UNDEFINED;
        if (!clean) worker_bail("texture contents");
        return img;
    }
    bool dirty = t->dirty.exchange(false);
    uint64_t dirty_lo = t->dirty_lo.exchange(UINT64_MAX), dirty_hi = t->dirty_hi.exchange(0);
    if (dirty_hi <= dirty_lo) dirty_lo = 0, dirty_hi = UINT64_MAX;  // marked without a range: all of it
    static const bool reupload = std::getenv("NX_TEXELS_REUPLOAD") != nullptr;  // bisecting: the old way
    bool upload = !img->rendered && (!img->uploaded || (dirty && (!img->texels || reupload)));
    // A render target (COMPRESSIBLE, flag 0x8) the GPU never wrote through this image: its guest memory never holds
    // GPU output here (another target over the same memory left only stale data), so it starts cleared, not uploaded
    // (G3 cause 2, docs/nvn/DEBUG.md #50). NX_RT_UPLOAD=1: uploaded as before; NX_ALIAS_MAGENTA=1: cleared magenta.
    static const bool rt_upload = std::getenv("NX_RT_UPLOAD") != nullptr;
    static const bool magenta = std::getenv("NX_ALIAS_MAGENTA") != nullptr;
    if (upload && !rt_upload && (t->flags & 0x8) && !img->uploaded && !img->texels && !dirty) {
        if (!t->has_writes.load(std::memory_order_acquire)) {
            clear_image(cb, *img, magenta);
            img->uploaded = true;
            upload = false;
        }
    }
    // A resized screen target (nvn_ultrawide.cpp): guest memory holds the game's size, not this one. Cleared.
    if (upload && img->uw && (img->sx != 1.0f || img->sy != 1.0f)) {
        static int logged = 0;
        if (logged++ < 8)
            log("nvn: screen target %llx (%ux%u, format %#x) not uploaded from guest memory%s", (unsigned long long)texture,
                t->base_w, t->base_h, t->format, dirty ? " (the CPU wrote it)" : "");
        clear_image(cb, *img, false);
        img->uploaded = true;
        upload = false;
    }
    // A texture already on the GPU that the CPU wrote part of: only those bytes go again (the glyph cache).
    if (upload) {
        const uint64_t u0 = plat::monotonic_ns();
        upload_texture(cb, *t, *img, img->uploaded && dirty ? dirty_lo : 0, img->uploaded && dirty ? dirty_hi : UINT64_MAX);
        note_texture_uploaded(plat::monotonic_ns() - u0, t->storage_size);
    }
    apply_texel_writes(cb, *t, *img);
    if (img->layout == VK_IMAGE_LAYOUT_UNDEFINED) {  // never written: black, not garbage
        transition(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange r{img->aspect, 0, img->levels, 0, img->layers};
        if (img->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
            VkClearColorValue v{};
            p_vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
        }
    }
    return img;
}

VkImageView sampled_view(VkCommandBuffer cb, uint64_t texture, const ImageShape& shape, const ViewInfo* view,
                         VkImageLayout* layout) {
    *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    GpuImage* img = ensure_contents(cb, texture);
    static uint64_t no_image = 0, incompatible = 0;
    if (!img) {
        if (++no_image % 100000 == 1) log("nvn/vk: dummy sampled: %llu with no image, %llu with an incompatible view",
                                          (unsigned long long)no_image, (unsigned long long)incompatible);
        return dummy_view(cb, shape);
    }
    *layout = img->rendered ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (img->layout != *layout && worker_bail("sampled layout")) return img->view;  // a layout change: the replay thread's
    transition(cb, *img, *layout);
    // The swizzle belongs to the texture (its builder, or its pool view), not to the image: an image may be shared
    // with or inherited from another texture over the same memory (#36, #42), whose swizzle differs. With the
    // image's, the UI's single-channel masks lost their alpha and drew as rectangles (docs/nvn/DEBUG.md #44).
    // The slot's own view decides: pool_swizzle is whichever slot registered the texture last (the world map's player
    // mask read as G G G G: a flat silhouette, DEBUG-2.md #67). NX_POOL_SWIZZLE_ANY=1: as before.
    static const bool pool_any = std::getenv("NX_POOL_SWIZZLE_ANY") != nullptr;
    ViewInfo vi = view ? *view : ViewInfo{};
    if (!vi.swizzle && !img->alpha_from_r)
        if (Texture* t = get<Texture>(texture)) {
            uint32_t s = pool_any || !view ? t->pool_swizzle.load() : 0u;
            if (!s)
                for (int k = 0; k < 4; k++) s |= uint32_t(std::min<uint8_t>(t->swizzle[k], 5)) << (8 * k);
            vi.swizzle = s;
        }
    // R11G11B10F: a channel reading A (NVNtextureSwizzle 5) reads ONE (1), after the texture's swizzle (G4).
    if (!ufloat_raw() && !img->alpha_from_r && (img->nvn_format == 0x3f || vi.format == 0x3f)) {
        uint32_t s = vi.swizzle ? vi.swizzle : 0x05040302u;  // 0: identity (R G B A)
        for (int k = 0; k < 4; k++)
            if ((s >> (8 * k) & 0xFF) == 5) s = (s & ~(0xFFu << (8 * k))) | (1u << (8 * k));
        vi.swizzle = s;
    }
    VkImageView v = view_of(*img, view_type(shape), vi);
    if (!v) {
        *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (++incompatible % 1000 == 1)
            log("nvn/vk: texture %llx (%ux%u x%u, view type %d) sampled as view type %d: dummy", (unsigned long long)texture,
                img->width, img->height, img->layers, int(img->view_type), int(view_type(shape)));
        return dummy_view(cb, shape);
    }
    return v;
}

VkPipeline compute_pipeline(const ShaderInfo* cs, const ProgramLayout& layout) {
    static std::map<std::pair<const ShaderInfo*, VkPipelineLayout>, VkPipeline> cache;
    auto& p = cache[{cs, layout.layout}];
    if (!p) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module_of(cs);
        ci.stage.pName = "main";
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sg{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        sg.requiredSubgroupSize = 32;
        if (gpu_require_subgroup32()) ci.stage.pNext = &sg;
        ci.layout = layout.layout;
        if (p_vkCreateComputePipelines(vk_device(), pipeline_cache(), 1, &ci, nullptr, &p) != VK_SUCCESS) {
            log("nvn/vk: compute pipeline for %s failed", cs->hash.c_str());
            p = VK_NULL_HANDLE;
        }
    }
    return p;
}

VkImageView storage_view(VkCommandBuffer cb, uint64_t texture, const ImageShape& shape, const ViewInfo* view) {
    GpuImage* img = texture ? ensure_contents(cb, texture) : nullptr;
    if (img && img->storage) {
        transition(cb, *img, VK_IMAGE_LAYOUT_GENERAL);
        img->rendered = true;  // the shader may write it
        static int logged_3d = 0;
        static const bool trace_3d = std::getenv("NX_TRACE_3D") != nullptr;
        if (trace_3d && img->view_type == VK_IMAGE_VIEW_TYPE_3D && logged_3d++ < 8)
            log("nvn: 3D texture %llx (%ux%ux%u) bound as a storage image (view type %d)", (unsigned long long)texture,
                img->width, img->height, img->depth, int(view_type(shape)));
        ViewInfo v = view ? *view : ViewInfo{};
        v.levels = 1, v.swizzle = 0;  // storage views: one level, identity
        if (VkImageView iv = view_of(*img, view_type(shape), v)) return iv;
    }
    if (img && !img->storage) {
        static int logged = 0;
        if (logged++ < 10) log("nvn/vk: texture %llx bound as a storage image without storage usage (flags %#x): not written",
                               (unsigned long long)texture, get<Texture>(texture) ? get<Texture>(texture)->flags : 0u);
    }
    static std::map<VkImageViewType, GpuImage> dummies;
    VkImageViewType type = view_type(shape);
    auto& d = dummies[type];
    if (!d.image) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R32_UINT;
        ci.extent = {1, 1, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        p_vkCreateImage(vk_device(), &ci, nullptr, &d.image);
        VkMemoryRequirements req{};
        p_vkGetImageMemoryRequirements(vk_device(), d.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        p_vkAllocateMemory(vk_device(), &ai, nullptr, &d.memory);
        p_vkBindImageMemory(vk_device(), d.image, d.memory, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = d.image;
        vi.viewType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                      : (type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
        vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        p_vkCreateImageView(vk_device(), &vi, nullptr, &d.view);
        d.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        d.format = ci.format;
    }
    transition(cb, d, VK_IMAGE_LAYOUT_GENERAL);
    return d.view;
}

void forget_image_views(const GpuImage& img) {
    std::lock_guard lk(cache_mutex());
    // The caches are ordered by image first: its views are one run from the smallest key with that image (this was
    // a scan of every cached view per destroyed texture, a visible share of the replay time).
    auto drop = [&](auto& cache, auto lowest) {
        for (auto it = cache.lower_bound(lowest); it != cache.end() && std::get<0>(it->first) == img.image;) {
            forget_framebuffers(it->second);
            if (it->second && it->second != img.view) p_vkDestroyImageView(vk_device(), it->second, nullptr);
            it = cache.erase(it);
        }
    };
    forget_framebuffers(img.view);  // nvn_renderpass.cpp (attachments may use the whole-image view)
    drop(g_sampled_views, std::make_tuple(img.image, VkImageViewType(0), 0u, uint64_t(0), uint64_t(0)));
    g_views_gen.fetch_add(1, std::memory_order_release);  // every thread's front entries
    drop(g_attachment_views, std::make_tuple(img.image, 0u, 0u, VkFormat(0)));
}

}  // namespace nx::nvn
