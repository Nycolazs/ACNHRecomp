// The settings screen's frame: Dear ImGui over the presented picture (ui.h). The pages are in settings.cpp.
//
// Drawn with dynamic rendering straight into the swapchain image after the game's frame was blitted there, so it
// costs nothing while closed and needs no extra image. Input does not go through ImGui's SDL backend (it calls SDL
// from the thread that draws, and SDL wants its video calls on the main thread on macOS): the main thread queues
// what happened and this file feeds ImGui on the presenter's thread.
//
// Size: everything is laid out in "design pixels" for a 1280x720 window and scaled by the window's size, so the
// same screen reads the same on a 13" laptop, a 4K monitor or (later) a phone; below an aspect of 1.2 the pages
// stack vertically (settings.cpp).
#include "ui/ui.h"

#if !defined(__ANDROID__)
#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

#include <nx/runtime.h>

#include "config.h"
#include "platform.h"
#include "ui/i18n.h"
#include "ui/settings.h"
#include "ui/theme.h"
#include "ui/ui_internal.h"

namespace nx::ui {

namespace {

// ---- input queue (main thread -> presenter) -------------------------------------------------------------------

struct Input {
    // MouseAt: x, y as fractions of the window (SDL's mouse is in points; the screen's pixels per point differ from
    // one display to another, and the window can move between them), made pixels of the UI's image when fed.
    enum Kind { Key, Char, MousePos, MouseAt, MouseButton, Wheel, Focus } kind;
    int key = 0;  // ImGuiKey or mouse button
    bool down = false;
    float x = 0, y = 0;
    char text[32] = {};
};

std::mutex g_in_lock;
std::deque<Input> g_in;
std::atomic<bool> g_visible{std::getenv("NX_UI_OPEN") != nullptr};  // NX_UI_OPEN=1: open from the start (tests)
std::atomic<bool> g_first_hint{true};  // the "F1: settings" hint at startup
std::atomic<int64_t> g_picture_ns{0};  // when the game's first picture was shown: the hint's clock starts there
std::atomic<bool> g_capture{false};     // a key binding is being chosen (settings.cpp)
std::atomic<int> g_captured{0};
std::atomic<bool> g_text_field{false};  // a text field has the keyboard (ui_text_field)

void push(const Input& in) {
    std::lock_guard lk(g_in_lock);
    if (g_in.size() < 512) g_in.push_back(in);
}

#if !defined(__ANDROID__)
Input mouse_at(SDL_WindowID id, float x, float y) {
    int w = 0, h = 0;
    SDL_Window* win = SDL_GetWindowFromID(id);
    if (!win || !SDL_GetWindowSize(win, &w, &h) || w <= 0 || h <= 0) return {Input::MousePos, 0, false, x, y};
    return {Input::MouseAt, 0, false, x / float(w), y / float(h)};
}

ImGuiKey imgui_key(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
        case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
        case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
        case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
        case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
        case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
        case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
        case SDL_SCANCODE_HOME: return ImGuiKey_Home;
        case SDL_SCANCODE_END: return ImGuiKey_End;
        case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
        case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
        case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
        case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
        case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
        case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
        case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
        case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
        case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
        case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
        case SDL_SCANCODE_A: return ImGuiKey_A;
        case SDL_SCANCODE_C: return ImGuiKey_C;
        case SDL_SCANCODE_V: return ImGuiKey_V;
        case SDL_SCANCODE_X: return ImGuiKey_X;
        default: return ImGuiKey_None;
    }
}
#endif

// ---- Vulkan ---------------------------------------------------------------------------------------------------

struct Vk {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdBeginRendering CmdBeginRendering = nullptr;
    PFN_vkCmdEndRendering CmdEndRendering = nullptr;
    // Without dynamic rendering (the Adreno 6xx's own Vulkan 1.1 driver): a render pass and a framebuffer per image.
    PFN_vkCreateRenderPass CreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
    PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
    bool dynamic = true;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
} g_vk;

bool g_vk_ok = false;           // functions loaded
bool g_imgui_ok = false;        // ImGui's Vulkan backend initialized
VkFormat g_format = VK_FORMAT_UNDEFINED;
std::map<VkImage, VkImageView> g_views;
VkRenderPass g_pass = VK_NULL_HANDLE;                   // render pass mode: loads and stores the swapchain image
std::map<VkImage, std::pair<VkFramebuffer, VkExtent2D>> g_framebuffers;

VkRenderPass make_pass(VkFormat format) {
    VkAttachmentDescription a{};
    a.format = format;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1;
    ci.pAttachments = &a;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    VkRenderPass rp = VK_NULL_HANDLE;
    g_vk.CreateRenderPass(g_vk.device, &ci, nullptr, &rp);
    return rp;
}

PFN_vkVoidFunction load(const char* name, void* user) {
    (void)user;
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(g_vk.gipa(g_vk.instance, "vkGetDeviceProcAddr"));
    if (PFN_vkVoidFunction f = gdpa ? gdpa(g_vk.device, name) : nullptr) return f;
    return g_vk.gipa(g_vk.instance, name);
}

template <typename F>
F dev_fn(const char* name) {
    return reinterpret_cast<F>(load(name, nullptr));
}

VkImageView view_for(VkImage image, VkFormat format) {
    auto it = g_views.find(image);
    if (it != g_views.end()) return it->second;
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = image;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = format;
    ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (g_vk.CreateImageView(g_vk.device, &ci, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    g_views[image] = v;
    return v;
}

void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
             VkAccessFlags dst, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from, b.newLayout = to;
    b.srcAccessMask = src, b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    g_vk.CmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ---- fonts and style (theme.h) --------------------------------------------------------------------------------

float g_font_scale = 0;
std::string g_font_lang;
ImFont* g_font_body = nullptr;
ImFont* g_font_title = nullptr;

void build_fonts(float scale) {
    ThemeFonts f = theme_build_fonts(scale, plat::data_root() + "/fonts/ui.ttf");  // a font of the player's choice
    g_font_body = f.body, g_font_title = f.title;
    g_font_scale = scale, g_font_lang = ui_language();
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    static int logged = 0;
    if (logged++ < 4) log("ui: fonts at scale %.2f: atlas %dx%d", double(scale), w, h);
    if (g_imgui_ok && w > 0 && h > 0) ImGui_ImplVulkan_CreateFontsTexture();  // never a zero-sized texture
}

bool init_imgui(VkFormat format) {
    if (!g_vk_ok) return false;
    if (g_imgui_ok && format == g_format) return true;
    if (g_imgui_ok) {
        g_vk.DeviceWaitIdle(g_vk.device);
        ImGui_ImplVulkan_Shutdown();
        g_imgui_ok = false;
    }
    if (!ImGui::GetCurrentContext()) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;  // our own config.ini keeps what matters
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        io.ConfigNavMoveSetMousePos = false;
        ui_set_language(config_get("ui.language", "auto"));
    }
    // The device's own version: on a Vulkan 1.1 device (the Adreno 6xx's driver) ImGui must not take 1.3 paths.
    const uint32_t api = g_vk.dynamic ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;
    if (!ImGui_ImplVulkan_LoadFunctions(api, load, nullptr)) {
        log("ui: Vulkan functions for ImGui missing: no settings screen");
        return false;
    }
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = api;
    ii.CheckVkResultFn = [](VkResult r) {
        if (r != VK_SUCCESS) log("ui: Vulkan call in ImGui's backend failed: %d", int(r));
    };
    ii.Instance = g_vk.instance;
    ii.PhysicalDevice = g_vk.physical;
    ii.Device = g_vk.device;
    ii.QueueFamily = g_vk.family;
    ii.Queue = g_vk.queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = 3;
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    static VkFormat fmt;
    fmt = format;
    if (g_vk.dynamic) {
        ii.UseDynamicRendering = true;
        ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &fmt;
    } else {
        if (g_pass) g_vk.DestroyRenderPass(g_vk.device, g_pass, nullptr);
        g_pass = make_pass(format);
        ii.UseDynamicRendering = false;
        ii.RenderPass = g_pass;
    }
    if (!ImGui_ImplVulkan_Init(&ii)) {
        log("ui: ImGui Vulkan backend failed: no settings screen");
        return false;
    }
    g_imgui_ok = true;
    g_format = format;
    g_font_scale = 0;  // fonts rebuilt for the new backend
    log("ui: settings screen ready (format %d)", int(format));
    return true;
}

void feed_input(ImGuiIO& io) {
    std::deque<Input> in;
    {
        std::lock_guard lk(g_in_lock);
        in.swap(g_in);
    }
    for (const Input& e : in) switch (e.kind) {
            case Input::Key: io.AddKeyEvent(ImGuiKey(e.key), e.down); break;
            case Input::Char: io.AddInputCharactersUTF8(e.text); break;
            case Input::MousePos: io.AddMousePosEvent(e.x, e.y); break;
            case Input::MouseAt: io.AddMousePosEvent(e.x * io.DisplaySize.x, e.y * io.DisplaySize.y); break;
            case Input::MouseButton: io.AddMouseButtonEvent(e.key, e.down); break;
            case Input::Wheel: io.AddMouseWheelEvent(e.x, e.y); break;
            case Input::Focus: io.AddFocusEvent(e.down); break;
        }
}

void draw_fps_overlay(float scale) {
    // The game's own frames a second (plat::game_frames: not the 30 fps repeats on a 60 Hz screen nor the generated
    // frames, which made it read 60 while the game ran at 30), and the pictures shown when frame generation adds some.
    using clock = std::chrono::steady_clock;
    static auto last = clock::now();
    static uint64_t frames0 = plat::game_frames(), shown = 0;
    static float s_fps = 0.0f, s_shown = 0.0f;
    shown++;
    const auto now = clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    if (dt >= 1.0f) {  // a second: steadier than the 0.25 s it was
        const uint64_t frames = plat::game_frames();
        s_fps = float(frames - frames0) / dt, s_shown = float(shown) / dt;
        frames0 = frames, shown = 0, last = now;
    }
    ImGuiIO& io = ImGui::GetIO();
    char buf[96];
    if (config_get("video.frame_gen", "off") == "builtin" && s_shown > s_fps * 1.5f)
        std::snprintf(buf, sizeof(buf), tr("ui.fps_generated"), s_fps, s_shown);
    else
        std::snprintf(buf, sizeof(buf), "%.1f FPS", s_fps);
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    const ImVec2 pad(10.0f * scale, 5.0f * scale);
    const float margin = 12.0f * scale;
    const ImVec2 p1(io.DisplaySize.x - margin, margin + ts.y + pad.y * 2.0f);
    const ImVec2 p0(p1.x - ts.x - pad.x * 2.0f, margin);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(20, 18, 15, 190), 8.0f * scale);
    dl->AddText(ImVec2(p0.x + pad.x, p0.y + pad.y), IM_COL32(255, 252, 240, 245), buf);
}

}  // namespace

