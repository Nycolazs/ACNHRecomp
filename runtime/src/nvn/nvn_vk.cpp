// NVN's GPU side on Vulkan (docs/NVN.md, N3): the host device, a VkImage per NVN texture the GPU touches, the
// replay of recorded command lists, and presenting a window texture on the host swapchain.
//
// First version: every submit is recorded into one Vulkan command buffer and waited for (NX_PIPELINED, NX_ASYNC_GPU:
// into a slot's, not waited for, nvn_frames.cpp); image layouts are tracked per image, in submission order. The image
// behind a texture is nvn_image.cpp. On macOS this is MoltenVK: the instance enumerates portability drivers and the
// device enables VK_KHR_portability_subset.
#if !defined(_WIN32)
#include <unistd.h>  // _exit (stdlib.h on Windows)
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "nvn_shaders.h"
#include "nvn_ultrawide.h"
#include "nvn_vk.h"
#include "ui/ui.h"
#include "platform.h"

#include <nx/game.h>

namespace nx {
void vblank_hold();  // hle_vi.cpp: waits for the vblank before the one the image is due at
}

namespace nx::nvn {

using namespace nx::vk;


namespace {

std::recursive_mutex g_gpu;  // the whole GPU side: one submitting thread at a time
std::mutex g_queue_mx;  // the queue: the presenter thread's vkQueuePresentKHR against the replay thread's submits
VkResult presenter_idle();  // the presenter thread's pending present done (below)
bool g_ready = false, g_failed = false;
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkQueue g_queue = VK_NULL_HANDLE;
uint32_t g_family = 0;
VkPhysicalDeviceMemoryProperties g_memory{};
VkCommandPool g_pool = VK_NULL_HANDLE;
VkCommandBuffer g_cb = VK_NULL_HANDLE;
VkFence g_fence = VK_NULL_HANDLE;

VkSurfaceKHR g_surface = VK_NULL_HANDLE;
VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
std::vector<VkImage> g_sc_images;
VkExtent2D g_sc_extent{};
VkExtent2D g_sc_current{};  // the surface's currentExtent when the swapchain was made (SUBOPTIMAL: did it change?)
VkSurfaceTransformFlagBitsKHR g_sc_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
VkFormat g_sc_format = VK_FORMAT_B8G8R8A8_UNORM;
VkSemaphore g_acquired = VK_NULL_HANDLE, g_rendered = VK_NULL_HANDLE;
VkBool32 g_depth_clip_control = VK_FALSE;
bool g_dynamic_rendering = true;  // vkCmdBeginRendering usable (core 1.3 or VK_KHR_dynamic_rendering)
std::atomic<bool> g_fifo{false};    // the swapchain presents in FIFO (VSync): present_vsync_paced
bool g_display_timing = false;      // VK_GOOGLE_display_timing enabled
bool g_integrated = false;
bool g_require_sg32 = false;  // compute pipelines are made with a required subgroup size of 32 (Maxwell's warp)  // the chosen GPU is an integrated one (unified memory, the GPU is the long side)
VkPhysicalDeviceFeatures g_features{};  // the core features enabled (vk_features())
VkDescriptorPool g_descriptors = VK_NULL_HANDLE;

// The per-submit upload buffer (nvn_vk.h): one host-visible buffer, bump-allocated, reset at every submit.
VkBuffer g_upload = VK_NULL_HANDLE;
VkDeviceMemory g_upload_memory = VK_NULL_HANDLE;
uint8_t* g_upload_ptr = nullptr;
uint64_t g_upload_size = 0, g_upload_used = 0;
void make_upload_buffer(uint64_t size);

// Commands the Hades bridge intercepts are not in the generated table: ours are loaded here.
PFN_vkEnumerateInstanceExtensionProperties p_enum_instance_ext = nullptr;
PFN_vkCreateInstance p_create_instance = nullptr;
PFN_vkEnumeratePhysicalDevices p_enum_physical = nullptr;
PFN_vkCreateDevice p_create_device = nullptr;

}  // namespace

// A lost device means the GPU hung (a shader looping for ever): the process ends at once. Submitting more work, or
// waiting, keeps the GPU stuck, and on macOS that froze WindowServer and restarted the machine.
// The programs drawn by the last three submits (vertex + fragment shader), logged when the GPU hangs: a hang shows
// up one or two submits later than the one that caused it (pipelined frames). Then NX_SKIP_FS finds the culprit.
std::mutex g_hang_lock;
std::vector<std::pair<const ShaderInfo*, const ShaderInfo*>> g_hang_lists[3];
size_t g_hang_at = 0;
std::atomic<uint64_t> g_hang_serial{1};  // hang_next_submit's count: empties hang_note's cache

void hang_note(const ShaderInfo* vs, const ShaderInfo* fs) {
    // Every draw: a direct-mapped cache of the pairs already noted in this submit, before the lock and the list's
    // linear search (2% of the replay thread at 60 fps). The replay thread is the only caller.
    struct Seen {
        const ShaderInfo *vs = nullptr, *fs = nullptr;
        uint64_t serial = 0;
    };
    static PerThread<Seen[512]> seen_pt;  // per replaying thread (docs/PARALLEL_REPLAY.md)
    auto& seen = *seen_pt;
    const uint64_t serial = g_hang_serial.load(std::memory_order_relaxed);
    Seen& e = seen[((reinterpret_cast<uintptr_t>(vs) >> 4) ^ (reinterpret_cast<uintptr_t>(fs) >> 6)) & 511];
    if (e.serial == serial && e.vs == vs && e.fs == fs) return;
    e = {vs, fs, serial};
    std::lock_guard lk(g_hang_lock);
    auto& v = g_hang_lists[g_hang_at];
    for (const auto& p : v)
        if (p.first == vs && p.second == fs) return;
    if (v.size() < 512) v.emplace_back(vs, fs);
}

void hang_next_submit() {
    std::lock_guard lk(g_hang_lock);
    g_hang_at = (g_hang_at + 1) % 3;
    g_hang_serial.fetch_add(1, std::memory_order_relaxed);
    g_hang_lists[g_hang_at].clear();
}

[[noreturn]] void gpu_hung(const char* why) {
    log("nvn/vk: GPU hang (%s): stopping", why);
    {
        std::lock_guard lk(g_hang_lock);
        for (size_t k = 0; k < 3; k++) {
            const auto& v = g_hang_lists[(g_hang_at + 1 + k) % 3];  // oldest first
            std::string line;
            for (const auto& [vs, fs] : v) line += " " + (fs ? fs->hash : std::string("-"));
            log("nvn/vk: hang: submit -%zu drew %zu programs, fragment shaders:%s", 2 - k, v.size(), line.c_str());
        }
    }
    std::fprintf(stderr, "nvn/vk: GPU hang (%s): stopping\n", why);
    _exit(4);
}

namespace {

bool check(VkResult r, const char* what) {
    if (r >= 0) return true;
    log("nvn/vk: %s failed (%d)", what, int(r));
    if (r == VK_ERROR_DEVICE_LOST) gpu_hung(what);
    return false;
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (auto& e : list)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

void* g_gipa = nullptr;  // vkGetInstanceProcAddr, for surfaces made later (make_surface)
bool make_surface();

bool init() {
    if (g_ready || g_failed) return g_ready;
    g_failed = true;  // until everything below succeeds
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(plat::vulkan_get_instance_proc_addr());
    if (!gipa) {
        log("nvn/vk: no Vulkan loader");
        return false;
    }
    for (size_t k = 0; k < g_entry_count; k++) *g_entries[k].pfn = gipa(nullptr, g_entries[k].name);
    p_enum_instance_ext = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    p_create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));

