// Linux host (x86-64): memory (mmap at fixed addresses), pthreads, an SDL3 window with an SDL-made Vulkan surface
// (X11 or Wayland, whichever SDL picked), SDL3 gamepad + keyboard, SDL3 audio, a signal crash handler and main.
//
// Same shape as platform_mac.cpp: main() starts the game (nx::boot) on its own thread and then runs the SDL event
// loop. Anything that touches the window (creating it, its Vulkan surface) is done on the main thread; the game's
// threads ask for it and wait. Input is sampled on the main thread into a snapshot read_pad() copies.
//
// Data: NX_DATA_ROOT or games/<game>'s pc_data_root ("private/<game>"): analysis/{main,sdk}.bin, extract/romfs, user/.
// Settings: NX_<KEY> environment variables, as on Windows and macOS.
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "config.h"
#include "ui/ui.h"
#include "bug_report.h"
#include "platform.h"

namespace nx {
void boot();
}

namespace nx::plat {

namespace {

pthread_t g_main_thread;

// Work for the main thread (window creation, surfaces), run between event polls.
std::mutex g_main_lock;
std::deque<std::function<void()>> g_main_work;

void run_on_main(std::function<void()> fn) {
    if (pthread_equal(pthread_self(), g_main_thread)) {
        fn();
        return;
    }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    {
        std::lock_guard lk(g_main_lock);
        g_main_work.push_back([&] {
            fn();
            std::lock_guard l2(m);
            done = true;
            cv.notify_one();
        });
    }
    std::unique_lock lk(m);
    cv.wait(lk, [&] { return done; });
}

SDL_Window* g_window = nullptr;
std::atomic<bool> g_minimized{false};
std::atomic<uint32_t> g_pixel_w{0}, g_pixel_h{0};

std::mutex g_pad_lock;
PadState g_pad;

}  // namespace
void update_refresh_hz();  // below, with display_refresh_hz

// ---- memory ---------------------------------------------------------------------------------------------------

bool reserve(uint64_t addr, uint64_t size) {
    void* p = mmap(reinterpret_cast<void*>(addr), size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (uint64_t(p) != addr) {  // kernels before 4.17 take MAP_FIXED_NOREPLACE as a hint
        munmap(p, size);
        return false;
    }
    return true;
}

bool commit(uint64_t addr, uint64_t size) {
    uint64_t page = uint64_t(getpagesize()) - 1;
    uint64_t a = addr & ~page, e = (addr + size + page) & ~page;
    return mprotect(reinterpret_cast<void*>(a), e - a, PROT_READ | PROT_WRITE) == 0;
}

// As on macOS: only whole host pages are released (MADV_DONTNEED gives zero pages back on the next touch); the freed
// part of a page shared with live data is zeroed instead. With 4 KiB host pages that is the guest's own granule.
void decommit(uint64_t addr, uint64_t size) {
    uint64_t page = uint64_t(getpagesize());
    uint64_t lo = (addr + page - 1) & ~(page - 1), hi = (addr + size) & ~(page - 1);
    if (lo >= hi) {
        std::memset(reinterpret_cast<void*>(addr), 0, size);
        return;
    }
    if (addr < lo) std::memset(reinterpret_cast<void*>(addr), 0, lo - addr);
    if (hi < addr + size) std::memset(reinterpret_cast<void*>(hi), 0, addr + size - hi);
    madvise(reinterpret_cast<void*>(lo), hi - lo, MADV_DONTNEED);
    mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_NONE);
}

// ---- threads and time -----------------------------------------------------------------------------------------

struct ThreadStart {
    ThreadFn fn;
    void* arg;
};

static void* thread_entry(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    s.fn(s.arg);
    return nullptr;
}

void start_thread(ThreadFn fn, void* arg, size_t stack_size) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    size_t page = size_t(getpagesize()) - 1;
    pthread_attr_setstacksize(&a, std::max<size_t>((stack_size + page) & ~page, PTHREAD_STACK_MIN));
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    pthread_create(&t, &a, thread_entry, new ThreadStart{fn, arg});
    pthread_attr_destroy(&a);
}

void exit_thread() { pthread_exit(nullptr); }
void exit_process(int code) {
    std::fflush(nullptr);
    _exit(code);
}

