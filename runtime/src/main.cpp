// Boot: map and link the recompiled modules (what rtld does), create the main thread and enter
// nn::init::Start(main thread handle, main module base, init, fini) like rtld's _start.
// The platform's entry point (platform_win.cpp, platform_mac.cpp, platform_linux.cpp, platform_android.cpp) calls
// nx::boot().
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"

#ifdef NX_GAME_DYNAMIC
// Android: the modules' tables are in lib<game>_game.so, loaded by the platform before boot (platform_android.cpp).
namespace nx::plat {
const nx::ModuleInfo* game_module(const char* name);
[[noreturn]] void game_longjmp(uint64_t buf, uint64_t val);
}
#define nx_module_main (*nx::plat::game_module("main"))
#define nx_module_sdk (*nx::plat::game_module("sdk"))
#else
extern const nx::ModuleInfo nx_module_main;
extern const nx::ModuleInfo nx_module_sdk;
#ifdef NX_HAS_SUBSDK0
// ACNH: subsdk0 is SDK_movie (nn::movie), imported by main. Recompiled like main and sdk.
extern const nx::ModuleInfo nx_module_subsdk0;
#endif
#endif

namespace nx {

void discord_presence_start();  // discord_presence.cpp

NX_GAME_API uint64_t mrs(CPU& c, uint32_t sysreg) {
    switch (sysreg) {
        case 0xDF02:  // CNTVCT_EL0
        case 0xDF01:  // CNTPCT_EL0
            return system_tick();
        case 0xDF00:  // CNTFRQ_EL0
            return 19200000;
        case 0xD807:  // DCZID_EL0: DC ZVA allowed, 64-byte blocks
            return 4;
        case 0xC000:  // MIDR_EL1 (Cortex-A57)
            return 0x411FD071;
        case 0xD801:  // CTR_EL0 (Cortex-A57: 64-byte lines, PIPT instruction cache)
            return 0x8444C004;
        default:
            log("mrs %#x at lr %s", sysreg, describe(c.x[30]).c_str());
            return 0;
    }
}

void msr(CPU& c, uint32_t sysreg, uint64_t value) {
    log("msr %#x = %llx at lr %s", sysreg, (unsigned long long)value, describe(c.x[30]).c_str());
}

// ---- module initializers --------------------------------------------------------------------------------------
// Each module's DT_INIT (at +0x30 in both main and sdk, see .dynamic) registers the module with the libc
// (__nnmusl_init_dso: TLS, unwind info) and runs its init_array, unless the registration reports 1: then the
// constructors are left to the loader and we run them. Registration only works once the program has given nn::os
// a TLS allocator (SetMemoryAllocatorForThreadLocal, in nninitStartup), so rtld's initializer callback does nothing
// and the modules are initialized right before nnMain, SDK first. Static TLS itself comes from the kernel
// (kernel.cpp: a block per thread at TLS region +0x1F0, where nn::os loads TPIDR_EL0 from).
// DT_INIT sat at +0x30 in SDK 9 and 16 modules; SDK 20 moved it (ACNH: main +0x10, sdk +0x400, subsdk0 +0x20), so it
// is read from the module's .dynamic: the MOD0 header (offset at image +4) gives .dynamic relative to itself.
static uint64_t dt_init(const ModuleInfo& m) {
    uint64_t mod0 = m.base + ld<uint32_t>(m.base + 4);
    uint64_t dyn = mod0 + int64_t(ld<int32_t>(mod0 + 4));
    for (uint64_t p = dyn;; p += 16) {
        int64_t tag = ld<int64_t>(p);
        if (tag == 0) break;
        if (tag == 12) return m.base + ld<uint64_t>(p + 8);  // DT_INIT
    }
    return m.base + 0x30;
}

static void call_preserving(CPU& c, uint64_t target) {
    uint64_t saved[13];
    for (int r = 19; r <= 31; r++) saved[r - 19] = c.x[r];
    call(c, target);
    for (int r = 19; r <= 31; r++) c.x[r] = saved[r - 19];
}

// DT_INIT loads the word where it keeps __nnmusl_init_dso's result with `adrp xN, page` ... `ldr w8, [xN, #off]`
// in its first instructions (SDK 9: adjacent; SDK 16: another adrp in between). Follow the adrp targets until
// that load.
static uint64_t init_status_addr(const ModuleInfo& m) {
    uint64_t page[32] = {};
    const uint64_t init = dt_init(m);
    for (uint64_t pc = init; pc < init + 0x40; pc += 4) {
        uint32_t w = ld<uint32_t>(pc);
        if ((w & 0x9F000000) == 0x90000000) {  // adrp
            int64_t imm = int64_t(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)) << 43 >> 31;  // sign-extended << 12
            page[w & 31] = (pc & ~0xFFFull) + uint64_t(imm);
        } else if ((w & 0xFFC0001F) == 0xB9400008 && page[(w >> 5) & 31]) {  // ldr w8, [xN, #imm]
            return page[(w >> 5) & 31] + ((w >> 10) & 0xFFF) * 4;
        }
    }
    fatal("%s: DT_INIT does not load its init status as expected", m.name);
}

