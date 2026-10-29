// Windows host (x86-64): memory (VirtualAlloc at fixed addresses), Win32 threads, high-resolution sleeps, files with
// UTF-8 paths, the crash handler, hardware watchpoints (NX_WATCH) and main(). The window, input (every controller SDL
// knows), audio, the settings screen and the event loop are platform_sdl.cpp's.
//
// main() starts the game (nx::boot) on its own thread with a 64 MiB stack and then
// runs the SDL event loop.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <windows.h>
#include <dxgi.h>
#include <timeapi.h>

#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED  // SDL_RegisterApp only: main() stays ours
#include <SDL3/SDL_main.h>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "config.h"
#include "bug_report.h"
#include "platform.h"
#include "platform_sdl.h"

namespace nx {
void boot();
}  // namespace nx

namespace nx::plat {

// ---- memory ---------------------------------------------------------------------------------------------------

bool reserve(uint64_t addr, uint64_t size) {
    return VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE, PAGE_NOACCESS) != nullptr;
}

bool commit(uint64_t addr, uint64_t size) {
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    return VirtualAlloc(reinterpret_cast<void*>(a), e - a, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

void decommit(uint64_t addr, uint64_t size) { VirtualFree(reinterpret_cast<void*>(addr), size, MEM_DECOMMIT); }

// ---- threads and time -----------------------------------------------------------------------------------------

struct ThreadStart {
    ThreadFn fn;
    void* arg;
};

static DWORD WINAPI thread_entry(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    s.fn(s.arg);
    return 0;
}

void start_thread(ThreadFn fn, void* arg, size_t stack_size) {
    HANDLE h = CreateThread(nullptr, stack_size, thread_entry, new ThreadStart{fn, arg},
                            STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (h) CloseHandle(h);
}

void exit_thread() { ExitThread(0); }
void exit_process(int code) {
    std::fflush(nullptr);
    ExitProcess(UINT(code));
}

// Sleep() and std::this_thread::sleep_for round up to the timer tick (1-15.6 ms), which made the vsync and present
// pacing uneven (G4). A high-resolution waitable timer per thread sleeps to within ~0.5 ms; the last stretch spins.
void sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                       TIMER_ALL_ACCESS);
    const uint64_t end = monotonic_ns() + uint64_t(ns);
    if (ns > 600000) {
        LARGE_INTEGER due;
        due.QuadPart = -((ns - 500000) / 100);  // relative, in 100 ns units
        if (timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
            WaitForSingleObject(timer, INFINITE);
        else
            Sleep(DWORD((ns - 500000) / 1000000));
    }
    while (monotonic_ns() < end) YieldProcessor();
}

void yield() { SwitchToThread(); }

// Thread names show in debuggers, profilers and crash dumps (G7).
void set_thread_name(const char* name) {
    int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w.data(), n);
    SetThreadDescription(GetCurrentThread(), w.c_str());
}

int64_t current_thread_id() { return int64_t(GetCurrentThreadId()); }
bool profile(const int64_t*, const uint64_t* const*, size_t, int, const std::string&) { return false; }

// The display's vblank through DXGI: the output of the monitor that shows the window, looked up again every 2 s (the
// window may move to another monitor; on a laptop with two GPUs the output belongs to whichever drives the panel).
uint64_t wait_vblank() {
    static IDXGIFactory1* factory = nullptr;
    static IDXGIOutput* output = nullptr;
    static HMONITOR monitor = nullptr;
    static uint64_t checked = 0;
    static bool failed = false;
    if (failed) return 0;
    const uint64_t now = monotonic_ns();
    if (!output || now - checked > 2000000000ull) {
        checked = now;
        if (!factory && FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
            failed = true;
            return 0;
        }
        HWND hwnd = static_cast<HWND>(sdl_native_window());
        HMONITOR m = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (m != monitor || !output) {
            IDXGIOutput* found = nullptr;
            IDXGIAdapter1* adapter = nullptr;
            for (UINT a = 0; !found && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
                IDXGIOutput* o = nullptr;
                for (UINT k = 0; !found && adapter->EnumOutputs(k, &o) != DXGI_ERROR_NOT_FOUND; k++) {
                    DXGI_OUTPUT_DESC d{};
                    if (SUCCEEDED(o->GetDesc(&d)) && d.Monitor == m) found = o;
                    else o->Release();
                }
                adapter->Release();
            }
            if (found) {
                if (output) output->Release();
                output = found, monitor = m;
            }
        }
        if (!output) {
            failed = true;
            return 0;
        }
    }
    if (FAILED(output->WaitForVBlank())) return 0;
    return monotonic_ns();
}

uint64_t monotonic_ns() {
    static const LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(t.QuadPart / f.QuadPart) * 1000000000ull + uint64_t(t.QuadPart % f.QuadPart) * 1000000000ull / uint64_t(f.QuadPart);
}

// ---- files and output -----------------------------------------------------------------------------------------

FILE* fopen_utf8(const std::string& path, const char* mode) {
    std::wstring wp = std::filesystem::u8path(path).wstring();
    std::wstring wm(mode, mode + std::strlen(mode));
    return _wfopen(wp.c_str(), wm.c_str());
}
int64_t ftell64(FILE* f) { return _ftelli64(f); }
int fseek64(FILE* f, int64_t off, int origin) { return _fseeki64(f, off, origin); }
uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size) {
    _lock_file(f);  // seek + read as one step for this FILE
    _fseeki64_nolock(f, off, SEEK_SET);
    uint64_t n = _fread_nolock(buf, 1, size, f);
    _unlock_file(f);
    return n;
}
bool make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(path), ec);
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

}  // namespace nx::plat

