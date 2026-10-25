// Host runtime: memory map, module loader/linker, dispatch, HLE registry, logging.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <nx/cpu.h>
#include <nx/module.h>

namespace nx {

// ---- logging --------------------------------------------------------------------------------------------------

void log(const char* fmt, ...);
[[noreturn]] void fatal(const char* fmt, ...);
extern bool g_trace;  // verbose tracing (NX_TRACE=1)

// ---- address space --------------------------------------------------------------------------------------------
// Guest pointers are host pointers. Everything the guest can see lives in these reserved ranges.

// macOS: the same layout as Linux, moved up by kGuestShift (see NX_BASE in nx/cpu.h): the guest's 39-bit address
// space is 0xC0_0800_0000..0x140_0000_0000 there. svcGetInfo hands every region to the guest, nothing assumes 0.
#if defined(__APPLE__)
constexpr uint64_t kGuestShift = 0xC000000000ull;
#else
constexpr uint64_t kGuestShift = 0;
#endif
constexpr uint64_t kAddressSpaceBase = kGuestShift + 0x0008000000ull;
constexpr uint64_t kAddressSpaceSize = 0x7FF8000000ull;  // 39-bit, like an application on Horizon
constexpr uint64_t kModuleBase = NX_BASE;  // main; sdk at +0x10000000 (NX_BASE, nx/cpu.h)
constexpr uint64_t kHeapMax = 0x180000000ull;  // 6 GiB
constexpr uint64_t kStackSize = 0x100000000ull;
constexpr uint64_t kKernelSize = 0x100000000ull;
constexpr uint64_t kThunkSize = 0x100000ull;
constexpr uint64_t kHeapBase = kGuestShift + 0x5000000000ull;
#if defined(__ANDROID__)
// Android maps the zygote's executable (app_process64, position independent) at a random address from about
// 0x55_5555_0000 to 0x65_5555_0000 at every boot, inside 0x58..0x68: a Galaxy S23 refused the alias reservation
// ("the range is in use", app_process64 at 0x5f_0972_b000). Below the modules nothing is mapped (ART keeps to the
// low 4 GiB), so the alias region goes there. The guest learns it from svcGetInfo, like every region but the modules.
constexpr uint64_t kAliasBase = 0x2000000000ull;
#else
constexpr uint64_t kAliasBase = kGuestShift + 0x5800000000ull;
#endif
constexpr uint64_t kAliasSize = 0x1000000000ull;
constexpr uint64_t kStackBase = kGuestShift + 0x6800000000ull;
// Kept below 0x70_0000_0000: Android (39-bit address space) maps the process's own libraries and heap up there.
constexpr uint64_t kKernelBase = kGuestShift + 0x6A00000000ull;
constexpr uint64_t kThunkBase = kGuestShift + 0x6F00000000ull;
// kKernelBase: TLS pages, main thread stack (memory we hand out). kThunkBase: HLE function addresses (never mapped).

void memory_init();
bool commit(uint64_t addr, uint64_t size);   // make [addr, addr+size) read/write
void decommit(uint64_t addr, uint64_t size);
uint64_t kernel_alloc(uint64_t size, uint64_t align = 0x1000);  // from the kernel region

// Horizon memory states for QueryMemory.
enum MemState : uint32_t {
    kMemFree = 0x00,
    kMemCode = 0x03,
    kMemCodeData = 0x04,
    kMemHeap = 0x05,
    kMemStatic = 0x0C,  // module ro/data seen as static
    kMemStack = 0x0B,
    kMemThreadLocal = 0x0C,
    kMemAlias = 0x0E,
    kMemInaccessible = 0x10,
};
void vm_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm);
struct VmInfo {
    uint64_t addr, size;
    uint32_t state, perm;
};
VmInfo vm_query(uint64_t addr);

// ---- modules --------------------------------------------------------------------------------------------------

struct LoadedModule {
    const ModuleInfo* info;
    uint64_t base, size;
};

void load_modules(const ModuleInfo* const* mods, size_t count, const std::string& data_dir);
uint64_t find_symbol(const char* name);  // 0 when not found
const LoadedModule* module_at(uint64_t addr);
std::string describe(uint64_t addr);  // "sdk+0x1234 (symbol+0x10)"

