// What the runtime needs from the host OS. platform_mac.cpp, platform_linux.cpp, platform_android.cpp and, on Windows,
// platform_win.cpp with platform_sdl.cpp implement it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

// The runtime's functions and variables the recompiled code calls (cpu.h, runtime.h, the generated funcs.h). On
// Android the game's code is its own library (lib<game>_game.so) and these are exported to it from the app's.
#if defined(__GNUC__) || defined(__clang__)
#define NX_GAME_API __attribute__((visibility("default")))
#else
#define NX_GAME_API
#endif

namespace nx {
struct ModuleInfo;
void log_flush();  // loader.cpp: the lines queued for the log file, written now
}  // namespace nx

struct SDL_Window;

namespace nx::plat {

void set_window_icon(SDL_Window* window);  // app_icon.cpp (computers)

#ifdef NX_GAME_DYNAMIC
// ---- the game's code from the game folder (game_folder.cpp on desktop, platform_android.cpp on Android) ----
const nx::ModuleInfo* game_module(const char* name);
[[noreturn]] void game_longjmp(uint64_t buf, uint64_t val);
#ifndef __ANDROID__
std::string load_game(const std::string& folder);  // "" when loaded, else why not
bool load_game_or_explain();  // the data root's; false after showing why it is not playable
#endif
#endif

// ---- memory: fixed-address reservations (guest pointers are host pointers) ----
bool reserve(uint64_t addr, uint64_t size);  // exactly at addr, no access
bool commit(uint64_t addr, uint64_t size);   // read/write
void decommit(uint64_t addr, uint64_t size);

// ---- threads and time ----
using ThreadFn = void (*)(void*);
void start_thread(ThreadFn fn, void* arg, size_t stack_size);
[[noreturn]] void exit_thread();
[[noreturn]] void exit_process(int code);
void sleep_ns(int64_t ns);
void yield();
void set_thread_name(const char* name);
int64_t current_thread_id();  // the host OS thread id
// Samples the given threads (thread CPU time, 1 kHz) for `seconds`: host pc, a few host frames and the guest lr
// (read through guest_lr[k], the thread's x30). Writes one line per sample to `path`:
//   <tid> <guest lr> <module>:<offset> [<module>:<offset> ...]   (host pc first, then callers)
// False where not implemented.
bool profile(const int64_t* tids, const uint64_t* const* guest_lr, size_t count, int seconds, const std::string& path);  // the host thread's name (debuggers, profilers, top)
uint64_t monotonic_ns();

// ---- files ----
FILE* fopen_utf8(const std::string& path, const char* mode);
int64_t ftell64(FILE* f);
int fseek64(FILE* f, int64_t off, int origin);
// Positional read, safe with other threads reading the same FILE (no shared file position). Bytes read.
uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size);
bool make_dirs(const std::string& path);

// ---- output ----
void log_line(const char* line);  // stderr / logcat
std::string data_root();
std::string setting(const std::string& key);  // a player setting: config.txt (Android), NX_<KEY> (PC); "" if unset          // where private/ lives ("private" on PC, the app's external files dir on Android)

// ---- display, input, audio ----
void* create_window();       // native window handle for the Vulkan surface (HWND / ANativeWindow*), kept alive
// Blocks until the next vertical blank of the display that shows the window (Windows: DXGI), and returns its time
// (monotonic_ns); 0 when there is none to wait on, at once.
uint64_t wait_vblank();
void release_window(void* w);  // done with a handle from create_window
// The game's first frame with something on it was presented. A window held back until then (made hidden when the
// launcher passes NX_WINDOW_RECT, so the boot's black frames never replace the launcher's screen) is shown.
void picture_shown();
uint64_t window_generation();  // changes when the OS replaced the window (Android: app back from the background)
bool window_minimized();     // no surface to draw to (minimized / app in background)
void window_size(uint32_t& w, uint32_t& h);  // client size in pixels
void screen_size(uint32_t& w, uint32_t& h);  // the display's own size, landscape (Android; the window on PC)
// The render scale to use when the player sets none: 1 on fast GPUs, less on slower ones (Android).
float default_render_scale();
#if defined(__ANDROID__)
// The app started again from its launcher activity, and this process ended (settings that take effect at start).
[[noreturn]] void restart_app();
// Android: `url` in the app `package` when it is installed (Instagram, GitHub), else in the browser (platform_android).
void open_url_in_app(const std::string& url, const char* package);
#endif
// thermal.cpp: the share of the asked rows the GPU keeps up with now (dynamic resolution; 1 elsewhere).
float dynamic_render_scale();
// The settings' defaults that differ on phones: there the 3D picture's rows follow the GPU ("auto", above) and FSR
// brings it to the screen's size.
#if defined(__ANDROID__)
inline constexpr const char* kDefaultResolution = "auto";
inline constexpr const char* kDefaultUpscaler = "fsr";
#else
inline constexpr const char* kDefaultResolution = "native";
inline constexpr const char* kDefaultUpscaler = "linear";
#endif