    uint32_t n = 0;
    p_enum_instance_ext(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> iext(n);
    p_enum_instance_ext(nullptr, &n, iext.data());
    std::vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME, plat::vulkan_surface_extension()};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (has_extension(iext, "VK_KHR_portability_enumeration")) {
        exts.push_back("VK_KHR_portability_enumeration");
        ici.flags |= 0x00000001;  // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = nx::game::kInfo.title;
    app.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    if (!check(p_create_instance(&ici, nullptr, &g_instance), "vkCreateInstance")) return false;
    for (size_t k = 0; k < g_entry_count; k++)
        if (auto p = gipa(g_instance, g_entries[k].name)) *g_entries[k].pfn = p;
    p_enum_physical = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(g_instance, "vkEnumeratePhysicalDevices"));
    p_create_device = reinterpret_cast<PFN_vkCreateDevice>(gipa(g_instance, "vkCreateDevice"));

    n = 0;
    p_enum_physical(g_instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    p_enum_physical(g_instance, &n, gpus.data());
    gpus.resize(n);
    g_physical = pick_physical_device(gpus);  // NX_GPU, default: the first discrete GPU (nvn_gpu_pick.cpp)
    if (!g_physical) {
        log("nvn/vk: no GPU");
        return false;
    }
    VkPhysicalDeviceProperties props{};
    p_vkGetPhysicalDeviceProperties(g_physical, &props);
    p_vkGetPhysicalDeviceMemoryProperties(g_physical, &g_memory);
    g_integrated = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    // An integrated GPU shares the package's power budget with the CPU: ask the desktop for its performance profile
    // (NX_POWER_PROFILE=1: also with a discrete GPU; 0: never). frame_stats.cpp.
    if (g_integrated || (std::getenv("NX_POWER_PROFILE") && std::getenv("NX_POWER_PROFILE")[0] == '1'))
        plat::request_performance_profile();

    g_gipa = reinterpret_cast<void*>(gipa);
    if (!make_surface()) return false;

    p_vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    p_vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &n, families.data());
    g_family = UINT32_MAX;
    for (uint32_t k = 0; k < n; k++) {
        VkBool32 present = VK_FALSE;
        p_vkGetPhysicalDeviceSurfaceSupportKHR(g_physical, k, g_surface, &present);
        if ((families[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            g_family = k;
            break;
        }
    }
    if (g_family == UINT32_MAX) {
        log("nvn/vk: no graphics + present queue");
        return false;
    }

    p_vkEnumerateDeviceExtensionProperties(g_physical, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> dext(n);
    p_vkEnumerateDeviceExtensionProperties(g_physical, nullptr, &n, dext.data());
    std::vector<const char*> dexts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    // Features: everything core the GPU has, dynamic rendering (1.3), 8-bit indices (NVN has them), clip-space
    // depth -1..1 (NVN's default depth mode, what Wonder uses), and on MoltenVK the portability subset's own
    // features, which must be enabled to be used.
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT fu8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT fclip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    struct {  // VkPhysicalDevicePortabilitySubsetFeaturesKHR, without its header (beta extensions)
        VkStructureType sType = VkStructureType(1000163000);
        void* pNext = nullptr;
        VkBool32 fields[15] = {};
    } fport;
    // 1.1 and 1.2 too: the translated shaders declare DrawParameters (gl_BaseInstance/DrawIndex; ~3,100 of ACNH's
    // blocks), and without shaderDrawParameters enabled the Intel driver drew nothing into ACNH's main target.
    // Fragment shader interlock: the translator emits it where the host has it (StaticGpuAccessor).
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT finterlock{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    // The core feature structs of the version the device has: Qualcomm's own driver for the Adreno 6xx is Vulkan
    // 1.1, where a 1.2 or 1.3 struct is invalid, and draw parameters then come in their own struct.
    VkPhysicalDeviceProperties dprops{};
    p_vkGetPhysicalDeviceProperties(g_physical, &dprops);
    const uint32_t api = dprops.apiVersion;
    VkPhysicalDeviceShaderDrawParametersFeatures fdraw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeatures fdyn{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    void** tail;
    if (api >= VK_API_VERSION_1_3) {
        features.pNext = &f11;
        f11.pNext = &f12;
        f12.pNext = &f13;
        tail = &f13.pNext;
    } else if (api >= VK_API_VERSION_1_2) {
        features.pNext = &f11;
        f11.pNext = &f12;
        tail = &f12.pNext;
    } else {
        features.pNext = &fdraw;
        tail = &fdraw.pNext;
    }
    if (api < VK_API_VERSION_1_3 && has_extension(dext, "VK_KHR_dynamic_rendering")) {
        dexts.push_back("VK_KHR_dynamic_rendering");
        *tail = &fdyn, tail = &fdyn.pNext;
    }
    if (has_extension(dext, "VK_EXT_fragment_shader_interlock")) {
        dexts.push_back("VK_EXT_fragment_shader_interlock");
        *tail = &finterlock, tail = &finterlock.pNext;
    }
    if (has_extension(dext, "VK_EXT_index_type_uint8")) {
        dexts.push_back("VK_EXT_index_type_uint8");
        *tail = &fu8, tail = &fu8.pNext;
    }
    if (has_extension(dext, "VK_GOOGLE_display_timing")) {  // when frames reach the screen (display_timing_log)
        dexts.push_back("VK_GOOGLE_display_timing");
        g_display_timing = true;
    }
    if (has_extension(dext, "VK_EXT_depth_clip_control")) {
        dexts.push_back("VK_EXT_depth_clip_control");
        *tail = &fclip, tail = &fclip.pNext;
    }
    if (has_extension(dext, "VK_KHR_portability_subset")) {
        dexts.push_back("VK_KHR_portability_subset");
        *tail = &fport, tail = &fport.pNext;
    }
    p_vkGetPhysicalDeviceFeatures2(g_physical, &features);
    {   // The translated shaders assume Maxwell's warp of 32 (TRANSLATOR.md). Intel runs subgroups of 8, 16 or 32 as its
        // compiler chooses; where the size can be pinned (core 1.3 subgroup size control), compute pipelines pin 32.
        VkPhysicalDeviceSubgroupSizeControlProperties sgc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &sgc;
        p_vkGetPhysicalDeviceProperties2(g_physical, &p2);
        const char* e = std::getenv("NX_SUBGROUP32");
        bool can = f13.subgroupSizeControl && sgc.minSubgroupSize <= 32 && sgc.maxSubgroupSize >= 32 &&
                   (sgc.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT);
        g_require_sg32 = can && e && e[0] == '1';  // experimental, off by default: no visible effect on the Intel silhouettes
        log("nvn/vk: subgroup sizes %u..%u, compute subgroup size %s", sgc.minSubgroupSize, sgc.maxSubgroupSize,
            g_require_sg32 ? "pinned to 32" : "left to the driver");
    }
    g_depth_clip_control = fclip.depthClipControl;
    g_dynamic_rendering = api >= VK_API_VERSION_1_3 ? f13.dynamicRendering == VK_TRUE : fdyn.dynamicRendering == VK_TRUE;
    log("nvn/vk: Vulkan %u.%u device, dynamic rendering %s", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
        g_dynamic_rendering ? "yes" : "no: render pass objects");
    g_features = features.features;
    // NX_ASTC_DECODE=1|linear|srgb: the CPU decoder even where the GPU has ASTC (to compare the two on one GPU).
    set_astc_native(g_features.textureCompressionASTC_LDR);
    log("nvn/vk: ASTC %s", astc_native() ? "native" : "decoded to RGBA8 at upload (no textureCompressionASTC_LDR)");
    log("nvn/vk: depthClamp %u, depthBiasClamp %u, depthBounds %u, depth clip control %u (extension %u)",
        g_features.depthClamp, g_features.depthBiasClamp, g_features.depthBounds, g_depth_clip_control,
        has_extension(dext, "VK_EXT_depth_clip_control") ? 1u : 0u);
    log("nvn/vk: portability: format reinterpretation %u, format swizzle %u", fport.fields[2], fport.fields[3]);
    log_device_caps(g_physical, dext);  // nvn_caps.cpp
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(dexts.size());
    dci.ppEnabledExtensionNames = dexts.data();
    dci.pNext = &features;
    if (!check(p_create_device(g_physical, &dci, nullptr, &g_device), "vkCreateDevice")) return false;
    p_vkGetDeviceQueue(g_device, g_family, 0, &g_queue);
    vk_defer_install();  // nvn_vk_defer.cpp
    static const bool ui_off = std::getenv("NX_UI") && std::getenv("NX_UI")[0] == '0';  // NX_UI=0: no settings screen
    if ((!plat::headless() || std::getenv("NX_UI_OPEN")) && !ui_off)  // the settings screen (ui/ui.cpp)
        ui::ui_vulkan_ready(reinterpret_cast<PFN_vkGetInstanceProcAddr>(g_gipa), g_instance, g_physical, g_device, g_family,
                            g_queue, g_dynamic_rendering);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_family;
    p_vkCreateCommandPool(g_device, &pci, nullptr, &g_pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    p_vkAllocateCommandBuffers(g_device, &cai, &g_cb);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    p_vkCreateFence(g_device, &fci, nullptr, &g_fence);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    p_vkCreateSemaphore(g_device, &sci, nullptr, &g_acquired);
    p_vkCreateSemaphore(g_device, &sci, nullptr, &g_rendered);

    // A busy submit reached 67 MB: the 2048x8192 glyph atlas's first upload (R8 widened to RGBA8), which now has a
    // staging buffer of its own (nvn_upload.cpp); the scripted run's other submits stay under ~12 MB. Android: 64 MB
    // (docs/nvn/android-perf/PIPELINES-1810.md: 0 "upload buffer full" lines at 64 and at 32 MB on the PC; staging_wanted
    // spills bigger uploads to staging buffers in proportion to the ring).
    {
#if defined(__ANDROID__)
        uint64_t ring_mb = 64;
#else
        uint64_t ring_mb = 256;
#endif
        if (const char* e = std::getenv("NX_UPLOAD_RING_MB"))  // config.txt; 16..1024
            if (long v = std::atol(e); v >= 16 && v <= 1024) ring_mb = uint64_t(v);
        log("nvn/vk: upload ring %llu MB", (unsigned long long)ring_mb);
        make_upload_buffer(ring_mb << 20);
    }
    if (async_gpu())
        frame_init(g_pool, g_family, g_upload, g_upload_ptr, g_upload_size);  // the upload buffer: the slots' ring
    else
        g_descriptors = make_descriptor_pool();
    log("nvn/vk: %s, Vulkan %u.%u, queue family %u", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
        VK_API_VERSION_MINOR(props.apiVersion), g_family);
    g_failed = false;
    g_ready = true;
    return true;
}

// The window's surface, and the swapchain format from what the surface offers: B8G8R8A8_UNORM (the Mac), else
// R8G8B8A8_UNORM (Android surfaces offer no BGRA: a swapchain made with it showed nothing on a Galaxy S23), else the
// first. A new Android window (the app back from home or the lock screen) needs a new surface (gpu_present).
void* g_window = nullptr;
uint64_t g_window_gen = 0;
bool make_surface() {
    if (g_surface) p_vkDestroySurfaceKHR(g_instance, g_surface, nullptr);
    if (g_window) plat::release_window(g_window);
    g_surface = VK_NULL_HANDLE;
    g_window = plat::create_window();  // waits for a window (Android); the generation is read once it is there
    g_window_gen = plat::window_generation();
    uint64_t surface = 0;
    if (!check(VkResult(plat::vulkan_create_surface(g_instance, g_window, &surface, g_gipa)), "surface")) return false;
    g_surface = reinterpret_cast<VkSurfaceKHR>(surface);
    uint32_t n = 0;
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(g_physical, g_surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(g_physical, g_surface, &n, formats.data());
    auto offered = [&](VkFormat f) {
        for (const VkSurfaceFormatKHR& x : formats)
            if (x.format == f) return true;
        return false;
    };
    g_sc_format = offered(VK_FORMAT_B8G8R8A8_UNORM)   ? VK_FORMAT_B8G8R8A8_UNORM
                  : offered(VK_FORMAT_R8G8B8A8_UNORM) ? VK_FORMAT_R8G8B8A8_UNORM
                  : n                                  ? formats[0].format
                                                       : VK_FORMAT_B8G8R8A8_UNORM;
    log("nvn/vk: surface: %u formats offered, swapchain format %d", n, int(g_sc_format));
    return true;
}

bool make_swapchain() {
    presenter_idle();
    ui::ui_swapchain_changed();
    if (g_swapchain) {
        if (async_gpu()) frame_wait_all();
        p_vkDeviceWaitIdle(g_device);
        present_pass_release();
        p_vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
        g_swapchain = VK_NULL_HANDLE;
    }
    VkSurfaceCapabilitiesKHR caps{};
    p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physical, g_surface, &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        uint32_t w = 0, h = 0;
        plat::window_size(w, h);
        extent = {w ? w : 1280, h ? h : 720};
    }
    if (!extent.width || !extent.height) return false;  // minimized
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_surface;
    // With frame generation two images are queued per game frame: with three, acquiring the next one waited for the
    // display (~14 ms a frame on a Snapdragon 865), and the replay thread, which presents, could not go on with the
    // next frame meanwhile. Five keep a free image, also for the 30 fps repeat without it, and frame generation can be
    // turned on while the game runs. NX_SWAP_IMAGES: the count.
    static const unsigned want = [] {
        const char* e = std::getenv("NX_SWAP_IMAGES");
        if (e && std::atoi(e) > 0) return unsigned(std::atoi(e));
#if defined(__ANDROID__)
        return 5u;
#else
        framegen_update();
        return framegen_wanted() ? 5u : 3u;
#endif
    }();
    ci.minImageCount = std::max(caps.minImageCount, want);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = g_sc_format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    // The present pass (nvn_present.cpp) draws the picture pre-rotated: the images are in the display's own
    // orientation (portrait on a phone held sideways). Without it the images stay upright (the Mac: identity).
    bool sideways = caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
    bool pass = present_pass_wanted() && !present_pass_failed();
    g_sc_current = caps.currentExtent;
    if (pass && sideways && extent.width > extent.height) std::swap(extent.width, extent.height);
    if (!pass && (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))
        ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    // Some drivers report maxImageExtent in the other orientation: the larger side bounds both.
    uint32_t most = std::max(caps.maxImageExtent.width, caps.maxImageExtent.height);
    if (most) extent.width = std::min(extent.width, most), extent.height = std::min(extent.height, most);
    ci.imageExtent = extent;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha))
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
            if (caps.supportedCompositeAlpha & a) {
                ci.compositeAlpha = a;
                break;
            }
    log("nvn/vk: surface %ux%u (current %ux%u), transform %u of %#x, composite alpha %#x of %#x, usage %#x",
        extent.width, extent.height, caps.currentExtent.width, caps.currentExtent.height, unsigned(caps.currentTransform),
        caps.supportedTransforms, unsigned(ci.compositeAlpha), caps.supportedCompositeAlpha,
        caps.supportedUsageFlags);
    // VSync (video.vsync, NX_VSYNC; on by default): FIFO. IMMEDIATE tore in full screen on the Mac (MoltenVK turns the
    // Metal layer's display sync off; only a window goes through the compositor, which hid it). Off: the game paces
    // itself on its vsync event (hle_vi.cpp, 60 Hz) and a FIFO present also waits for the display's own refresh, two
    // clocks drifting against each other (stutter near 60 fps, docs/nvn/DEBUG.md #33): MAILBOX never blocks and shows
    // the newest frame at each refresh, else IMMEDIATE. NX_FIFO=1: FIFO either way (DEBUG.md #62).
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    const bool vsync = !(std::getenv("NX_VSYNC") && std::getenv("NX_VSYNC")[0] == '0');
    uint32_t modes_n = 0;
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(g_physical, g_surface, &modes_n, nullptr);
    std::vector<VkPresentModeKHR> modes(modes_n);
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(g_physical, g_surface, &modes_n, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    if (!vsync && !std::getenv("NX_FIFO")) {
        if (has(VK_PRESENT_MODE_MAILBOX_KHR))
            ci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR))
            ci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    g_fifo = ci.presentMode == VK_PRESENT_MODE_FIFO_KHR;
    static const char* const mode_names[] = {"IMMEDIATE", "MAILBOX", "FIFO", "FIFO_RELAXED"};
    log("nvn/vk: present mode %s (%u modes offered), %u images asked", ci.presentMode <= 3 ? mode_names[ci.presentMode] : "?",
        modes_n, ci.minImageCount);
    ci.clipped = VK_TRUE;
    if (!check(p_vkCreateSwapchainKHR(g_device, &ci, nullptr, &g_swapchain), "vkCreateSwapchainKHR")) return false;
    uint32_t n = 0;
    p_vkGetSwapchainImagesKHR(g_device, g_swapchain, &n, nullptr);
    g_sc_images.resize(n);
    p_vkGetSwapchainImagesKHR(g_device, g_swapchain, &n, g_sc_images.data());
    g_sc_extent = extent;
    g_sc_transform = ci.preTransform;
    ui::ui_set_rotation(ci.preTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR    ? 1
                        : ci.preTransform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR ? 2
                        : ci.preTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR ? 3
                                                                                     : 0);
    if (n > 8) log("nvn/vk: %u swapchain images: more than the 8 present semaphores (nvn_frames.cpp)", n);
    bool drawn = pass && present_pass_targets(g_sc_format, g_sc_images, extent, ci.preTransform);
    if (pass && !drawn && present_pass_failed()) {  // the pass is out: an upright swapchain for the blit
        log("nvn/vk: present pass failed: swapchain made again for the blit");
        return make_swapchain();
    }
    log("nvn/vk: swapchain %ux%u, %u images, transform %u, %s", extent.width, extent.height, n,
        unsigned(ci.preTransform), drawn ? "drawn by the present pass" : "blitted");
    return true;
}

}  // namespace

std::recursive_mutex& gpu_lock() { return g_gpu; }
VkDevice vk_device() { return g_device; }
VkPhysicalDevice vk_physical() { return g_physical; }
VkQueue vk_queue() { return g_queue; }
std::mutex& vk_queue_mutex() { return g_queue_mx; }
uint32_t vk_queue_family() { return g_family; }
bool vk_depth_clip_control() { return g_depth_clip_control; }
bool vk_dynamic_rendering() { return g_dynamic_rendering; }
const VkPhysicalDeviceFeatures& vk_features() { return g_features; }
bool gpu_is_integrated() { return g_integrated; }
bool gpu_require_subgroup32() { return g_require_sg32; }

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t k = 0; k < g_memory.memoryTypeCount; k++)
        if ((bits >> k) & 1 && (g_memory.memoryTypes[k].propertyFlags & want) == want) return k;
    return 0;
}

Upload upload_alloc(uint64_t size, uint64_t align) {
    if (async_gpu()) return frame_upload_alloc(size, align);
    uint64_t at = (g_upload_used + align - 1) & ~(align - 1);
    if (at + size > g_upload_size) {
        log("nvn/vk: upload buffer full (%llu + %llu bytes)", (unsigned long long)at, (unsigned long long)size);
        at = 0;  // wraps: this submit's earlier data may be overwritten (grow the buffer when this shows)
        replay::dedup_reset();  // shared copies may be among them
    }
    g_upload_used = at + size;
    return {g_upload, at, g_upload_ptr + at};
}

Upload upload(const void* data, uint64_t size, uint64_t align) {
    Upload u = upload_alloc(size, align);
    if (data && size) std::memcpy(u.ptr, data, size);
    return u;
}

VkDescriptorSet descriptor_set(VkDescriptorSetLayout layout) {
    if (async_gpu()) return frame_descriptor_set(layout);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g_descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (p_vkAllocateDescriptorSets(g_device, &ai, &set) != VK_SUCCESS) log("nvn/vk: descriptor pool exhausted");
    return set;
}

VkDescriptorPool make_descriptor_pool() {
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 65536}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16384},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 65536}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 32768;
    dpi.poolSizeCount = uint32_t(sizeof sizes / sizeof sizes[0]);
    dpi.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    p_vkCreateDescriptorPool(g_device, &dpi, nullptr, &pool);
    return pool;
}

