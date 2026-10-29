// Module loader and dynamic linker (what rtld does on Horizon), indirect-call dispatch and HLE thunks.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

bool g_trace = false;
std::atomic<uint64_t> g_frame{0};

namespace {

std::mutex g_log_lock;
FILE* g_log_file = nullptr;

std::vector<LoadedModule> g_modules;
std::unordered_map<std::string, uint64_t> g_exports;

struct Thunk {
    std::string name;
    HleFn fn;
    bool leaf = false;  // a host libc routine (below): called straight, without the name bookkeeping
};
std::vector<Thunk> g_thunks;
std::unordered_map<std::string, uint64_t> g_thunk_by_name;

std::unordered_map<std::string, HleFn>& registry() {
    static std::unordered_map<std::string, HleFn> r;
    return r;
}

thread_local const char* t_missing_name = nullptr;

}  // namespace

// ---- logging --------------------------------------------------------------------------------------------------

std::string g_log_path;  // set with the file, under g_log_lock

std::string log_file_path() {
    std::lock_guard lk(g_log_lock);
    return g_log_path;
}

// Lines go to the file and the system log on a writer thread of their own: written and flushed from the calling thread,
// a line on the phone's SD card (FUSE) now and then took 20-35 ms, and the replay thread's periodic counters made a
// frame late (a 50 ms hitch every minute or so on the S20 FE). A crash, a fatal error or a GPU hang switches to
// writing at once (everything queued first), so that the lines before the process ends are on disk; log_flush() too.
namespace {
std::mutex g_write_lock;  // taken before g_log_lock: the order of the lines on disk
std::condition_variable g_log_cv;
std::string g_pending;
// Elsewhere every line is written at once, as before (a local disk; exit_process does not flush there).
#if defined(__ANDROID__)
bool g_log_sync = false;
#else
bool g_log_sync = true;
#endif
bool g_writer = false;

void open_log() {  // under g_log_lock
    if (g_log_file) return;
    const char* path = std::getenv("NX_LOG");
#if defined(__ANDROID__)
    std::string def = plat::data_root() + "/nx.log";  // next to the data, readable from the phone
#else
    std::string def = plat::data_root() + "/../nx.log";
#endif
    std::string file = path ? path : def;
    g_log_path = file;
    // Keep the previous session's log (a problem report should survive reopening the game).
    std::remove((file + ".prev").c_str());
    std::rename(file.c_str(), (file + ".prev").c_str());
    g_log_file = std::fopen(file.c_str(), "w");
}

// What is queued, written; the caller holds g_write_lock and not g_log_lock.
void write_pending() {
    std::string lines;
    FILE* f;
    {
        std::lock_guard lk(g_log_lock);
        lines.swap(g_pending);
        f = g_log_file;
    }
    if (lines.empty()) return;
    for (size_t at = 0; at < lines.size();) {
        size_t end = lines.find('\n', at);
        lines[end] = 0;
        plat::log_line(lines.c_str() + at);
        lines[end] = '\n';
        at = end + 1;
    }
    if (f) {
        std::fwrite(lines.data(), 1, lines.size(), f);
        std::fflush(f);
    }
}
}  // namespace

void log_flush() {
    std::lock_guard w(g_write_lock);
    write_pending();
}

void log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    bool now;
    {
        std::lock_guard lk(g_log_lock);
        open_log();
        if (std::strncmp(buf, "CRASH", 5) == 0 || std::strncmp(buf, "FATAL", 5) == 0 || std::strstr(buf, "GPU hang"))
            g_log_sync = true;
        g_pending += buf;
        g_pending += '\n';
        now = g_log_sync;
        if (!now && !g_writer) {
            g_writer = true;
            std::thread([] {
                for (;;) {
                    {
                        std::unique_lock lk(g_log_lock);
                        g_log_cv.wait_for(lk, std::chrono::milliseconds(250), [] { return !g_pending.empty(); });
                    }
                    log_flush();
                }
            }).detach();
        }
    }
    if (now) log_flush();
    else g_log_cv.notify_one();
}