void sleep_ns(int64_t ns) {
    timespec ts{time_t(ns / 1000000000), long(ns % 1000000000)};
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR) {
    }
}

void yield() { sched_yield(); }

void set_thread_name(const char* name) {
    char n[16];  // the kernel keeps 15 characters
    std::snprintf(n, sizeof n, "%s", name);
    pthread_setname_np(pthread_self(), n);
}

int64_t current_thread_id() { return int64_t(syscall(SYS_gettid)); }

bool profile(const int64_t*, const uint64_t* const*, size_t, int, const std::string&) { return false; }  // use perf

uint64_t monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// ---- files and output -----------------------------------------------------------------------------------------

FILE* fopen_utf8(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }
int64_t ftell64(FILE* f) { return ftello(f); }
int fseek64(FILE* f, int64_t off, int origin) { return fseeko(f, off, origin); }

uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size) {
    if (size > uint64_t(SSIZE_MAX)) return 0;  // the game passes -1 as "everything": fread read nothing
    int fd = fileno(f);
    uint64_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, static_cast<uint8_t*>(buf) + done, size_t(size - done), off + int64_t(done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += uint64_t(n);
    }
    return done;
}

bool make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

void log_line(const char* line) { std::fprintf(stderr, "%s\n", line); }

std::string data_root() {
    const char* r = std::getenv("NX_DATA_ROOT");
    return r ? r : nx::game::kInfo.pc_data_root;
}



std::string setting(const std::string& key) {
    std::string env = "NX_";
    for (char ch : key) env += char(std::toupper(static_cast<unsigned char>(ch)));
    const char* v = std::getenv(env.c_str());
    return v ? v : "";
}

// ---- window ---------------------------------------------------------------------------------------------------

bool headless() {
    static const bool on = std::getenv("NX_HEADLESS") != nullptr;
    return on;
}

void* create_window() {
    run_on_main([] {
        if (g_window) return;
        int w = 1280, h = 720;
        std::string r = setting("resolution");
        if (!r.empty()) std::sscanf(r.c_str(), "%dx%d", &w, &h);
        g_window = SDL_CreateWindow(nx::game::kInfo.title, w, h,
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN |
                                        (headless() ? SDL_WINDOW_HIDDEN : 0));
        if (!g_window) {
            log_line(SDL_GetError());
            return;
        }
        nx::plat::set_window_icon(g_window);
        if (!headless()) {
            SDL_RaiseWindow(g_window);
            if (setting("fullscreen") == "1") SDL_SetWindowFullscreen(g_window, true);
        }
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
        g_pixel_w = uint32_t(pw), g_pixel_h = uint32_t(ph);
        update_refresh_hz();
    });
    return g_window;  // the Vulkan surface is made from it (on the main thread, vulkan_create_surface)
}

void release_window(void*) {}
// The window is shown from the start here (the launcher's hand-over, NX_WINDOW_RECT, is Windows only).
void picture_shown() {}
uint64_t wait_vblank() { return 0; }  // no display vblank to wait on here: hle_vi.cpp keeps its timed tick
uint64_t window_generation() { return 1; }
bool window_minimized() { return g_minimized; }

void window_size(uint32_t& w, uint32_t& h) {
    w = g_pixel_w;
    h = g_pixel_h;
}

void screen_size(uint32_t& w, uint32_t& h) { window_size(w, h); }
float default_render_scale() { return 1.0f; }

// ---- input: first SDL gamepad + keyboard (sampled on the main thread) -----------------------------------------