// ---- HLE ------------------------------------------------------------------------------------------------------

using HleFn = void (*)(CPU&);
// A module recompiled with --variant (recomp/__main__.py): the words its patch changes, (offset, word) pairs. When
// nx_variant_on (NX_SPEEDFIX) the loader writes them into the module's image and the changed functions run their
// patched bodies.
struct VariantRegistrar {
    VariantRegistrar(const char* module, const uint32_t* words, size_t count);
};

struct HleRegistrar {
    HleRegistrar(const char* name, HleFn fn);
};
#define NX_HLE_CAT2(a, b) a##b
#define NX_HLE_CAT(a, b) NX_HLE_CAT2(a, b)
#define NX_HLE(sym, fn) static ::nx::HleRegistrar NX_HLE_CAT(hle_reg_, __LINE__)(sym, fn)

// Instruction hooks: the recompiler (--hooks <file>) makes the instruction at main+offset call nx_hook first, with
// the guest registers in the context; the function registered for that offset may read and change them. Offsets
// are from the first module's base (main).
struct HookRegistrar {
    HookRegistrar(uint64_t offset, HleFn fn);
};
#define NX_HOOK(offset, fn) static ::nx::HookRegistrar NX_HLE_CAT(hook_reg_, __LINE__)(offset, fn)
// The same hook in two updates of a game whose code moved (ACNH 3.0.2 and 3.0.3): both offsets are registered, and
// only the one the recompiler instrumented for the dump is ever called.
struct HookRegistrar2 {
    HookRegistrar first, second;
    HookRegistrar2(uint64_t a, uint64_t b, HleFn fn) : first(a, fn), second(b, fn) {}
};
#define NX_HOOK_VERSIONS(offset, offset2, fn) \
    static ::nx::HookRegistrar2 NX_HLE_CAT(hook_reg_, __LINE__)(offset, offset2, fn)

uint64_t hle_address(const char* name);  // thunk address; unknown names get a logging stub
uint64_t hle_address_or(const char* name, HleFn fallback);  // unknown names get `fallback`
const char* hle_current_name();  // inside an HLE function: the name its thunk was made for
HleFn hle_lookup(const char* name);

// Call guest code at `addr` on context c (args already in x0..).
inline void guest_call(CPU& c, uint64_t addr) { call(c, addr); }

// ---- kernel ---------------------------------------------------------------------------------------------------

extern std::atomic<uint64_t> g_frame;  // frames presented so far (vkQueuePresentKHR)

// ---- input (hle_hid.cpp), for games that drive their own UI from the touch screen / mouse ----------------------

// The last touch that ended (finger lifted: a mouse click released): where on the 1280x720 screen and on which
// frame. False when there was none since the last call.
bool touch_take_tap(uint32_t& x, uint32_t& y, uint64_t& frame);
// Where the mouse is (1280x720 screen, button up), when it moved since the last call; false when it did not.
bool mouse_take_hover(uint32_t& x, uint32_t& y);
// Holds these npad buttons (nn::hid::NpadButton bits: A = 1) down for `frames` frames from the next one (after the
// one before it, if that is still held, and two frames up).
void hid_hold_buttons(uint64_t buttons, uint32_t frames);

void kernel_init();
uint32_t create_main_thread(uint64_t stack_size);  // the calling host thread becomes it
CPU& current_cpu();
uint64_t system_tick();
// A file the game can open under a mount (e.g. "Content/Mods/X/a.lua" for Content:/Mods/X/a.lua), served from
// memory; it takes precedence over a real file of the same path (hle_fs.cpp).
void add_virtual_file(const std::string& path, std::string data);
// The contents of a real game file under the data folder ("" if missing).
std::string read_game_file(const std::string& relative_to_romfs);
// Guest memory written by the host behind the GPU's back (a movie frame): textures over it are uploaded again
// (nvn_objects.cpp).
void textures_written(uint64_t addr, uint64_t size);
// The last file and directory opens of every thread (a small ring, always on), logged by the crash handlers: what
// the game was loading when it crashed (hle_fs.cpp).
void log_recent_files();

}  // namespace nx