void fatal(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log("FATAL: %s", buf);
    plat::exit_process(1);
}

// ---- HLE registry ---------------------------------------------------------------------------------------------

HleRegistrar::HleRegistrar(const char* name, HleFn fn) { registry()[name] = fn; }

// ---- libc string routines --------------------------------------------------------------------------------------
// The SDK's memcpy/memmove/memset/memcmp/strlen (ARM's optimized routines) recompiled are many times slower than the
// host's: memcpy (sdk __memcpy_aarch64) was the busiest recompiled function of the process. Every call to them goes
// through a PLT slot, so the host versions take their place by name. Same results: memcpy is memmove-safe in ARM's
// routines (and here), memcmp returns the difference of the first differing bytes as the guest's does. The game's
// main and the SDK reach them through nx::call's thunk path (leaf: no hle_current_name bookkeeping).
// NX_HLE_LIBC=0: the recompiled routines (bisecting).
namespace {
void hle_memmove(CPU& c) {
    if (c.x[2]) std::memmove(reinterpret_cast<void*>(c.x[0]), reinterpret_cast<const void*>(c.x[1]), c.x[2]);
}
void hle_memset(CPU& c) {
    if (c.x[2]) std::memset(reinterpret_cast<void*>(c.x[0]), int(c.x[1] & 0xFF), c.x[2]);
}
void hle_memcmp(CPU& c) {
    const uint8_t* a = reinterpret_cast<const uint8_t*>(c.x[0]);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(c.x[1]);
    uint64_t n = c.x[2], k = 0;
    for (; k + 8 <= n; k += 8) {
        uint64_t x, y;
        std::memcpy(&x, a + k, 8), std::memcpy(&y, b + k, 8);
        if (x != y) break;
    }
    for (; k < n; k++)
        if (a[k] != b[k]) {
            c.x[0] = uint32_t(int32_t(a[k]) - int32_t(b[k]));
            return;
        }
    c.x[0] = 0;
}
void hle_strlen(CPU& c) { c.x[0] = std::strlen(reinterpret_cast<const char*>(c.x[0])); }

struct LibcFn {
    const char* name;
    HleFn fn;
};
constexpr LibcFn kLibc[] = {{"memcpy", hle_memmove}, {"memmove", hle_memmove}, {"memset", hle_memset},
                            {"memcmp", hle_memcmp},  {"strlen", hle_strlen}};

bool libc_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_HLE_LIBC");
        return !(e && e[0] == '0');
    }();
    return on;
}

bool is_leaf(HleFn fn) {
    for (const LibcFn& l : kLibc)
        if (l.fn == fn) return true;
    return false;
}
}  // namespace

HleFn hle_lookup(const char* name) {
    if (libc_on())
        for (const LibcFn& l : kLibc)
            if (std::strcmp(l.name, name) == 0) return l.fn;
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : it->second;
}

// ---- instruction hooks (recomp --hooks) -------------------------------------------------------------------------
// Registered at static init, read-only afterwards: the generated code asks for every hooked instruction it runs.

namespace {
std::unordered_map<uint64_t, HleFn>& hooks() {
    static std::unordered_map<uint64_t, HleFn> h;
    return h;
}
}  // namespace

HookRegistrar::HookRegistrar(uint64_t offset, HleFn fn) { hooks()[offset] = fn; }

}  // namespace nx

// Called by the generated code (declared there: recomp/emit.py).
NX_GAME_API void nx_hook(nx::CPU& c, uint64_t pc) {
    auto it = nx::hooks().find(pc - nx::kModuleBase);
    if (it != nx::hooks().end()) [[likely]]
        it->second(c);
    else {
        static std::atomic<int> logged{0};
        if (logged++ < 8) nx::log("hook at %s compiled in, nothing registered for it", nx::describe(pc).c_str());
    }
}