// Input options from config.ini (the settings screen writes them), re-read when the config changes.
struct InputOptions {
    bool xbox_layout = false;  // confirm on the bottom face button (Xbox/PlayStation habit)
    int deadzone = 7849;       // of 32767
    bool rumble = true;
    SDL_Scancode keys[22] = {};  // the order of kKeyNames
};
const char* const kKeyNames[22][2] = {
    {"move_up", "W"},     {"move_down", "S"},  {"move_left", "A"},  {"move_right", "D"},  {"cam_up", "I"},
    {"cam_down", "K"},    {"cam_left", "J"},   {"cam_right", "L"},  {"a", "Space"},       {"b", "Left Shift"},
    {"x", "X"},           {"y", "F"},          {"l", "Q"},          {"r", "E"},           {"zl", "Z"},
    {"zr", "C"},          {"plus", "Return"},  {"minus", "Escape"}, {"dpad_up", "Up"},    {"dpad_down", "Down"},
    {"dpad_left", "Left"}, {"dpad_right", "Right"},
};
const InputOptions& input_options() {
    static InputOptions o;
    static uint64_t gen = 0;
    if (uint64_t g = nx::config_generation(); g != gen) {
        gen = g;
        o.xbox_layout = nx::config_get("input.layout", "console") == "xbox";
        o.deadzone = std::clamp(nx::config_get_int("input.deadzone", 24), 5, 50) * 32767 / 100;
        o.rumble = nx::config_get_bool("input.rumble", true);
        for (int k = 0; k < 22; k++) {
            std::string name = nx::config_get(std::string("input.key.") + kKeyNames[k][0], kKeyNames[k][1]);
            SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
            o.keys[k] = sc != SDL_SCANCODE_UNKNOWN ? sc : SDL_GetScancodeFromName(kKeyNames[k][1]);
        }
    }
    return o;
}

SDL_Gamepad* g_gamepad = nullptr;  // player 1's controller: the settings screen's name and icons, and rumble

// ---- several controllers, one per player (hle_hid.cpp reads players 2.. through read_player_pad) ---------------

namespace {
SDL_Gamepad* g_pads[kMaxPlayers] = {};  // the controller that holds each player
PadState g_players[kMaxPlayers];        // players 2..: their last sample, under g_pad_lock
bool g_player_on[kMaxPlayers] = {};
}  // namespace

// One controller's buttons and sticks, in the layout chosen in the settings.
static void read_gamepad(SDL_Gamepad* pad, const InputOptions& opt, PadState& p) {
    // SDL names buttons by position (south/east/...). Console layout: the game's A is the east button, as on a
    // Switch controller. Xbox layout: A is the bottom one (confirm where Xbox and PlayStation players expect it).
    struct Map {
        SDL_GamepadButton b;
        uint64_t bit;
    };
    static const Map map[] = {
        {SDL_GAMEPAD_BUTTON_NORTH, kX},          {SDL_GAMEPAD_BUTTON_WEST, kY},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL},  {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR},
        {SDL_GAMEPAD_BUTTON_START, kPlus},       {SDL_GAMEPAD_BUTTON_BACK, kMinus},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, kStickL}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kStickR},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft},   {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
    };
    for (const Map& m : map)
        if (SDL_GetGamepadButton(pad, m.b)) p.buttons |= m.bit;
    bool east = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST), south = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
    if (opt.xbox_layout ? south : east) p.buttons |= kA;
    if (opt.xbox_layout ? east : south) p.buttons |= kB;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000) p.buttons |= kZL;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000) p.buttons |= kZR;
    auto axis = [&](SDL_GamepadAxis a, bool invert) {
        int v = SDL_GetGamepadAxis(pad, a);
        if (v > -opt.deadzone && v < opt.deadzone) return 0;
        v = std::clamp(v, -kStickMax, kStickMax);
        return invert ? -v : v;
    };
    p.lx = axis(SDL_GAMEPAD_AXIS_LEFTX, false);
    p.ly = axis(SDL_GAMEPAD_AXIS_LEFTY, true);  // SDL: down is positive; Horizon: up is positive
    p.rx = axis(SDL_GAMEPAD_AXIS_RIGHTX, false);
    p.ry = axis(SDL_GAMEPAD_AXIS_RIGHTY, true);
}