// NpadButton bits (console layout: A is the right face button, B the bottom one).
enum : uint64_t {
    kA = 1ull << 0, kB = 1ull << 1, kX = 1ull << 2, kY = 1ull << 3, kStickL = 1ull << 4, kStickR = 1ull << 5,
    kL = 1ull << 6, kR = 1ull << 7, kZL = 1ull << 8, kZR = 1ull << 9, kPlus = 1ull << 10, kMinus = 1ull << 11,
    kLeft = 1ull << 12, kUp = 1ull << 13, kRight = 1ull << 14, kDown = 1ull << 15,
    kStickLLeft = 1ull << 16, kStickLUp = 1ull << 17, kStickLRight = 1ull << 18, kStickLDown = 1ull << 19,
    kStickRLeft = 1ull << 20, kStickRUp = 1ull << 21, kStickRRight = 1ull << 22, kStickRDown = 1ull << 23,
    kSettingsButton = 1ull << 63,  // not a pad button: the drawn controls' gear, which opens the settings screen
};
constexpr int32_t kStickMax = 0x7FFF;

struct PadState {
    uint64_t buttons = 0;  // NpadButton bits
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
};
void read_pad(PadState& p);  // player 1
// Players 2.. (index 1..kMaxPlayers-1): one more controller each (platform_mac.cpp, platform_linux.cpp). false
// while that player has none, and on hosts that only drive player 1.
constexpr int kMaxPlayers = 4;
bool read_player_pad(int index, PadState& p);
void rumble(float low_freq, float high_freq, uint32_t duration_ms);
// The mouse as the console's touch screen (hle_hid.cpp): the left button held at (u, v), across the window (0..1).
// The picture's place in the window comes from the present step (set_picture_box: x, y, w, h in 0..1), so a click
// on the bars touches nothing.
void set_mouse_touch(bool down, float u, float v);
void set_picture_box(float x, float y, float w, float h);
// The PC keyboard as the console's USB keyboard (hle_hid.cpp): the held keys as USB HID usage ids (SDL's scancodes
// are the same numbers) and KeyboardModifier bits. The game's text entry reads them; keyboard_typing() says it is
// doing so now (it turned a key into a character lately), and while it is the keys stop driving the pad.
void set_keyboard(uint64_t modifiers, const uint8_t keys[32]);
bool keyboard_typing();
// A game that can tell when its text entry is open says so each frame (games/acnh/v2_0_7.cpp): that decides typing
// from then on, not the GetKeyCode guess (the game turns keys into characters whether its text entry is up or not).
void keyboard_text_entry(bool open);
bool keyboard_state_known();
// Keys that type a character (letters, digits, space, punctuation): the ones whose press tells typing from playing.
inline bool key_types(int usage) { return (usage >= 4 && usage <= 39) || (usage >= 44 && usage <= 56); }
// Touch controls to draw over the picture (touch_layout.cpp): circles at x, y (0..1
// across the landscape screen), radius r (a fraction of its height), opacity alpha. 0 where nothing is drawn.
struct OverlayShape {
    float x, y, r, alpha;
};
size_t touch_overlay(OverlayShape* out, size_t max);
void set_gamepad_connected(bool connected);  // Android: a controller connected hides the touch controls
// Performance hints (perf_hint.cpp; Android ADPF, nothing elsewhere): the calling thread takes part in making frames;
// a frame's work took `ns`.
void perf_hint_thread();
void perf_hint_frame(int64_t ns);
void gpu_turbo();  // NX_GPU_TURBO=1: the Adreno GPU at its highest clock (perf_hint.cpp; Android)
// Thermal governor (thermal.cpp; Android, nothing elsewhere): video.thermal / NX_THERMAL, and with NX_ROOT_CLOCKS=1 the
// GPU and big-core clock caps (a rooted phone).
void thermal_start();             // after config_apply
void thermal_pause(bool paused);  // the app in the background: the system's caps put back until it returns
int thermal_mode();               // 0 performance, 1 balanced, 2 cool
double thermal_adpf_share(int mode);  // the ADPF target as a share of the frame period
float thermal_headroom();         // AThermal: 1 = severe throttling; -1 unknown
void frame_period(int64_t ns);    // the game's frame period: its present interval (nvn_queue.cpp)
int64_t frame_period_ns();        // 0 before the first present
void frame_replay_work(int64_t ns);  // the replay thread's work for one frame (nvn_vk.cpp)
int64_t frame_replay_work_ns();
void thermal_main_work(int64_t ns);  // the game thread's work for one frame (perf_hint_frame)
void thermal_frame(uint64_t interval_ns);  // one of the game's frames shown, this long after the last (frame_stats.cpp)
// Frame threads and present intervals (frame_stats.cpp): the cores they run on, their CPU time, NX_AFFINITY.
// role 0: the thread that replays and presents (frame_presented ticks it); 1: the game's main thread.
void frame_thread_tick(int role);
void frame_helper_tick();  // a helper thread of the frame, placed with the game main thread (NX_AFFINITY)
// The host thread id of a role (0 replay+present, 1 game main) as last seen by frame_thread_tick, 0 before (sampler.cpp).
uint64_t frame_thread_id(int role);
void frame_presented();  // after each present to the display
uint64_t game_frames();  // the game's frames presented so far (not the repeats or generated ones): the FPS counter
// Linux: asks power-profiles-daemon for its performance profile for as long as this process lives (frame_stats.cpp).
void request_performance_profile();
// Write watches (write_watch.cpp; page protection, POSIX): add fails when unsupported or full; take re-arms and
// says whether the range was written since the last take; the crash handler calls fault first (true: handled).
bool write_watch_add(uint64_t addr, uint64_t size);
void write_watch_remove(uint64_t addr);
// lo/hi (optional): the written bytes as [lo, hi) from addr, page-rounded; [0, UINT64_MAX) when unknown, empty when none.
bool write_watch_take(uint64_t addr, uint64_t* lo = nullptr, uint64_t* hi = nullptr);
bool write_watch_fault(void* at);
// NX_HEADLESS=1: no visible window (it is created hidden) and nothing presented; frames still render and
// NX_DUMP_FRAMES still writes them. For unattended test runs.
bool headless();
// Test files polled in the data folder while the game runs (input_live, dump_frames, window_size; the debugging ones:
// debug_files). On Android the data folder is on shared storage (FUSE): each check of a
// file that is not there took milliseconds, on the replay thread (~15% of its time on a Snapdragon 865). There they are
// off unless NX_LIVE=1 or a test run (NX_LIVE_FRAME); elsewhere on unless NX_LIVE=0.
inline bool live_files() {
    static const bool on = [] {
        if (const char* e = std::getenv("NX_LIVE")) return e[0] == '1';
#if defined(__ANDROID__)
        return std::getenv("NX_LIVE_FRAME") != nullptr;
#else
        return true;
#endif
    }();
    return on;
}
// Debugging files polled while the game runs (trace_cmds, trace_draws, skip_fs, uw_nosqueeze, uw_trace, uw_cams,
// uw_probe): some were checked every 2000-3000 draws, ~500 file opens a second at 60 fps. Only with NX_DEBUG_FILES=1.
inline bool debug_files() {
    static const bool on = std::getenv("NX_DEBUG_FILES") && std::getenv("NX_DEBUG_FILES")[0] == '1';
    return on;
}
// Ends the process when its memory footprint passes NX_MEM_LIMIT_GB (default 8); logs it every 30 s (macOS).
void start_memory_guard();
// How much of [from, to) (monotonic ns) the memory guard spent reading (memguard_android.cpp; 0 elsewhere).
uint64_t memguard_overlap_ns(uint64_t from, uint64_t to);
// The calling thread's CPU time, page faults (major, minor) and context switches (voluntary, involuntary)
// (frame_stats.cpp; Linux and Android, zeros elsewhere).
void thread_counters(uint64_t& cpu_ns, long faults[2], long switches[2]);
void input_script(PadState& p);  // NX_INPUT_SCRIPT presses (input_script.cpp), for unattended runs

