// The desktop player's game code: game/<platform>/lib<game>_game from the game folder ACNHRecomp-Importer prepared
// (NX_GAME_DYNAMIC, CMake's NX_GAME_FROM_FOLDER), as the Android app loads lib<game>_game.so. The executable holds no
// game code: it exports the runtime's calls (NX_GAME_API) and refuses a library built against other runtime headers
// (NX_GAME_ABI).
#if defined(NX_GAME_DYNAMIC) && !defined(__ANDROID__)

#include <cstring>
#include <string>

#include <SDL3/SDL.h>

#include <nx/game.h>
#include <nx/runtime.h>

#include "platform.h"

#ifdef _WIN32
void nx_hook(nx::CPU& c, uint64_t pc);  // loader.cpp
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace nx::plat {

namespace {
#ifdef _WIN32
HMODULE g_game_lib = nullptr;
void* sym(const char* name) { return reinterpret_cast<void*>(GetProcAddress(g_game_lib, name)); }
#else
void* g_game_lib = nullptr;
void* sym(const char* name) { return dlsym(g_game_lib, name); }
#endif
using LongJmpFn = void (*)(uint64_t, uint64_t);
LongJmpFn g_game_longjmp = nullptr;

std::string library_path(const std::string& folder) {
    std::string id = nx::game::kInfo.id;
#if defined(_WIN32)
    return folder + "/game/windows-x64/" + id + "_game.dll";
#elif defined(__APPLE__)
    return folder + "/game/macos-arm64/lib" + id + "_game.dylib";
#else
    return folder + "/game/linux-x64/lib" + id + "_game.so";
#endif
}
}  // namespace

std::string load_game(const std::string& folder) {
    std::string lib = library_path(folder);
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, lib.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, lib.c_str(), -1, w.data(), n);
    if (GetFileAttributesW(w.c_str()) == INVALID_FILE_ATTRIBUTES) return "missing " + lib;
    // the library's own folder first for what it needs beside it
    g_game_lib = LoadLibraryExW(w.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_game_lib) return "LoadLibrary failed (" + std::to_string(GetLastError()) + "): " + lib;
#else
    if (access(lib.c_str(), R_OK) != 0) return "missing " + lib;
    g_game_lib = dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!g_game_lib) return std::string("dlopen: ") + dlerror();
#endif
    auto abi = reinterpret_cast<const char* (*)()>(sym("nx_game_abi"));
    auto jmp = reinterpret_cast<LongJmpFn>(sym("nx_game_longjmp"));
    if (!abi || !jmp || !sym("nx_module_main") || !sym("nx_module_sdk")) return "not a game library: " + lib;
    if (std::strcmp(abi(), NX_GAME_ABI) != 0) return "abi " + std::string(abi()) + " (this ACNHRecomp: " + NX_GAME_ABI + ")";
#ifdef _WIN32
    // The runtime's calls, handed to the library (game_abi.cpp: a DLL cannot import them from the player by name)
    struct Api {
        void (*call)(nx::CPU&, uint64_t);
        void (*svc)(nx::CPU&, uint32_t, uint64_t);
        uint64_t (*mrs)(nx::CPU&, uint32_t);
        void (*msr)(nx::CPU&, uint32_t, uint64_t);
        void (*unimpl)(nx::CPU&, uint64_t, const char*);
        void (*hook)(nx::CPU&, uint64_t);
        void (*trace_sp)(nx::CPU&, uint64_t);
    } api = {&nx::call, &nx::svc, &nx::mrs, &nx::msr, &nx::unimpl, &nx_hook, &nx::trace_sp};
    auto bind = reinterpret_cast<void (*)(const void*, size_t)>(sym("nx_game_bind"));
    if (!bind) return "not a game library (no nx_game_bind): " + lib;
    bind(&api, sizeof api);
#endif
    g_game_longjmp = jmp;
    return "";
}

const nx::ModuleInfo* game_module(const char* name) {
    std::string s = std::string("nx_module_") + name;
    return g_game_lib ? static_cast<const nx::ModuleInfo*>(sym(s.c_str())) : nullptr;
}

[[noreturn]] void game_longjmp(uint64_t buf, uint64_t val) {
    g_game_longjmp(buf, val);
    std::abort();
}

bool load_game_or_explain() {
    std::string why = load_game(data_root());
    if (why.empty()) return true;
    // not a folder prepared for this ACNHRecomp: what to do, with the importer's place
    std::string msg = why.rfind("abi", 0) == 0
        ? "This game folder was prepared for another version of ACNHRecomp. Prepare it again with the "
          "ACNHRecomp-Importer that matches this version (on our Discord: discord.gg/VJW9Dtdu7a).\n\n" + why
        : "This is not a game folder prepared by ACNHRecomp-Importer for this computer. Prepare your game with "
          "ACNHRecomp-Importer (on our Discord: discord.gg/VJW9Dtdu7a) and choose the folder it writes.\n\n" + why;
    nx::log("platform: %s", why.c_str());
    const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Discord"},
                                                {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Close"}};
    const SDL_MessageBoxData box = {SDL_MESSAGEBOX_ERROR, nullptr, nx::game::kInfo.title, msg.c_str(), 2, buttons, nullptr};
    int pressed = 0;
    if (SDL_ShowMessageBox(&box, &pressed) && pressed == 1) SDL_OpenURL("https://discord.gg/VJW9Dtdu7a");
    return false;
}

}  // namespace nx::plat

#endif