namespace {

void make_upload_buffer(uint64_t size) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = kUploadUsage;
    p_vkCreateBuffer(g_device, &bi, nullptr, &g_upload);
    VkMemoryRequirements req{};
    p_vkGetBufferMemoryRequirements(g_device, g_upload, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = upload_memory_type(req.memoryTypeBits);  // nvn_caps.cpp (NX_RING_CACHED)
    p_vkAllocateMemory(g_device, &ai, nullptr, &g_upload_memory);
    p_vkBindBufferMemory(g_device, g_upload, g_upload_memory, 0);
    void* p = nullptr;
    p_vkMapMemory(g_device, g_upload_memory, 0, VK_WHOLE_SIZE, 0, &p);
    g_upload_ptr = static_cast<uint8_t*>(p);
    g_upload_size = size;
}

bool begin(bool replay = false) {
    if (async_gpu()) return (g_cb = frame_begin(replay)) != VK_NULL_HANDLE;
    p_vkResetCommandBuffer(g_cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return check(p_vkBeginCommandBuffer(g_cb, &bi), "vkBeginCommandBuffer");
}

// Where the submitting thread's time goes, logged with the draw stats, per presented frame: the replay (recording
// into Vulkan), vkQueueSubmit (Metal encoding happens inside it), fences waited for, the rest of gpu_submit (object
// drain, image frees, write-backs), and gpu_present as a whole; bytes through the upload buffer.
struct Timing {
    uint64_t replay_cpu_ns = 0;
    uint64_t submits = 0, replay_ns = 0, encode_ns = 0, fence_ns = 0, submit_ns = 0, present_ns = 0, upload_bytes = 0;
} g_timing;

void end_and_wait(VkSemaphore wait, VkSemaphore signal) {
    uint64_t t0 = plat::monotonic_ns();
    p_vkEndCommandBuffer(g_cb);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cb;
    if (wait) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait;
        si.pWaitDstStageMask = &stage;
    }
    if (signal) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signal;
    }
    p_vkResetFences(g_device, 1, &g_fence);
    bool queued;
    {
        std::lock_guard q(g_queue_mx);
        queued = check(p_vkQueueSubmit(g_queue, 1, &si, g_fence), "vkQueueSubmit");
    }
    uint64_t t1 = plat::monotonic_ns();
    // 2 s at most: a frame's work takes milliseconds; longer is a shader that does not end.
    if (queued) {
        VkResult w = p_vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, 2'000'000'000ull);
        if (w == VK_TIMEOUT) gpu_hung("a submit did not finish in 2 s");
        check(w, "vkWaitForFences");
    }
    frame_times().encode_ns += t1 - t0, frame_times().fence_ns += plat::monotonic_ns() - t1;
}