namespace nx {

static void missing_stub(CPU& c) {
    // Name comes from the thunk that dispatched here.
    log("missing import called: %s (lr %s) -> 0", t_missing_name, describe(c.x[30]).c_str());
    c.x[0] = 0;
}

uint64_t hle_address_or(const char* name, HleFn fallback) {
    static std::mutex lock;  // NVN hands out procs by name at run time, from any thread
    std::lock_guard lk(lock);
    // Other threads index g_thunks while we add to it: never reallocate (one entry per 16 bytes of the region).
    if (g_thunks.capacity() < kThunkSize / 16) g_thunks.reserve(kThunkSize / 16);
    auto it = g_thunk_by_name.find(name);
    if (it != g_thunk_by_name.end()) return it->second;
    if (g_thunks.size() == kThunkSize / 16) fatal("out of HLE thunks");
    HleFn fn = hle_lookup(name);
    uint64_t addr = kThunkBase + g_thunks.size() * 16;
    g_thunks.push_back({name, fn ? fn : fallback, fn && is_leaf(fn)});
    g_thunk_by_name[name] = addr;
    return addr;
}

uint64_t hle_address(const char* name) { return hle_address_or(name, missing_stub); }

const char* hle_current_name() { return t_missing_name; }

// ---- dispatch -------------------------------------------------------------------------------------------------

const LoadedModule* module_at(uint64_t addr) {
    for (auto& m : g_modules)
        if (addr - m.base < m.size) return &m;
    return nullptr;
}

std::string describe(uint64_t addr) {
    char buf[512];
    if (addr - kThunkBase < kThunkSize) {
        size_t i = (addr - kThunkBase) / 16;
        snprintf(buf, sizeof buf, "hle:%s", i < g_thunks.size() ? g_thunks[i].name.c_str() : "?");
        return buf;
    }
    const LoadedModule* m = module_at(addr);
    if (!m) {
        snprintf(buf, sizeof buf, "%llx", (unsigned long long)addr);
        return buf;
    }
    uint64_t off = addr - m->base;
    // Nearest preceding function symbol.
    const Symbol* best = nullptr;
    for (size_t k = 0; k < m->info->symbol_count; k++) {
        const Symbol& s = m->info->symbols[k];
        if (s.imported || s.type != 2 || s.value > off) continue;
        if (!best || s.value > best->value) best = &s;
    }
    if (best && off - best->value < 0x10000)
        snprintf(buf, sizeof buf, "%s+%llx (%s+%llx)", m->info->name, (unsigned long long)off, best->name,
                 (unsigned long long)(off - best->value));
    else
        snprintf(buf, sizeof buf, "%s+%llx", m->info->name, (unsigned long long)off);
    return buf;
}

void backtrace(CPU& c);

// Indirect calls (every blr of the game) look the target up here: the modules' text ranges in a small flat array.
// Each module's functions are sorted by offset; `block[off / 64]` is the index of the first function at or after
// that 64-byte block, so a lookup reads one index and one or two entries. A dense pointer per instruction would
// cost twice the text size in RAM (150 MB for ACNH's main).
struct FastModule {
    uint64_t base, text_size;
    const FuncEntry* funcs;
    const uint32_t* block;
};
FastModule g_fast[4];
size_t g_fast_count = 0;

Fn fast_lookup(const FastModule& m, uint64_t off) {
    const FuncEntry* f = m.funcs + m.block[off >> 6];
    while (f->offset < off) f++;
    return f->offset == off ? f->fn : nullptr;
}

// In front of fast_lookup: a direct-mapped cache of recent targets (virtual calls in the game's loops hit the same
// few hundred functions; the index and entry arrays above are megabytes, two dependent cache misses per call).
// An entry is one 64-bit word, so threads read and write it without a lock and never see half of one: the high half
// is the target's key ((target - g_cache_base) / 4 + 1, 0 = empty), the low half the host function's offset from
// kCallAnchor (all generated code is in one binary). NX_CALL_CACHE=0: off.
// 65536 entries, 512 KB: with 8192 a scene load (the game's loader threads walk thousands of functions) missed often
// enough that call_slow took 11% of a loader thread on the S20 FE; in play the hot entries are as few lines either way.
constexpr unsigned kCallCacheBits = 16;
std::atomic<uint64_t> g_call_cache[1u << kCallCacheBits];
uint64_t g_cache_base = 0, g_cache_span = 0;  // modules within [base, base + span); span 0: the cache is off

#ifdef NX_GAME_DYNAMIC
// The game's code is another library (Android), anywhere in the address space: the offsets are from its first
// function instead, set by load_modules.
uintptr_t g_anchor = 0;
inline uintptr_t anchor() { return g_anchor; }
#else
void call_anchor(CPU&) {}
inline uintptr_t anchor() { return reinterpret_cast<uintptr_t>(&call_anchor); }
#endif

inline uint32_t cache_slot(uint64_t rel) {
    return uint32_t((uint32_t(rel >> 2) * 0x9E3779B1u) >> (32 - kCallCacheBits));
}

[[gnu::noinline]] void call_slow(CPU& c, uint64_t t) {
    for (size_t k = 0; k < g_fast_count; k++) {
        const FastModule& m = g_fast[k];
        uint64_t off = t - m.base;
        if (off < m.text_size) [[likely]] {
            Fn fn = fast_lookup(m, off);
            if (!fn) [[unlikely]] {
                for (int k = 0; k < 31; k += 4)
                    log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k],
                        (unsigned long long)c.x[k + 1], (unsigned long long)c.x[k + 2],
                        (unsigned long long)(k + 3 < 31 ? c.x[k + 3] : c.x[31]));
                backtrace(c);
                fatal("call into the middle of a function: %s (lr %s)", describe(t).c_str(), describe(c.x[30]).c_str());
            }
            uint64_t rel = t - g_cache_base;
            int64_t delta = int64_t(reinterpret_cast<uintptr_t>(fn) - anchor());
            if (rel < g_cache_span && !(rel & 3) && delta >= INT32_MIN && delta <= INT32_MAX)
                g_call_cache[cache_slot(rel)].store((uint64_t((rel >> 2) + 1) << 32) | uint32_t(int32_t(delta)),
                                                    std::memory_order_relaxed);
            fn(c);
            return;
        }
    }
    uint64_t off = t - kThunkBase;
    if (off < kThunkSize && off / 16 < g_thunks.size()) {
        const Thunk& th = g_thunks[off / 16];
        if (th.leaf && !g_trace) {
            th.fn(c);
            return;
        }
        if (g_trace) log("hle %s", th.name.c_str());
        const char* prev = t_missing_name;
        t_missing_name = th.name.c_str();
        th.fn(c);
        t_missing_name = prev;
        return;
    }
    fatal("call to %llx (%s), lr %s", (unsigned long long)t, describe(t).c_str(), describe(c.x[30]).c_str());
}