// Controllers join as on the console: one that connects waits, the first to press a button becomes player 1, the
// next player 2, and so on, and each keeps its player while it stays connected. The press that joins is kept from
// the game until it is released.
static void sample_gamepads(const InputOptions& opt, PadState (&players)[kMaxPlayers]) {
    static std::vector<SDL_Gamepad*> waiting;
    static bool joining[kMaxPlayers] = {};
    for (int k = 0; k < kMaxPlayers; k++)
        if (g_pads[k] && !SDL_GamepadConnected(g_pads[k])) {
            nx::log("input: player %d's controller disconnected", k + 1);
            SDL_CloseGamepad(g_pads[k]);
            g_pads[k] = nullptr;
        }
    std::erase_if(waiting, [](SDL_Gamepad* pad) {
        if (SDL_GamepadConnected(pad)) return false;
        SDL_CloseGamepad(pad);
        return true;
    });
    static uint64_t next_scan = 0;  // new controllers: looked for 4 times a second, not on every loop
    if (monotonic_ns() >= next_scan) {
        next_scan = monotonic_ns() + 250'000'000;
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        for (int i = 0; ids && i < n; i++) {
            bool known = false;
            for (SDL_Gamepad* pad : g_pads) known |= pad && SDL_GetGamepadID(pad) == ids[i];
            for (SDL_Gamepad* pad : waiting) known |= SDL_GetGamepadID(pad) == ids[i];
            if (known) continue;
            if (SDL_Gamepad* pad = SDL_OpenGamepad(ids[i])) {
                nx::log("input: controller %s, joins on its first button", SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "?");
                waiting.push_back(pad);
            }
        }
        SDL_free(ids);
    }
    for (auto it = waiting.begin(); it != waiting.end();) {
        PadState press;
        read_gamepad(*it, opt, press);
        int k = 0;
        while (k < kMaxPlayers && g_pads[k]) k++;
        if (!press.buttons || k == kMaxPlayers) {
            ++it;
            continue;
        }
        g_pads[k] = *it;
        joining[k] = true;
        SDL_SetGamepadPlayerIndex(g_pads[k], k);  // the player lights, on controllers that have them
        nx::log("input: player %d: %s", k + 1, SDL_GetGamepadName(g_pads[k]) ? SDL_GetGamepadName(g_pads[k]) : "?");
        it = waiting.erase(it);
    }
    for (int k = 0; k < kMaxPlayers; k++) {
        if (!g_pads[k]) continue;
        read_gamepad(g_pads[k], opt, players[k]);
        if (joining[k] && players[k].buttons) players[k] = PadState{};
        else joining[k] = false;
    }
    g_gamepad = g_pads[0];
}

