// Temporal anti-aliasing and upscaling (video.temporal / NX_TEMPORAL): fsr = AMD FSR 3.1, or FSR 4 where AMD's DLL
// offers it (RDNA 2 and newer with a current driver; it decides), through the FidelityFX SDK 2.4 loader
// (amd_fidelityfx_*_vk.dll beside the exe, private/third_party/ffx/bin, copied by the build); dlaa = NVIDIA DLSS
// (nvn_temporal_dlss.cpp, RTX GPUs). What they need that the Switch game never made:
//   - a jittered camera: each frame the world's sead::PerspectiveProjection gets a sub-pixel offset (Halton 2,3), put
//     in its offset fields by the projection hook (nvn_ultrawide.cpp) before sead makes the matrix;
//   - motion vectors: the camera's (motion.comp: the depth carried from this frame's camera to the last one's; the
//     cameras are sead::LookAtCamera, hooked below), merged with the objects' own (nvn_spirv_motion.cpp: every
//     pipeline keeps its clip positions a frame and its fragments write their motion), and a reactive mask from the
//     translucent effects;
//   - the scene before the tonemap: DLSS / FSR run on the tonemap's input (fs 64ae17e74cc3b846 / 3e1bdb382f32d6fe,
//     set 2 binding 128, nvn_draw.cpp), so the UI, the tonemap and the HDR output stay as they are. At the picture's
//     size the output is blitted back in place; upscaling (video.upscale) makes the scene's targets smaller
//     (nvn_ultrawide.cpp) and the tonemap reads the picture-sized output instead (temporal_sample_redirect).
// AMD FSR 3 frame generation (video.frame_gen = fsr) is below: its swapchain, a HUD-less picture and the UI as a layer
// of its own. The settings screen switches all of it while the game runs. NX_TEMPORAL_TRACE=1: the cameras the hooks
// see, once a second.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "kernel.h"
#include "nvn_shaders.h"
#include "nvn_temporal.h"
#include "nvn_ultrawide.h"
#include "nvn_vk.h"
#include "platform.h"
#include <nx/runtime.h>

#if defined(_WIN32) && (defined(NX_FFX) || defined(NX_DLSS))
#define NX_TEMPORAL_BACKENDS 1
#endif
#if defined(_WIN32) && defined(NX_FFX)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "ffx_api.h"
#include "ffx_upscale.h"
#include "vk/ffx_api_vk.h"
#include "ffx_framegeneration.h"
#include "vk/ffx_api_framegeneration_vk.h"
#endif

namespace nx::nvn {

VkQueue vk_queue();             // nvn_vk.cpp
std::mutex& vk_queue_mutex();  // nvn_vk.cpp: held around vkQueueSubmit

using namespace nx::vk;

namespace {

#include "nvn_temporal_spirv.inc"

std::string wanted_mode() {
    if (const char* e = config_env_override("NX_TEMPORAL")) return e;
    return config_get("video.temporal", "off");
}

// ---- the game's cameras (game threads) ---------------------------------------------------------------------------
// sead::LookAtCamera: pos +0x38, at +0x44, up +0x50; doUpdateMatrix (main+37e4624, 3.0.3 +37e4634) makes the view
// matrix from them: z = normalize(pos - at), x = normalize(up x z), y = z x x, rows (axis, -axis . pos).
struct LookAt {
    float pos[3], at[3], up[3];
    uint64_t lr = 0, updates = 0, present = 0;
};
// A world projection (nvn_ultrawide.cpp's world cameras): what its matrix was made from, the jitter included.
struct Proj {
    float n = 0, f = 0, tan_half = 0, aspect = 0, base_x = 0, base_y = 0;
    float jx = 0, jy = 0;  // the jitter in pixels it was made with (x right, y down)
    uint64_t present = 0, updates = 0;
};
std::mutex g_cam_lock;
std::unordered_map<uint64_t, LookAt> g_lookats;
std::unordered_map<uint64_t, Proj> g_projs;
std::atomic<uint64_t> g_present{0};  // the game thread's presents (temporal_present)
std::atomic<float> g_jx{0}, g_jy{0};  // this frame's jitter in pixels
std::atomic<uint32_t> g_scene_w{1920}, g_scene_h{1080};  // the scene's size (the last tonemap's input)

float halton(uint32_t i, uint32_t base) {
    float f = 1, r = 0;
    for (; i > 0; i /= base) f /= float(base), r += f * float(i % base);
    return r;
}

bool trace_on() {
    static const bool on = std::getenv("NX_TEMPORAL_TRACE") != nullptr;
    return on;
}

}  // namespace

// What one frame was drawn with: carried from its submit (game thread) to the replay of it.
struct TemporalFrame {
    bool ok = false;
    bool no_camera = false;  // the view is unknown (taken as still)
    double view[12] = {};    // row-major 3x4
    float n = 0, f = 0, tan_half = 0, aspect = 0, base_x = 0, base_y = 0;
    float jx = 0, jy = 0;  // the jitter in pixels
    uint64_t present = 0, ns = 0;
};

namespace {

void make_view(const LookAt& c, double (&m)[12]) {
    double z[3] = {double(c.pos[0]) - c.at[0], double(c.pos[1]) - c.at[1], double(c.pos[2]) - c.at[2]};
    auto norm = [](double (&v)[3]) {
        double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 0) v[0] /= l, v[1] /= l, v[2] /= l;
    };
    norm(z);
    double x[3] = {c.up[1] * z[2] - c.up[2] * z[1], c.up[2] * z[0] - c.up[0] * z[2], c.up[0] * z[1] - c.up[1] * z[0]};
    norm(x);
    double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    const double* axes[3] = {x, y, z};
    for (int r = 0; r < 3; r++) {
        m[r * 4 + 0] = axes[r][0], m[r * 4 + 1] = axes[r][1], m[r * 4 + 2] = axes[r][2];
        m[r * 4 + 3] = -(axes[r][0] * c.pos[0] + axes[r][1] * c.pos[1] + axes[r][2] * c.pos[2]);
    }
}

// The camera this frame is drawn with: the world projection made most often among those made in the last two frames,
// and the sead::LookAtCamera updated most often among the recent ones (the field's and the rooms' follow the player
// every frame; the UI's 3D screens and the map's are not world projections).
std::shared_ptr<const TemporalFrame> snapshot() {
    auto fr = std::make_shared<TemporalFrame>();
    const uint64_t now = g_present.load(std::memory_order_relaxed);
    std::lock_guard lk(g_cam_lock);
    const Proj* p = nullptr;
    // The field's projection is made twice a frame; two others share its far plane (100000) with another field of view
    // (25 degrees against its 22): taking one of those made the motion vectors wrong in scale (a jelly-like wobble).
    for (auto& [obj, q] : g_projs)
        if (q.present + 2 >= now && (!p || q.updates > p->updates || (q.updates == p->updates && q.f > p->f))) p = &q;
    // The island has three: two copies of the real camera (from main+292cb0c and +374b944), and one updated from
    // main+7a8830 that is the same camera moved so that its target's z is 0 (the curved world's frame, it seems): it
    // goes along with the player, so reprojecting with it would take walking for standing still. Never that one.
    const LookAt* c = nullptr;
    for (auto& [obj, l] : g_lookats)
        if (l.present + 2 >= now && l.lr != 0x7a8830 && (!c || l.updates > c->updates)) c = &l;
    if (!p) return fr;
    // No sead::LookAtCamera hook in this build's main (recompiled before the hook was listed): a still camera, so no
    // motion at all. Right while the camera holds still; what moves smears.
    if (c) make_view(*c, fr->view);
    else fr->view[0] = fr->view[5] = fr->view[10] = 1.0;
    fr->no_camera = !c;
    fr->n = p->n, fr->f = p->f, fr->tan_half = p->tan_half, fr->aspect = p->aspect;
    fr->base_x = p->base_x, fr->base_y = p->base_y, fr->jx = p->jx, fr->jy = p->jy;
    fr->present = now, fr->ns = plat::monotonic_ns();
    fr->ok = true;
    if (trace_on()) {
        static uint64_t next = 0;
        if (fr->ns >= next) {
            next = fr->ns + 1'000'000'000;
            log("temporal: drawn with projection near %g far %g tan %g x%llu, camera from main+%llx", double(p->n),
                double(p->f), double(p->tan_half), (unsigned long long)p->updates, (unsigned long long)(c ? c->lr : 0));
        }
    }
    return fr;
}

// main+37e4624 (3.0.2) / +37e4634 (3.0.3): sead::LookAtCamera::doUpdateMatrix(this = x0, Matrix34f* dst = x1).
void lookat_update(CPU& c) {
    if (!temporal_on()) return;
    const uint64_t cam = c.x[0];
    if (cam < kHeapBase || cam >= kHeapBase + heap_size()) return;
    const float* f = reinterpret_cast<const float*>(cam + 0x38);
    std::lock_guard lk(g_cam_lock);
    LookAt& l = g_lookats[cam];
    std::memcpy(l.pos, f, 12), std::memcpy(l.at, f + 3, 12), std::memcpy(l.up, f + 6, 12);
    l.lr = c.x[30] - kModuleBase, l.updates++, l.present = g_present.load(std::memory_order_relaxed);
}

}  // namespace

NX_HOOK(0x37e4634, lookat_update);  // 3.0.3 (games/acnh/hooks-3.0.3.txt)
void temporal_lookat_hook(CPU& c) { lookat_update(c); }  // 3.0.2's, shared with native60_diag.cpp

namespace {
enum class Backend { None, Fsr, Dlss };
Backend backend_of(const std::string& m) {
#if defined(_WIN32) && defined(NX_FFX)
    if (m == "fsr") return Backend::Fsr;
#endif
#if defined(_WIN32) && defined(NX_DLSS)
    if (m == "dlaa" || m == "dlss") return Backend::Dlss;
#endif
    (void)m;
    return Backend::None;
}
// The settings screen switches it while the game runs: read again once a present (temporal_update), and a backend
// that cannot start here (no DLSS on this GPU) stays off.
std::atomic<int> g_backend{-1};
std::atomic<bool> g_backend_failed[3] = {};
Backend backend() {
    int b = g_backend.load(std::memory_order_relaxed);
    if (b < 0) g_backend = b = int(backend_of(wanted_mode()));
    return g_backend_failed[b].load(std::memory_order_relaxed) ? Backend::None : Backend(b);
}
}  // namespace

// Built with a backend (Windows with FidelityFX or DLSS): what the device and every pipeline get at boot, whether
// temporal AA is on now or not (the device's extensions and queues, the motion patch and its push constants), so
// the settings screen can turn it on later. NX_TEMPORAL_LIVE=0: only when on at boot (as before).
bool temporal_capable() {
    static const bool c = [] {
#if defined(_WIN32) && (defined(NX_FFX) || defined(NX_DLSS))
        if (std::getenv("NX_TEMPORAL_LIVE") && std::getenv("NX_TEMPORAL_LIVE")[0] == '0') return backend() != Backend::None;
        return true;
#else
        return false;
#endif
    }();
    return c;
}

bool temporal_on() { return backend() != Backend::None; }

// video.upscale / NX_UPSCALE: native (DLAA, FSR native AA), quality (67%), balanced (58%), performance (50%), ultra
// (33%): NVIDIA's and AMD's ratios. Wanted live (nvn_ultrawide.cpp's scale takes it, so a change makes the screen
// targets again as a window resize does); 1 without temporal AA.
float temporal_wanted_ratio() {
    if (!temporal_on()) return 1.0f;
    std::string m = config_get("video.upscale", "native");
    if (const char* e = std::getenv("NX_UPSCALE")) m = e;
    return m == "quality" ? 1.0f / 1.5f : m == "balanced" ? 1.0f / 1.7f : m == "performance" ? 0.5f
           : m == "ultra" ? 1.0f / 3.0f : 1.0f;
}
float temporal_render_ratio() { return uw_render_ratio(); }