// DT_INIT hands __nnmusl_init_dso the module's layout: consecutive GOT slots it loads with adrp + ldr. Slots 13
// and 14 are the start and end of the module's .tdata (checked on SDK 16: Wonder's main 0x359cc18..0x359cc28,
// sdk empty). Returns false when the pattern is not there (SDK 9 modules have no .tdata to worry about).
static bool tdata_range(const ModuleInfo& m, uint64_t& start, uint64_t& end) {
    uint64_t page[32] = {}, lo = ~0ull, hi = 0;
    const uint64_t init = dt_init(m);
    for (uint64_t pc = init; pc < init + 0x100; pc += 4) {
        uint32_t w = ld<uint32_t>(pc);
        if ((w & 0x9F000000) == 0x90000000) {  // adrp
            int64_t imm = int64_t(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)) << 43 >> 31;
            page[w & 31] = (pc & ~0xFFFull) + uint64_t(imm);
        } else if ((w & 0xFFC00000) == 0xF9400000 && page[(w >> 5) & 31]) {  // ldr xT, [xN, #imm]
            uint64_t slot = page[(w >> 5) & 31] + ((w >> 10) & 0xFFF) * 8;
            lo = std::min(lo, slot), hi = std::max(hi, slot);
        }
    }
    if (hi < lo || (hi - lo) / 8 + 1 < 15) return false;
    start = ld<uint64_t>(lo + 13 * 8), end = ld<uint64_t>(lo + 14 * 8);
    return start <= end && end - start <= 0x1000 && start >= m.base && end <= m.base + 0x10000000;
}

static void init_module(CPU& c, const ModuleInfo& m) {
    call_preserving(c, dt_init(m));
    uint32_t status = ld<uint32_t>(init_status_addr(m));
    if (status == 1)
        for (size_t k = 0; k < m.init_count; k++) call_preserving(c, m.base + m.init_array[k]);
    log("%s constructors: %zu", m.name, status == 1 ? m.init_count : 0);
}

static void rtld_init(CPU&) {}
static void rtld_fini(CPU&) {}
static void rtld_mark(CPU&) {}

// nnMain, as called by nn::init::Start: the constructors need the program's allocator, which nninitStartup has
// set up by now, so they run here, right before the real nnMain.
void cpu_profile_register(const char* name);  // cpu_profile.cpp