static void sample_input() {
    const InputOptions& opt = input_options();
    PadState players[kMaxPlayers];
    sample_gamepads(opt, players);
    PadState& p = players[0];  // player 1: its controller, the keyboard and the settings screen
    // Keyboard: the bindings from config.ini (defaults laid out for ACNH: WASD left stick, IJKL camera, arrows
    // D-pad, Space A, Shift B, X X, F Y, Q/E L/R, Z/C ZL/ZR, Enter +, Esc -).
    // The keys also go to the game as a USB keyboard (hle_hid.cpp). While its text entry takes them, they type and
    // only the arrows, Enter and Backspace stay on the pad (cursor, confirm, delete); otherwise a key that types
    // drives the pad once held kTypeWaitMs, so the first letter typed in the text entry does not also press a button.
    constexpr uint64_t kTypeWaitMs = 120;
    static uint64_t down_since[SDL_SCANCODE_COUNT] = {};
    uint8_t usb[32] = {};
    uint64_t mods = 0;
    const bool focused = g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS);
    const bool typing = nx::plat::keyboard_typing();
    // The settings screen's text fields: SDL only sends typed characters while text input is on.
    static bool text_on = false;
    if (g_window && nx::ui::ui_text_field() != text_on) {
        text_on = !text_on;
        if (text_on) SDL_StartTextInput(g_window);
        else SDL_StopTextInput(g_window);
    }
    if (focused && typing) {
        const bool* k = SDL_GetKeyboardState(nullptr);
        if (k[SDL_SCANCODE_UP]) p.buttons |= kUp;
        if (k[SDL_SCANCODE_DOWN]) p.buttons |= kDown;
        if (k[SDL_SCANCODE_LEFT]) p.buttons |= kLeft;
        if (k[SDL_SCANCODE_RIGHT]) p.buttons |= kRight;
        if (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER]) p.buttons |= kPlus;
        if (k[SDL_SCANCODE_BACKSPACE]) p.buttons |= kB;
    } else if (focused && !nx::ui::ui_text_field()) {
        const bool* k = SDL_GetKeyboardState(nullptr);
        const uint64_t now = SDL_GetTicks();
        auto key = [&](int i) {
            int sc = opt.keys[i];
            return k[sc] && (!nx::plat::key_types(sc) || nx::plat::keyboard_state_known() ||
                             now - down_since[sc] >= kTypeWaitMs);
        };
        if (key(2)) p.lx = -kStickMax;
        if (key(3)) p.lx = kStickMax;
        if (key(0)) p.ly = kStickMax;
        if (key(1)) p.ly = -kStickMax;
        if (key(6)) p.rx = -kStickMax;
        if (key(7)) p.rx = kStickMax;
        if (key(4)) p.ry = kStickMax;
        if (key(5)) p.ry = -kStickMax;
        static const uint64_t bits[14] = {kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus, kUp, kDown, kLeft, kRight};
        for (int i = 0; i < 14; i++)
            if (key(8 + i)) p.buttons |= bits[i];
        if (k[SDL_SCANCODE_BACKSPACE] && opt.keys[17] == SDL_SCANCODE_ESCAPE) p.buttons |= kMinus;
    }
    if (focused) {
        const bool* k = SDL_GetKeyboardState(nullptr);
        const uint64_t now = SDL_GetTicks();
        for (int u = 4; u < 232; u++) {
            if (!k[u]) {
                down_since[u] = 0;
                continue;
            }
            if (!down_since[u]) down_since[u] = now;
            bool pad = typing && (u == SDL_SCANCODE_UP || u == SDL_SCANCODE_DOWN || u == SDL_SCANCODE_LEFT ||
                                  u == SDL_SCANCODE_RIGHT || u == SDL_SCANCODE_RETURN || u == SDL_SCANCODE_KP_ENTER ||
                                  u == SDL_SCANCODE_BACKSPACE);
            if (!pad) usb[u / 8] |= uint8_t(1u << (u % 8));
        }
        // KeyboardModifier bits: Control 0, Shift 1, LeftAlt 2, RightAlt 3, Gui 4, CapsLock 8, ScrollLock 9, NumLock 10.
        SDL_Keymod m = SDL_GetModState();
        mods = (m & SDL_KMOD_CTRL ? 1u : 0u) | (m & SDL_KMOD_SHIFT ? 2u : 0u) | (m & SDL_KMOD_LALT ? 4u : 0u) |
               (m & SDL_KMOD_RALT ? 8u : 0u) | (m & SDL_KMOD_GUI ? 16u : 0u) | (m & SDL_KMOD_CAPS ? 256u : 0u) |
               (m & SDL_KMOD_SCROLL ? 512u : 0u) | (m & SDL_KMOD_NUM ? 1024u : 0u);
    }
    // The settings screen takes the input while it is open (and watches for its Minus + Plus combo).
    bool ui = nx::ui::ui_pad(p.buttons, p.lx, p.ly);
    if (ui) {
        for (PadState& q : players) q = PadState{};
        std::memset(usb, 0, sizeof usb);
        mods = 0;
    }
    nx::plat::set_keyboard(mods, usb);
    // The mouse: its left button touches the console's screen where the picture is (the game's own touch support).
    if (g_window) {
        float mx = 0, my = 0;
        int ww = 0, wh = 0;
        SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
        SDL_GetWindowSize(g_window, &ww, &wh);
        bool focus = SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS;
        nx::plat::set_mouse_touch(!ui && focus && (mb & SDL_BUTTON_LMASK) && ww > 0 && wh > 0, mx / float(ww), my / float(wh));
    }
    std::lock_guard lk(g_pad_lock);
    g_pad = p;
    for (int k = 1; k < kMaxPlayers; k++) g_players[k] = players[k], g_player_on[k] = g_pads[k] != nullptr;
}

// ---- settings screen hooks (platform.h) --------------------------------------------------------------------------

std::atomic<float> g_volume{-1.0f};  // master volume 0..1 (-1: not read from config.ini yet; audio_cb applies it)

// The refresh rate of the window's display, read on the main thread when the window is made and when it moves to
// another display (nvn_vk.cpp presents each game frame for that many vsyncs).
std::atomic<int> g_refresh_hz{60};
void update_refresh_hz() {
    if (!g_window) return;
    if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window)); m && m->refresh_rate > 20)
        g_refresh_hz = int(m->refresh_rate + 0.5f);
}
int display_refresh_hz() { return g_refresh_hz.load(); }

void set_fullscreen(bool on) {
    run_on_main([on] {
        if (g_window) SDL_SetWindowFullscreen(g_window, on);
    });
}

void set_master_volume(float v) { g_volume = std::clamp(v, 0.0f, 1.0f); }

std::string controller_name() {
    SDL_Gamepad* pad = g_gamepad;
    const char* n = pad ? SDL_GetGamepadName(pad) : nullptr;
    return n ? n : "";
}

