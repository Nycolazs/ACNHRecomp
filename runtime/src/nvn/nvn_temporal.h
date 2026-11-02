// Temporal anti-aliasing's backends (nvn_temporal.cpp): what the shared part (jitter, camera, depth, motion vectors)
// hands each of them. FSR is in nvn_temporal.cpp itself; DLSS in nvn_temporal_dlss.cpp.
#pragma once

#include <string>
#include <vector>

#include "nvn_vk.h"

namespace nx::nvn {

// One frame's inputs, all w x h, in these layouts: color, depth and motion vectors SHADER_READ_ONLY_OPTIMAL (depth
// through depth_view, its depth aspect; motion vectors RG16F in UV, the last frame's minus this one's, without the
// jitter), out RGBA16F in GENERAL.
struct TemporalInputs {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    GpuImage* color = nullptr;
    GpuImage* depth = nullptr;
    VkImageView depth_view = VK_NULL_HANDLE;
    GpuImage* mv = nullptr;
    GpuImage* out = nullptr;
    GpuImage* reactive = nullptr;  // R8: translucent effects' cover (FSR's reactive mask, DLSS's current-colour bias)
    float jx = 0, jy = 0;  // the jitter in pixels: how far the picture was moved (x right, y down)
    bool reset = false;
    float frame_ms = 33.3f;
};

// NVIDIA DLSS (nvn_temporal_dlss.cpp; NX_DLSS builds on Windows): DLAA, DLSS at the picture's own size.
void dlss_vk_extensions(std::vector<std::string>& instance, std::vector<std::string>& device);  // before they are made
bool dlss_init(std::string& version);  // NGX on the device; false: no DLSS here (the log says why)
bool dlss_create(VkCommandBuffer cb, uint32_t w, uint32_t h, uint32_t dw, uint32_t dh);  // w x h in, dw x dh out
void dlss_destroy();
bool dlss_evaluate(const TemporalInputs& in);

}  // namespace nx::nvn