// ---- for the settings screen (ui/settings.cpp) ----
void set_fullscreen(bool on);                  // live
int display_refresh_hz();                      // the display the window is on (60 when unknown)
void set_master_volume(float v);               // 0..1, live (scales the device output)
std::string controller_name();                 // the connected controller ("" = none)
std::string controller_kind();                 // "switch", "xbox", "playstation", "generic" or "" (none)
void open_path(const std::string& path);       // shows a folder in the file manager
// Asks the player for a folder (or a .zip when `zip`): the answer arrives later; poll with pick_result.
void pick_path(bool zip, const std::string& title);
bool pick_result(std::string& path);           // true once, when an answer came ("" = cancelled)

// Audio: s16 stereo 48 kHz. `done` is called (on any thread) when a submitted buffer has been played.
using AudioDone = void (*)(void* ctx);
bool audio_open(AudioDone done);
bool audio_submit(const void* data, size_t bytes, void* ctx);  // false: no device (caller paces itself)
uint32_t audio_queued();
void audio_pause(bool paused);  // app in the background

// Vulkan: the loader's vkGetInstanceProcAddr and the surface extension of this platform.
void* vulkan_get_instance_proc_addr();
const char* vulkan_surface_extension();
// Creates the platform surface for `native_window` (VkInstance, VkSurfaceKHR*), returns a VkResult.
int vulkan_create_surface(void* instance, void* native_window, uint64_t* surface, void* gipa);

}  // namespace nx::plat
