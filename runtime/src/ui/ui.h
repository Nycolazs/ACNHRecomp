// The settings screen: Dear ImGui drawn over the presented picture (ui.cpp), with the pages in settings.cpp.
//
// Threads: the platform's main thread owns SDL events and the pad; the presenter draws. Events and pad edges are
// queued by the main thread (ui_event, ui_pad) and turned into ImGui input on the presenter's thread (ui_record), so
// ImGui is only ever touched by one thread.
#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

union SDL_Event;

namespace nx::ui {

// Presenter (nvn_vk.cpp), its thread.
void ui_vulkan_ready(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                     uint32_t family, VkQueue queue, bool dynamic_rendering);
// After the frame reached the swapchain image (left in PRESENT_SRC_KHR), before the command buffer is submitted.
void ui_set_rotation(int quarter_turns);  // the swapchain's preTransform, as quarter turns (Android)
void ui_record(VkCommandBuffer cb, VkImage image, VkFormat format, VkExtent2D extent);
void ui_picture_shown();      // the game's first frame with something on it was presented (the startup hint follows)
void ui_swapchain_changed();  // the swapchain's images were destroyed: drop their views
void ui_shutdown();

// Platform, main thread.
void ui_event(const SDL_Event& e);               // every SDL event (keys, mouse, text, gamepad)
// The pad as the game would see it this tick (NpadButton bits, sticks): the menu's toggle combo and navigation.
// Returns true when the menu has the input: the game must then see a neutral pad.
bool ui_pad(uint64_t buttons, int32_t lx, int32_t ly);
bool ui_visible();
// A text field has the keyboard: its keys type there and must not drive the pad (Shift is B, which would close the menu).
bool ui_text_field();
void ui_toggle();
void ui_open_online();  // the settings screen, on its Online page: from any thread
// Touch screens (Android): one finger as the mouse, in pixels of the presented image. Returns true when the menu
// took the touch (it is open): the game must not see it.
bool ui_touch(float x, float y, bool down);

}  // namespace nx::ui