std::string controller_kind() {
    SDL_Gamepad* pad = g_gamepad;
    if (!pad) return "";
    switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_XBOX360:
        case SDL_GAMEPAD_TYPE_XBOXONE: return "xbox";
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5: return "playstation";
        default: {
            // The console's own pads, by SDL's name for their type ("switchpro", "joyconleft", ...).
            const char* t = SDL_GetGamepadStringForType(SDL_GetGamepadType(pad));
            std::string s = t ? t : "";
            return s.rfind("switch", 0) == 0 || s.rfind("joycon", 0) == 0 ? "switch" : "generic";
        }
    }
}

void rumble(float low_freq, float high_freq, uint32_t duration_ms) {
    uint16_t low16 = static_cast<uint16_t>(std::clamp(low_freq, 0.0f, 1.0f) * 65535.0f);
    uint16_t high16 = static_cast<uint16_t>(std::clamp(high_freq, 0.0f, 1.0f) * 65535.0f);
    std::lock_guard lk(g_main_lock);
    g_main_work.push_back([low16, high16, duration_ms] {
        if (g_gamepad) SDL_RumbleGamepad(g_gamepad, low16, high16, duration_ms);
    });
}

void open_path(const std::string& path) {
    std::string url = "file://" + path;
    if (!path.empty() && path[0] != '/') {
        char cwd[4096];
        if (getcwd(cwd, sizeof cwd)) url = std::string("file://") + cwd + "/" + path;
    }
    run_on_main([url] { SDL_OpenURL(url.c_str()); });
}

namespace {
std::mutex g_pick_lock;
bool g_pick_done = false;
std::string g_pick_path;
void SDLCALL picked(void*, const char* const* list, int) {
    std::lock_guard lk(g_pick_lock);
    g_pick_path = list && list[0] ? list[0] : "";
    g_pick_done = true;
}
}  // namespace

void pick_path(bool zip, const std::string& title) {
    {
        std::lock_guard lk(g_pick_lock);
        g_pick_done = false;
    }
    run_on_main([zip, title] {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, g_window);
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
        if (zip) {
            static const SDL_DialogFileFilter filter = {"ZIP", "zip"};
            SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter*>(&filter));
            SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 1);
        }
        SDL_ShowFileDialogWithProperties(zip ? SDL_FILEDIALOG_OPENFILE : SDL_FILEDIALOG_OPENFOLDER, picked, nullptr, props);
        SDL_DestroyProperties(props);
    });
}

bool pick_result(std::string& path) {
    std::lock_guard lk(g_pick_lock);
    if (!g_pick_done) return false;
    g_pick_done = false;
    path = g_pick_path;
    return true;
}

void read_pad(PadState& p) {
    {
        std::lock_guard lk(g_pad_lock);
        p.buttons |= g_pad.buttons;
        if (g_pad.lx) p.lx = g_pad.lx;
        if (g_pad.ly) p.ly = g_pad.ly;
        if (g_pad.rx) p.rx = g_pad.rx;
        if (g_pad.ry) p.ry = g_pad.ry;
    }
    input_script(p);
}

bool read_player_pad(int index, PadState& p) {
    if (index < 1 || index >= kMaxPlayers) return false;
    std::lock_guard lk(g_pad_lock);
    if (!g_player_on[index]) return false;
    p = g_players[index];
    return true;
}

// ---- audio: SDL3 stream, s16 stereo 48 kHz, fed from the queue of submitted buffers ----------------------------

namespace {
struct Pending {
    const uint8_t* data;
    size_t size, pos;
    void* ctx;
};
AudioDone g_done = nullptr;
SDL_AudioStream* g_stream = nullptr;
std::mutex g_audio;
std::deque<Pending> g_pending;

// Master volume (settings screen, live) and the mute while the window is in the background (audio.mute_background).
void apply_gain(int16_t* pcm, int count) {
    float v = g_volume.load(std::memory_order_relaxed);
    if (v < 0) {
        v = std::clamp(nx::config_get_int("audio.volume", 100), 0, 100) / 100.0f;
        g_volume = v;
    }
    if (g_window && !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) &&
        nx::config_get_bool("audio.mute_background", false))
        v = 0;
    if (v >= 0.999f) return;
    for (int i = 0; i < count; i++) pcm[i] = int16_t(float(pcm[i]) * v);
}