void ui_vulkan_ready(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                     uint32_t family, VkQueue queue, bool dynamic_rendering) {
    g_vk.gipa = gipa, g_vk.instance = instance, g_vk.physical = physical, g_vk.device = device;
    g_vk.dynamic = dynamic_rendering;
    g_vk.family = family, g_vk.queue = queue;
    g_vk.CreateImageView = dev_fn<PFN_vkCreateImageView>("vkCreateImageView");
    g_vk.DestroyImageView = dev_fn<PFN_vkDestroyImageView>("vkDestroyImageView");
    g_vk.CmdPipelineBarrier = dev_fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    g_vk.CmdBeginRendering = dev_fn<PFN_vkCmdBeginRendering>("vkCmdBeginRendering");
    if (!g_vk.CmdBeginRendering) g_vk.CmdBeginRendering = dev_fn<PFN_vkCmdBeginRendering>("vkCmdBeginRenderingKHR");
    g_vk.CmdEndRendering = dev_fn<PFN_vkCmdEndRendering>("vkCmdEndRendering");
    if (!g_vk.CmdEndRendering) g_vk.CmdEndRendering = dev_fn<PFN_vkCmdEndRendering>("vkCmdEndRenderingKHR");
    g_vk.DeviceWaitIdle = dev_fn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle");
    g_vk.CreateRenderPass = dev_fn<PFN_vkCreateRenderPass>("vkCreateRenderPass");
    g_vk.DestroyRenderPass = dev_fn<PFN_vkDestroyRenderPass>("vkDestroyRenderPass");
    g_vk.CreateFramebuffer = dev_fn<PFN_vkCreateFramebuffer>("vkCreateFramebuffer");
    g_vk.DestroyFramebuffer = dev_fn<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer");
    g_vk.CmdBeginRenderPass = dev_fn<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass");
    g_vk.CmdEndRenderPass = dev_fn<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass");
    g_vk_ok = g_vk.CreateImageView && g_vk.CmdPipelineBarrier &&
              (g_vk.dynamic ? g_vk.CmdBeginRendering && g_vk.CmdEndRendering
                            : g_vk.CreateRenderPass && g_vk.CreateFramebuffer && g_vk.CmdBeginRenderPass);
    if (!g_vk_ok) log("ui: Vulkan functions missing: no settings screen");
}