NX_GAME_API void call(CPU& c, uint64_t t) {
    uint64_t rel = t - g_cache_base;
    if (rel < g_cache_span) [[likely]] {
        uint64_t e = g_call_cache[cache_slot(rel)].load(std::memory_order_relaxed);
        if (uint32_t(e >> 32) == uint32_t((rel >> 2) + 1) && !(rel & 3)) [[likely]] {
            reinterpret_cast<Fn>(anchor() + intptr_t(int32_t(uint32_t(e))))(c);
            return;
        }
    }
    call_slow(c, t);
}

NX_GAME_API void trace_sp(CPU& c, uint64_t pc) {
    log("sp %llx x19 %llx at %s", (unsigned long long)c.x[31], (unsigned long long)c.x[19], describe(pc).c_str());
}

[[noreturn]] NX_GAME_API void unimpl(CPU& c, uint64_t pc, const char* what) {
    for (int k = 0; k < 31; k += 4)
        log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k], (unsigned long long)c.x[k + 1],
            (unsigned long long)c.x[k + 2], (unsigned long long)(k + 3 < 32 ? c.x[k + 3] : 0));
    backtrace(c);
    fatal("unimplemented instruction at %s: %s (lr %s)", describe(pc).c_str(), what, describe(c.x[30]).c_str());
}