// Object motion vectors (nvn_spirv_motion.cpp): the patch and its push constants in every pipeline when temporal AA
// can be on (temporal_capable), unless NX_TAA_OBJECT_MOTION=0; kept per draw only while it is on.
bool temporal_object_motion_on() {
    static const bool on = temporal_capable() && !(std::getenv("NX_TAA_OBJECT_MOTION") && std::getenv("NX_TAA_OBJECT_MOTION")[0] == '0');
    return on;
}
bool temporal_object_motion_active() { return temporal_object_motion_on() && temporal_on(); }

// The texture LOD bias of temporal AA (nvn_pipeline.cpp): made into the modules, so decided once, by whether temporal
// AA was on at boot (turned on later: no bias; turned off: the bias stays until the next start).
bool temporal_mip_bias_on() {
    static const bool on = temporal_on();
    return on;
}

// nvn_vk.cpp, before the instance and the device are made: the extensions the backends need (DLSS's NGX), any that
// may be turned on while the game runs.
void temporal_vk_extensions(std::vector<std::string>& instance, std::vector<std::string>& device) {
#if defined(_WIN32) && defined(NX_DLSS)
    if (temporal_capable() || backend() == Backend::Dlss) dlss_vk_extensions(instance, device);
#else
    if (backend() == Backend::Dlss) dlss_vk_extensions(instance, device);
#endif
}

// nvn_ultrawide.cpp, a world projection about to be made (x0 = the sead::PerspectiveProjection, under its camera
// lock): its offset fields get this frame's jitter on top of the game's own (`base`, a fraction of the frustum: the
// matrix's NDC shift is -2 * offset, NVN's NDC +Y the top row).
void temporal_projection(uint64_t object, float base_x, float base_y, float (&out)[2]) {
    out[0] = base_x, out[1] = base_y;
    if (!temporal_on()) return;
    const float jx = g_jx.load(std::memory_order_relaxed), jy = g_jy.load(std::memory_order_relaxed);
    const float w = float(g_scene_w.load(std::memory_order_relaxed)), h = float(g_scene_h.load(std::memory_order_relaxed));
    out[0] = base_x - jx / w, out[1] = base_y + jy / h;
    const float* f = reinterpret_cast<const float*>(object + 0x98);
    std::lock_guard lk(g_cam_lock);
    Proj& p = g_projs[object];
    p.n = f[0], p.f = f[1], p.tan_half = f[5], p.aspect = f[6];
    p.base_x = base_x, p.base_y = base_y, p.jx = jx, p.jy = jy;
    p.present = g_present.load(std::memory_order_relaxed), p.updates++;
}

// Game thread, each present (uw_cameras): the next frame's jitter. FSR's sequence: Halton (2, 3) over
// 8 * (picture / scene)^2 phases, centred (-0.5 .. 0.5 pixel).
bool temporal_present(uint64_t present) {
    temporal_update();
    if (!temporal_on()) return false;
    g_present.store(present, std::memory_order_relaxed);
    const float r = temporal_render_ratio();
    const uint32_t phases = uint32_t(std::ceil(8.0f / (r * r))), i = uint32_t(present % phases) + 1;
    g_jx = halton(i, 2) - 0.5f, g_jy = halton(i, 3) - 0.5f;
    if (trace_on()) {
        static uint64_t next = 0;
        if (uint64_t now = plat::monotonic_ns(); now >= next) {
            next = now + 1'000'000'000;
            std::lock_guard lk(g_cam_lock);
            for (auto& [obj, l] : g_lookats)
                if (l.present + 2 >= present)
                    log("temporal: lookat %llx (main+%llx) x%llu pos %.1f %.1f %.1f at %.1f %.1f %.1f",
                        (unsigned long long)obj, (unsigned long long)l.lr, (unsigned long long)l.updates,
                        double(l.pos[0]), double(l.pos[1]), double(l.pos[2]), double(l.at[0]), double(l.at[1]),
                        double(l.at[2]));
            for (auto& [obj, p] : g_projs)
                if (p.present + 2 >= present)
                    log("temporal: projection %llx x%llu near %g far %g tan %g aspect %g offset %g %g",
                        (unsigned long long)obj, (unsigned long long)p.updates, double(p.n), double(p.f),
                        double(p.tan_half), double(p.aspect), double(p.base_x), double(p.base_y));
        }
    }
    return true;  // the world projections are made again (marked dirty) with the new jitter
}

std::shared_ptr<const TemporalFrame> temporal_submit() { return temporal_on() ? snapshot() : nullptr; }

#if defined(NX_TEMPORAL_BACKENDS)