void SDLCALL audio_cb(void*, SDL_AudioStream* s, int additional, int) {
    uint8_t buf[4096];
    while (additional > 0) {
        int n = std::min<int>(additional, sizeof buf);
        int filled = 0;
        std::vector<void*> finished;
        {
            std::lock_guard lk(g_audio);
            while (filled < n && !g_pending.empty()) {
                Pending& p = g_pending.front();
                size_t take = std::min(size_t(n - filled), p.size - p.pos);
                std::memcpy(buf + filled, p.data + p.pos, take);
                filled += int(take);
                p.pos += take;
                if (p.pos == p.size) {
                    finished.push_back(p.ctx);
                    g_pending.pop_front();
                }
            }
        }
        if (filled < n) std::memset(buf + filled, 0, size_t(n - filled));  // underrun: silence
        apply_gain(reinterpret_cast<int16_t*>(buf), n / 2);
        SDL_PutAudioStreamData(s, buf, n);
        for (void* ctx : finished) g_done(ctx);
        additional -= n;
    }
}
}  // namespace

bool audio_open(AudioDone done) {
    g_done = done;
    // Headless test runs (and NX_MUTE=1) are silent: no device, the callers pace themselves with a timer.
    if (headless() || std::getenv("NX_MUTE")) return false;
    SDL_AudioSpec spec{SDL_AUDIO_S16LE, 2, 48000};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, nullptr);
    if (!g_stream) {
        log("audio: %s", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    return true;
}

bool audio_submit(const void* data, size_t bytes, void* ctx) {
    if (!g_stream) return false;
    std::lock_guard lk(g_audio);
    g_pending.push_back({static_cast<const uint8_t*>(data), bytes, 0, ctx});
    return true;
}

uint32_t audio_queued() {
    std::lock_guard lk(g_audio);
    return uint32_t(g_pending.size());
}

void audio_pause(bool paused) {
    if (!g_stream) return;
    if (paused)
        SDL_PauseAudioStreamDevice(g_stream);
    else
        SDL_ResumeAudioStreamDevice(g_stream);
}

// ---- Vulkan (the system loader, through SDL) ------------------------------------------------------------------

void* vulkan_get_instance_proc_addr() {
    static void* gipa = [] {
        if (!SDL_Vulkan_LoadLibrary(nullptr)) {
            log_line(SDL_GetError());
            return static_cast<void*>(nullptr);
        }
        return reinterpret_cast<void*>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    }();
    return gipa;
}

// The window system's surface extension (VK_KHR_xlib_surface, VK_KHR_wayland_surface, ...): SDL lists it next to
// VK_KHR_surface, which vk_bridge.cpp adds by itself.
const char* vulkan_surface_extension() {
    static const char* ext = [] {
        const char* found = "VK_KHR_xlib_surface";
        Uint32 n = 0;
        const char* const* names = SDL_Vulkan_GetInstanceExtensions(&n);
        for (Uint32 k = 0; names && k < n; k++)
            if (std::strcmp(names[k], VK_KHR_SURFACE_EXTENSION_NAME) != 0) found = names[k];
        log("platform: %s video driver, %s", SDL_GetCurrentVideoDriver(), found);
        return found;
    }();
    return ext;
}

int vulkan_create_surface(void* instance, void* native_window, uint64_t* surface, void* /*gipa: SDL's own*/) {
    VkSurfaceKHR s = VK_NULL_HANDLE;
    bool ok = false;
    run_on_main([&] {
        ok = native_window && SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(native_window),
                                                       static_cast<VkInstance>(instance), nullptr, &s);
        if (!ok) log("platform: SDL_Vulkan_CreateSurface: %s", SDL_GetError());
    });
    *surface = reinterpret_cast<uint64_t>(s);
    return ok ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

}  // namespace nx::plat

// ---- crash handler and entry ----------------------------------------------------------------------------------

