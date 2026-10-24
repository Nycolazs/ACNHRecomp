// Builds without Dear ImGui (or a platform without SDL3 yet): no settings screen; config.ini still applies.
#include "ui/ui.h"

namespace nx::ui {

void ui_vulkan_ready(PFN_vkGetInstanceProcAddr, VkInstance, VkPhysicalDevice, VkDevice, uint32_t, VkQueue, bool) {}
void ui_record(VkCommandBuffer, VkImage, VkFormat, VkExtent2D) {}
void ui_swapchain_changed() {}
void ui_shutdown() {}
void ui_event(const SDL_Event&) {}
bool ui_pad(uint64_t, int32_t, int32_t) { return false; }
bool ui_visible() { return false; }
void ui_toggle() {}
bool ui_touch(float, float, bool) { return false; }

}  // namespace nx::ui