static void nn_main(CPU& c) {
    init_module(c, nx_module_sdk);
#ifdef NX_GAME_DYNAMIC
    if (const ModuleInfo* sub = plat::game_module("subsdk0")) init_module(c, *sub);
#elif defined(NX_HAS_SUBSDK0)
    init_module(c, nx_module_subsdk0);
#endif
    init_module(c, nx_module_main);
    game::after_init();
    log("entering nnMain");
    discord_presence_start();  // discord.client_id in config.ini; nothing without it
    cpu_profile_register("main");  // NX_CPU_PROFILE (cpu_profile.cpp)
    uint64_t real = 0;
    for (size_t k = 0; k < nx_module_main.symbol_count; k++)
        if (!nx_module_main.symbols[k].imported && std::string(nx_module_main.symbols[k].name) == "nnMain")
            real = nx_module_main.base + nx_module_main.symbols[k].value;
    call(c, real);
}

// longjmp(buf, val): unwinds the host stack to the recompiled function that called setjmp on buf.
#ifdef NX_GAME_DYNAMIC
// Thrown inside lib<game>_game.so, where the setjmp frames catch it: the type must be that library's own.
static void long_jmp(CPU& c) { plat::game_longjmp(c.x[0], uint32_t(c.x[1])); }
#else
static void long_jmp(CPU& c) { throw LongJmp{c.x[0], uint32_t(c.x[1])}; }
#endif

static void process_return(CPU&) {
    log("nn::init::Start returned");
    plat::exit_process(0);
}

NX_HLE("nnMain", nn_main);
NX_HLE("longjmp", long_jmp);
NX_HLE("_longjmp", long_jmp);
NX_HLE("__rtld_init", rtld_init);
NX_HLE("__rtld_fini", rtld_fini);
NX_HLE("__rtld_mark", rtld_mark);
NX_HLE("__process_return", process_return);

// ---- boot -----------------------------------------------------------------------------------------------------

void boot() {
    if (const char* t = std::getenv("NX_TRACE")) g_trace = *t == '1';
    if (plat::setting("trace") == "1") g_trace = true;
    memory_init();
    kernel_init();
#ifdef NX_GAME_DYNAMIC
    std::vector<const ModuleInfo*> mods = {&nx_module_main};
    if (const ModuleInfo* sub = plat::game_module("subsdk0")) mods.push_back(sub);
    mods.push_back(&nx_module_sdk);
    load_modules(mods.data(), mods.size(), plat::data_root() + "/analysis");
#else
#ifdef NX_HAS_SUBSDK0
    const ModuleInfo* mods[] = {&nx_module_main, &nx_module_subsdk0, &nx_module_sdk};
#else
    const ModuleInfo* mods[] = {&nx_module_main, &nx_module_sdk};
#endif
    load_modules(mods, sizeof(mods) / sizeof(mods[0]), plat::data_root() + "/analysis");
#endif

    uint64_t tdata = 0, tdata_end = 0;
    if (tdata_range(nx_module_main, tdata, tdata_end) && tdata_end > tdata) {
        set_tls_template(tdata, tdata_end);
        log("main .tdata: %llu bytes", (unsigned long long)(tdata_end - tdata));
    }
    uint32_t main_thread = create_main_thread(16ull << 20);
    CPU& c = current_cpu();
    c.x[0] = main_thread;
    c.x[1] = nx_module_main.base;
    // SDK 9: Start(thread, main base, init, fini). SDK 16 adds a first callback: rtld's sets a word to 1 (rtld+0x1330
    // in Wonder), before its init and fini that walk the module list.
    uint64_t start = find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (start) {
        c.x[2] = hle_address("__rtld_init");
        c.x[3] = hle_address("__rtld_fini");
    } else if ((start = find_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_"))) {
        c.x[2] = hle_address("__rtld_mark");
        c.x[3] = hle_address("__rtld_init");
        c.x[4] = hle_address("__rtld_fini");
    } else {
        fatal("nn::init::Start not found");
    }
    c.x[30] = hle_address("__process_return");
    log("entering nn::init::Start at %s", describe(start).c_str());
    call(c, start);
    log("returned from nn::init::Start");
}

}  // namespace nx