// The swapchain's preTransform as quarter turns (Android: 1, the landscape picture in a portrait image). The screen is
// laid out upright and its vertices turned into the image as the present pass does (nvn_present.cpp kRot): drawn
// straight into the portrait image, it came out sideways in a corner.
std::atomic<int> g_quarter{0};
void ui_set_rotation(int quarter_turns) { g_quarter = quarter_turns & 3; }

namespace {
void rotate_draw_data(ImDrawData* dd, int r, float uw, float uh) {
    auto map = [&](float x, float y) -> ImVec2 {
        switch (r) {
            case 1: return ImVec2(uh - y, x);
            case 2: return ImVec2(uw - x, uh - y);
            case 3: return ImVec2(y, uw - x);
            default: return ImVec2(x, y);
        }
    };
    for (int n = 0; n < dd->CmdListsCount; n++) {
        ImDrawList* l = dd->CmdLists[n];
        for (ImDrawVert& v : l->VtxBuffer) v.pos = map(v.pos.x, v.pos.y);
        for (ImDrawCmd& c : l->CmdBuffer) {
            ImVec2 a = map(c.ClipRect.x, c.ClipRect.y), b = map(c.ClipRect.z, c.ClipRect.w);
            c.ClipRect = ImVec4(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y));
        }
    }
    const bool side = r == 1 || r == 3;
    dd->DisplayPos = ImVec2(0, 0);
    dd->DisplaySize = side ? ImVec2(uh, uw) : ImVec2(uw, uh);
}
}  // namespace