// NX_AB=<name>: an A/B test within one run. ab_b(name) is false for 1200 presents (logged as "ab A"), then true for
// as many ("ab B"), and so on: both halves see the same scene and the same heat, which runs one after the other on the
// fanless M1 did not (the later run was always slower). tools/play/ab_report.py averages each half.
std::atomic<int> g_ab_phase{0};
}  // namespace
bool ab_b(const char* name) {
    static const char* e = std::getenv("NX_AB");
    return e && g_ab_phase.load(std::memory_order_relaxed) && std::strcmp(e, name) == 0;
}
namespace {

void log_timing(uint64_t frames) {
    auto ms = [&](uint64_t ns) { return double(ns) / 1e6 / double(frames); };
    static uint64_t ab_frames = 0;
    if (std::getenv("NX_AB") && (ab_frames += frames) >= 1200) ab_frames = 0, g_ab_phase.store(!g_ab_phase.load());
    static uint64_t last = 0;  // wall clock: the real frame rate
    uint64_t now = plat::monotonic_ns();
    double fps = last ? double(frames) * 1e9 / double(now - last) : 0.0;
    last = now;
    const Timing& t = g_timing;
    uint64_t known = t.replay_ns + t.encode_ns + t.fence_ns;
    log("nvn/vk: per frame over %llu: replay on the CPU %.2f ms of it", (unsigned long long)frames, ms(t.replay_cpu_ns));
    log("nvn/vk: per frame over %llu: %.1f fps, %.1f submits, replay %.2f ms, submit (encode) %.2f ms, fence wait "
        "%.2f ms, other %.2f ms, present %.2f ms, upload %.2f MB%s",
        (unsigned long long)frames, fps, double(t.submits) / double(frames), ms(t.replay_ns), ms(t.encode_ns),
        ms(t.fence_ns), ms(t.submit_ns > known ? t.submit_ns - known : 0), ms(t.present_ns),
        double(t.upload_bytes) / 1e6 / double(frames), std::getenv("NX_AB") ? (g_ab_phase.load() ? ", ab B" : ", ab A") : "");
    frame_log(frames);
    creation_log(frames);  // pipelines and ASTC decodes of the window; saves the pipeline cache now and then
    g_timing = {};
}

// gpu_present's time, whatever way it returns.
struct PresentTimer {
    uint64_t t0 = plat::monotonic_ns();
    ~PresentTimer() { g_timing.present_ns += plat::monotonic_ns() - t0; }
};

}  // namespace