// ---- loading and linking --------------------------------------------------------------------------------------

uint64_t find_symbol(const char* name) {
    auto it = g_exports.find(name);
    return it == g_exports.end() ? 0 : it->second;
}

// The lookup index of one module (see FastModule). The entries end with a sentinel past the text, so a scan
// always stops; the recompiler writes them sorted, anything else is sorted here.
FastModule fast_module(const ModuleInfo* mi) {
    auto* funcs = new FuncEntry[mi->func_count + 1];
    std::copy(mi->funcs, mi->funcs + mi->func_count, funcs);
    std::sort(funcs, funcs + mi->func_count, [](const FuncEntry& a, const FuncEntry& b) { return a.offset < b.offset; });
    funcs[mi->func_count] = {~0ull, nullptr};
    size_t blocks = (mi->text_size + 63) / 64;
    auto* block = new uint32_t[blocks];
    size_t k = 0;
    for (size_t b = 0; b < blocks; b++) {
        while (k < mi->func_count && funcs[k].offset < b * 64) k++;
        block[b] = uint32_t(k);
    }
    return {mi->base, mi->text_size, funcs, block};
}

// ---- variants (VariantRegistrar, runtime.h) ---------------------------------------------------------------------

struct Variant {
    const uint32_t* words;
    size_t count;
};
std::unordered_map<std::string, Variant>& variants() {
    static std::unordered_map<std::string, Variant> v;
    return v;
}

}  // namespace nx

// Read by the dispatchers of the --variant functions; set by load_modules (after config.ini), before any guest code.
extern "C" NX_GAME_API bool nx_variant_on = false;
// NX_VARIANT_RANGE=<lo>-<hi> (hex module offsets): only the functions starting there take their patched bodies.
extern "C" NX_GAME_API uint64_t nx_variant_lo = 0, nx_variant_hi = ~0ull;