// ---- crash handler, watchpoints, entry --------------------------------------------------------------------------

namespace {

// NX_WATCH=<hex address>: hardware write watchpoint (DR0, 8 bytes) on the game's thread; each write (up to 30) logs
// the host module/offset and the guest context.
uint64_t g_watch = 0;

void arm_watch(HANDLE thread) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(thread, &ctx);
    ctx.Dr0 = g_watch;
    ctx.Dr7 = (1ull << 0) | (1ull << 16) | (3ull << 18);  // L0, R/W0 = write, LEN0 = 8 bytes
    SetThreadContext(thread, &ctx);
}

void dump_guest(nx::CPU& c) {
    using namespace nx;
    log("  guest lr %s  sp %llx  tpidr %llx  tpidrro %llx", describe(c.x[30]).c_str(), (unsigned long long)c.x[31],
        (unsigned long long)c.tpidr, (unsigned long long)c.tpidrro);
    for (int k = 0; k < 32; k += 4)
        log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k], (unsigned long long)c.x[k + 1],
            (unsigned long long)c.x[k + 2], (unsigned long long)c.x[k + 3]);
    backtrace(c);
}

uint64_t exe_va(void* p) { return uint64_t(p) - uint64_t(GetModuleHandleW(nullptr)) + 0x140000000ull; }

LONG WINAPI watch_handler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_watch) return EXCEPTION_CONTINUE_SEARCH;
    nx::log("WATCH: %llx written (now %llx) at %s.exe+%llx (tools/symbolize.py)", (unsigned long long)g_watch,
            (unsigned long long)nx::ld<uint64_t>(g_watch), nx::game::kInfo.id, (unsigned long long)exe_va(e->ExceptionRecord->ExceptionAddress));
    dump_guest(nx::current_cpu());
    static int hits = 0;
    if (++hits >= 30) e->ContextRecord->Dr7 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// "<module>+<offset>" for a host code address; our own exe as the linker's VA (tools/symbolize.py reads the map).
std::string host_where(uint64_t pc) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(pc), &m))
        return "?";
    char b[MAX_PATH + 32];
    if (m == GetModuleHandleW(nullptr)) {
        std::snprintf(b, sizeof b, "%s.exe+%llx", nx::game::kInfo.id, (unsigned long long)exe_va(reinterpret_cast<void*>(pc)));
        return b;
    }
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = std::max(std::strrchr(path, '\\'), std::strrchr(path, '/'));
    std::snprintf(b, sizeof b, "%s+%llx", name ? name + 1 : path, (unsigned long long)(pc - uint64_t(m)));
    return b;
}