uint64_t draw_take_dump_request();          // nvn_draw.cpp

namespace {
std::shared_ptr<const DeferredValues> g_deferred;  // the submit being replayed (replay thread only)
}
const DeferredValues* current_deferred() { return g_deferred.get(); }

void gpu_submit(const std::vector<std::shared_ptr<CmdList>>& lists, std::shared_ptr<const DeferredValues> deferred) {
    g_deferred = std::move(deferred);
    std::lock_guard lk(g_gpu);
    uint64_t t_in = plat::monotonic_ns();
    const FrameTimes f_in = frame_times();
    uint64_t lists_epoch = 0;
    for (auto& l : lists) lists_epoch = std::max(lists_epoch, l->epoch);
    hang_next_submit();
    ReplayScope replay(lists_epoch);  // the texture mark (#55, #57), then drain_objects (under the GPU lock, #50)
    if (!init()) return;
    uw_apply();  // a new screen scale starts with a frame (nvn_ultrawide.cpp)
    bool async = async_gpu();
    if (!async) {  // the previous submit was waited for: all of it is free
        free_retired_images(UINT64_MAX);
        p_vkResetDescriptorPool(g_device, g_descriptors, 0);
        g_upload_used = 0;
    } else if (pipelined()) {  // what the retired submits were the last to use (they free nothing themselves)
        free_retired_images(frame_completed());
    }
    if (!begin(true)) return;
    VkDeferScope defer;  // the replay's Vulkan calls on the recorder thread (nvn_vk_defer.cpp), drained by the end
    draw_begin_submit();
    gpu_profile_begin(g_cb);
    zero_draws_begin_submit(g_cb);
    // Pipelined: the part up to the last dispatch is queued as soon as it is recorded (nvn_frames.cpp). Not with the
    // profile or the zero-draw queries: they span the submit, which is then waited for whole.
    size_t split_after = gpu_profile_on() || zero_draws_on() ? SIZE_MAX : frame_plan_split(lists);
    if (parallel_plan_on()) parallel_plan_frame(lists);  // docs/PARALLEL_REPLAY.md, P0: logged only
    uint64_t t0 = plat::monotonic_ns();
    const uint64_t cpu0 = thread_cpu_ns();
    const FrameTimes f0 = frame_times();
    Targets st;
    if (parallel_replay_on()) {  // docs/PARALLEL_REPLAY.md
        parallel_replay(g_cb, st, lists, split_after);
        g_cb = frame_cb(g_cb);
    } else {
        for (size_t k = 0; k < lists.size(); k++) {
            replay_list(g_cb, st, *lists[k], 0);
            // After the last list with a dispatch, unless nothing follows it (replay_list may have split inside a list).
            g_cb = k == split_after && k + 1 < lists.size() ? frame_split(g_cb) : frame_cb(g_cb);
        }
    }
    draw_end_rendering(g_cb);
    const FrameTimes f1 = frame_times();  // the first part's submit and ring waits are not replay time
    g_timing.submits++;
    g_timing.replay_ns += plat::monotonic_ns() - t0 - (f1.encode_ns - f0.encode_ns) - (f1.fence_ns - f0.fence_ns);
    // the replay thread's own CPU time over the same span: the rest of the replay time it was off the CPU
    g_timing.replay_cpu_ns += thread_cpu_ns() - cpu0;
    g_timing.upload_bytes += async ? frame_upload_used() : g_upload_used;
    gpu_profile_mark(g_cb, "end");
    if (async)  // the profile, zero-draw queries and snapshots read this submit's results right below: waited for
        frame_submit(VK_NULL_HANDLE, VK_NULL_HANDLE, draw_take_write_backs(),
                     gpu_profile_on() || zero_draws_on() || debug_snapshots_pending());
    else
        end_and_wait(VK_NULL_HANDLE, VK_NULL_HANDLE);
    draw_end_submit();  // not waited for: snapshots only (the write-backs went with the submit)
    gpu_profile_collect(), zero_draws_collect();
    debug_dump_requested(draw_take_dump_request());  // reads back in a submit of its own, which it waits for
    const FrameTimes f2 = frame_times();
    g_timing.encode_ns += f2.encode_ns - f_in.encode_ns, g_timing.fence_ns += f2.fence_ns - f_in.fence_ns;
    g_timing.submit_ns += plat::monotonic_ns() - t_in;
}

