// Part of lib<game>_game (the game's recompiled code as its own library: Android's lib<game>_game.so, the desktop
// game folder's game/<platform>/lib<game>_game): the player holds no game code (CMakeLists.txt, docs/ANDROID.md). It
// loads this library from the game folder and finds the modules' tables (nx_module_<name>) and these functions in it.
#include <nx/cpu.h>

#ifdef _WIN32
#include <cstddef>
#include <cstring>
#define NX_GAME_EXPORT __declspec(dllexport)
#else
#define NX_GAME_EXPORT __attribute__((visibility("default")))
#endif

// The headers this library was built from (CMakeLists.txt: a hash of nx/cpu.h, module.h, runtime.h): the player
// refuses a library built from other ones.
extern "C" NX_GAME_EXPORT const char* nx_game_abi() { return NX_GAME_ABI; }

// A guest longjmp: thrown here so that the exception's type is this library's, the one its setjmp frames catch.
extern "C" NX_GAME_EXPORT [[noreturn]] void nx_game_longjmp(uint64_t buf, uint64_t val) {
    throw nx::LongJmp{buf, val};
}

#ifdef _WIN32
// Windows: a DLL cannot leave the runtime's calls (nx::call, svc, ...) for the loader to find in the player, as an
// ELF or Mach-O library does. They are defined here instead, through the player's functions, which it hands over
// with nx_game_bind right after loading the library (game_folder.cpp). The order is the player's NxRuntimeApi's.
namespace {
struct Api {
    void (*call)(nx::CPU&, uint64_t);
    void (*svc)(nx::CPU&, uint32_t, uint64_t);
    uint64_t (*mrs)(nx::CPU&, uint32_t);
    void (*msr)(nx::CPU&, uint32_t, uint64_t);
    void (*unimpl)(nx::CPU&, uint64_t, const char*);
    void (*hook)(nx::CPU&, uint64_t);
    void (*trace_sp)(nx::CPU&, uint64_t);
} g_api;
}  // namespace

extern "C" NX_GAME_EXPORT void nx_game_bind(const void* api, size_t size) {
    std::memcpy(&g_api, api, size < sizeof g_api ? size : sizeof g_api);
}

namespace nx {
void call(CPU& c, uint64_t target) { g_api.call(c, target); }
void svc(CPU& c, uint32_t imm, uint64_t pc) { g_api.svc(c, imm, pc); }
uint64_t mrs(CPU& c, uint32_t sysreg) { return g_api.mrs(c, sysreg); }
void msr(CPU& c, uint32_t sysreg, uint64_t value) { g_api.msr(c, sysreg, value); }
[[noreturn]] void unimpl(CPU& c, uint64_t pc, const char* what) {
    g_api.unimpl(c, pc, what);
    __builtin_unreachable();
}
void trace_sp(CPU& c, uint64_t pc) { g_api.trace_sp(c, pc); }
}  // namespace nx
void nx_hook(nx::CPU& c, uint64_t pc) { g_api.hook(c, pc); }
#endif
