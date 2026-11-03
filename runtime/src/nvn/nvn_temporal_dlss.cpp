// NVIDIA DLSS for temporal anti-aliasing (video.temporal = dlaa, nvn_temporal.cpp): DLSS Super Resolution at the
// picture's own size (DLAA), its transformer model (preset K, or video.dlss_preset). NVIDIA's DLSS SDK (NGX, MIT-style
// licence; private/third_party/dlss): its static library here, nvngx_dlss.dll beside the exe (copied by the build;
// NVIDIA's driver may load a newer one of its own). NGX needs instance and device extensions of its own, enabled
// when the device is made (dlss_vk_extensions, nvn_vk.cpp). RTX GPUs only; anywhere else dlss_init says so.
#include "nvn_temporal.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "config.h"
#include "platform.h"

#if defined(_WIN32) && defined(NX_DLSS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_vk.h"
#endif

namespace nx::nvn {

using namespace nx::vk;

#if defined(_WIN32) && defined(NX_DLSS)

namespace {

struct State {
    bool ready = false;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    uint32_t w = 0, h = 0;
    std::wstring dir;  // the exe's folder, where nvngx_dlss.dll is
} g;

std::wstring exe_dir() {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d(exe, n);
    d.resize(d.find_last_of(L"\\/") + 1);
    return d;
}

NVSDK_NGX_Resource_VK resource(VkImageView view, const GpuImage& img, VkImageAspectFlags aspect, bool rw) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(view, img.image, VkImageSubresourceRange{aspect, 0, 1, 0, 1}, img.format,
                                                  img.width, img.height, rw);
}

// video.dlss_model: 4.5 (the second-generation transformer, preset M; the default) or 4 (the first, preset K).
// video.dlss_preset / NX_DLSS_PRESET: a letter of NVIDIA's render presets instead (J, K, L, M, ...).
uint32_t preset() {
    std::string p = config_get("video.dlss_preset", "");
    if (const char* e = std::getenv("NX_DLSS_PRESET")) p = e;
    if (p.size() == 1 && p[0] >= 'A' && p[0] <= 'O') return uint32_t(p[0] - 'A' + 1);
    return config_get("video.dlss_model", "4.5") == "4" ? uint32_t('K' - 'A' + 1) : uint32_t('M' - 'A' + 1);
}

}  // namespace

void dlss_vk_extensions(std::vector<std::string>& instance, std::vector<std::string>& device) {
    unsigned int ni = 0, nd = 0;
    const char** ie = nullptr;
    const char** de = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_RequiredExtensions(&ni, &ie, &nd, &de))) return;
    for (unsigned int k = 0; k < ni; k++) instance.emplace_back(ie[k]);
    for (unsigned int k = 0; k < nd; k++) device.emplace_back(de[k]);
}

bool dlss_init(std::string& version) {
    if (g.ready) return true;
    static bool tried = false;
    if (tried) return false;
    tried = true;
    g.dir = exe_dir();
    const std::string logs = plat::data_root() + "/logs";
    std::wstring wlogs(logs.begin(), logs.end());
    const wchar_t* paths[] = {g.dir.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths, info.PathListInfo.Length = 1;
    info.LoggingInfo.MinimumLoggingLevel = std::getenv("NX_DLSS_LOG") ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_OFF;
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(plat::vulkan_get_instance_proc_addr());
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(vk_instance(), "vkGetDeviceProcAddr"));
    // A GUID-shaped project ID of our own (anything else: InvalidParameter).
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID("a3c1e7d2-5b4f-4e8a-9c6d-2f1b0e7a4c93",
                                                              NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", wlogs.c_str(),
                                                              vk_instance(), vk_physical(), vk_device(), gipa, gdpa,
                                                              &info);
    if (NVSDK_NGX_FAILED(r)) {
        log("nvn/dlss: NGX could not start (%#x): not an RTX GPU, or its driver is too old", unsigned(r));
        return false;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&g.params))) {
        log("nvn/dlss: no NGX capability parameters");
        return false;
    }
    int available = 0, driver = 0;
    NVSDK_NGX_Parameter_GetI(g.params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(g.params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &driver);
    if (!available) {
        log("nvn/dlss: DLSS is not available here%s (nvngx_dlss.dll beside the exe? an RTX GPU?)",
            driver ? ": the driver needs an update" : "");
        return false;
    }
    version = "DLSS (SDK 310.9.16)";
    g.ready = true;
    return true;
}

