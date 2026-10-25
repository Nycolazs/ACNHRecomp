// Animal Crossing: New Horizons (01006F8002326000, 3.0.2 or 3.0.3): what the shared runtime needs to know about the game.
// The game draws through NVN (docs/upstream/NVN.md); what we know about it is in docs/ACNH.md.
#include <atomic>
#include <cstdlib>
#include <cstring>

#include <nx/game.h>
#include <nx/runtime.h>

#include "version.h"

namespace nx {

const game::Info game::kInfo = {"acnh", "Animal Crossing: New Horizons", 0x01006F8002326000ull,
                                "io.github.acnhrecomp", "private/acnh",
                                0 /* main.npdm: no system resource */,
                                // guiTouchManager::update (main+31c3b50) moves every touch 15 up: the UI then hits
                                // what the mouse points at.
                                15};

namespace {

// The engine splits its memory into fixed budgets, a table in main's read-only data (3.0.2 and 3.0.3: main+0x404d658):
// cSys, cGfx, cRes, cSnd, cScene, cOthers (the last one is created with size 0: whatever is left).
// cSys runs out here before the title (AppSysHeap, created last in cSys with "the rest", is ~11 MB and a 3.8 MB
// allocation fails, main+1450630): the engine creates task heaps in cSys with "the rest" too and trims them when
// the task is done, and our threads do not run in the console's order, so a heap is created while another is not
// trimmed yet. 64 MiB move from cOthers (1.6 GB, a remainder) to cSys. Only applied when the table holds the
// values of 3.0.2.
constexpr uint64_t kBudgetTable = 0x404d658;
constexpr uint64_t kBudgets[6] = {0xa8df000, 0x9900000, 0x4a271f8, 0x900000, 0x23d00000, 0x65e00000};
constexpr uint64_t kSysExtra = 64ull << 20;

uint64_t main_base() {
    const LoadedModule* m = module_at(find_symbol("nnMain"));
    return m ? m->base : 0;
}

}  // namespace

// 3.0.3 is told apart by the colour clear call site of the engine's clear on bind (bind_clears below), which it has
// 0x10 further than 3.0.2: mov w1, w22 / mov w3, #0xf / blr x8 / ldr w8, [x19, #0x5c8] at main+37fb500.
bool acnh::is_303() {
    static const bool v = [] {
        static constexpr uint32_t kSite303[4] = {0x2a1603e1, 0x528001e3, 0xd63f0100, 0xb945ca68};
        uint64_t base = main_base();
        if (!base) return false;
        for (int k = 0; k < 4; k++)
            if (ld<uint32_t>(base + 0x37fb500 + 4 * k) != kSite303[k]) return false;
        return true;
    }();
    return v;
}

namespace {

void widen_sys_budget() {
    uint64_t base = main_base();
    if (!base) return;
    uint64_t t = base + kBudgetTable;
    for (int k = 0; k < 6; k++)
        if (ld<uint64_t>(t + 8 * k) != kBudgets[k]) {
            log("acnh: memory budget table not as in 3.0.2/3.0.3: left alone");
            return;
        }
    st<uint64_t>(t, kBudgets[0] + kSysExtra);
    st<uint64_t>(t + 8 * 5, kBudgets[5] - kSysExtra);
    log("acnh: cSys budget %#llx -> %#llx (from cOthers)", (unsigned long long)kBudgets[0],
        (unsigned long long)(kBudgets[0] + kSysExtra));
}

}  // namespace

// The engine's "clear on bind" (main+37fb390) clears a render target every time a pass binds it: colour from
// main+37fb4fc, depth/stencil from main+37fb744 (3.0.3: main+37fb3a0, +37fb50c and +37fb754). On the console the clear is skipped when the target needs none
// (per-buffer state the GPU keeps). Kept every time:
//  - a re-bind of the HDR target wiped the UI captures drawn into it before (a black screen): those colour clears
//    are dropped (NX_SKIP_CLEAR_AT);
//  - a re-bind of the scene's depth wiped the depth pre-pass, and the colour pass (depth EQUAL) drew the back wall
//    over the characters: only the first depth clear of each target in a frame is kept (NX_CLEAR_ONCE_AT,
//    runtime/src/nvn/nvn_cmd.cpp).
// Either variable set by hand (empty: off) overrides these.
void bind_clears() {
    const char* skip = acnh::is_303() ? "37fb50c" : "37fb4fc";
    const char* once = acnh::is_303() ? "37fb754" : "37fb744";
#if defined(_WIN32)
    if (!std::getenv("NX_SKIP_CLEAR_AT")) _putenv_s("NX_SKIP_CLEAR_AT", skip);
    if (!std::getenv("NX_CLEAR_ONCE_AT")) _putenv_s("NX_CLEAR_ONCE_AT", once);
#else
    setenv("NX_SKIP_CLEAR_AT", skip, 0);
    setenv("NX_CLEAR_ONCE_AT", once, 0);
#endif
}

// The job workers (sead threads made by the factory at main+6c5b20, vtable main+4816770) register with Havok on a
// message (calc_, main+6d6514): if the Phive singleton exists (main+5a05378), they call its system's thread setup
// ([[[g]+8]+8]->vtable[0x20]) and keep the answer at this+0x18c (-1 until then, so the next message retries). The
// main thread publishes the singleton (main+3dfc120, in main+3dfc044) and only then builds the system (g->init,
// main+3dfc2a8: the 32 MB PhiveSystemHeap and Havok's memory router, ~3 ms here) and stores it at g+8. A worker
// that wakes in between loaded g+8 = null and faulted on address 8 (1 run in ~12, while the Nook Inc. scene loads).
// The wakeups come from the job queue (TrySendMessageQueue at main+3e42c08: the main thread and the workers wake
// idle workers); on the console the workers own cores 1 and 2 and a stale wakeup is handled within microseconds, here
// a host thread under load is woken milliseconds late and lands in that window. Hooked on the null check
// (cbz x8 at main+6d659c, x8 = g): a half-built singleton counts as no singleton, as the game's own physics step
// already does (main+3e01a98 tests g+8).
static void phive_half_built(CPU& c) {
    if (c.x[8] && ld<uint64_t>(c.x[8] + 8) == 0) {
        c.x[8] = 0;
        static std::atomic<int> seen{0};
        if (seen++ < 4) log("acnh: a job worker woke while the Phive singleton is half built: its Havok setup waits");
    }
}
NX_HOOK(0x6d659c, phive_half_built);

void game::after_init() {
    log("acnh: main is %s", acnh::is_303() ? "3.0.3" : "3.0.2");
    widen_sys_budget();
    bind_clears();
}
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