// The host call stack at the fault, unwound from its context with the x64 unwind tables.
void host_backtrace(const CONTEXT& at) {
    CONTEXT c = at;
    for (int k = 0; k < 16 && c.Rip; k++) {
        nx::log("  host #%-2d %s", k, host_where(c.Rip).c_str());
        DWORD64 base = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (!f) {  // a leaf function: the return address is at rsp
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &handler_data, &frame, nullptr);
    }
}

LONG WINAPI crash_handler(EXCEPTION_POINTERS* e) {
    auto* r = e->ExceptionRecord;
    uint64_t info = r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0;
    nx::log("CRASH: exception %08lx at %s (tools/symbolize.py for the exe), data address %llx", r->ExceptionCode,
            host_where(uint64_t(r->ExceptionAddress)).c_str(), (unsigned long long)info);
    host_backtrace(*e->ContextRecord);
    dump_guest(nx::current_cpu());
    nx::log_recent_files();
    std::fflush(nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}

// NX_THROW_TRACE=1: log the host call stack of every C++ throw that is not a guest longjmp (first chance).
LONG WINAPI throw_tracer(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != 0xE06D7363) return EXCEPTION_CONTINUE_SEARCH;
    void* frames[24];
    USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    std::string s = "C++ throw, host stack:";
    for (USHORT k = 0; k < n; k++) {
        char b[32];
        std::snprintf(b, sizeof b, " %llx", (unsigned long long)exe_va(frames[k]));
        s += b;
    }
    nx::log("%s", s.c_str());
    return EXCEPTION_CONTINUE_SEARCH;
}

// The process at full speed: 1 ms timer resolution for the waits the game's own threads make, and no EcoQoS power
// throttling when the window is in the background or the laptop is on battery (G4, G10).
void tune_process() {
    timeBeginPeriod(1);
    PROCESS_POWER_THROTTLING_STATE pt{};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    pt.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
}

void game_thread() {
    nx::plat::set_thread_name("game");
    if (g_watch) arm_watch(GetCurrentThread());
    try {
        nx::boot();
    } catch (const nx::LongJmp& j) {
        nx::fatal("longjmp to buffer %llx with no setjmp frame", (unsigned long long)j.buf);
    } catch (const std::exception& e) {
        nx::fatal("host exception: %s", e.what());
    }
}

}  // namespace

int main() {
    nx::config_apply();  // config.ini -> NX_* defaults, before anything reads them (config.h)
    if (nx::report_mode()) return 0;
    SetUnhandledExceptionFilter(crash_handler);
    if (const char* t = std::getenv("NX_THROW_TRACE"); t && *t == '1') AddVectoredExceptionHandler(1, throw_tracer);
    if (const char* w = std::getenv("NX_WATCH")) {
        g_watch = std::strtoull(w, nullptr, 16);
        AddVectoredExceptionHandler(1, watch_handler);
        nx::log("watching writes to %llx", (unsigned long long)g_watch);
    }
    tune_process();

    // The window class stays "<game title>Recomp": tools/capture.ps1 finds the window by it.
    std::string cls = std::string(nx::game::kInfo.title) + "Recomp";
    SDL_RegisterApp(cls.c_str(), 0, nullptr);
    if (!nx::plat::sdl_start()) return 1;

    nx::plat::start_memory_guard();  // before anything can grow: a leak must never freeze the machine (memguard_win.cpp)
#ifdef NX_GAME_DYNAMIC
    if (!nx::plat::load_game_or_explain()) return 1;  // game_folder.cpp
#endif

    // The game on its own thread, with a big stack (recompiled calls nest deeply); the main thread keeps the event
    // loop.
    nx::plat::start_thread([](void*) { game_thread(); nx::plat::exit_process(0); }, nullptr, 64ull << 20);

    nx::plat::sdl_main_loop();
}

namespace nx {
void cpu_profile_register(const char*) {}  // the sampling profiler is macOS and Linux only (cpu_profile.cpp)
}  // namespace nx