bool dlss_create(VkCommandBuffer cb, uint32_t w, uint32_t h, uint32_t dw, uint32_t dh) {
    dlss_destroy();
    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = w, cp.Feature.InHeight = h, cp.Feature.InTargetWidth = dw, cp.Feature.InTargetHeight = dh;
    // DLAA at the picture's size; the mode whose ratio the scene was made at when upscaling (video.upscale).
    const float r = float(w) / float(dw);
    cp.Feature.InPerfQualityValue = r > 0.99f   ? NVSDK_NGX_PerfQuality_Value_DLAA
                                    : r > 0.62f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                                    : r > 0.55f ? NVSDK_NGX_PerfQuality_Value_Balanced
                                    : r > 0.4f  ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                                                : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    // Linear light before the tonemap (its exposure the game's own: DLSS measures one), motion vectors at the scene's
    // size and without the jitter, depth 0 near.
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    for (const char* mode : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance})
        NVSDK_NGX_Parameter_SetUI(g.params, mode, preset());
    if (NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT(cb, 1, 1, &g.feature, g.params, &cp); NVSDK_NGX_FAILED(r)) {
        log("nvn/dlss: the DLSS feature could not be made (%#x) at %ux%u -> %ux%u", unsigned(r), w, h, dw, dh);
        g.feature = nullptr;
        return false;
    }
    g.w = w, g.h = h;
    return true;
}

void dlss_destroy() {
    if (!g.feature) return;
    NVSDK_NGX_VULKAN_ReleaseFeature(g.feature);
    g.feature = nullptr;
    g.w = g.h = 0;
}

bool dlss_evaluate(const TemporalInputs& in) {
    if (!g.feature) return false;
    NVSDK_NGX_Resource_VK color = resource(in.color->view, *in.color, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK out = resource(in.out->view, *in.out, VK_IMAGE_ASPECT_COLOR_BIT, true);
    NVSDK_NGX_Resource_VK depth = resource(in.depth_view, *in.depth, VK_IMAGE_ASPECT_DEPTH_BIT, false);
    NVSDK_NGX_Resource_VK mv = resource(in.mv->view, *in.mv, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_VK_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = &color, ep.Feature.pInOutput = &out;
    ep.pInDepth = &depth, ep.pInMotionVectors = &mv;
    NVSDK_NGX_Resource_VK bias{};
    if (in.reactive) {
        bias = resource(in.reactive->view, *in.reactive, VK_IMAGE_ASPECT_COLOR_BIT, false);
        ep.pInBiasCurrentColorMask = &bias;
    }
    // NX_TAA_JITTER_SIGN=-1 (bisecting a wobble at rest): the jitter DLSS is told, mirrored.
    static const float js = std::getenv("NX_TAA_JITTER_SIGN") ? float(std::atof(std::getenv("NX_TAA_JITTER_SIGN"))) : 1.0f;
    ep.InJitterOffsetX = js * in.jx, ep.InJitterOffsetY = js * in.jy;
    ep.InRenderSubrectDimensions = {g.w, g.h};
    ep.InReset = in.reset ? 1 : 0;
    ep.InMVScaleX = float(g.w), ep.InMVScaleY = float(g.h);  // UV -> pixels
    ep.InPreExposure = 1.0f, ep.InExposureScale = 1.0f;
    ep.InFrameTimeDeltaInMsec = in.frame_ms;
    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(in.cb, g.feature, g.params, &ep);
    if (NVSDK_NGX_FAILED(r)) {
        static int n = 0;
        if (n++ < 5) log("nvn/dlss: evaluate failed (%#x)", unsigned(r));
        return false;
    }
    return true;
}

#else

void dlss_vk_extensions(std::vector<std::string>&, std::vector<std::string>&) {}
bool dlss_init(std::string&) { return false; }
bool dlss_create(VkCommandBuffer, uint32_t, uint32_t) { return false; }
void dlss_destroy() {}
bool dlss_evaluate(const TemporalInputs&) { return false; }

#endif

}  // namespace nx::nvn