// Headless: the presented texture is only counted, logged and dumped (no swapchain). Nothing is submitted unless a
// frame dump is due: the empty submit was a Metal command buffer and a fence wait per frame.
void present_headless(uint64_t texture) {
    GpuImage* src = image_of(texture);  // made here if new, as before (the targets rendered into it share it, #44)
    static uint64_t presents = 0;
    presents++;
    if (presents % (presents < 300 ? 60 : 300) == 0) draw_log_stats();
    if (presents % 60 == 0) log_timing(60), gpu_profile_log(60);
    if (presents % 300 == 0) zero_draws_log();
    // NX_PRESENT_TEST_ALL=1 (with NX_PRESENT_TEST): the present pass runs for every frame, and its submit is timed.
    static const bool every = std::getenv("NX_PRESENT_TEST_ALL") != nullptr;
    bool due = frame_dump_due(presents);
    if (due && (!src || src->layout == VK_IMAGE_LAYOUT_UNDEFINED))
    {
        log("nvn/vk: frame dump %llu skipped: the presented texture %llx was never drawn into",
            (unsigned long long)presents, (unsigned long long)texture);
        static int detail = 0;
        if (detail++ < 3) {
            debug_log_texture("presented", texture);
            // NX_DEBUG_TEXTURES=<hex>,<hex>: other textures to compare with (render targets seen in NX_TRACE_DRAWS)
            if (const char* e = std::getenv("NX_DEBUG_TEXTURES"))
                for (const char* p = e; *p;) {
                    char* end = nullptr;
                    uint64_t g = std::strtoull(p, &end, 16);
                    if (end == p) break;
                    debug_log_texture("listed", g);
                    p = *end == ',' ? end + 1 : end;
                }
        }
    }
    // NX_FRAMEGEN: the in-between frames are made here too (timed), for every frame whatever the pace; a dump also
    // writes the one before this frame (NNNNN_gen) and the last real frame (NNNNN_prev), next to this one.
    if (framegen_wanted() && src && src->layout != VK_IMAGE_LAYOUT_UNDEFINED && begin()) {
        GpuImage* prev = nullptr;
        GpuImage* gen = framegen_record(g_cb, texture, *src, true, &prev);
        auto submit = [&](bool wait) {
            if (async_gpu())
                frame_submit(VK_NULL_HANDLE, VK_NULL_HANDLE, {}, wait);
            else
                end_and_wait(VK_NULL_HANDLE, VK_NULL_HANDLE);
        };
        if (due && gen) {
            transition(g_cb, *gen, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            bool dump = frame_dump_record(g_cb, *gen);
            submit(dump);
            if (dump) frame_dump_write(*gen, presents, "_gen");
            if (prev && begin()) {
                transition(g_cb, *prev, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                dump = frame_dump_record(g_cb, *prev);
                submit(dump);
                if (dump) frame_dump_write(*prev, presents, "_prev");
            }
        } else {
            submit(false);
        }
    }
    if (!due && !every) return;
    static uint64_t timed_ns = 0, timed = 0;
    const uint64_t t0 = plat::monotonic_ns();
    if (src && src->layout != VK_IMAGE_LAYOUT_UNDEFINED && begin()) {
        GpuImage* shown = src;  // NX_PRESENT_TEST: what the present pass would put on a display (nvn_present.cpp)
        if (GpuImage* test = present_pass_test_target(); test && present_pass_record(g_cb, *src, nullptr, 0))
            test->layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, shown = test;
        // NX_UI_OPEN=1 headless: the settings screen drawn over the dumped frame (to test it without a display).
        static const bool ui_test = std::getenv("NX_UI_OPEN") != nullptr;
        if (ui_test && due) {
            transition(g_cb, *shown, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            ui::ui_record(g_cb, shown->image, shown->format, VkExtent2D{shown->width, shown->height});
        }
        transition(g_cb, *shown, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        bool dump = due && frame_dump_record(g_cb, *shown);
        if (async_gpu())
            frame_submit(VK_NULL_HANDLE, VK_NULL_HANDLE, {}, dump);  // a dumped frame is waited for
        else
            end_and_wait(VK_NULL_HANDLE, VK_NULL_HANDLE);
        if (dump) frame_dump_write(*shown, presents);
        if (every && !due && ++timed % 120 == 0) {
            log("nvn/vk: present test: pass + wait %.2f ms/frame over the last %llu", double(timed_ns) / 1e6 / 120, 120ull);
            timed_ns = 0;
        } else if (every && !due) {
            timed_ns += plat::monotonic_ns() - t0;
        }
    }
    debug_dump_textures(presents);
}

namespace {

// VK_GOOGLE_display_timing: each present gets an id; which ids were the game's own frames (not the vsync repeats) is
// kept, and every 300 frames the times the frames actually reached the screen are read back and logged as how many
// refreshes each frame stayed (at 30 fps on a 60 Hz display: 2 each, when it is smooth).
uint32_t g_present_id = 0;
std::vector<uint8_t> g_present_real(4096);
uint64_t g_last_real_shown = 0, g_refresh_ns = 0;
uint64_t g_shown_for[6] = {};  // 1, 2, 3, 4, 5, 6+ refreshes

uint32_t display_timing_next(bool real) {
    uint32_t id = ++g_present_id;
    g_present_real[id % g_present_real.size()] = real;
    return id;
}

void display_timing_log() {
    static uint64_t frames = 0;
    if (++frames % 30) return;
    if (!g_refresh_ns) {
        VkRefreshCycleDurationGOOGLE rc{};
        if (p_vkGetRefreshCycleDurationGOOGLE(g_device, g_swapchain, &rc) == VK_SUCCESS) g_refresh_ns = rc.refreshDuration;
        if (!g_refresh_ns) g_refresh_ns = 16'666'667;
    }
    uint32_t n = 0;
    if (p_vkGetPastPresentationTimingGOOGLE(g_device, g_swapchain, &n, nullptr) != VK_SUCCESS || !n) return;
    std::vector<VkPastPresentationTimingGOOGLE> t(n);
    if (p_vkGetPastPresentationTimingGOOGLE(g_device, g_swapchain, &n, t.data()) != VK_SUCCESS) return;
    for (uint32_t k = 0; k < n; k++) {
        if (!g_present_real[t[k].presentID % g_present_real.size()] || !t[k].actualPresentTime) continue;
        if (g_last_real_shown && t[k].actualPresentTime > g_last_real_shown) {
            double refreshes = double(t[k].actualPresentTime - g_last_real_shown) / double(g_refresh_ns);
            int r = int(refreshes + 0.5);
            g_shown_for[std::clamp(r, 1, 6) - 1]++;
        }
        g_last_real_shown = t[k].actualPresentTime;
    }
    if (frames % 300 == 0) {
        log("display: game frames shown for 1/2/3/4/5/6+ refreshes (%.2f ms): %llu %llu %llu %llu %llu %llu",
            double(g_refresh_ns) / 1e6, (unsigned long long)g_shown_for[0], (unsigned long long)g_shown_for[1],
            (unsigned long long)g_shown_for[2], (unsigned long long)g_shown_for[3], (unsigned long long)g_shown_for[4],
            (unsigned long long)g_shown_for[5]);
        for (auto& c : g_shown_for) c = 0;
    }
}

// ---- the presenter thread -----------------------------------------------------------------------------------------
// On Android vkQueuePresentKHR does not return until the GPU has finished the image queued before (the BufferQueue
// throttles an EGL/Vulkan producer to two queued buffers: "Throttling EGL Production"). With frame generation two
// images go out per game frame, so the game frame's present waited for the generated one, that is for the whole
// frame's GPU work: ~10 ms a frame on a Snapdragon 865 in which the replay thread did nothing. The game frame's
// present is handed to this thread instead, and the replay goes on with the next frame. Everything else that uses the
// queue or the swapchain first waits for that present to be done (presenter_wait), or holds the queue lock for its
// submit (vkQueueSubmit and vkQueuePresentKHR on one queue must not overlap).
// Without frame generation, the game frame's repeat (its second refresh at 30 fps, present_vsync_paced) is handed over
// the same way: presented from the replay thread it waited ~16 ms for the frame before on the GPU (26 fps at 1080 rows
// on the S20 FE, the GPU idle meanwhile and clocked down to 400 MHz by the system).
// NX_PRESENT_THREAD=1/0: on/off (default: on Android and on macOS).
struct PendingPresent {
    VkFence before = VK_NULL_HANDLE;  // the image queued before it done on the GPU (g_gen_fence), or none
    VkSemaphore rendered = VK_NULL_HANDLE;
    uint32_t index = 0;
    bool timing = false;
    VkPresentTimeGOOGLE when{};
};
std::mutex g_pres_mx;
std::condition_variable g_pres_cv;
bool g_pres_pending = false, g_pres_done = true;
PendingPresent g_pres_job;
VkResult g_pres_result = VK_SUCCESS;
// Signalled once the generated image queued before a game frame is done on the GPU: the presenter waits for it without
// the queue lock, so that its vkQueuePresentKHR (throttled on that image) then holds the lock for a moment only, not
// while the GPU finishes (the replay thread's next submit waited ~5 ms a frame for the lock).
VkFence g_gen_fence = VK_NULL_HANDLE;
bool g_gen_fence_used = false;

bool presenter_wanted() {
    static const bool on = [] {
        const char* e = std::getenv("NX_PRESENT_THREAD");
        if (e) return e[0] == '1';
#if defined(__ANDROID__)
        return true;
#elif defined(__APPLE__)
        // MoltenVK: the present waits for the next drawable; off the replay thread the north walk's lowest windows on
        // a warm M1 Air went from 53-56 to 57-59 fps.
        return true;
#else
        return false;
#endif
    }();
    return on;
}

void presenter_main() {
    plat::set_thread_name("vk presenter");
    for (;;) {
        PendingPresent job;
        {
            std::unique_lock lk(g_pres_mx);
            g_pres_cv.wait(lk, [] { return g_pres_pending; });
            job = g_pres_job;
            g_pres_pending = false;
        }
        plat::frame_helper_tick();
        if (job.before) {  // reset once signalled (a lost device: left as it is, nothing waits on it again)
            VkResult w;
            while ((w = p_vkWaitForFences(g_device, 1, &job.before, VK_TRUE, 1'000'000'000ull)) == VK_TIMEOUT) {}
            if (w == VK_SUCCESS) p_vkResetFences(g_device, 1, &job.before);
        }
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &job.rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &g_swapchain;  // not re-made while a present is pending (presenter_wait first)
        pi.pImageIndices = &job.index;
        VkPresentTimesInfoGOOGLE times{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
        if (job.timing) times.swapchainCount = 1, times.pTimes = &job.when, pi.pNext = &times;
        VkResult r;
        {
            std::lock_guard q(g_queue_mx);
            r = p_vkQueuePresentKHR(g_queue, &pi);
        }
        std::lock_guard lk(g_pres_mx);
        g_pres_result = r;
        g_pres_done = true;
        g_pres_cv.notify_all();
    }
}

void after_present(VkResult r, bool real);

// The handed-off present done. presenter_idle: only that (the swapchain is about to be re-made); presenter_wait: its
// result acted on too. Before anything else touches the swapchain, or the queue without the queue lock.
bool g_pres_unreaped = false;  // replay thread only
VkResult presenter_idle() {
    if (!g_pres_unreaped) return VK_SUCCESS;
    g_pres_unreaped = false;
    std::unique_lock lk(g_pres_mx);
    g_pres_cv.wait(lk, [] { return g_pres_done && !g_pres_pending; });
    return g_pres_result;
}

void presenter_wait() {
    if (!g_pres_unreaped) return;
    after_present(presenter_idle(), true);
}

void presenter_post(const PendingPresent& job) {
    static std::once_flag started;
    std::call_once(started, [] { std::thread(presenter_main).detach(); log("nvn/vk: game frames presented from their own thread (NX_PRESENT_THREAD)"); });
    g_pres_unreaped = true;
    std::lock_guard lk(g_pres_mx);
    g_pres_job = job;
    g_pres_pending = true;
    g_pres_done = false;
    g_pres_cv.notify_all();
}

// Where a present's time goes (acquire, recording, submit, vblank hold, vkQueuePresentKHR), logged every 600 presents.
struct PresentParts {
    uint64_t last = plat::monotonic_ns(), ns[5] = {};
    void mark(int k) {
        uint64_t now = plat::monotonic_ns();
        ns[k] = now - last, last = now;
    }
    void done(bool real) {
        static uint64_t sum[2][5], n[2];
        for (int k = 0; k < 5; k++) sum[real][k] += ns[k];
        if (++n[real] % 600) return;
        log("nvn/vk: present (%s, ms each): acquire %.2f, record %.2f, submit %.2f, vblank hold %.2f, queue present %.2f",
            real ? "game frames" : "generated", sum[real][0] / 6e8, sum[real][1] / 6e8, sum[real][2] / 6e8,
            sum[real][3] / 6e8, sum[real][4] / 6e8);
        for (auto& v : sum[real]) v = 0;
    }
};

// One image to the swapchain: acquired, `record(cb, real)` (recorded first; returns the image to show, and whether it
// is one of the game's frames: those are counted and dumped), the present pass or the blit of it, the settings screen,
// the submit and the present.
template <class Record>
void present_image(const int32_t* crop, Record&& record) {
    presenter_wait();
    if (!g_swapchain) return;
    uint32_t index = 0;
    VkSemaphore acquired = async_gpu() ? frame_acquire_semaphore() : g_acquired;
    PresentParts parts;
    VkResult r = p_vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        make_swapchain();
        return;
    }
    if (!check(r, "vkAcquireNextImageKHR") || !begin()) return;
    parts.mark(0);
    gpu_busy_note_pass("present (frame generation, scaling, overlay)");  // NX_GPU_PROFILE=3: its own label
    bool real = true;
    GpuImage* src = record(g_cb, real);
    GpuImage dst;
    dst.image = g_sc_images[index];
    dst.format = g_sc_format;
    if (src) {
        if (src->layout == VK_IMAGE_LAYOUT_UNDEFINED) {  // never rendered: dark grey, to tell it from a failure
            transition(g_cb, *src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue v{{0.08f, 0.08f, 0.1f, 1.0f}};
            VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            p_vkCmdClearColorImage(g_cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rr);
        }
    }
    bool drawn = src && present_pass_record(g_cb, *src, crop, index);  // nvn_present.cpp: leaves dst PRESENT_SRC
    if (src && !drawn) transition(g_cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (!drawn) transition(g_cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (src && !drawn) {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(src->width), int32_t(src->height), 1};
        if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] + crop[2] <= int32_t(src->width) &&
            crop[1] + crop[3] <= int32_t(src->height))  // nvnWindowSetCrop
            blit.srcOffsets[0] = {crop[0], crop[1], 0}, blit.srcOffsets[1] = {crop[0] + crop[2], crop[1] + crop[3], 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        // Fit with bars, as the present pass does (nvn_present.cpp): a picture of another shape than the window keeps
        // its own (a fixed aspect ratio setting, and while the window is being resized).
        const float cw = float(blit.srcOffsets[1].x - blit.srcOffsets[0].x), ch = float(blit.srcOffsets[1].y - blit.srcOffsets[0].y);
        const float tw = float(g_sc_extent.width), th = float(g_sc_extent.height), fit = std::min(tw / cw, th / ch);
        int32_t bw = std::min(int32_t(tw), int32_t(cw * fit + 0.5f)), bh = std::min(int32_t(th), int32_t(ch * fit + 0.5f));
        int32_t bx = (int32_t(tw) - bw) / 2, by = (int32_t(th) - bh) / 2;
        blit.dstOffsets[0] = {bx, by, 0};
        blit.dstOffsets[1] = {bx + bw, by + bh, 1};
        if (bw < int32_t(tw) || bh < int32_t(th)) {
            VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
            VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            p_vkCmdClearColorImage(g_cb, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
        }
        plat::set_picture_box(float(bx) / tw, float(by) / th, float(bw) / tw, float(bh) / th);  // mouse clicks (hle_hid.cpp)
        p_vkCmdBlitImage(g_cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    if (!drawn) transition(g_cb, dst, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    ui::ui_record(g_cb, dst.image, g_sc_format, g_sc_extent);  // the settings screen over the picture, when open
    static uint64_t presents = 0;  // the game's frames (not the generated ones)
    if (real) {
        presents++;
        if (presents % (presents < 300 ? 60 : 300) == 0) draw_log_stats();
        if (presents % 60 == 0) log_timing(60), gpu_profile_log(60);
        if (presents % 300 == 0) zero_draws_log();
        frame_sig_flush(presents);
    }
    const bool due = real && frame_dump_due(presents);
    // The game's boot shows black frames for a while: every other one is read back until one has something on it. A
    // window held back for that (plat::picture_shown) appears then, and the startup hint follows (ui.cpp).
    static bool picture = false;
    static uint64_t first_ns = 0;
    const bool probe = real && !picture && src && presents % 2 == 0;
    bool dump = src && (due || probe);
    if (dump) transition(g_cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);  // the present pass left it sampled
    dump = dump && frame_dump_record(g_cb, *src);
    VkSemaphore rendered = async_gpu() ? frame_rendered_semaphore(index) : g_rendered;
    parts.mark(1);
    if (async_gpu())  // the blit's fence is not waited for (vkQueuePresentKHR waits on `rendered`); a dumped frame is
        frame_submit(acquired, rendered, {}, dump);
    else
        end_and_wait(acquired, rendered);
    // Handed to the presenter thread: game frames, and on Android without frame generation their repeats too.
#if defined(__ANDROID__)
    const bool handed = presenter_wanted() && (real || !framegen_wanted()), fence_own = handed && real;
#else
    const bool handed = presenter_wanted() && real, fence_own = false;
#endif
    // A fence behind the generated image (see g_gen_fence); without frame generation behind the game frame, for its
    // repeat: the presenter waits for the frame's GPU work without the queue lock before presenting the repeat, whose
    // vkQueuePresentKHR (throttled on that frame) held the lock ~10 ms while the replay's submits waited (27 fps).
    if (presenter_wanted() && (framegen_wanted() ? !real : fence_own)) {
        if (!g_gen_fence) {
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            p_vkCreateFence(g_device, &fci, nullptr, &g_gen_fence);
        } else if (g_gen_fence_used) {  // the last one's game frame was not presented (minimized): not reset yet
            p_vkWaitForFences(g_device, 1, &g_gen_fence, VK_TRUE, UINT64_MAX);
            p_vkResetFences(g_device, 1, &g_gen_fence);
        }
        std::lock_guard q(g_queue_mx);
        g_gen_fence_used = g_gen_fence && p_vkQueueSubmit(g_queue, 0, nullptr, g_gen_fence) == VK_SUCCESS;
    }
    parts.mark(2);
    if (dump && due) frame_dump_write(*src, presents);
    if (due) debug_dump_textures(presents);
    bool shown_now = false;
    if (real && !picture) {
        if (!first_ns) first_ns = plat::monotonic_ns();
        // Not every format can be read back; after a minute the window shows whatever it holds.
        picture = (dump && probe && frame_readback_lit(*src)) || plat::monotonic_ns() - first_ns > 60'000'000'000ull;
        shown_now = picture;
    }
    VkPresentTimeGOOGLE when{uint32_t(display_timing_next(real)), 0};
    if (real) vblank_hold();  // on the display's vblank its interval asks for (hle_vi.cpp)
    parts.mark(3);
    if (handed) {
        // The fence is for the image after this one (the repeat) when it is this frame's own.
        const bool give = g_gen_fence_used && !(fence_own && !framegen_wanted());
        presenter_post({give ? g_gen_fence : VK_NULL_HANDLE, rendered, index, bool(g_display_timing), when});
        if (give) g_gen_fence_used = false;
    } else {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &g_swapchain;
        pi.pImageIndices = &index;
        VkPresentTimesInfoGOOGLE times{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
        if (g_display_timing) times.swapchainCount = 1, times.pTimes = &when, pi.pNext = &times;
        std::lock_guard q(g_queue_mx);
        r = p_vkQueuePresentKHR(g_queue, &pi);
    }
    parts.mark(4);
    parts.done(real);
    if (real) plat::frame_presented();  // cores, CPU time, present intervals (frame_stats.cpp)
    if (shown_now) {
        log("nvn/vk: first picture at present %llu", (unsigned long long)presents);
        plat::picture_shown();
        ui::ui_picture_shown();
    }
    if (!g_pres_unreaped) after_present(r, real);  // else when it is done (presenter_wait)
}

void after_present(VkResult r, bool real) {
    if (g_display_timing && real) display_timing_log();
    // SUBOPTIMAL re-makes the swapchain only when the surface size changed: Android returns it for every present when
    // the swapchain's transform is not the display's (a Galaxy S23 did for Hades), a rebuild per frame.
    bool remake = r == VK_ERROR_OUT_OF_DATE_KHR;
    if (r == VK_SUBOPTIMAL_KHR) {  // compared with what the swapchain was made from (its extent may be turned)
        VkSurfaceCapabilitiesKHR caps{};
        p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physical, g_surface, &caps);
        remake = caps.currentExtent.width != g_sc_current.width || caps.currentExtent.height != g_sc_current.height ||
                 (present_pass_wanted() && !present_pass_failed() && caps.currentTransform != g_sc_transform);
    }
    if (remake) make_swapchain();
}

}  // namespace

uint64_t g_replay_presents = 0;

// Frame pacing on the display's own vsync (with VSync, FIFO): each of the game's frames is presented for as many
// refreshes as its present interval asks (interval x refresh / 60: twice at 30 fps on a 60 Hz display), and the FIFO
// queue holds the replay, and through the two present slots the game, to that rhythm. Paced by a timer instead
// (pace_present), frames became ready at times that wandered against the display's refresh: some were shown for three
// refreshes and the next for one (50 ms then 17 ms), a judder at a steady 30 fps. NX_VSYNC_PACING=0: the timer.
bool present_vsync_paced() {
    static const bool off = std::getenv("NX_VSYNC_PACING") && std::getenv("NX_VSYNC_PACING")[0] == '0';
    return !off && g_fifo.load() && !framegen_wanted() && !plat::headless();
}

void gpu_present(uint64_t texture, const int32_t* crop, int interval) {
    plat::perf_hint_thread();  // the thread that presents takes part in the frame (perf_hint.cpp)
    std::unique_lock lk(g_gpu);
    framegen_update();  // the settings screen's choice, the same for all of this frame's presents (nvn_framegen.cpp)
    g_replay_presents++;
    {  // this frame's replay work and uploads, from the window's running sums (log_timing clears them)
        static uint64_t work0 = 0, up0 = 0, pres0 = 0, fence0 = 0;
        uint64_t work = g_timing.replay_ns + g_timing.encode_ns + g_timing.fence_ns, up = g_timing.upload_bytes;
        uint64_t pres = g_timing.present_ns, fence = g_timing.fence_ns;
        present_hitch_check(work >= work0 ? work - work0 : work, up >= up0 ? up - up0 : up,
                            pres >= pres0 ? pres - pres0 : pres, fence >= fence0 ? fence - fence0 : fence);
        pres0 = pres, fence0 = fence;
        plat::frame_replay_work(int64_t(work >= work0 ? work - work0 : work));  // ADPF and the thermal governor
        work0 = work, up0 = up;
    }
    PresentTimer timer;
    drain_objects();  // under the GPU lock, as in gpu_submit
    if (!init()) return;
    if (!async_gpu() || pipelined()) free_retired_images(async_gpu() ? frame_completed() : UINT64_MAX);
    if (plat::headless()) return present_headless(texture);
    if (plat::window_minimized()) return;
    if (plat::window_generation() != g_window_gen) {  // Android: a new window after home or the lock screen
        presenter_idle();
        if (async_gpu()) frame_wait_all();
        p_vkDeviceWaitIdle(g_device);
        present_pass_release();
        ui::ui_swapchain_changed();
        if (g_swapchain) p_vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
        g_swapchain = VK_NULL_HANDLE;
        log("nvn/vk: new window: surface and swapchain re-made");
        if (!make_surface()) return;
    }
    if (!g_swapchain && !make_swapchain()) return;
    GpuImage* src = image_of(texture);
    int32_t src_crop[4];  // the game's crop, on the window texture's size (a resized screen target, nvn_ultrawide.cpp)
    if (crop && src) uw_crop(*src, crop, src_crop), crop = src_crop;
    // NX_FRAMEGEN (nvn_framegen.cpp): the frame halfway between the last one and this is shown now, this one half a
    // game frame later. The GPU lock is let go meanwhile: the game's next frame is replayed after it.
    if (framegen_wanted() && src && src->layout != VK_IMAGE_LAYOUT_UNDEFINED) {
        bool generate = framegen_frame_begin(plat::monotonic_ns());
        GpuImage* gen = nullptr;
        present_image(crop, [&](VkCommandBuffer cb, bool& real) {
            gen = framegen_record(cb, texture, *src, generate, nullptr);
            real = !gen;
            return gen ? gen : src;
        });
        if (!gen) return;
        // FIFO (VSync): the real frame is queued right behind the generated one and the display shows them a refresh
        // apart, by itself. The half-frame sleep that timed it made the replay thread wait (replay + delay per game
        // frame: ~21 fps where the replay took 29 ms on a Snapdragon 865). Without FIFO, or NX_FG_SLEEP=1: the sleep.
        static const bool sleep_env = std::getenv("NX_FG_SLEEP") && std::getenv("NX_FG_SLEEP")[0] == '1';
        if (sleep_env || !g_fifo.load()) {
            uint64_t at = plat::monotonic_ns() + framegen_real_delay_ns(), slept = plat::monotonic_ns();
            lk.unlock();
            if (uint64_t now = plat::monotonic_ns(); at > now) plat::sleep_ns(at - now);
            lk.lock();
            g_timing.present_ns -= std::min(g_timing.present_ns, plat::monotonic_ns() - slept);
        }
        if (plat::window_minimized() || !g_swapchain) return;
        present_image(crop, [&](VkCommandBuffer, bool&) { return src; });  // not replayed into meanwhile (same thread)
        return;
    }
    present_image(crop, [&](VkCommandBuffer, bool&) { return src; });
    if (present_vsync_paced() && interval > 0) {
        const int repeats = std::max(1, int(std::lround(double(interval) * plat::display_refresh_hz() / 60.0))) - 1;
        for (int k = 0; k < repeats && g_swapchain && !plat::window_minimized(); k++)
            present_image(crop, [&](VkCommandBuffer, bool& real) {
                real = false;
                return src;
            });
    }
}

}  // namespace nx::nvn