namespace {

// ---- the replay side -------------------------------------------------------------------------------------------
#if defined(NX_FFX)
struct Ffx {
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
};
#endif

struct State {
    bool tried = false, ok = false;
#if defined(NX_FFX)
    Ffx fx;
    VkDeviceContext dev{};
    ffxContext ctx = nullptr;
#endif
    bool have_context = false;
    uint32_t w = 0, h = 0;
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    std::string version, dlss_version;
    std::string context_key;  // what the context was made for (backend, DLSS model, FSR version): another remakes it
    // Motion vectors.
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline motion = VK_NULL_HANDLE;
    // RCAS after DLAA (rcas.comp): out -> sharp, which goes back into the scene instead.
    VkDescriptorSetLayout rcas_set = VK_NULL_HANDLE;
    VkPipelineLayout rcas_layout = VK_NULL_HANDLE;
    VkPipeline rcas = VK_NULL_HANDLE;
    GpuImage sharp;
    VkSampler sampler = VK_NULL_HANDLE;
    GpuImage mv, out;  // out: the picture's size (dw x dh), the scene's when not upscaling
    uint32_t dw = 0, dh = 0;
    GpuImage* redirect_from = nullptr;  // this frame's tonemap reads `redirect_to` for it (temporal_sample_redirect)
    GpuImage* redirect_to = nullptr;
    GpuImage reactive;  // R8: how much translucent effects cover each pixel (motion.comp, from temporal_reactive_draw)
    VkImage depth_image = VK_NULL_HANDLE;  // the depth-aspect view below is of it
    VkImageView depth_view = VK_NULL_HANDLE;
    // The last frame drawn: its camera, for the reprojection.
    TemporalFrame last;
    uint64_t dispatched = 0, failed = 0;
    // The scene's depth: the last depth target of the scene's size written in the frame being replayed.
    GpuImage* depth = nullptr;
    uint64_t depth_frame = 0;
} g;

// Our images about to be made again (a new size, a backend switched in the settings): the GPU done with them. The
// game's queue only while AMD frame generation runs: vkDeviceWaitIdle needs every queue left alone meanwhile, and its
// threads present and compute on theirs (the game crashed in its DLL when the picture's size changed with it on).
void gpu_wait_game() {
    if (async_gpu()) frame_wait_all();
    if (!ffx_fg_on()) {
        p_vkDeviceWaitIdle(vk_device());
        return;
    }
    std::lock_guard lk(vk_queue_mutex());
    p_vkQueueWaitIdle(vk_queue());
}

PerThread<std::shared_ptr<const TemporalFrame>> t_frame;  // the submit being replayed (temporal_allow)

void destroy_image(GpuImage& img) {
    VkDevice d = vk_device();
    if (img.view) p_vkDestroyImageView(d, img.view, nullptr);
    if (img.image) p_vkDestroyImage(d, img.image, nullptr);
    if (img.memory) p_vkFreeMemory(d, img.memory, nullptr);
    img = GpuImage{};
}

bool make_image(GpuImage& img, VkFormat format, uint32_t w, uint32_t h) {
    VkDevice d = vk_device();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = format, ci.extent = {w, h, 1};
    ci.mipLevels = 1, ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
    img.format = format, img.width = w, img.height = h, img.rendered = true, img.storage = true;
    return true;
}

#if defined(NX_FFX)
void ffx_message(uint32_t type, const wchar_t* msg) {
    char b[512];
    std::snprintf(b, sizeof b, "%ls", msg ? msg : L"");
    log("nvn/fsr: %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", b);
}

// AMD's loader, once.
bool ffx_load();  // the loader (shared with frame generation, below)

bool ffx_init() {
    if (!ffx_load()) {
        log("nvn/fsr: amd_fidelityfx_loader_vk.dll not found beside the exe, or not the ffx API: temporal AA off");
        return false;
    }
    // The versions the loader offers on this GPU (FSR 4 only where AMD's DLL allows it). video.fsr_version = 3: the
    // newest 3.x; otherwise the loader's default (the newest).
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = &g.dev;
    uint64_t count = 0;
    q.outputCount = &count;
    g.fx.query(nullptr, &q.header);
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.versionIds = ids.data(), q.versionNames = names.data();
    if (count) g.fx.query(nullptr, &q.header);
    std::string list;
    for (uint64_t k = 0; k < count; k++) list += std::string(k ? ", " : "") + (names[k] ? names[k] : "?");
    log("nvn/fsr: upscaler versions on this GPU: %s", count ? list.c_str() : "none");
    return count > 0;
}
#endif

// The backend in use started (each one once, when first wanted: DLSS's NGX takes ~2 s), and the motion vector pass.
bool backend_ready() {
    static int fsr = 0, dlss = 0;  // 1 ready, -1 failed
    const Backend b = backend();
#if defined(NX_FFX)
    if (b == Backend::Fsr && !fsr) fsr = ffx_init() ? 1 : -1;
    if (b == Backend::Fsr && fsr < 0) return g_backend_failed[int(Backend::Fsr)] = true, false;
#endif
    if (b == Backend::Dlss && !dlss) {
        std::string v;
        dlss = dlss_init(v) ? 1 : -1;
        if (dlss > 0) g.dlss_version = v, log("nvn/dlss: %s ready", v.c_str());
    }
    if (b == Backend::Dlss && dlss < 0) return g_backend_failed[int(Backend::Dlss)] = true, false;
    if (b == Backend::Dlss) g.version = g.dlss_version;
    return b != Backend::None;
}

bool init_passes();  // below
bool init() {
    if (!g.tried) g.tried = true, g.ok = init_passes();
    return g.ok && backend_ready();
}

bool init_passes() {
    VkDevice d = vk_device();
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (p_vkCreateSampler(d, &si, nullptr, &g.sampler) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[4] = {};
    b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g.sampler};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 4, dli.pBindings = b;
    if (p_vkCreateDescriptorSetLayout(d, &dli, nullptr, &g.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 96};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1, pli.pSetLayouts = &g.set_layout;
    pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &push;
    if (p_vkCreatePipelineLayout(d, &pli, nullptr, &g.layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof kTaaMotion, mi.pCode = kTaaMotion;
    VkShaderModule sm = VK_NULL_HANDLE;
    if (p_vkCreateShaderModule(d, &mi, nullptr, &sm) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT, ci.stage.module = sm, ci.stage.pName = "main";
    ci.layout = g.layout;
    VkResult r = p_vkCreateComputePipelines(d, pipeline_cache(), 1, &ci, nullptr, &g.motion);
    p_vkDestroyShaderModule(d, sm, nullptr);
    if (r != VK_SUCCESS) return false;
    {  // RCAS after DLSS: two storage images, the sharpness pushed
        VkDescriptorSetLayoutBinding rb[2] = {};
        rb[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        rb[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        dli.bindingCount = 2, dli.pBindings = rb;
        VkPushConstantRange rp{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
        pli.pSetLayouts = &g.rcas_set, pli.pPushConstantRanges = &rp;
        mi.codeSize = sizeof kTaaRcas, mi.pCode = kTaaRcas;
        if (p_vkCreateDescriptorSetLayout(d, &dli, nullptr, &g.rcas_set) == VK_SUCCESS &&
            p_vkCreatePipelineLayout(d, &pli, nullptr, &g.rcas_layout) == VK_SUCCESS &&
            p_vkCreateShaderModule(d, &mi, nullptr, &sm) == VK_SUCCESS) {
            ci.stage.module = sm, ci.layout = g.rcas_layout;
            if (p_vkCreateComputePipelines(d, pipeline_cache(), 1, &ci, nullptr, &g.rcas) != VK_SUCCESS) g.rcas = VK_NULL_HANDLE;
            p_vkDestroyShaderModule(d, sm, nullptr);
        }
        if (!g.rcas) log("nvn/temporal: no RCAS pass: DLAA unsharpened");
    }
    return true;
}

#if defined(NX_FFX)
uint64_t pick_version() {
    std::string want = config_get("video.fsr_version", "auto");
    if (const char* e = std::getenv("NX_FSR_VERSION")) want = e;
    if (want != "3") return 0;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = &g.dev;
    uint64_t count = 0;
    q.outputCount = &count;
    g.fx.query(nullptr, &q.header);
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.versionIds = ids.data(), q.versionNames = names.data();
    if (count) g.fx.query(nullptr, &q.header);
    for (uint64_t k = 0; k < count; k++)
        if (names[k] && names[k][0] == '3') return ids[k];
    return 0;
}

bool ffx_create(uint32_t w, uint32_t h, uint32_t dw, uint32_t dh) {
    ffxCreateContextDescUpscale cd{};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    // The scene is linear light before the tonemap; its exposure is the game's own (FSR measures one).
    cd.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if (std::getenv("NX_FSR_DEBUG")) cd.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING | FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
    cd.maxRenderSize = {w, h}, cd.maxUpscaleSize = {dw, dh};
    cd.fpMessage = ffx_message;
    ffxCreateBackendVKDesc be{};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.deviceContext = g.dev;
    ffxCreateContextDescUpscaleVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.version = FFX_UPSCALER_VERSION;
    ffxOverrideVersion ov{};
    ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    ov.versionId = pick_version();
    cd.header.pNext = &be.header, be.header.pNext = &ver.header;
    if (ov.versionId) ver.header.pNext = &ov.header;
    if (ffxReturnCode_t r = g.fx.create(&g.ctx, &cd.header, nullptr); r != FFX_API_RETURN_OK) {
        log("nvn/fsr: ffxCreateContext failed (%u)", r);
        g.ctx = nullptr;
        return false;
    }
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    g.fx.query(&g.ctx, &pv.header);
    g.version = std::string("FSR ") + (pv.versionName ? pv.versionName : "?");
    return true;
}
#endif

void destroy_context() {
    if (!g.have_context) return;
    if (async_gpu()) frame_wait_all();
    gpu_wait_game();
#if defined(NX_FFX)
    if (g.ctx) g.fx.destroy(&g.ctx, nullptr), g.ctx = nullptr;
#endif
    dlss_destroy();
    destroy_image(g.mv), destroy_image(g.out), destroy_image(g.sharp), destroy_image(g.reactive);
    g.w = g.h = 0, g.have_context = false;
}

// The backend's context and the images for a w x h scene of `format`.
bool make_context(VkCommandBuffer cb, uint32_t w, uint32_t h, uint32_t dw, uint32_t dh, VkFormat format) {
    destroy_context();
    if (!make_image(g.mv, VK_FORMAT_R16G16_SFLOAT, w, h) || !make_image(g.out, VK_FORMAT_R16G16B16A16_SFLOAT, dw, dh) ||
        !make_image(g.reactive, VK_FORMAT_R8_UNORM, w, h)) {
        log("nvn/temporal: images for %ux%u could not be made", w, h);
        return false;
    }
    bool ok = false;
#if defined(NX_FFX)
    if (backend() == Backend::Fsr) ok = ffx_create(w, h, dw, dh);
#endif
    if (backend() == Backend::Dlss) ok = dlss_create(cb, w, h, dw, dh);
    if (ok && g.rcas && !make_image(g.sharp, VK_FORMAT_R16G16B16A16_SFLOAT, dw, dh)) destroy_image(g.sharp);
    if (!ok) {
        destroy_image(g.mv), destroy_image(g.out);
        return false;
    }
    g.have_context = true;
    g.w = w, g.h = h, g.dw = dw, g.dh = dh, g.color_format = format;
    g.context_key = std::to_string(int(backend())) + " " + config_get("video.dlss_model", "4.5") + " " +
                    config_get("video.fsr_version", "auto");
    g.last = TemporalFrame{};
    log("nvn/temporal: %s at %ux%u -> %ux%u (scene format %d)", g.version.c_str(), w, h, dw, dh, int(format));
    return true;
}

#if defined(NX_FFX)
FfxApiResource resource(const GpuImage& img, uint32_t state, uint32_t extra_usage = 0) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = img.format, ci.extent = {img.width, img.height, 1};
    ci.mipLevels = 1, ci.arrayLayers = 1;
    ci.usage = img.storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0;
    return ffxApiGetResourceVK(reinterpret_cast<void*>(img.image), ffxApiGetImageResourceDescriptionVK(img.image, ci, extra_usage),
                               state);
}
#endif

// ---- object motion vectors (nvn_spirv_motion.cpp) -----------------------------------------------------------------
// The skinned models' draws into the scene's targets keep each vertex's clip position: this frame's in one half of the
// history buffer, the last frame's in the other. A draw finds its last-frame range by a key (its vertex shader, its
// counts and offsets, a hash of its indices) and its rank among draws of the same key this frame; a draw with none
// (new, culled last frame, another shape) gets the camera's motion. Element 0 of the history is the slot that writes
// which are off go to. The per-pixel buffer (two words a pixel: the packed motion, the depth) is cleared to all ones
// after each frame's merge. NX_TAA_OBJECT_MOTION=0: off (camera motion only).
struct MotionPush {  // the shaders' push constant block (nvn_spirv_motion.cpp, Push)
    uint64_t history = 0, pixels = 0;
    uint32_t write_off = 0, read_off = 0, min_vertex = 0, range = 0, first_instance = 0, flags = 0, width = 0;
    uint32_t reactive = 0;  // the pixels buffer's entry where the reactive mask's half starts
    float jc[2] = {}, jp[2] = {};
    float bias = 0;  // the fragment shaders' texture LOD bias (patch_mip_bias_push)
    uint32_t pad = 0;
};
static_assert(sizeof(MotionPush) == kMotionPushSize);

// The texture LOD bias a draw gets (NVIDIA's and AMD's guides: log2(render / picture) - 1): with temporal AA on, into
// the 3D scene (a pass of its size); 0 for the rest (the UI, the screen target) and with it off. NX_TAA_MIP_BIAS: the
// native part (default -1; 0: none).
float draw_mip_bias() {
    static const float base = std::getenv("NX_TAA_MIP_BIAS") ? float(std::atof(std::getenv("NX_TAA_MIP_BIAS"))) : -1.0f;
    if (!temporal_on() || base == 0.0f) return 0.0f;
    const VkExtent2D pe = pass_extent();
    if (pe.width != g_scene_w.load(std::memory_order_relaxed) || pe.height != g_scene_h.load(std::memory_order_relaxed))
        return 0.0f;
    return base + std::log2(std::max(temporal_render_ratio(), 0.1f));
}
struct MotionRange {
    uint32_t offset = 0, range = 0, instances = 0;
};
struct ObjectMotion {
    static constexpr uint32_t kPerFrame = 4u << 20;  // vertices a frame (64 MB a half)
    VkBuffer history = VK_NULL_HANDLE, pixels = VK_NULL_HANDLE, placeholder = VK_NULL_HANDLE;
    VkDeviceMemory history_mem = VK_NULL_HANDLE, pixels_mem = VK_NULL_HANDLE, placeholder_mem = VK_NULL_HANDLE;
    uint64_t history_addr = 0, pixels_addr = 0, pixels_bytes = 0;
    uint32_t reactive = 0;  // the pixels buffer's entry where its reactive half starts (temporal_reactive_draw)
    uint64_t frame = ~0ull;
    uint32_t half = 0, used = 0;
    std::unordered_map<uint64_t, MotionRange> cur, prev;
    std::unordered_map<uint64_t, uint32_t> ranks;  // key -> draws of it so far this frame
    uint64_t draws = 0, matched = 0, full = 0;      // this frame's, for the log
    bool failed = false;
    // Pixel buffers replaced by a larger one: freed once the frames that may still use them are done (the draws of the
    // frame that grew were recorded with the old one; freeing it at once hung the GPU on a switch to 4K).
    struct Retired {
        VkBuffer buffer;
        VkDeviceMemory memory;
        uint64_t frame;
    };
    std::vector<Retired> retired;
} om;

bool make_buffer(VkBuffer& b, VkDeviceMemory& m, uint64_t& addr, uint64_t bytes, VkBufferUsageFlags usage) {
    VkDevice d = vk_device();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes, bi.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (p_vkCreateBuffer(d, &bi, nullptr, &b) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    p_vkGetBufferMemoryRequirements(d, b, &req);
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &fl, ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (p_vkAllocateMemory(d, &ai, nullptr, &m) != VK_SUCCESS) return false;
    p_vkBindBufferMemory(d, b, m, 0);
    VkBufferDeviceAddressInfo ad{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ad.buffer = b;
    addr = p_vkGetBufferDeviceAddress(d, &ad);
    return addr != 0;
}

// The buffers, for a scene of w x h (the per-pixel one made again, larger, when the scene grows: the GPU idle first).
bool motion_buffers(uint32_t w, uint32_t h) {
    if (om.failed) return false;
    if (!om.history &&
        !make_buffer(om.history, om.history_mem, om.history_addr, uint64_t(2 * ObjectMotion::kPerFrame + 1) * 16,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        log("nvn/temporal: object motion history could not be made: camera motion only");
        return !(om.failed = true);
    }
    // Two halves: the nearest object fragment's motion and depth, then the reactive mask (temporal_reactive_draw).
    const uint64_t need = uint64_t(std::max(w, 1920u)) * std::max(h, 1080u) * 8 * 2;
    if (om.pixels_bytes < need) {
        if (om.pixels) {
            om.retired.push_back({om.pixels, om.pixels_mem, g_replay_presents});
            om.pixels = VK_NULL_HANDLE, om.pixels_mem = VK_NULL_HANDLE, om.pixels_bytes = 0;
        }
        if (!make_buffer(om.pixels, om.pixels_mem, om.pixels_addr, need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
            log("nvn/temporal: object motion pixels could not be made: camera motion only");
            return !(om.failed = true);
        }
        om.pixels_bytes = need, om.reactive = uint32_t(need / 16);
        log("nvn/temporal: object motion buffers: history %u vertices a frame, pixels %ux%u",
            ObjectMotion::kPerFrame, std::max(w, 1920u), std::max(h, 1080u));
    }
    return true;
}

// After the frame's merge (and on a frame without one): the pixels cleared for the next frame, and the next frame's
// vertex shaders made to see this one's history.
// NX_TAA_MV_VIEW: a 5x5 grid of the per-pixel buffer (the motion pass left the scene's depth in x where an object
// wrote), copied to a host buffer and logged every second (read a few frames late: a debugging aid).
// NX_TAA_PROBE: the motion vectors (in the scene's pixels) at a 5x5 grid and this frame's jitter, logged every second
// (read a few frames late: a debugging aid). Standing still, the camera's are about 0.
void mv_readback(VkCommandBuffer cb, const TemporalFrame& fr) {
    static VkBuffer buf = VK_NULL_HANDLE;
    static VkDeviceMemory mem = VK_NULL_HANDLE;
    static const uint16_t* mapped = nullptr;
    static uint64_t next = 0;
    VkDevice d = vk_device();
    if (!buf) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = 25 * 4, bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (p_vkCreateBuffer(d, &bi, nullptr, &buf) != VK_SUCCESS) return;
        VkMemoryRequirements req{};
        p_vkGetBufferMemoryRequirements(d, buf, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (p_vkAllocateMemory(d, &ai, nullptr, &mem) != VK_SUCCESS) return;
        p_vkBindBufferMemory(d, buf, mem, 0);
        void* m = nullptr;
        p_vkMapMemory(d, mem, 0, VK_WHOLE_SIZE, 0, &m);
        mapped = static_cast<const uint16_t*>(m);
    }
    if (!mapped || !g.mv.image) return;
    if (const uint64_t now = plat::monotonic_ns(); now >= next) {
        next = now + 1'000'000'000;
        auto half = [](uint16_t h) {
            const uint32_t e = h >> 10 & 31, m = h & 1023, sgn = h >> 15;
            const float v = e == 0 ? std::ldexp(float(m), -24) : std::ldexp(float(m | 1024), int(e) - 25);
            return sgn ? -v : v;
        };
        std::string line;
        for (int k = 0; k < 25; k++) {
            char t[40];
            std::snprintf(t, sizeof t, " %+.2f,%+.2f", double(half(mapped[k * 2]) * float(g.w)), double(half(mapped[k * 2 + 1]) * float(g.h)));
            line += t;
        }
        log("temporal: probe jitter %+.3f %+.3f (scene %ux%u), motion px:%s", double(fr.jx), double(fr.jy), g.w, g.h, line.c_str());
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    transition(cb, g.mv, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy cp[25];
    for (int k = 0; k < 25; k++) {
        cp[k] = {};
        cp[k].bufferOffset = uint64_t(k) * 4;
        cp[k].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp[k].imageOffset = {int32_t(g.w * uint32_t(1 + k % 5) / 6), int32_t(g.h * uint32_t(1 + k / 5) / 6), 0};
        cp[k].imageExtent = {1, 1, 1};
    }
    p_vkCmdCopyImageToBuffer(cb, g.mv.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 25, cp);
    transition(cb, g.mv, VK_IMAGE_LAYOUT_GENERAL);
}

bool push_view() {
    static const bool view = std::getenv("NX_TAA_MV_VIEW") != nullptr;
    return view;
}

void mv_probe(VkCommandBuffer cb) {
    static VkBuffer buf = VK_NULL_HANDLE;
    static VkDeviceMemory mem = VK_NULL_HANDLE;
    static const uint32_t* mapped = nullptr;
    static uint64_t next = 0;
    VkDevice d = vk_device();
    if (!buf) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = 25 * 8, bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (p_vkCreateBuffer(d, &bi, nullptr, &buf) != VK_SUCCESS) return;
        VkMemoryRequirements req{};
        p_vkGetBufferMemoryRequirements(d, buf, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (p_vkAllocateMemory(d, &ai, nullptr, &mem) != VK_SUCCESS) return;
        p_vkBindBufferMemory(d, buf, mem, 0);
        void* p = nullptr;
        p_vkMapMemory(d, mem, 0, VK_WHOLE_SIZE, 0, &p);
        mapped = static_cast<const uint32_t*>(p);
    }
    const uint32_t w = g_scene_w.load(), h = g_scene_h.load();
    if (!mapped || !w || !h) return;
    if (const uint64_t now = plat::monotonic_ns(); now >= next) {
        next = now + 1'000'000'000;
        std::string line;
        for (int k = 0; k < 25; k++) {
            const uint32_t sd = mapped[k * 2], od = mapped[k * 2 + 1];
            float a, b;
            std::memcpy(&a, &sd, 4), std::memcpy(&b, &od, 4);
            char t[64];
            if (od == 0xffffffffu) std::snprintf(t, sizeof t, " [-]");
            else std::snprintf(t, sizeof t, " [%.7f %.7f]", double(a), double(b));
            line += t;
        }
        log("temporal: probe (scene depth, object depth):%s", line.c_str());
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    VkBufferCopy cp[25];
    for (int k = 0; k < 25; k++) {
        const uint32_t x = w * uint32_t(1 + k % 5) / 6, y = h * uint32_t(1 + k / 5) / 6;
        cp[k] = {(uint64_t(y) * w + x) * 8, uint64_t(k) * 8, 8};
    }
    p_vkCmdCopyBuffer(cb, om.pixels, buf, 25, cp);
}

void motion_frame_end(VkCommandBuffer cb) {
    for (size_t k = 0; k < om.retired.size();) {  // past every frame in flight (two at most, NX_PIPELINED)
        if (om.retired[k].frame + 4 <= g_replay_presents) {
            p_vkDestroyBuffer(vk_device(), om.retired[k].buffer, nullptr), p_vkFreeMemory(vk_device(), om.retired[k].memory, nullptr);
            om.retired.erase(om.retired.begin() + long(k));
        } else {
            k++;
        }
    }
    if (!om.pixels) return;
    if (push_view()) mv_probe(cb);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    p_vkCmdFillBuffer(cb, om.pixels, 0, VK_WHOLE_SIZE, 0xffffffffu);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
}

uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ull; }

// 4x4 helpers, row-major doubles.
using M4 = std::array<double, 16>;
M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return r;
}
bool invert(const M4& m, M4& out) {
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12) return false;
        if (p != c)
            for (int j = 0; j < 8; j++) std::swap(a[p][j], a[c][j]);
        const double inv = 1.0 / a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] *= inv;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                const double f = a[r][c];
                for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j];
            }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = a[i][j + 4];
    return true;
}
// A frame's view-projection without its jitter (sead's matrix, disassembled from doUpdateMatrix: GL clip space).
M4 view_proj(const TemporalFrame& f) {
    M4 p{};
    p[0] = 1.0 / (double(f.tan_half) * f.aspect), p[2] = 2.0 * f.base_x;
    p[5] = 1.0 / double(f.tan_half), p[6] = 2.0 * f.base_y;
    p[10] = -(double(f.f) + f.n) / (double(f.f) - f.n), p[11] = -2.0 * f.f * f.n / (double(f.f) - f.n);
    p[14] = -1.0;
    M4 v{};
    for (int k = 0; k < 12; k++) v[k] = f.view[k];
    v[15] = 1.0;
    return mul(p, v);
}

// The backend's dispatch into in.out.
bool dispatch(const TemporalInputs& in, const TemporalFrame& fr) {
    if (backend() == Backend::Dlss) return dlss_evaluate(in);
#if defined(NX_FFX)
    ffxDispatchDescUpscale dd{};
    dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dd.commandList = in.cb;
    dd.color = resource(*in.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dd.depth = resource(*in.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dd.motionVectors = resource(*in.mv, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dd.output = resource(*in.out, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    if (in.reactive) dd.reactive = resource(*in.reactive, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    // NX_TAA_JITTER_SIGN=-1 (bisecting a wobble at rest): the jitter FSR is told, mirrored.
    static const float js = std::getenv("NX_TAA_JITTER_SIGN") ? float(std::atof(std::getenv("NX_TAA_JITTER_SIGN"))) : 1.0f;
    dd.jitterOffset = {js * in.jx, js * in.jy};
    dd.motionVectorScale = {float(g.w), float(g.h)};
    dd.renderSize = {g.w, g.h}, dd.upscaleSize = {g.dw, g.dh};
    // FSR's RCAS (video.temporal_sharpness_pct, read live; 0 off): its accumulation alone comes out soft.
    const float sharp = std::clamp(std::atoi(config_get("video.temporal_sharpness_pct", "60").c_str()), 0, 100) / 100.0f;
    dd.enableSharpening = sharp > 0.0f, dd.sharpness = sharp;
    dd.frameTimeDelta = in.frame_ms;
    dd.preExposure = 1.0f;
    dd.reset = in.reset;
    dd.cameraNear = fr.n, dd.cameraFar = fr.f;
    dd.cameraFovAngleVertical = 2.0f * std::atan(fr.tan_half);
    dd.viewSpaceToMetersFactor = 0.1f;  // the island's grid: 10 units a tile, about a metre
    dd.flags = std::getenv("NX_FSR_DEBUG") ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0;
    if (ffxReturnCode_t r = g.fx.dispatch(&g.ctx, &dd.header); r != FFX_API_RETURN_OK) {
        if (g.failed < 5) log("nvn/fsr: ffxDispatch failed (%u)", r);
        return false;
    }
    return true;
#else
    (void)fr;
    return false;
#endif
}

}  // namespace

#if defined(NX_FFX)
void ffx_fg_prepare(const TemporalInputs& in, const TemporalFrame& fr, uint32_t w, uint32_t h);  // below
#endif

// Each submit is drawn with the camera of the submit before it: the snapshot taken at a submit (temporal_submit) holds
// the projection the game has already made for the next frame, jitter and all. Measured standing still (NX_TAA_PROBE):
// the static objects' motion read about a pixel each frame, following the jitter (DLSS told the next frame's jitter:
// a heat-haze vibration, 1.5 screen pixels when upscaling); with this, 0.00. NX_TAA_LAG=0: as before.
void temporal_allow(std::shared_ptr<const TemporalFrame> frame) {
    static const bool lag = !(std::getenv("NX_TAA_LAG") && std::getenv("NX_TAA_LAG")[0] == '0');
    static std::shared_ptr<const TemporalFrame> held;
    if (lag) std::swap(held, frame);
    *t_frame = std::move(frame);
}

// nvn_draw.cpp: a draw with depth writes into a target of the scene's size.
void temporal_note_depth(GpuImage* depth) {
    if (!depth || depth->width != g_scene_w.load(std::memory_order_relaxed) ||
        depth->height != g_scene_h.load(std::memory_order_relaxed))
        return;
    g.depth = depth, g.depth_frame = g_replay_presents;
}

// nvn_draw.cpp, right before a skinned model's draw is issued (its pipeline and layout bound): its history slots.
// The vertex range of an index list: scanned once per list (its guest address, count and type), then taken from here
// while its first and last indices are unchanged (scanning every skinned draw's indices each frame cost the replay
// ~3 ms a frame on the island).
struct IndexRange {
    uint32_t lo = 0, hi = 0;
    uint64_t probe = 0;
};
std::unordered_map<uint64_t, IndexRange> g_index_ranges;

void temporal_motion_none(VkCommandBuffer cb, VkPipelineLayout layout) {
    MotionPush mp{};
    mp.bias = draw_mip_bias();
    p_vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof mp, &mp);
}

// nvn_draw.cpp: a translucent draw into the scene (blended, depth-tested, not written: particles, fire, sparkles,
// splashes): its fragments mark how much they cover each pixel (nvn_spirv_motion.cpp), which temporal AA takes as
// its reactive mask (FSR) / current-colour bias (DLSS): the history is trusted less there, so a moving effect does
// not smear. `added`: an additive blend, whose cover is its brightness (its alpha means nothing).
void temporal_reactive_draw(VkCommandBuffer cb, VkPipelineLayout layout, bool added) {
    MotionPush mp;
    mp.bias = draw_mip_bias();
    const VkExtent2D pe = pass_extent();
    if (om.pixels && om.reactive && pe.width == g_scene_w.load(std::memory_order_relaxed) &&
        pe.height == g_scene_h.load(std::memory_order_relaxed))
        mp.pixels = om.pixels_addr, mp.flags = added ? 4u : 2u, mp.width = pe.width, mp.reactive = om.reactive;
    p_vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof mp, &mp);
}

// Draws that are not skinned keep their history when they are this small (an object: a bug, a fish, a tool, hair; not
// the terrain or a building, which only the camera moves). Copies of a mesh are told apart by their uniform blocks
// where those stay put (`ident`, nvn_draw.cpp), by draw order elsewhere; a copy given another's history (the game
// culls and re-sorts them: a flower flickered, the twins doubled) has its motion dropped by motion.comp.
// NX_TAA_OBJECT_VERTICES=<n>: another limit (0: skinned draws only).
const uint32_t kObjectVertices = std::getenv("NX_TAA_OBJECT_VERTICES") ? uint32_t(std::atoi(std::getenv("NX_TAA_OBJECT_VERTICES"))) : 8192;
struct Excluded {
    uint64_t draws = 0, verts = 0;
    uint32_t largest = 0, smallest = ~0u;
} g_excluded;
struct {
    uint64_t draws = 0, matched = 0;
} g_rigid;

void temporal_motion_draw(VkCommandBuffer cb, VkPipelineLayout layout, const ShaderInfo* vs, bool skinned, bool indexed,
                          const int32_t* counts, uint64_t index_address, const uint8_t* indices, VkIndexType index_type,
                          uint64_t ident) {
    MotionPush mp;
    mp.bias = draw_mip_bias();
    // Every pipeline reads these (nvn_spirv_motion.cpp): zeros, never last draw's, when there is nothing to keep.
    if ((!om.history && !motion_buffers(g_scene_w.load(), g_scene_h.load())) || !om.history || !om.pixels)
        return temporal_motion_none(cb, layout);
    mp.history = om.history_addr, mp.pixels = om.pixels_addr;
    if (om.frame != g_replay_presents) {  // a new frame: this one's history goes to the other half
        static uint64_t next_log = 0;
        if (uint64_t now = plat::monotonic_ns(); om.frame != ~0ull && trace_on() && om.draws && now >= next_log &&
                                                 (next_log = now + 1'000'000'000))
            log("temporal: object motion: %llu draws, %llu with last frame's, %llu past the budget", (unsigned long long)om.draws,
                (unsigned long long)om.matched, (unsigned long long)om.full);
        static uint64_t next_ex = 0;
        if (uint64_t now = plat::monotonic_ns(); std::getenv("NX_TAA_MOTION_LOG") && now >= next_ex && (next_ex = now + 2'000'000'000))
            log("temporal: object motion: %llu draws kept, %llu matched (rigid %llu, %llu matched); %llu past the size limit "
                "(%u..%u vertices, %llu in all)",
                (unsigned long long)om.draws, (unsigned long long)om.matched, (unsigned long long)g_rigid.draws,
                (unsigned long long)g_rigid.matched, (unsigned long long)g_excluded.draws,
                g_excluded.draws ? g_excluded.smallest : 0, g_excluded.largest, (unsigned long long)g_excluded.verts);
        g_excluded = {}, g_rigid = {};
        om.prev.swap(om.cur), om.cur.clear(), om.ranks.clear();
        om.half ^= 1, om.used = 0, om.draws = om.matched = om.full = 0;
        om.frame = g_replay_presents;
    }
    const VkExtent2D pe = pass_extent();
    const bool scene = pe.width == g_scene_w.load(std::memory_order_relaxed) && pe.height == g_scene_h.load(std::memory_order_relaxed);
    if (scene) {
        const uint32_t instances = uint32_t(std::max(1, counts[3])), first_instance = uint32_t(counts[2]);
        uint32_t lo = 0, hi = 0;
        uint64_t key = mix(mix(0xcbf29ce484222325ull, reinterpret_cast<uintptr_t>(vs)), instances);
        if (indexed) {
            const uint32_t n = uint32_t(counts[0]);
            if (!indices || !n) goto push;
            const bool u16 = index_type == VK_INDEX_TYPE_UINT16, u8 = index_type == VK_INDEX_TYPE_UINT8_EXT;
            const uint32_t restart = u16 ? 0xffffu : u8 ? 0xffu : 0xffffffffu;
            auto at = [&](uint32_t k) {
                return u16 ? uint32_t(reinterpret_cast<const uint16_t*>(indices)[k])
                       : u8 ? uint32_t(indices[k]) : reinterpret_cast<const uint32_t*>(indices)[k];
            };
            uint64_t probe = 0xcbf29ce484222325ull;
            for (uint32_t k = 0; k < 4 && k < n; k++) probe = mix(mix(probe, at(k)), at(n - 1 - k));
            const uint64_t list = mix(mix(mix(0x84222325cbf29ce4ull, index_address), n), uint32_t(index_type));
            IndexRange& r = g_index_ranges[list];
            if (r.probe != probe || r.lo > r.hi) {
                r.lo = ~0u, r.hi = 0, r.probe = probe;
                for (uint32_t k = 0; k < n; k++)
                    if (const uint32_t v = at(k); v != restart) r.lo = std::min(r.lo, v), r.hi = std::max(r.hi, v);
            }
            if (g_index_ranges.size() > 65536) g_index_ranges.clear();  // a bound (a draw's own entry is rescanned)
            lo = r.lo, hi = r.hi;
            if (lo > hi) goto push;
            key = mix(mix(key, index_address), probe);
            lo += uint32_t(counts[1]), hi += uint32_t(counts[1]);  // the vertex offset
            key = mix(mix(key, n), uint32_t(counts[1]));
        } else {
            lo = uint32_t(counts[0]), hi = lo + uint32_t(std::max(1, counts[1])) - 1;
            key = mix(mix(key, lo), hi);
        }
        const uint32_t range = hi - lo + 1, need = range * instances;
        if (!skinned && need > kObjectVertices) {  // the camera's motion is right for it
            g_excluded.draws++, g_excluded.verts += need;
            g_excluded.largest = std::max(g_excluded.largest, need), g_excluded.smallest = std::min(g_excluded.smallest, need);
            goto push;
        }
        // A rigid mesh's copies by where their uniform blocks are (the order the game draws them in changes).
        // Where they do not (blocks made again every frame: ident 0), the order; motion.comp drops a copy's motion
        // that is not its own.
        if (ident) key = mix(key, ident);
        if (!skinned) g_rigid.draws++;
        key = mix(key, om.ranks[key]++);  // the k-th draw of this shape this frame
        om.draws++;
        if (need > ObjectMotion::kPerFrame - om.used) {
            om.full++;
            goto push;
        }
        mp.write_off = 1 + om.half * ObjectMotion::kPerFrame + om.used;
        om.used += need;
        om.cur[key] = {mp.write_off, range, instances};
        if (auto it = om.prev.find(key); it != om.prev.end() && it->second.range == range && it->second.instances == instances)
            mp.read_off = it->second.offset, om.matched++, g_rigid.matched += skinned ? 0 : 1;
        mp.min_vertex = lo, mp.range = range, mp.first_instance = first_instance;
        mp.flags = mp.read_off ? 1u : 0u;  // its fragments write their motion
        mp.width = pe.width;
        // The jitter of this frame and the last, in NDC (as the camera's motion vectors remove it).
        if (const auto& fr = *t_frame; fr && fr->ok) {
            mp.jc[0] = 2.0f * fr->jx / float(pe.width), mp.jc[1] = -2.0f * fr->jy / float(pe.height);
            if (g.last.ok) mp.jp[0] = 2.0f * g.last.jx / float(pe.width), mp.jp[1] = -2.0f * g.last.jy / float(pe.height);
            else mp.jp[0] = mp.jc[0], mp.jp[1] = mp.jc[1];
        }
    }
push:
    p_vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof mp, &mp);
}

// nvn_vk.cpp, once the device is made: FSR's or DLSS's start (DLSS's ~2 s) while the game boots, not on the title
// screen's first frame.
void temporal_device_ready() {
    if (temporal_on()) init();
}

// Each present (temporal_present, game thread): the settings screen's choices taken. A backend change is taken by the
// next frame's context (temporal_before_tonemap remakes it); one that failed to start may be chosen again later.
void temporal_update() {
    static uint64_t gen = ~0ull;
    if (const uint64_t c = config_generation(); c != gen) {
        gen = c;
        const Backend b = backend_of(wanted_mode());
        if (int(b) != g_backend.load(std::memory_order_relaxed)) {
            log("nvn/temporal: anti-aliasing now %s", b == Backend::Dlss ? "DLSS" : b == Backend::Fsr ? "FSR" : "off");
            g_backend_failed[int(b)] = false;
            g_backend = int(b);
        }
    }
}

struct {
    uint64_t hits = 0, frames = 0, next = 0;
} g_redirects;  // NX_TAA_REDIRECT_LOG: how often the tonemap read the upscaled picture
GpuImage* temporal_sample_redirect(GpuImage* img) {
    if (img && img == g.redirect_from && g.redirect_to) {
        g_redirects.hits++;
        return g.redirect_to;
    }
    return img;
}
void temporal_tonemap_done() {
    static const bool trace = std::getenv("NX_TAA_REDIRECT_LOG") != nullptr;
    if (trace) {
        g_redirects.frames += g.redirect_to ? 1 : 0;
        if (const uint64_t now = plat::monotonic_ns(); now >= g_redirects.next) {
            g_redirects.next = now + 2'000'000'000;
            log("temporal: tonemap redirect: %llu reads over %llu upscaled frames", (unsigned long long)g_redirects.hits,
                (unsigned long long)g_redirects.frames);
        }
    }
    g.redirect_from = g.redirect_to = nullptr;
}

bool temporal_is_tonemap(const std::string& fs_hash) {
    return temporal_on() && (fs_hash == "64ae17e74cc3b846" || fs_hash == "3e1bdb382f32d6fe");
}

// nvn_draw.cpp, right before the tonemap draw (no pass open, its textures gathered): `scene` is what it will sample.
void temporal_before_tonemap(VkCommandBuffer cb, GpuImage& scene, const GpuImage* screen) {
    g.redirect_from = g.redirect_to = nullptr;
    // Upscaling: the output at the tonemap's target's size (the scene's targets were made smaller, nvn_ultrawide.cpp).
    uint32_t dw = scene.width, dh = scene.height;
    if (temporal_render_ratio() < 1.0f && screen && screen->width > scene.width && screen->height > scene.height)
        dw = screen->width, dh = screen->height;
    g_scene_w = scene.width, g_scene_h = scene.height;  // the jitter's pixels from the next frame on
    if (!init()) return;
    if (temporal_object_motion_on()) motion_buffers(scene.width, scene.height);
    const std::shared_ptr<const TemporalFrame> fr = *t_frame;
    GpuImage* depth = g.depth_frame == g_replay_presents ? g.depth : nullptr;
    if (!fr || !fr->ok || !depth || depth->width != scene.width || depth->height != scene.height) {
        static uint64_t n = 0;
        if (n++ % 600 == 0)
            log("nvn/temporal: no %s this frame: the scene is left as drawn", !fr || !fr->ok ? "camera" : "depth");
        g.last = TemporalFrame{};
        motion_frame_end(cb);
        return;
    }
    const std::string key = std::to_string(int(backend())) + " " + config_get("video.dlss_model", "4.5") + " " +
                            config_get("video.fsr_version", "auto");
    if ((g.w != scene.width || g.h != scene.height || g.dw != dw || g.dh != dh || g.color_format != scene.format ||
         g.context_key != key) &&
        !make_context(cb, scene.width, scene.height, dw, dh, scene.format))
        return;
    VkDevice d = vk_device();

    // The depth-aspect view the motion vectors read.
    if (g.depth_image != depth->image) {
        if (g.depth_view) p_vkDestroyImageView(d, g.depth_view, nullptr);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = depth->image, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = depth->format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        g.depth_view = VK_NULL_HANDLE;
        if (p_vkCreateImageView(d, &vi, nullptr, &g.depth_view) != VK_SUCCESS) return;
        g.depth_image = depth->image;
    }

    // A camera jump (a scene change, a cut): no history.
    bool reset = !g.last.ok;
    if (!reset) {
        const double dx = fr->view[3] - g.last.view[3], dy = fr->view[7] - g.last.view[7], dz = fr->view[11] - g.last.view[11];
        reset = dx * dx + dy * dy + dz * dz > 400.0 * 400.0 || fr->tan_half != g.last.tan_half;
    }
    const TemporalFrame& prev = reset ? *fr : g.last;
    M4 cur_vp = view_proj(*fr), cur_inv{}, reproj{};
    if (!invert(cur_vp, cur_inv)) return;
    reproj = mul(view_proj(prev), cur_inv);

    const VkImageLayout scene_was = scene.layout, depth_was = depth->layout;
    transition(cb, *depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cb, scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cb, g.mv, VK_IMAGE_LAYOUT_GENERAL);
    transition(cb, g.out, VK_IMAGE_LAYOUT_GENERAL);
    transition(cb, g.reactive, VK_IMAGE_LAYOUT_GENERAL);

    // 1. Motion vectors.
    {
        VkDescriptorSet set = descriptor_set(g.set_layout);
        VkDescriptorImageInfo di{g.sampler, g.depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo mo{VK_NULL_HANDLE, g.mv.view, VK_IMAGE_LAYOUT_GENERAL};
        const bool objects = om.pixels != VK_NULL_HANDLE;
        if (!objects && !om.placeholder) {  // binding 2 must hold a buffer: a small one when there is no object motion
            uint64_t addr = 0;
            make_buffer(om.placeholder, om.placeholder_mem, addr, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        }
        VkDescriptorBufferInfo pb{objects ? om.pixels : om.placeholder, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo ro{VK_NULL_HANDLE, g.reactive.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[4] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                     {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstSet = set, w[0].dstBinding = 0, w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w[0].pImageInfo = &di;
        w[1].dstSet = set, w[1].dstBinding = 1, w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w[1].pImageInfo = &mo;
        w[2].dstSet = set, w[2].dstBinding = 2, w[2].descriptorCount = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[2].pBufferInfo = &pb;
        w[3].dstSet = set, w[3].dstBinding = 3, w[3].descriptorCount = 1;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w[3].pImageInfo = &ro;
        p_vkUpdateDescriptorSets(d, 4, w, 0, nullptr);
        if (objects) {  // the skinned models' fragments, written this frame
            VkMemoryBarrier fb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            fb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, fb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &fb,
                                   0, nullptr, 0, nullptr);
        }
        struct {
            float reproj[16];  // GLSL's mat4 is column-major
            float jitter_size[4];
            uint32_t objects;
            float limit;
            uint32_t reactive;  // the buffer's entry where its reactive half starts (0: none this frame)
            float reactive_scale;
        } push{};
        static const bool view = std::getenv("NX_TAA_MV_VIEW") != nullptr;
        push.objects = (objects && !reset ? 1 : 0) | (view ? 2 : 0);
        // NX_TAA_OBJECT_LIMIT: pixels at 1440 lines (default 24): a run's or a bug's own motion in a frame stays under.
        static const float limit = std::getenv("NX_TAA_OBJECT_LIMIT") ? float(std::atof(std::getenv("NX_TAA_OBJECT_LIMIT"))) : 24.0f;
        push.limit = limit * float(scene.height) / 1440.0f;
        // NX_TAA_REACTIVE: how far the translucent effects' cover is trusted over the history (default 0.9, AMD's
        // ceiling; 0: no reactive mask).
        static const float reactive_scale = std::getenv("NX_TAA_REACTIVE") ? float(std::atof(std::getenv("NX_TAA_REACTIVE"))) : 0.9f;
        push.reactive = objects ? om.reactive : 0, push.reactive_scale = reactive_scale;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) push.reproj[c * 4 + r] = float(reproj[r * 4 + c]);
        const float w_px = float(scene.width), h_px = float(scene.height);
        push.jitter_size[0] = 2.0f * fr->jx / w_px, push.jitter_size[1] = -2.0f * fr->jy / h_px;
        push.jitter_size[2] = w_px, push.jitter_size[3] = h_px;
        p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.motion);
        p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 1, &set, 0, nullptr);
        p_vkCmdPushConstants(cb, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
        p_vkCmdDispatch(cb, (scene.width + 7) / 8, (scene.height + 7) / 8, 1);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb,
                               0, nullptr, 0, nullptr);
    }

    // NX_TAA_MV_VIEW: the motion pass's picture of where each pixel's motion came from, in place of the scene.
    if (push_view()) {
        transition(cb, g.mv, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(cb, scene, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit vb{};
        vb.srcSubresource = vb.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        vb.srcOffsets[1] = vb.dstOffsets[1] = {int32_t(scene.width), int32_t(scene.height), 1};
        p_vkCmdBlitImage(cb, g.mv.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &vb, VK_FILTER_NEAREST);
        transition(cb, scene, scene_was == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : scene_was);
        transition(cb, *depth, depth_was == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : depth_was);
        g.last = *fr;
        motion_frame_end(cb);
        return;
    }

    if (std::getenv("NX_TAA_PROBE")) {
        mv_readback(cb, *fr);
        static uint64_t next = 0, burst = 0;
        if (const uint64_t now = plat::monotonic_ns(); now >= next) next = now + 1'000'000'000, burst = 4;
        if (burst && burst--)
            log("temporal: probe camera base %+.6f %+.6f tan %.6f aspect %.6f pos %.4f %.4f %.4f jitter %+.3f %+.3f (last base %+.6f %+.6f)",
                double(fr->base_x), double(fr->base_y), double(fr->tan_half), double(fr->aspect), fr->view[3], fr->view[7],
                fr->view[11], double(fr->jx), double(fr->jy), double(prev.base_x), double(prev.base_y));
    }

    // 2. FSR or DLSS.
    TemporalInputs in;
    in.cb = cb, in.color = &scene, in.depth = depth, in.depth_view = g.depth_view, in.mv = &g.mv, in.out = &g.out;
    in.reactive = &g.reactive;
    in.jx = fr->jx, in.jy = fr->jy, in.reset = reset;
    in.frame_ms = g.last.ok ? float(double(fr->ns - g.last.ns) / 1e6) : 33.3f;
#if defined(NX_FFX)
    ffx_fg_prepare(in, *fr, scene.width, scene.height);  // AMD frame generation's depth and motion (below)
#endif
    if (!dispatch(in, *fr)) {
        if (g.failed++ < 5) log("nvn/temporal: %s failed this frame", g.version.c_str());
        transition(cb, scene, scene_was), transition(cb, *depth, depth_was);
        motion_frame_end(cb);
        return;
    }
    g.dispatched++;

    // 3. Back into the scene, which the tonemap samples next.
    // DLAA's: RCAS (video.temporal_sharpness_pct, read live; 0 off). FSR sharpens inside.
    GpuImage* result = &g.out;
    if (const float sharp = std::clamp(std::atoi(config_get("video.temporal_sharpness_pct", "60").c_str()), 0, 100) / 100.0f;
        backend() == Backend::Dlss && g.rcas && g.sharp.image && sharp > 0.0f) {
        transition(cb, g.sharp, VK_IMAGE_LAYOUT_GENERAL);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                               nullptr, 0, nullptr);
        VkDescriptorSet set = descriptor_set(g.rcas_set);
        VkDescriptorImageInfo src{VK_NULL_HANDLE, g.out.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo dst{VK_NULL_HANDLE, g.sharp.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstSet = set, w[0].dstBinding = 0, w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w[0].pImageInfo = &src;
        w[1].dstSet = set, w[1].dstBinding = 1, w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w[1].pImageInfo = &dst;
        p_vkUpdateDescriptorSets(d, 2, w, 0, nullptr);
        const float amount = std::exp2(-2.0f * (1.0f - sharp));  // 100%: RCAS at full strength, 0%: two stops softer
        p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.rcas);
        p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.rcas_layout, 0, 1, &set, 0, nullptr);
        p_vkCmdPushConstants(cb, g.rcas_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &amount);
        p_vkCmdDispatch(cb, (g.dw + 7) / 8, (g.dh + 7) / 8, 1);
        result = &g.sharp;
    }
    if (g.dw != scene.width || g.dh != scene.height) {
        // Upscaled: the tonemap reads the output itself, as the scene's texture (same host format: R11G11B10F is
        // RGBA16F here), at the picture's scale (its render scale entry, nvn_sets.cpp).
        if (result->format == scene.format) {
            VkMemoryBarrier rb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            rb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, rb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &rb,
                                   0, nullptr, 0, nullptr);
            result->nvn_format = scene.nvn_format, result->uw = true, result->rendered = true, result->mutable_format = false;
            result->sx = screen ? screen->sx : scene.sx / temporal_render_ratio();
            result->sy = screen ? screen->sy : scene.sy / temporal_render_ratio();
            g.redirect_from = &scene, g.redirect_to = result;
        } else {
            static int n = 0;
            if (n++ < 3) log("nvn/temporal: the scene's format (%d) is not the output's: not upscaled", int(scene.format));
        }
        transition(cb, scene, scene_was == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : scene_was);
        transition(cb, *depth, depth_was == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : depth_was);
        g.last = *fr;
        motion_frame_end(cb);
        return;
    }
    transition(cb, *result, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(cb, scene, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit bl{};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = {int32_t(scene.width), int32_t(scene.height), 1};
    bl.dstOffsets[1] = {int32_t(scene.width), int32_t(scene.height), 1};
    p_vkCmdBlitImage(cb, result->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene.image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    transition(cb, scene, scene_was == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : scene_was);
    transition(cb, *depth, depth_was == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : depth_was);
    g.last = *fr;
    motion_frame_end(cb);
    if (g.dispatched == 1 || (g.dispatched & (g.dispatched - 1)) == 0)
        log("nvn/temporal: %llu frames (%s)%s", (unsigned long long)g.dispatched, g.version.c_str(),
            fr->no_camera ? ", no camera hook in this build: motion taken as none" : "");
}

#if defined(NX_FFX)
// ---- frame generation: AMD FSR 3 (video.frame_gen = fsr) ---------------------------------------------------------
// AMD's frame generation swapchain stands in for ours: it is made, acquired from and presented through the functions
// its context returns (nvn_vk.cpp's sc_* wrappers), and between two of the game's frames it presents one it made
// from them, paced by threads of its own on queues of its own (a present queue, an image acquire queue, an async
// compute queue: requested with the device, as AMD's sample picks them). Each frame temporal AA's pass hands it the
// scene's depth and motion vectors (ffx_fg_prepare). Needs temporal AA (the motion vectors) and amd_fidelityfx_
// framegeneration_vk.dll beside the exe. The UI is interpolated with the picture for now (no HUD-less copy yet).
namespace {
struct FgQueue {
    uint32_t family = UINT32_MAX, index = 0;
    VkQueue queue = VK_NULL_HANDLE;
};
struct FrameGen {
    FgQueue present, acquire, async;
    VkQueue game = VK_NULL_HANDLE;
    uint32_t game_family = 0;
    bool queues = false;               // the device has them
    ffxContext swapchain_ctx = nullptr, fg_ctx = nullptr;
    ffxQueryDescSwapchainReplacementFunctionsVK fns{};
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent{};
    uint64_t frame_id = 0;
    bool prepared = false;             // this frame's depth and motion went to it
    bool failed = false;
    // The HUD-less picture: the capture (the screen target's format and size) and the swapchain-shaped images the
    // present pass draws it into, one per frame in turn (AMD reads one while the next is drawn).
    GpuImage capture;
    uint64_t captured = 0;             // frame_id of the capture
    VkImage hudless[kHudlessImages] = {};
    VkFormat hudless_format = VK_FORMAT_UNDEFINED;
    VkExtent2D hudless_extent{};
    // The UI layer (ui_split.comp) and the views it reads the frame and the HUD-less picture through.
    GpuImage ui;
    VkDescriptorSetLayout split_set = VK_NULL_HANDLE;
    VkPipelineLayout split_layout = VK_NULL_HANDLE;
    VkPipeline split = VK_NULL_HANDLE;
    VkSampler split_sampler = VK_NULL_HANDLE;
    std::unordered_map<VkImage, VkImageView> views;  // the swapchain's and the HUD-less images', made on first use
    bool split_this_frame = false;
    bool split_failed = false;
} fg;

// NX_FG_TIMING (always for now): where frame generation's CPU time goes, every 120 frames.
void fg_time(int what, uint64_t ns) {
    static uint64_t sum[3] = {}, worst[3] = {}, n = 0;
    sum[what] += ns, worst[what] = std::max(worst[what], ns);
    if (what == 2 && ++n % 120 == 0) {
        log("nvn/fg-fsr: CPU ms a frame: configure %.2f (worst %.2f), prepare %.2f (%.2f), present %.2f (%.2f)",
            sum[0] / 120e6, worst[0] / 1e6, sum[1] / 120e6, worst[1] / 1e6, sum[2] / 120e6, worst[2] / 1e6);
        for (int k = 0; k < 3; k++) sum[k] = worst[k] = 0;
    }
}

bool ffx_load() {
    static int state = 0;  // 1 loaded, -1 not there
    if (state) return state > 0;
    state = -1;
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe, n);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    HMODULE m = LoadLibraryW((dir + L"amd_fidelityfx_loader_vk.dll").c_str());
    if (!m) return false;
    g.fx.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(m, "ffxCreateContext"));
    g.fx.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(m, "ffxDestroyContext"));
    g.fx.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(m, "ffxQuery"));
    g.fx.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(m, "ffxDispatch"));
    g.fx.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(m, "ffxConfigure"));
    if (!g.fx.create || !g.fx.destroy || !g.fx.query || !g.fx.dispatch || !g.fx.configure) return false;
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(plat::vulkan_get_instance_proc_addr());
    g.dev.vkDevice = vk_device(), g.dev.vkPhysicalDevice = vk_physical();
    g.dev.vkDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(vk_instance(), "vkGetDeviceProcAddr"));
    state = 1;
    return true;
}

ffxReturnCode_t fg_dispatch(ffxDispatchDescFrameGeneration* params, void* ctx) {
    // AMD's swapchain takes the HDR luminance range from vkSetHdrMetadataEXT, which we never call: left at 0 nits, its
    // conversion of an HDR picture to linear (for the optical flow) breaks. The display's peak (video.hdr_peak_nits).
    const float given = params->minMaxLuminance[1];
    if (params->backbufferTransferFunction != FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB && params->minMaxLuminance[1] <= 0.0f) {
        params->minMaxLuminance[0] = 0.0f;
        params->minMaxLuminance[1] = std::clamp(config_get_float("video.hdr_peak_nits", 1000), 400.0f, 10000.0f);
    }
    static int logged = 0;
    if (logged++ < 3)
        log("nvn/fg-fsr: generating: transfer %u, luminance given %.4g, used %.4g..%.4g nits",
            params->backbufferTransferFunction, given, params->minMaxLuminance[0], params->minMaxLuminance[1]);
    return g.fx.dispatch(reinterpret_cast<ffxContext*>(ctx), &params->header);
}

void fg_destroy_contexts() {
    if (fg.fg_ctx) g.fx.destroy(&fg.fg_ctx, nullptr), fg.fg_ctx = nullptr;
}

// The frame generation context for a swapchain of `ci` (made again with it).
bool fg_make_context(const VkSwapchainCreateInfoKHR& ci) {
    fg_destroy_contexts();
    ffxCreateContextDescFrameGeneration cd{};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    // An HDR swapchain carries linear scRGB or PQ: either is HDR to it (the transfer function comes from the swapchain).
    cd.flags = ci.imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0;
    cd.displaySize = {ci.imageExtent.width, ci.imageExtent.height};
    // Temporal AA's scene can be larger than the window (supersampling): up to 2x 4K.
    cd.maxRenderSize = {std::max(ci.imageExtent.width, 7680u), std::max(ci.imageExtent.height, 4320u)};
    cd.backBufferFormat = ffxApiGetSurfaceFormatVK(ci.imageFormat);
    ffxCreateBackendVKDesc be{};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.deviceContext = g.dev;
    cd.header.pNext = &be.header;
    if (ffxReturnCode_t r = g.fx.create(&fg.fg_ctx, &cd.header, nullptr); r != FFX_API_RETURN_OK) {
        log("nvn/fg-fsr: frame generation context failed (%u)", r);
        fg.fg_ctx = nullptr;
        return false;
    }
    fg.extent = ci.imageExtent;
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    g.fx.query(&fg.fg_ctx, &pv.header);
    log("nvn/fg-fsr: frame generation %s at %ux%u", pv.versionName ? pv.versionName : "?", ci.imageExtent.width,
        ci.imageExtent.height);
    return true;
}
}  // namespace

// Read live (the settings screen): nvn_vk.cpp makes the swapchain again, through AMD's or not, when this changes.
bool ffx_fg_wanted() {
    std::string v = config_get("video.frame_gen", "off");
    if (const char* e = config_env_override("NX_FFX_FRAMEGEN")) v = e[0] == '1' ? "fsr" : "off";
    return v == "fsr" && temporal_on();  // its depth and motion vectors are temporal AA's
}

// Whether the swapchain should be AMD's now: wanted, and possible here.
bool ffx_fg_should() { return ffx_fg_wanted() && fg.queues && !fg.failed && ffx_load(); }

// nvn_vk.cpp, choosing the device's queues: the three frame generation's own, as AMD's sample picks them (`counts`:
// queues taken per family so far, the game's included).
void ffx_fg_queue_requests(VkPhysicalDevice pd, VkSurfaceKHR surface, const std::vector<VkQueueFamilyProperties>& families,
                           uint32_t game_family, std::vector<uint32_t>& counts) {
    if (!temporal_capable() && !ffx_fg_wanted()) return;  // may be turned on later: its queues at boot
    fg.game_family = game_family;
    auto has = [&](uint32_t k, VkQueueFlags f) { return (families[k].queueFlags & f) == f; };
    auto take = [&](uint32_t k, FgQueue& q) {
        q.family = k, q.index = counts[k]++;
        return true;
    };
    auto free_in = [&](uint32_t k) { return counts[k] < families[k].queueCount; };
    auto presents = [&](uint32_t k) {
        VkBool32 p = VK_FALSE;
        return p_vkGetPhysicalDeviceSurfaceSupportKHR(pd, k, surface, &p) == VK_SUCCESS && p;
    };
    const uint32_t n = uint32_t(families.size());
    for (uint32_t k = 0; k < n && fg.present.family == UINT32_MAX; k++)  // present: graphics or compute, presenting
        if (free_in(k) && (has(k, VK_QUEUE_GRAPHICS_BIT) || has(k, VK_QUEUE_COMPUTE_BIT)) && presents(k)) take(k, fg.present);
    for (uint32_t k = 0; k < n && fg.present.family == UINT32_MAX; k++)
        if (free_in(k) && has(k, VK_QUEUE_TRANSFER_BIT) && presents(k)) take(k, fg.present);
    for (uint32_t k = 0; k < n && fg.acquire.family == UINT32_MAX; k++)  // image acquire: anything, least capable first
        if (free_in(k) && !has(k, VK_QUEUE_GRAPHICS_BIT) && !has(k, VK_QUEUE_COMPUTE_BIT)) take(k, fg.acquire);
    for (uint32_t k = 0; k < n && fg.acquire.family == UINT32_MAX; k++)
        if (free_in(k) && !has(k, VK_QUEUE_GRAPHICS_BIT)) take(k, fg.acquire);
    for (uint32_t k = 0; k < n && fg.acquire.family == UINT32_MAX; k++)
        if (free_in(k)) take(k, fg.acquire);
    for (uint32_t k = 0; k < n && fg.async.family == UINT32_MAX; k++)  // async compute: compute-only first
        if (free_in(k) && has(k, VK_QUEUE_COMPUTE_BIT) && !has(k, VK_QUEUE_GRAPHICS_BIT)) take(k, fg.async);
    for (uint32_t k = 0; k < n && fg.async.family == UINT32_MAX; k++)
        if (free_in(k) && has(k, VK_QUEUE_COMPUTE_BIT)) take(k, fg.async);
    log("nvn/fg-fsr: queues: present %u/%u, image acquire %u/%u, async compute %u/%u", fg.present.family,
        fg.present.index, fg.acquire.family, fg.acquire.index, fg.async.family, fg.async.index);
}

// nvn_vk.cpp, once the device is made.
void ffx_fg_device_ready(VkDevice d, VkQueue game) {
    if (!temporal_capable() && !ffx_fg_wanted()) return;
    fg.game = game;
    if (fg.present.family == UINT32_MAX || fg.acquire.family == UINT32_MAX) {
        log("nvn/fg-fsr: no present / image acquire queue for frame generation: off");
        return;
    }
    p_vkGetDeviceQueue(d, fg.present.family, fg.present.index, &fg.present.queue);
    p_vkGetDeviceQueue(d, fg.acquire.family, fg.acquire.index, &fg.acquire.queue);
    if (fg.async.family != UINT32_MAX) p_vkGetDeviceQueue(d, fg.async.family, fg.async.index, &fg.async.queue);
    fg.queues = true;
}

bool ffx_fg_on() { return fg.swapchain_ctx != nullptr; }

// make_swapchain (nvn_vk.cpp): the swapchain through AMD's (made in place again when there is one). False: not this
// way (frame generation off or not possible), make it as usual.
bool ffx_fg_create_swapchain(const VkSwapchainCreateInfoKHR& ci, VkSwapchainKHR* out, VkResult* result) {
    if (!ffx_fg_should()) return false;
    *result = VK_SUCCESS;
    if (fg.swapchain_ctx) {
        fg_destroy_contexts();
        *result = fg.fns.pOutCreateSwapchainFFXAPI(vk_device(), &ci, nullptr, out, reinterpret_cast<void*>(fg.swapchain));
        if (*result != VK_SUCCESS) return true;
    } else {
        ffxCreateContextDescFrameGenerationSwapChainVK sd{};
        sd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
        sd.physicalDevice = vk_physical(), sd.device = vk_device();
        VkSwapchainKHR none = VK_NULL_HANDLE;
        sd.swapchain = &none;
        sd.createInfo = ci;
        sd.gameQueue = {fg.game, fg.game_family, nullptr};
        sd.presentQueue = {fg.present.queue, fg.present.family, nullptr};
        sd.imageAcquireQueue = {fg.acquire.queue, fg.acquire.family, nullptr};
        sd.asyncComputeQueue = fg.async.queue ? VkQueueInfoFFXAPI{fg.async.queue, fg.async.family, nullptr}
                                              : VkQueueInfoFFXAPI{fg.present.queue, fg.present.family, nullptr};
        ffxCreateContextDescFrameGenerationSwapChainVersionVK ver{};
        ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
        ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;
        sd.header.pNext = &ver.header;
        if (ffxReturnCode_t r = g.fx.create(&fg.swapchain_ctx, &sd.header, nullptr); r != FFX_API_RETURN_OK) {
            log("nvn/fg-fsr: frame generation swapchain failed (%u): frame generation off", r);
            fg.swapchain_ctx = nullptr, fg.failed = true;
            return false;
        }
        fg.fns.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
        g.fx.query(&fg.swapchain_ctx, &fg.fns.header);
        *out = none;
    }
    fg.swapchain = *out;
    fg.frame_id = 0;
    if (!fg_make_context(ci)) log("nvn/fg-fsr: presenting without generated frames");
    return true;
}

void ffx_fg_destroy_swapchain(VkSwapchainKHR sc) {
    fg_destroy_contexts();
    if (fg.swapchain_ctx) g.fx.destroy(&fg.swapchain_ctx, nullptr), fg.swapchain_ctx = nullptr;
    if (fg.fns.pOutDestroySwapchainFFXAPI) fg.fns.pOutDestroySwapchainFFXAPI(vk_device(), sc, nullptr, reinterpret_cast<void*>(sc));
    fg.swapchain = VK_NULL_HANDLE;
}

VkResult ffx_fg_get_images(VkSwapchainKHR sc, uint32_t* n, VkImage* images) {
    return fg.fns.pOutGetSwapchainImagesKHR(vk_device(), sc, n, images);
}
VkResult ffx_fg_acquire(VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t* index) {
    return fg.fns.pOutAcquireNextImageKHR(vk_device(), sc, timeout, sem, fence, index);
}
// A game frame's present: frame generation off for it if temporal AA's pass did not run this frame (a menu with no
// scene): it would interpolate from what it has not been given.
VkResult ffx_fg_present(VkQueue q, const VkPresentInfoKHR* pi) {
    if (fg.fg_ctx && !fg.prepared) {
        ffxConfigureDescFrameGeneration c{};
        c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        c.swapChain = reinterpret_cast<void*>(fg.swapchain);
        c.frameGenerationEnabled = false;
        c.frameID = fg.frame_id;
        g.fx.configure(&fg.fg_ctx, &c.header);
    }
    fg.prepared = false;
    if (fg.swapchain_ctx && !fg.split_this_frame) {
        ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceVK u{};
        u.header.type = FFX_API_CONFIGURE_DESC_TYPE_FGSWAPCHAIN_REGISTERUIRESOURCE_VK;
        g.fx.configure(&fg.swapchain_ctx, &u.header);
    }
    fg.split_this_frame = false;
    const uint64_t t0 = plat::monotonic_ns();
    const VkResult r = fg.fns.pOutQueuePresentKHR(q, pi);
    fg_time(2, plat::monotonic_ns() - t0);
    return r;
}

// nvn_draw.cpp, right after the tonemap draw (its pass ended): the screen target before any UI, copied.
void temporal_after_tonemap(VkCommandBuffer cb, GpuImage& screen) {
    if (!fg.fg_ctx || !fg.prepared || !fg.hudless[0]) return;
    if (fg.capture.width != screen.width || fg.capture.height != screen.height || fg.capture.format != screen.format) {
        if (fg.capture.image) {  // a new size: the old one may still be read by a frame in flight
            if (async_gpu()) frame_wait_all();
            gpu_wait_game();
            destroy_image(fg.capture);
        }
        if (!make_image(fg.capture, screen.format, screen.width, screen.height)) {
            destroy_image(fg.capture);
            return;
        }
        fg.capture.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    const VkImageLayout was = screen.layout;
    transition(cb, screen, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(cb, fg.capture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy cp{};
    cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.extent = {screen.width, screen.height, 1};
    p_vkCmdCopyImage(cb, screen.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fg.capture.image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    transition(cb, screen, was == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_GENERAL : was);
    fg.captured = fg.frame_id;
}

uint64_t ffx_fg_presents() {
    return fg.swapchain && fg.fns.pOutGetLastPresentCountFFXAPI ? fg.fns.pOutGetLastPresentCountFFXAPI(fg.swapchain) : 0;
}

void ffx_fg_hudless_images(const VkImage* images, VkFormat format, VkExtent2D extent) {
    for (auto& [img, view] : fg.views) p_vkDestroyImageView(vk_device(), view, nullptr);  // the swapchain was made again
    fg.views.clear();
    for (uint32_t k = 0; k < kHudlessImages; k++) fg.hudless[k] = images ? images[k] : VK_NULL_HANDLE;
    fg.hudless_format = format, fg.hudless_extent = extent;
}

// present_image (nvn_vk.cpp), a game frame: this frame's capture, and the HUD-less image it goes to.
GpuImage* ffx_fg_hudless_source(uint32_t& slot) {
    if (!fg.fg_ctx || !fg.capture.image || fg.captured != fg.frame_id || !fg.hudless[0]) return nullptr;
    slot = uint32_t(fg.frame_id % kHudlessImages);
    return &fg.capture;
}

namespace {
void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                   VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from, b.newLayout = to, b.srcAccessMask = src_access, b.dstAccessMask = dst_access;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img, b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkImageView split_view(VkImage img) {
    auto it = fg.views.find(img);
    if (it != fg.views.end()) return it->second;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = fg.hudless_format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    p_vkCreateImageView(vk_device(), &vi, nullptr, &v);
    return fg.views[img] = v;
}

bool split_init() {
    if (fg.split) return true;
    if (fg.split_failed) return false;
    fg.split_failed = true;
    VkDevice d = vk_device();
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (p_vkCreateSampler(d, &si, nullptr, &fg.split_sampler) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[3] = {};
    b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &fg.split_sampler};
    b[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &fg.split_sampler};
    b[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 3, dli.pBindings = b;
    if (p_vkCreateDescriptorSetLayout(d, &dli, nullptr, &fg.split_set) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1, pli.pSetLayouts = &fg.split_set;
    if (p_vkCreatePipelineLayout(d, &pli, nullptr, &fg.split_layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof kFgUiSplit, mi.pCode = kFgUiSplit;
    VkShaderModule sm = VK_NULL_HANDLE;
    if (p_vkCreateShaderModule(d, &mi, nullptr, &sm) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT, ci.stage.module = sm, ci.stage.pName = "main";
    ci.layout = fg.split_layout;
    VkResult r = p_vkCreateComputePipelines(d, pipeline_cache(), 1, &ci, nullptr, &fg.split);
    p_vkDestroyShaderModule(d, sm, nullptr);
    if (r != VK_SUCCESS) return false;
    fg.split_failed = false;
    return true;
}
}  // namespace

// The frame's UI apart: the UI layer = the presented frame where it differs from the HUD-less picture of it (both drawn
// by the present pass), and the HUD-less picture copied into the backbuffer. Frame generation interpolates the
// backbuffer and lays the UI layer (registered with its swapchain, double-buffered by it) over the real and the
// generated frames alike.
void ffx_fg_ui_split(VkCommandBuffer cb, VkImage backbuffer, uint32_t slot) {
    const VkImage hudless = fg.hudless[slot];
    if (!fg.swapchain_ctx || !hudless || !split_init()) return;
    const VkExtent2D e = fg.hudless_extent;
    if (fg.ui.width != e.width || fg.ui.height != e.height) {
        if (fg.ui.image) {
            if (async_gpu()) frame_wait_all();
            gpu_wait_game();
            destroy_image(fg.ui);
        }
        if (!make_image(fg.ui, VK_FORMAT_R16G16B16A16_SFLOAT, e.width, e.height)) {
            destroy_image(fg.ui);
            return;
        }
        fg.ui.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    const VkPipelineStageFlags color = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               copy = VK_PIPELINE_STAGE_TRANSFER_BIT;
    image_barrier(cb, backbuffer, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, color, compute);
    image_barrier(cb, hudless, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, color, compute);
    transition(cb, fg.ui, VK_IMAGE_LAYOUT_GENERAL);
    VkDescriptorSet set = descriptor_set(fg.split_set);
    VkDescriptorImageInfo fi{fg.split_sampler, split_view(backbuffer), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo hi{fg.split_sampler, split_view(hudless), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo ui{VK_NULL_HANDLE, fg.ui.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    const VkDescriptorImageInfo* infos[3] = {&fi, &hi, &ui};
    for (uint32_t k = 0; k < 3; k++) {
        w[k].dstSet = set, w[k].dstBinding = k, w[k].descriptorCount = 1, w[k].pImageInfo = infos[k];
        w[k].descriptorType = k < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    p_vkUpdateDescriptorSets(vk_device(), 3, w, 0, nullptr);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, fg.split);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, fg.split_layout, 0, 1, &set, 0, nullptr);
    p_vkCmdDispatch(cb, (e.width + 7) / 8, (e.height + 7) / 8, 1);
    // The UI layer for frame generation to read; the HUD-less picture into the backbuffer.
    transition(cb, fg.ui, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    image_barrier(cb, backbuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, compute, copy);
    image_barrier(cb, hudless, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, compute, copy);
    VkImageCopy cp{};
    cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.extent = {e.width, e.height, 1};
    p_vkCmdCopyImage(cb, hudless, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, backbuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    image_barrier(cb, backbuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0, copy, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    image_barrier(cb, hudless, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0, copy,
                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    // Registered for this present (premultiplied: ui_split.comp stores colour times alpha).
    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceVK u{};
    u.header.type = FFX_API_CONFIGURE_DESC_TYPE_FGSWAPCHAIN_REGISTERUIRESOURCE_VK;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = fg.ui.format, ci.extent = {e.width, e.height, 1}, ci.mipLevels = 1;
    ci.arrayLayers = 1, ci.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    u.uiResource = ffxApiGetResourceVK(reinterpret_cast<void*>(fg.ui.image),
                                       ffxApiGetImageResourceDescriptionVK(fg.ui.image, ci, 0), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    u.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA |
              FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
    g.fx.configure(&fg.swapchain_ctx, &u.header);
    fg.split_this_frame = true;
}

// temporal_before_tonemap: this frame's depth and motion vectors (in temporal AA's states: read-only, the motion
// vectors in GENERAL), and the frame generation set for it.
void ffx_fg_prepare(const TemporalInputs& in, const TemporalFrame& fr, uint32_t w, uint32_t h) {
    if (!fg.fg_ctx) return;
    fg.frame_id++;
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = reinterpret_cast<void*>(fg.swapchain);
    c.frameGenerationCallback = fg_dispatch, c.frameGenerationCallbackUserContext = &fg.fg_ctx;
    c.frameGenerationEnabled = true;
    c.generationRect = {0, 0, int32_t(fg.extent.width), int32_t(fg.extent.height)};
    // The UI is not given as a HUD-less colour to extract it from (that left it see-through and blurred in the
    // generated frames): the backbuffer is the HUD-less picture and the UI a layer of its own (ffx_fg_ui_split).
    c.frameID = fg.frame_id;
    const uint64_t t0 = plat::monotonic_ns();
    g.fx.configure(&fg.fg_ctx, &c.header);
    const uint64_t t1 = plat::monotonic_ns();
    fg_time(0, t1 - t0);
    ffxDispatchDescFrameGenerationPrepareV2 d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    d.frameID = fg.frame_id;
    d.commandList = in.cb;
    d.renderSize = {w, h};
    d.jitterOffset = {in.jx, in.jy};
    d.motionVectorScale = {float(w), float(h)};
    d.frameTimeDelta = in.frame_ms;
    d.reset = in.reset;
    d.cameraNear = fr.n, d.cameraFar = fr.f, d.cameraFovAngleVertical = 2.0f * std::atan(fr.tan_half);
    d.viewSpaceToMetersFactor = 0.1f;
    d.depth = resource(*in.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = resource(*in.mv, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    // The camera from the view matrix's rows (right, up, back) and translation.
    for (int k = 0; k < 3; k++) {
        d.cameraRight[k] = float(fr.view[k]), d.cameraUp[k] = float(fr.view[4 + k]), d.cameraForward[k] = -float(fr.view[8 + k]);
        d.cameraPosition[k] = -float(fr.view[k] * fr.view[3] + fr.view[4 + k] * fr.view[7] + fr.view[8 + k] * fr.view[11]);
    }
    const ffxReturnCode_t r = g.fx.dispatch(&fg.fg_ctx, &d.header);
    fg_time(1, plat::monotonic_ns() - t1);
    if (r != FFX_API_RETURN_OK) {
        static int n = 0;
        if (n++ < 5) log("nvn/fg-fsr: prepare failed (%u)", r);
        return;
    }
    fg.prepared = true;
}
#endif  // NX_FFX

#else

void temporal_motion_draw(VkCommandBuffer, VkPipelineLayout, const ShaderInfo*, bool, bool, const int32_t*, uint64_t,
                          const uint8_t*, VkIndexType, uint64_t) {}
void temporal_motion_none(VkCommandBuffer, VkPipelineLayout) {}
void temporal_allow(std::shared_ptr<const TemporalFrame>) {}
void temporal_note_depth(GpuImage*) {}
bool temporal_is_tonemap(const std::string&) { return false; }
void temporal_device_ready() {}
void temporal_update() {}
void temporal_before_tonemap(VkCommandBuffer, GpuImage&, const GpuImage*) {}
GpuImage* temporal_sample_redirect(GpuImage* img) { return img; }
void temporal_tonemap_done() {}

#endif

#if !defined(NX_FFX)
bool ffx_fg_wanted() { return false; }
bool ffx_fg_should() { return false; }
void ffx_fg_queue_requests(VkPhysicalDevice, VkSurfaceKHR, const std::vector<VkQueueFamilyProperties>&, uint32_t,
                           std::vector<uint32_t>&) {}
void ffx_fg_device_ready(VkDevice, VkQueue) {}
bool ffx_fg_on() { return false; }
bool ffx_fg_create_swapchain(const VkSwapchainCreateInfoKHR&, VkSwapchainKHR*, VkResult*) { return false; }
void ffx_fg_destroy_swapchain(VkSwapchainKHR) {}
VkResult ffx_fg_get_images(VkSwapchainKHR, uint32_t*, VkImage*) { return VK_ERROR_FEATURE_NOT_PRESENT; }
VkResult ffx_fg_acquire(VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*) { return VK_ERROR_FEATURE_NOT_PRESENT; }
VkResult ffx_fg_present(VkQueue, const VkPresentInfoKHR*) { return VK_ERROR_FEATURE_NOT_PRESENT; }
uint64_t ffx_fg_presents() { return 0; }
void temporal_after_tonemap(VkCommandBuffer, GpuImage&) {}
void ffx_fg_hudless_images(const VkImage*, VkFormat, VkExtent2D) {}
GpuImage* ffx_fg_hudless_source(uint32_t&) { return nullptr; }
void ffx_fg_ui_split(VkCommandBuffer, VkImage, uint32_t) {}
#endif

}  // namespace nx::nvn