namespace nx {

NX_GAME_API VariantRegistrar::VariantRegistrar(const char* module, const uint32_t* words, size_t count) {
    variants()[module] = {words, count};
}

void load_modules(const ModuleInfo* const* mods, size_t count, const std::string& data_dir) {
    nx_variant_on = std::getenv("NX_SPEEDFIX") && std::getenv("NX_SPEEDFIX")[0] == '1';
    if (const char* r = std::getenv("NX_VARIANT_RANGE")) {
        unsigned long long lo = 0, hi = 0;
        if (std::sscanf(r, "%llx-%llx", &lo, &hi) == 2) nx_variant_lo = lo, nx_variant_hi = hi;
    }
    const bool variant_code = !(std::getenv("NX_VARIANT_CODE") && std::getenv("NX_VARIANT_CODE")[0] == '0');
    // Map every module first so symbols of all of them are known before relocating.
    for (size_t i = 0; i < count; i++) {
        const ModuleInfo* mi = mods[i];
        uint64_t image = mi->data + mi->data_size;
        uint64_t size = ((mi->bss + mi->bss_size) + 0xFFF) & ~0xFFFull;
        if (!commit(mi->base, size)) fatal("cannot commit module %s", mi->name);
        std::string path = data_dir + "/" + mi->name + ".bin";
        std::ifstream f(path, std::ios::binary);
        if (!f) fatal("missing %s (run tools/nso2bin.py)", path.c_str());
        f.read(reinterpret_cast<char*>(mi->base), std::streamsize(image));
        if (uint64_t(f.gcount()) != image) fatal("%s: expected %llx bytes", path.c_str(), (unsigned long long)image);
        if (auto it = variants().find(mi->name); it != variants().end()) {
            // NX_VARIANT_WORDS=0 / NX_VARIANT_CODE=0: one half only (bisecting).
            static const bool words = !(std::getenv("NX_VARIANT_WORDS") && std::getenv("NX_VARIANT_WORDS")[0] == '0');
            if (nx_variant_on && words)
                for (size_t k = 0; k + 1 < 2 * it->second.count; k += 2)
                    std::memcpy(reinterpret_cast<void*>(mi->base + it->second.words[k]), &it->second.words[k + 1], 4);
            log("loader: %s: variant %s (%zu words)", mi->name, nx_variant_on ? "on" : "off", it->second.count);
        }
        if (!variant_code) nx_variant_on = false;  // the words are in; the functions keep their own bodies
        vm_set(mi->base + mi->text, mi->text_size, kMemCode, 5);
        vm_set(mi->base + mi->ro, mi->ro_size, kMemCode, 1);
        vm_set(mi->base + mi->data, size - mi->data, kMemCodeData, 3);

        g_modules.push_back(LoadedModule{mi, mi->base, size});
#ifdef NX_GAME_DYNAMIC
        if (!g_anchor && mi->func_count) g_anchor = reinterpret_cast<uintptr_t>(mi->funcs[0].fn);
#endif
        if (g_fast_count < 4) g_fast[g_fast_count++] = fast_module(mi);
        {  // the call cache covers every module while they all fit 2^34 bytes (keys are 32-bit words)
            static const bool cache = [] {
                const char* e = std::getenv("NX_CALL_CACHE");
                return !(e && e[0] == '0');
            }();
            uint64_t lo = ~0ull, hi = 0;
            for (size_t k = 0; k < g_fast_count; k++)
                lo = std::min(lo, g_fast[k].base), hi = std::max(hi, g_fast[k].base + g_fast[k].text_size);
            g_cache_span = 0;  // set again below, with the base, before any call (load_modules runs first)
            if (cache && hi - lo < (1ull << 34)) g_cache_base = lo, g_cache_span = hi - lo;
            for (auto& e : g_call_cache) e.store(0, std::memory_order_relaxed);
        }

        for (size_t k = 0; k < mi->symbol_count; k++) {
            const Symbol& s = mi->symbols[k];
            if (s.imported || !s.name[0] || s.bind == 0) continue;  // skip locals
            auto it = g_exports.find(s.name);
            if (it == g_exports.end() || s.bind == 1)  // global beats weak; first global wins
                if (it == g_exports.end()) g_exports[s.name] = mi->base + s.value;
        }
        log("loaded %s at %llx (%llu functions, %llu relocs)", mi->name, (unsigned long long)mi->base,
            (unsigned long long)mi->func_count, (unsigned long long)mi->reloc_count);
    }

    size_t hle = 0, missing = 0, linked = 0;
    for (auto& lm : g_modules) {
        const ModuleInfo* mi = lm.info;
        for (size_t k = 0; k < mi->reloc_count; k++) {
            const Reloc& r = mi->relocs[k];
            uint64_t* slot = reinterpret_cast<uint64_t*>(lm.base + r.offset);
            if (r.type == R_AARCH64_RELATIVE) {
                *slot = lm.base + uint64_t(r.addend);
                continue;
            }
            if (r.type != R_AARCH64_GLOB_DAT && r.type != R_AARCH64_JUMP_SLOT && r.type != R_AARCH64_ABS64) {
                log("%s: relocation type %u at %llx not handled", mi->name, r.type, (unsigned long long)r.offset);
                continue;
            }
            const Symbol& s = mi->symbols[r.sym];
            uint64_t target = 0;
            if (s.type == 2 || s.imported) {
                if (hle_lookup(s.name)) {
                    target = hle_address(s.name);
                    hle++;
                }
            }
            if (!target) {
                target = find_symbol(s.name);
                if (target) linked++;
            }
            if (!target && !s.imported) target = lm.base + s.value;
            if (!target) {
                // Unresolved: functions get a logging stub; data stays null.
                if (s.type == 2 || s.type == 0) target = hle_address(s.name);
                missing++;
            }
            *slot = target + uint64_t(r.addend);
        }
    }
    log("linked: %zu to HLE, %zu across modules, %zu unresolved", hle, linked, missing);
}

}  // namespace nx