void ui_record(VkCommandBuffer cb, VkImage image, VkFormat format, VkExtent2D extent) {
    static bool open_checked = false;  // NX_UI_OPEN read here too: Android sets config.txt's variables after static init
    if (!open_checked) open_checked = true, g_visible = g_visible || std::getenv("NX_UI_OPEN") != nullptr;
    const int quarter = g_quarter.load();
    const bool sideways = quarter == 1 || quarter == 3;
    const float up_w = float(sideways ? extent.height : extent.width), up_h = float(sideways ? extent.width : extent.height);
    using clock = std::chrono::steady_clock;
    // The hint comes a second after the game's first picture (not over the black frames of its boot), for 8 s.
    const int64_t pic = g_picture_ns;
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
    const float hint_t = pic ? float(now_ns - pic) / 1e9f - 1.0f : -1.0f;
    if (hint_t > 8.0f) g_first_hint = false;
    const bool hint = g_first_hint && pic;
    const bool show_fps = config_show_fps();
    if ((!g_visible && !hint && !show_fps) || !extent.width || !extent.height) return;
    if (!init_imgui(format)) return;
    ImGuiIO& io = ImGui::GetIO();
    float scale = theme_scale(up_w, up_h);
    if (std::fabs(scale - g_font_scale) > 0.04f || g_font_lang != ui_language()) {
        build_fonts(scale);
        theme_apply_style(scale);
    }
    io.DisplaySize = ImVec2(up_w, up_h);
    static auto last = clock::now();
    auto now = clock::now();
    io.DeltaTime = std::clamp(std::chrono::duration<float>(now - last).count(), 1.0f / 240.0f, 0.25f);
    last = now;
    feed_input(io);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    g_text_field = io.WantTextInput;
    ImGui::PushFont(g_font_body);
    if (g_visible) {
        if (!settings_draw(scale, g_font_title)) g_visible = false;  // closed from the screen
    } else if (hint) {
        settings_hint(scale, hint_t);
    }
    if (show_fps) {
        draw_fps_overlay(scale);
    }
    ImGui::PopFont();
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->CmdListsCount == 0) return;
    if (quarter) rotate_draw_data(dd, quarter, up_w, up_h);
    VkImageView view = view_for(image, format);
    if (!view) return;
    barrier(cb, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    if (g_vk.dynamic) {
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = view;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        g_vk.CmdBeginRendering(cb, &ri);
        ImGui_ImplVulkan_RenderDrawData(dd, cb);
        g_vk.CmdEndRendering(cb);
    } else {
        auto& fb = g_framebuffers[image];
        if (fb.first && (fb.second.width != extent.width || fb.second.height != extent.height))
            g_vk.DestroyFramebuffer(g_vk.device, fb.first, nullptr), fb.first = VK_NULL_HANDLE;
        if (!fb.first) {
            VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fci.renderPass = g_pass;
            fci.attachmentCount = 1;
            fci.pAttachments = &view;
            fci.width = extent.width, fci.height = extent.height, fci.layers = 1;
            g_vk.CreateFramebuffer(g_vk.device, &fci, nullptr, &fb.first);
            fb.second = extent;
        }
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = g_pass;
        bi.framebuffer = fb.first;
        bi.renderArea = {{0, 0}, extent};
        g_vk.CmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(dd, cb);
        g_vk.CmdEndRenderPass(cb);
    }
    barrier(cb, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

void ui_swapchain_changed() {
    if (!g_vk_ok) return;
    for (auto& [image, fb] : g_framebuffers) g_vk.DestroyFramebuffer(g_vk.device, fb.first, nullptr);
    g_framebuffers.clear();
    for (auto& [image, view] : g_views) g_vk.DestroyImageView(g_vk.device, view, nullptr);
    g_views.clear();
}

void ui_shutdown() {
    if (g_imgui_ok) {
        g_vk.DeviceWaitIdle(g_vk.device);
        ImGui_ImplVulkan_Shutdown();
        g_imgui_ok = false;
    }
    ui_swapchain_changed();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
}

// ---- main thread ----------------------------------------------------------------------------------------------

#if !defined(__ANDROID__)
void ui_event(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            bool down = e.type == SDL_EVENT_KEY_DOWN;
            if (down && !e.key.repeat && g_visible && g_capture.exchange(false)) {
                g_captured = int(e.key.scancode);
                return;
            }
            if (down && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F1) {
                ui_toggle();
                return;
            }
            if (!g_visible) return;
            if (ImGuiKey k = imgui_key(e.key.scancode); k != ImGuiKey_None) push({Input::Key, k, down});
            Input mod{Input::Key, ImGuiMod_Ctrl, (e.key.mod & SDL_KMOD_CTRL) != 0};
            push(mod);
            Input sh{Input::Key, ImGuiMod_Shift, (e.key.mod & SDL_KMOD_SHIFT) != 0};
            push(sh);
            break;
        }
        case SDL_EVENT_TEXT_INPUT: {
            if (!g_visible) return;
            Input in{Input::Char};
            std::snprintf(in.text, sizeof in.text, "%s", e.text.text);
            push(in);
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            if (g_visible) push(mouse_at(e.motion.windowID, e.motion.x, e.motion.y));
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (!g_visible) return;
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            push(mouse_at(e.button.windowID, e.button.x, e.button.y));
            push({Input::MouseButton, b, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN});
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            if (g_visible) push({Input::Wheel, 0, false, e.wheel.x, e.wheel.y});
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            push({Input::Focus, 0, e.type == SDL_EVENT_WINDOW_FOCUS_GAINED});
            break;
        default: break;
    }
}

#else
void ui_event(const SDL_Event&) {}
#endif

bool ui_touch(float x, float y, bool down) {
    static bool held = false;
    if (!g_visible && !held) return false;
    push({Input::MousePos, 0, false, x, y});
    if (down != held) push({Input::MouseButton, 0, down});
    held = down;
    return true;
}

bool ui_pad(uint64_t buttons, int32_t lx, int32_t ly) {
    // NpadButton bits (platform.h): A 0, B 1, X 2, Y 3, L 6, R 7, ZL 8, ZR 9, Plus 10, Minus 11, D-pad 12..15.
    static uint64_t prev = 0;
    uint64_t pressed = buttons & ~prev, released = prev & ~buttons;
    prev = buttons;
    constexpr uint64_t kPlus = 1ull << 10, kMinus = 1ull << 11;
    if ((buttons & (kPlus | kMinus)) == (kPlus | kMinus) && (pressed & (kPlus | kMinus))) {
        ui_toggle();
        return true;
    }
    if (!g_visible) return false;
    struct Map {
        uint64_t bit;
        ImGuiKey key;
    };
    // The game's A confirms whatever controller it is (the platform already mapped the physical button to it).
    static const Map map[] = {
        {1ull << 0, ImGuiKey_GamepadFaceDown},   {1ull << 1, ImGuiKey_GamepadFaceRight},
        {1ull << 2, ImGuiKey_GamepadFaceUp},     {1ull << 3, ImGuiKey_GamepadFaceLeft},
        {1ull << 6, ImGuiKey_GamepadL1},         {1ull << 7, ImGuiKey_GamepadR1},
        {1ull << 8, ImGuiKey_GamepadL2},         {1ull << 9, ImGuiKey_GamepadR2},
        {1ull << 10, ImGuiKey_GamepadStart},    {1ull << 11, ImGuiKey_GamepadBack},
        {1ull << 12, ImGuiKey_GamepadDpadLeft},  {1ull << 13, ImGuiKey_GamepadDpadUp},
        {1ull << 14, ImGuiKey_GamepadDpadRight}, {1ull << 15, ImGuiKey_GamepadDpadDown},
    };
    for (const Map& m : map) {
        if (pressed & m.bit) push({Input::Key, m.key, true});
        if (released & m.bit) push({Input::Key, m.key, false});
    }
    // The left stick as a D-pad with hysteresis.
    static int sx = 0, sy = 0;
    auto stick = [&](int32_t v, int& state, ImGuiKey neg, ImGuiKey pos) {
        int now = v > 16000 ? 1 : v < -16000 ? -1 : (std::abs(v) < 9000 ? 0 : state);
        if (now == state) return;
        if (state < 0) push({Input::Key, neg, false});
        if (state > 0) push({Input::Key, pos, false});
        if (now < 0) push({Input::Key, neg, true});
        if (now > 0) push({Input::Key, pos, true});
        state = now;
    };
    stick(lx, sx, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight);
    stick(ly, sy, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadUp);
    return true;
}

bool ui_visible() { return g_visible; }

bool ui_text_field() { return g_visible && g_text_field; }

void ui_begin_key_capture() {
    g_captured = 0;
    g_capture = true;
}

int ui_take_captured_key() { return g_captured.exchange(0); }

void ui_picture_shown() {
    using namespace std::chrono;
    if (!g_picture_ns) g_picture_ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

void ui_open_online() {
    settings_show_online();
    g_visible = true;
}

void ui_toggle() {
    g_visible = !g_visible;
    g_first_hint = false;
    log("ui: settings %s", g_visible ? "opened" : "closed");
}

}  // namespace nx::ui