namespace {

void crash_handler(int sig, siginfo_t* info, void* uc) {
    if ((sig == SIGSEGV || sig == SIGBUS) && nx::plat::write_watch_fault(info->si_addr)) return;  // write_watch.cpp
    const auto& m = static_cast<ucontext_t*>(uc)->uc_mcontext;
    Dl_info d{};
    uint64_t pc = uint64_t(m.gregs[REG_RIP]);
    dladdr(reinterpret_cast<void*>(pc), &d);
    nx::log("CRASH: signal %d at %s+%llx (%s), data address %llx", sig, d.dli_fname ? d.dli_fname : "?",
            (unsigned long long)(pc - uint64_t(d.dli_fbase)), d.dli_sname ? d.dli_sname : "?",
            (unsigned long long)uint64_t(info->si_addr));
    nx::CPU& c = nx::current_cpu();
    nx::log("  guest lr %s  sp %llx", nx::describe(c.x[30]).c_str(), (unsigned long long)c.x[31]);
    for (int k = 0; k < 32; k += 4)
        nx::log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k], (unsigned long long)c.x[k + 1],
                (unsigned long long)c.x[k + 2], (unsigned long long)c.x[k + 3]);
    nx::backtrace(c);
    nx::log_recent_files();
    std::fflush(nullptr);
    signal(sig, SIG_DFL);
    raise(sig);
}

void game_thread() {
    try {
        nx::boot();
    } catch (const nx::LongJmp& j) {
        nx::fatal("longjmp to buffer %llx with no setjmp frame", (unsigned long long)j.buf);
    } catch (const std::exception& e) {
        nx::fatal("host exception: %s", e.what());
    }
}

}  // namespace

int main(int argc, char** argv) {
    nx::plat::g_main_thread = pthread_self();
    // acnh <game folder>: the folder ACNHRecomp-Importer prepared (else NX_DATA_ROOT, else the setup's data folder)
    if (argc > 1) setenv("NX_DATA_ROOT", argv[1], 1);
    nx::config_apply();  // config.ini -> NX_* defaults, before anything reads them (config.h)
    if (nx::report_mode()) return 0;

    // The crash handler runs on its own stack: a guest stack overflow would leave it none.
    static std::vector<uint8_t> alt(1 << 20);
    stack_t ss{};
    ss.ss_sp = alt.data();
    ss.ss_size = alt.size();
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);

    // On Wayland SDL draws window decorations through libdecor, whose GTK plugin aborts the process on some GNOME
    // setups (a gsettings schema without the "antialiasing" key: GLib-GIO-ERROR in gtk_init_check). XWayland
    // decorates the window itself, so prefer X11 when it is there; SDL_VIDEO_DRIVER still decides when set.
    if (!std::getenv("SDL_VIDEO_DRIVER") && std::getenv("DISPLAY")) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11,wayland");
    // One app id with the launcher: Wayland and the taskbars find the icon in io.github.acnhrecomp.desktop.
    SDL_SetAppMetadata("ACNHRecomp", NX_VERSION_STRING, "io.github.acnhrecomp");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    nx::plat::start_memory_guard();  // before anything can grow: a leak must never freeze the machine

#ifdef NX_GAME_DYNAMIC
    if (!nx::plat::load_game_or_explain()) return 1;  // game_folder.cpp
#endif

    // The game on its own thread, with a big stack (recompiled calls nest deeply); the main thread keeps the event
    // loop.
    nx::plat::start_thread([](void*) { game_thread(); nx::plat::exit_process(0); }, nullptr, 64ull << 20);

    using namespace nx::plat;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            nx::ui::ui_event(e);
            switch (e.type) {
            case SDL_EVENT_QUIT:
                nx::log("platform: quit requested (window closed)");
                exit_process(0);
            case SDL_EVENT_WINDOW_MINIMIZED:
            case SDL_EVENT_WINDOW_HIDDEN:
                g_minimized = true;
                break;
            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_SHOWN:
                g_minimized = false;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                g_pixel_w = uint32_t(e.window.data1), g_pixel_h = uint32_t(e.window.data2);
                break;
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
                update_refresh_hz();
                break;
            case SDL_EVENT_KEY_DOWN:
                if ((e.key.scancode == SDL_SCANCODE_F11 ||
                     (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT))) && g_window)
                    SDL_SetWindowFullscreen(g_window, !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN));
                break;
            default:
                break;
            }
        }
        std::deque<std::function<void()>> work;
        {
            std::lock_guard lk(g_main_lock);
            work.swap(g_main_work);
        }
        for (auto& fn : work) fn();
        sample_input();
        SDL_Delay(2);
    }
}
