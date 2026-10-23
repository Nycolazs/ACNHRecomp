#include "nvn_ultrawide.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "nvn.h"
#include "nvn_pipeline.h"
#include "nvn_vk.h"
#include <nx/module.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"

namespace nx::nvn {

namespace {

constexpr double k169 = 16.0 / 9.0;

struct Scale {
    float s = 1, k = 1;
    bool operator==(const Scale& o) const { return s == o.s && k == o.k; }
    bool operator!=(const Scale& o) const { return !(*this == o); }
};
std::mutex g_scale_lock;
Scale g_wanted, g_current;                // under g_scale_lock; g_current written by uw_apply (GPU lock)
uint32_t g_wanted_gen = 0;                // under g_scale_lock
std::map<uint32_t, Scale> g_history;      // wanted generation -> its scale, until applied (under g_scale_lock)
std::atomic<uint32_t> g_allowed{1};       // the newest wanted generation the submit being replayed may apply
std::atomic<uint32_t> g_gen{0};           // the generation of g_current
std::atomic<float> g_k{1.0f}, g_s{1.0f};  // g_current, read without the lock (draws)
std::atomic<float> g_wanted_k{1.0f};      // g_wanted.k (cameras: the frames the game records from now on)

// The environment (scripts, presets) wins over config.ini, which the settings screen changes while the game runs.
std::string setting(const char* env, const char* key, const char* fallback) {
    if (const char* e = std::getenv(env); e && *e) return e;
    return config_get(key, fallback);
}

// NX_WINDOW_SCRIPT=<present>:<w>x<h>,...: from that present on, the window is taken to be that size (headless runs,
// where nothing resizes the hidden window: live resizing checked with dumped frames).
void window_px(uint64_t present, uint32_t& w, uint32_t& h) {
    plat::window_size(w, h);
    static const char* e = std::getenv("NX_WINDOW_SCRIPT");
    for (const char* p = e; p && *p;) {
        unsigned long long at = 0;
        unsigned sw = 0, sh = 0;
        int used = 0;
        if (std::sscanf(p, "%llu:%ux%u%n", &at, &sw, &sh, &used) < 3) break;
        if (present >= at) w = sw, h = sh;
        p += used;
        if (*p == ',') p++;
    }
    if (!w || !h) {  // no window yet (the first targets are made before it shows): its configured size
        unsigned rw = 1280, rh = 720;
        std::string r = setting("NX_RESOLUTION", "video.window", "");
        std::sscanf(r.c_str(), "%ux%u", &rw, &rh);
        w = rw, h = rh;
    }
}

// video.aspect / NX_ULTRAWIDE: "16:9" (or empty, "0": the game's own), "auto" ("window", "1": the window's shape),
// "21:9", "2560x1080", ... k = that aspect over 16:9. video.resolution / NX_RENDER_HEIGHT: "native" (the game's own
// 1080 rows, 720 in handheld mode), a row count for the 16:9 part of the picture (540 .. 2160), or "window" (the
// rows the picture takes in the window).
Scale wanted(uint64_t present) {
    uint32_t ww = 0, wh = 0;
    window_px(present, ww, wh);
    const double window = double(ww) / double(wh);
    std::string a = setting("NX_ULTRAWIDE", "video.aspect", "16:9");
    double aspect = k169, n = 0, d = 0;
    if (a == "auto" || a == "window" || a == "1")
        aspect = window;
    else if ((std::sscanf(a.c_str(), "%lf:%lf", &n, &d) == 2 || std::sscanf(a.c_str(), "%lfx%lf", &n, &d) == 2) && n > 0 && d > 0)
        aspect = n / d;
    double k = std::clamp(aspect / k169, 0.5, 3.0);
    if (std::fabs(k - 1.0) < 0.02) k = 1.0;
    k = std::round(k * 256.0) / 256.0;  // a few distinct values: each makes its own UI pipelines
    const double native = std::getenv("NX_HANDHELD") ? 720.0 : 1080.0;
    std::string r = setting("NX_RENDER_HEIGHT", "video.resolution", "native");
    double s = 1.0;
    if (r == "window") {
        double rows = std::min(double(wh), double(ww) / (k169 * k));  // the picture's rows, fit in the window
        s = rows / (native * std::max(1.0, 1.0 / k));
    } else if (int rows = std::atoi(r.c_str()); rows > 0) {
        s = rows / native;
    }
    s = std::clamp(s, 0.25, 2.0);  // 2: 2160 rows (4K)
    s = std::round(s * native) / native;
    return {float(s), float(k)};
}

}  // namespace

float uw_factor() { return g_k.load(std::memory_order_relaxed); }
uint32_t uw_generation() { return g_gen.load(std::memory_order_acquire); }
bool uw_scaling() { return g_k.load(std::memory_order_relaxed) != 1.0f || g_s.load(std::memory_order_relaxed) != 1.0f; }

namespace {
// The scale the first targets are made with (they are made before the first present).
void init_scale() {
    static std::once_flag once;
    std::call_once(once, [] {
        Scale w = wanted(0);
        std::lock_guard lk(g_scale_lock);
        g_wanted = g_current = w;
        g_wanted_gen = 1;
        g_k = g_wanted_k = w.k, g_s = w.s;
        g_gen = 1;
        log("nvn: screen scale: resolution x%.3f, aspect x%.4f", double(w.s), double(w.k));
    });
}
}  // namespace

void uw_update(uint64_t present) {
    init_scale();
    // A new size is taken once it has held for a quarter of a second: dragging a window's edge makes one change,
    // not one per frame (the present step keeps the old picture's shape meanwhile, with bars).
    static Scale last;
    static uint64_t since = 0;
    static uint64_t next_check = 0;
    uint64_t now = plat::monotonic_ns();
    if (now < next_check) return;
    next_check = now + 50'000'000;
    Scale w = wanted(present);
    if (w != last) {
        last = w, since = now;
        return;
    }
    if (now - since < 250'000'000) return;
    std::lock_guard lk(g_scale_lock);
    if (w == g_wanted) return;
    g_wanted = w;
    g_history[++g_wanted_gen] = w;
    g_wanted_k = w.k;  // the cameras of the next frame the game records (uw_cameras, right after this)
}

uint32_t uw_wanted_generation() {
    init_scale();
    std::lock_guard lk(g_scale_lock);
    return g_wanted_gen;
}
void uw_allow(uint32_t wanted_gen) { g_allowed.store(wanted_gen, std::memory_order_release); }

void uw_apply() {
    init_scale();
    static uint32_t applied = 1;
    const uint32_t allowed = g_allowed.load(std::memory_order_acquire);
    std::lock_guard lk(g_scale_lock);
    if (allowed <= applied) return;
    auto it = g_history.find(allowed);
    if (it == g_history.end()) return;
    const Scale next = it->second;
    g_history.erase(g_history.begin(), std::next(it));
    applied = allowed;
    if (next == g_current) return;
    log("nvn: screen scale: resolution x%.3f -> x%.3f, aspect x%.4f -> x%.4f (screen targets made again)",
        double(g_current.s), double(next.s), double(g_current.k), double(next.k));
    g_current = next;
    g_k = g_current.k, g_s = g_current.s;
    g_gen.fetch_add(1, std::memory_order_acq_rel);
}

bool uw_widen_texture(Texture& t) {
    // Render targets (flags 8), the window textures (display, 1) and the scene colour (flags 0); not packaged assets (4).
    static const bool off = std::getenv("NX_SCREEN_SCALE") && std::getenv("NX_SCREEN_SCALE")[0] == '0';
    if (off || t.target != 1 || (t.flags & 4) || t.width < 16 || t.height < 9) return false;
    // ACNH copies fixed-size NV12 movie planes into these textures. Widening them leaves part of each
    // plane unwritten because the buffer-to-texture copy still covers only the original dimensions.
    if (t.flags == 0 && ((t.format == 0x1 && t.width == 1920 && t.height == 1080) ||
                         (t.format == 0x5 && t.width == 960 && t.height == 540)))
        return false;
    double r = double(t.width) / double(t.height);
    if (std::fabs(r - k169) / k169 > 0.03) return false;
    // The 1920x1080 and 1280x720 chains (halved down to the bloom's 30x17, heights padded: 480x272), not a UI layer
    // that happens to be about 16:9 (318x177).
    bool chain = false;
    for (uint32_t w : {1920u, 1280u})
        for (uint32_t n = 0; n < 8; n++) chain |= t.width == w >> n;
    if (!chain) return false;
    init_scale();
    static std::map<std::string, int> seen;  // each distinct shape once, for the study log
    static std::mutex seen_lock;
    char key[64];
    std::snprintf(key, sizeof key, "%ux%u f%x fmt%x", t.width, t.height, t.flags, t.format);
    if (std::lock_guard lk(seen_lock); seen[key]++ == 0) log("nvn: screen target %s", key);
    t.uw = true;
    t.base_w = t.width, t.base_h = t.height;
    uw_resize(t);
    return true;
}

void uw_resize(Texture& t) {
    if (!t.uw) return;
    Scale c;
    uint32_t gen;
    {
        std::lock_guard lk(g_scale_lock);
        c = g_current, gen = g_gen.load();
    }
    double sx = double(c.s) * std::max(1.0, double(c.k)), sy = double(c.s) * std::max(1.0, 1.0 / double(c.k));
    t.width = std::max(1u, uint32_t(std::lround(t.base_w * sx)));
    t.height = std::max(1u, uint32_t(std::lround(t.base_h * sy)));
    t.uw_gen = gen;
}

void uw_crop(const GpuImage& img, const int32_t in[4], int32_t out[4]) {
    std::memcpy(out, in, 4 * sizeof(int32_t));
    if (!img.uw || in[2] <= 0 || in[3] <= 0) return;
    int32_t x = in[0], w = in[2], y = in[1], h = in[3];
    uw_span(x, w, img.width, img.sx);
    uw_span(y, h, img.height, img.sy);
    out[0] = x, out[1] = y, out[2] = w, out[3] = h;
}

// NX_UW_SCAN=<present>: once, at that present, lists where guest memory (heap, main's data and bss) holds the numbers
// of the 16:9 picture (aspect 16/9 and 9/16 floats, 1920x1080 and 1280x720 as int and float pairs), with context.
void uw_scan(uint64_t present) {
    static const char* e = std::getenv("NX_UW_SCAN");
    static const uint64_t at = e ? std::strtoull(e, nullptr, 10) : 0;
    static bool done = false;
    if (!e || done || present < at) return;
    done = true;
    auto f32 = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
    struct Pat { const char* name; uint32_t a, b; bool pair; };
    const Pat pats[] = {{"f32 16/9", f32(16.f / 9.f), 0, false}, {"f32 9/16", f32(9.f / 16.f), 0, false},
                        {"u32 1920,1080", 1920, 1080, true}, {"f32 1920,1080", f32(1920.f), f32(1080.f), true},
                        {"u32 1280,720", 1280, 720, true}, {"f32 1280,720", f32(1280.f), f32(720.f), true},
                        {"u32 1080,1920", 1080, 1920, true}, {"f32 960,540", f32(960.f), f32(540.f), true}};
    struct Range { uint64_t lo, hi; const char* name; };
    std::vector<Range> ranges{{kHeapBase, kHeapBase + heap_size(), "heap"}};
    if (const LoadedModule* m = module_at(kModuleBase)) {
        const ModuleInfo* mi = m->info;
        ranges.push_back({mi->base + mi->data, mi->base + mi->bss + mi->bss_size, "main data/bss"});
    }
    for (const Range& r : ranges)
        for (const Pat& p : pats) {
            int hits = 0;
            for (uint64_t a = r.lo; a + 8 <= r.hi; a += 4) {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(a);
                if (w[0] != p.a || (p.pair && w[1] != p.b)) continue;
                if (hits++ >= 40) break;
                std::string ctx;
                for (int k = -4; k < 8; k++) {
                    uint64_t q = a + 4 * k;
                    if (q < r.lo || q + 4 > r.hi) continue;
                    uint32_t u = *reinterpret_cast<const uint32_t*>(q);
                    float f;
                    std::memcpy(&f, &u, 4);
                    char b[40];
                    std::snprintf(b, sizeof b, " %s%g/%x", k == 0 ? "[" : "", double(f), u);
                    ctx += b;
                }
                log("uwscan %s %s at %llx (main+%llx):%s", r.name, p.name, (unsigned long long)a,
                    (unsigned long long)(a - kModuleBase), ctx.c_str());
            }
            log("uwscan %s %s: %d hits%s", r.name, p.name, hits, hits > 40 ? "+" : "");
        }
}

// ---- camera aspect -----------------------------------------------------------------------------------------------
// The game draws its views through sead::PerspectiveProjection objects (sead's layout: vptr, mDirty and mDeviceDirty
// at +8/+9, the matrices, then near +0x98, far, fovy +0xA0, sin, cos and tan of fovy / 2, aspect +0xB0). ACNH 3.0.2's
// vtable is main+0x4a65608 (with NX_UW_SCAN every {fovy, sin, cos, tan, 16/9} on the heap sits 0xB0 into an object
// with that vptr); its slot 11, doUpdateMatrix (main+37e5958), makes the projection matrix from near, tan(fovy / 2),
// aspect and offset whenever the object is dirty. An instruction hook on its first instruction (games/acnh/hooks.txt,
// x0 = this) sets the aspect to k * 16/9 before the matrix is made: the Hor+ view (same vertical field of view, wider
// horizontal); a narrower picture (k < 1) also gets a taller field of view, tan(fovy' / 2) = tan(fovy / 2) / k (Vert+).
// The fields are rewritten, not only the matrix, so the game's own getters (culling) agree with it. The objects the
// hook has seen are kept; at each present they are rewritten again and marked dirty when the wanted k changed (a
// window resize), so a view the game does not touch every frame follows too. A projection whose aspect the game set to
// something else (shadow and cube-map views) is left alone; a field of view the game changes is followed.
// (Before the hook, the objects were found by a heap scan every 2 s: 0.7-1.8 s of a core each time.)
namespace {
constexpr uint64_t kProjectionVtable = 0x4a65608;  // sead::PerspectiveProjection's vptr (main+, 3.0.2)
struct Camera {
    uint32_t aspect_set = 0, fov_set = 0;  // what was written last
    uint32_t fov_base = 0;                 // the game's own field of view
};
std::mutex g_cam_lock;
std::unordered_map<uint64_t, Camera> g_cams;  // the sead::PerspectiveProjection objects the hook has seen

float f32_of(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
uint32_t bits_of(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

bool is_projection(uint64_t object) {
    return *reinterpret_cast<volatile uint64_t*>(object) == kModuleBase + kProjectionVtable;
}

// One projection for the wanted k (under g_cam_lock). `dirty`: mark it for sead to make the matrix again.
void rewrite(uint64_t object, Camera& c, float k, bool dirty) {
    const uint32_t asp_bits = 0x3fe38e39u, wide_bits = bits_of(float(16.0 / 9.0) * k);
    auto* w = reinterpret_cast<volatile uint32_t*>(object);
    uint32_t a = w[0xB0 / 4], f = w[0xA0 / 4];
    if (a == asp_bits) c.fov_base = f;            // the game's 16:9 view (new, or set again)
    else if (!c.aspect_set || a != c.aspect_set) return;  // an aspect of the game's own: not ours to change
    else if (f != c.fov_set) c.fov_base = f;      // the game changed the field of view
    uint32_t fov = c.fov_base;
    if (k < 1.0f) fov = bits_of(2.0f * std::atan(std::tan(f32_of(c.fov_base) * 0.5f) / k));
    if (a != wide_bits || f != fov) {  // with the half-angle terms the matrix is made from
        float half = f32_of(fov) * 0.5f;
        w[0xA0 / 4] = fov;
        w[0xB0 / 4] = wide_bits;
        w[0xA4 / 4] = bits_of(std::sin(half));
        w[0xA8 / 4] = bits_of(std::cos(half));
        w[0xAC / 4] = bits_of(std::tan(half));
        if (dirty) {
            reinterpret_cast<volatile uint8_t*>(w)[8] = 1;
            reinterpret_cast<volatile uint8_t*>(w)[9] = 1;
        }
    }
    c.aspect_set = wide_bits, c.fov_set = fov;
}

bool camera_off() {
    static const bool off = std::getenv("NX_UW_CAMERA") && std::getenv("NX_UW_CAMERA")[0] == '0';
    return off;
}

// main+37e5958, sead::PerspectiveProjection::doUpdateMatrix(this = x0, Matrix44f* dst = x1), before its first load.
void projection_update(CPU& c) {
    const uint64_t object = c.x[0];
    if (camera_off() || !is_projection(object)) return;
    const float k = g_wanted_k.load(std::memory_order_relaxed);
    std::lock_guard lk(g_cam_lock);
    auto it = g_cams.find(object);
    if (it == g_cams.end()) {
        if (k == 1.0f) return;  // nothing to change yet: the object is kept from the first view that needs it
        it = g_cams.emplace(object, Camera{}).first;
        if (g_cams.size() == 1 || (g_cams.size() & (g_cams.size() - 1)) == 0)
            log("nvn: screen scale: %zu camera projections seen", g_cams.size());
    }
    rewrite(object, it->second, k, false);  // made right after this: no need to mark it
}
}  // namespace
NX_HOOK(0x37e5958, projection_update);

void uw_cameras(uint64_t present) {
    uw_update(present);
    if (camera_off()) return;
    static float last_k = 1.0f;
    const float k = g_wanted_k.load(std::memory_order_relaxed);  // the frames the game records from now on
    if (k == last_k) return;  // the hook keeps every projection the game updates in step
    last_k = k;
    std::lock_guard lk(g_cam_lock);
    for (auto it = g_cams.begin(); it != g_cams.end();) {
        if (!is_projection(it->first)) {  // freed or reused memory: no longer a projection
            it = g_cams.erase(it);
            continue;
        }
        rewrite(it->first, it->second, k, true);
        ++it;
    }
}

// ---- UI layouts ----------------------------------------------------------------------------------------------------
// The UI is nn::ui2d. Its panes (ACNH 3.0.2's nn::ui2d::Pane, vtable main+474d1f8; Picture main+474d418, Window
// main+474de58) hold: parent +0x18, children list head +0x20 (a child's node at +8), translate +0x30, scale +0x48,
// size +0x50, flags +0x58, basePosition +0x5b, global matrix +0x70, name +0xb0. Pane::Calculate (main+27d4c0, slot
// 0x98; Picture, Window and TextBox call it first) makes the global matrix from them (main+27d74c): a pane whose
// basePosition bits 4-5 / 6-7 name its parent's left/right or top/bottom edge is placed from the parent's size.
// The draws already show the 16:9 layout in proportion in the middle of a wider or taller target (the UI squeeze,
// nvn_draw.cpp: layout units then span the whole target, 1920 * k wide). An instruction hook at Pane::Calculate's
// entry (x0 = the pane) also, at another aspect:
// - gives a screen layout's root pane (no parent, 16:9, 1280 wide or more) the size of the whole target, so the panes
//   anchored to its edges (clock, button hints) go to the edges of the screen;
// - scales the pictures and windows that cover the whole 16:9 layout (backgrounds) up to cover the target (the
//   outermost such pane only), instead of leaving bars beside them.
// Sizes and scales the game (or an animation) sets again are followed, as the cameras' fields are. NX_UW_LAYOUT=0: off.
namespace {
constexpr uint64_t kPictureVtable = 0x474d418, kWindowVtable = 0x474de58, kPartsVtable = 0x474d310;
constexpr uint64_t kGamePictureVtable = 0x4a87220;  // the game's own Picture class (P_BlackBG_00, P_Up_00)
struct PaneFix {
    uint32_t set[2] = {};   // the size (root) or scale (background) written last
    float base[2] = {};     // the game's own
    uint32_t tset[2] = {};  // a background's translate written last, and the game's own
    float tbase[2] = {};
};
std::mutex g_pane_lock;
std::unordered_map<uint64_t, PaneFix> g_panes;

float pane_f(uint64_t pane, uint32_t off) { return f32_of(*reinterpret_cast<volatile uint32_t*>(pane + off)); }

// The pair of floats at `off`: values the game set again (not what we wrote last, `set`) become the `base`; with
// `want`, written as base * want[0..1] (`add`: base + want).
void follow(uint64_t pane, uint32_t off, uint32_t set[2], float base[2], const float* want, bool add = false) {
    auto* w = reinterpret_cast<volatile uint32_t*>(pane + off);
    uint32_t cur[2] = {w[0], w[1]};
    if (cur[0] != set[0] || cur[1] != set[1]) base[0] = f32_of(cur[0]), base[1] = f32_of(cur[1]);
    if (!want) return;
    for (int i = 0; i < 2; i++) {
        uint32_t v = bits_of(add ? base[i] + want[i] : base[i] * want[i]);
        w[i] = v, set[i] = v;
    }
}

bool is_screen_root(uint64_t pane, float w, float h) {
    return *reinterpret_cast<volatile uint64_t*>(pane + 0x18) == 0 && w >= 1279.0f && std::fabs(w / h - 16.0f / 9.0f) < 0.02f;
}

// main+27d4c0, nn::ui2d::Pane::Calculate(this = x0, DrawInfo&, CalculateContext&, bool), before its first instruction.
void pane_calculate(CPU& c) {
    static const bool off = std::getenv("NX_UW_LAYOUT") && std::getenv("NX_UW_LAYOUT")[0] == '0';
    const uint64_t pane = c.x[0];
    const float k = g_wanted_k.load(std::memory_order_relaxed);
    if (off || !pane) return;
    const uint64_t vptr = *reinterpret_cast<volatile uint64_t*>(pane) - kModuleBase;
    const bool root = *reinterpret_cast<volatile uint64_t*>(pane + 0x18) == 0;  // a layout's root pane (any class)
    // Pictures and windows; and a screen mask (a parts pane named L_Mask: what is drawn beside it is cut away).
    const bool picture = vptr == kPictureVtable || vptr == kWindowVtable || vptr == kGamePictureVtable ||
                         (vptr == kPartsVtable && std::memcmp(reinterpret_cast<const char*>(pane + 0xb0), "L_Mask", 6) == 0);
    static std::atomic<bool> touched{false};  // 16:9 since the start: nothing to look at
    if ((!root && !picture) || (k == 1.0f && !touched.load(std::memory_order_relaxed))) return;
    touched = true;
    std::lock_guard lk(g_pane_lock);
    auto it = g_panes.find(pane);
    if (k == 1.0f && it == g_panes.end()) return;  // nothing changed yet
    PaneFix& fix = it != g_panes.end() ? it->second : g_panes[pane];
    const float kx = std::max(1.0f, k), ky = std::max(1.0f, 1.0f / k);
    if (root) {
        // The size the game gave it (followed), widened to the target when it is a screen layout.
        follow(pane, 0x50, fix.set, fix.base, nullptr);
        static const bool trace_roots = std::getenv("NX_UW_LAYOUT_TRACE") != nullptr;
        static std::set<uint64_t> seen_roots;
        if (trace_roots && seen_roots.insert(pane).second) {
            char name[25] = {};
            std::memcpy(name, reinterpret_cast<const char*>(pane + 0xb0), 24);
            log("uwlayout root %llx: %s vt +%llx %.0fx%.0f", (unsigned long long)pane, name, (unsigned long long)vptr,
                double(fix.base[0]), double(fix.base[1]));
        }
        if (!is_screen_root(pane, fix.base[0], fix.base[1])) return void(g_panes.erase(pane));
        const float want[2] = {kx, ky};
        follow(pane, 0x50, fix.set, fix.base, want);
        static const bool trace = std::getenv("NX_UW_LAYOUT_TRACE") != nullptr;  // each screen layout's top panes, once
        static std::set<uint64_t> traced;
        if (trace && traced.insert(pane).second) {
            const uint64_t head = pane + 0x20;
            for (uint64_t n = *reinterpret_cast<volatile uint64_t*>(head + 8); n && n != head;
                 n = *reinterpret_cast<volatile uint64_t*>(n + 8)) {
                const uint64_t ch = n - 8;
                char name[25] = {};
                std::memcpy(name, reinterpret_cast<const char*>(ch + 0xb0), 24);
                log("uwlayout root %llx: %-24s vt +%llx base %02x pos %.0f,%.0f size %.0fx%.0f scale %.2f,%.2f",
                    (unsigned long long)pane, name, (unsigned long long)(*reinterpret_cast<volatile uint64_t*>(ch) - kModuleBase),
                    *reinterpret_cast<volatile uint8_t*>(ch + 0x5b), double(pane_f(ch, 0x30)), double(pane_f(ch, 0x34)),
                    double(pane_f(ch, 0x50)), double(pane_f(ch, 0x54)), double(pane_f(ch, 0x48)), double(pane_f(ch, 0x4c)));
            }
        }
        return;
    }
    // A picture or window that covered the whole 16:9 layout last frame (its global matrix +0x70, layout units), under
    // no other such pane: scaled up about its middle to cover the target.
    follow(pane, 0x48, fix.set, fix.base, nullptr);
    follow(pane, 0x30, fix.tset, fix.tbase, nullptr);
    const float w = pane_f(pane, 0x50), h = pane_f(pane, 0x54);
    const uint8_t basepos = *reinterpret_cast<volatile uint8_t*>(pane + 0x5b);
    const float cx = (basepos & 3) == 1 ? w * 0.5f : (basepos & 3) == 2 ? -w * 0.5f : 0.0f;  // the middle, from the
    const float cy = (basepos >> 2 & 3) == 1 ? -h * 0.5f : (basepos >> 2 & 3) == 2 ? h * 0.5f : 0.0f;  // base point
    uint64_t p = *reinterpret_cast<volatile uint64_t*>(pane + 0x18), top = 0;
    bool nested = false;
    for (int depth = 0; p && depth < 32; depth++) {
        uint64_t parent = *reinterpret_cast<volatile uint64_t*>(p + 0x18);
        if (!parent) top = p;
        else if (auto q = g_panes.find(p); q != g_panes.end() && q->second.tset[0] | q->second.tset[1] | q->second.set[0]) nested = true;
        p = parent;
    }
    auto r = top ? g_panes.find(top) : g_panes.end();
    bool full = r != g_panes.end() && !nested, wide = false, tall = false;
    if (full) {
        const float m00 = pane_f(pane, 0x70), m03 = pane_f(pane, 0x7c), m11 = pane_f(pane, 0x84), m13 = pane_f(pane, 0x8c);
        const float hw = std::fabs(m00) * w * 0.5f, hh = std::fabs(m11) * h * 0.5f;
        const float mx = m03 + m00 * cx, my = m13 + m11 * cy;  // its middle in the layout
        // (90%: a background under a pane an animation scales to 0.94 counts as one)
        const float W = r->second.base[0] * 0.45f, H = r->second.base[1] * 0.45f;
        wide = mx - hw <= -W && mx + hw >= W && hw < W * 4;
        tall = my - hh <= -H && my + hh >= H && hh < H * 4;
        // A band only when it is thin and at an edge (the cutscene bars, 342 rows), not a 600-row panel.
        if (wide && !tall && hh > H * 0.7f) wide = false;
        if (tall && !wide && hw > W * 0.7f) tall = false;
        full = (wide && kx > 1.0f) || (tall && ky > 1.0f) || (wide && tall);
        static const bool trace = std::getenv("NX_UW_LAYOUT_TRACE") != nullptr;
        static std::set<uint64_t> traced;
        if (trace && !full && w >= 1000.0f && traced.insert(pane).second) {
            char name[25] = {};
            std::memcpy(name, reinterpret_cast<const char*>(pane + 0xb0), 24);
            log("uwlayout large %llx: %s vt +%llx %.0fx%.0f mid %.0f,%.0f half %.0fx%.0f", (unsigned long long)pane, name,
                (unsigned long long)vptr, double(w), double(h), double(mx), double(my), double(hw), double(hh));
        }
    }
    if (!full) {
        if (fix.set[0]) {  // scaled before (another aspect, or no longer full-screen): the game's values back
            const float one[2] = {1.0f, 1.0f}, zero[2] = {0.0f, 0.0f};
            follow(pane, 0x48, fix.set, fix.base, one);
            follow(pane, 0x30, fix.tset, fix.tbase, zero, true);
        }
        g_panes.erase(pane);
        return;
    }
    // As large as the layout: scaled evenly to cover the target. As wide (or as tall) only, a band at an edge (the
    // cutscene bars): stretched along it.
    const float cover = std::max(kx, ky);
    const float want[2] = {wide && tall ? cover : wide ? kx : 1.0f, wide && tall ? cover : tall ? ky : 1.0f};
    const float shift[2] = {-(want[0] - 1.0f) * fix.base[0] * cx, -(want[1] - 1.0f) * fix.base[1] * cy};  // middle kept
    follow(pane, 0x48, fix.set, fix.base, want);
    follow(pane, 0x30, fix.tset, fix.tbase, shift, true);
    static const bool trace = std::getenv("NX_UW_LAYOUT_TRACE") != nullptr;
    static std::set<uint64_t> traced;
    if (trace && traced.insert(pane).second) {
        char name[25] = {};
        std::memcpy(name, reinterpret_cast<const char*>(pane + 0xb0), 24);
        log("uwlayout cover %llx: %s %.0fx%.0f base %02x x%.2f", (unsigned long long)pane, name, double(w), double(h), basepos, double(cover));
    }
}
}  // namespace
NX_HOOK(0x27d4c0, pane_calculate);

UwTarget g_uw;

void uw_span(int32_t& x, int32_t& w, uint32_t extent, float f) {
    int64_t x0 = std::llround(double(x) * f);
    int64_t ww = std::llround(double(std::max(0, w)) * f);
    if (std::llabs(int64_t(extent) - ww) <= 2 && x == 0) ww = extent;  // the whole target
    if (x0 + ww > int64_t(extent)) ww = std::max<int64_t>(0, int64_t(extent) - x0);
    x = int32_t(x0), w = int32_t(ww);
}

namespace {
std::mutex g_ui_lock;
std::set<const ShaderInfo*> g_ui_vs;
}  // namespace
void uw_ui_vertex_shader(const ShaderInfo* vs) {
    std::lock_guard lk(g_ui_lock);
    if (g_ui_vs.insert(vs).second) log("nvn: screen scale: UI vertex shader %s", vs->hash.c_str());
}
bool uw_is_ui_vertex_shader(const ShaderInfo* vs) {
    std::lock_guard lk(g_ui_lock);
    return g_ui_vs.count(vs) != 0;
}

bool uw_vs_scaled(const ShaderInfo* vs) {
    static std::map<const ShaderInfo*, bool> cache;
    static const std::string off = std::getenv("NX_UW_NOSCALE") ? std::getenv("NX_UW_NOSCALE") : "";
    static const std::string only = std::getenv("NX_UW_SCALE") ? std::getenv("NX_UW_SCALE") : "";
    auto it = cache.find(vs);
    if (it != cache.end()) return it->second;
    bool scaled = false;
    for (const DescriptorBinding& b : vs->refl.bindings)
        if (b.type == DescriptorType::UniformBuffer && b.name != "support_buffer") scaled = true;
    if (!off.empty() && off.find(vs->hash) != std::string::npos) scaled = false;
    if (!only.empty()) scaled = only.find(vs->hash) != std::string::npos;
    return cache[vs] = scaled;
}


// ---- gl_Position.xy * spec constants (modelled on patch_clip_z, nvn_spirv_clip.cpp) ----------------------------------

namespace {
enum : uint32_t {
    OpEntryPoint = 15, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpSpecConstant = 50,
    OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65,
    OpDecorate = 71, OpMemberDecorate = 72, OpCompositeExtract = 81, OpCompositeInsert = 82, OpConstant = 43,
    OpFMul = 133, OpReturn = 253, OpStructType = 30,
    DecorationBuiltIn = 11, DecorationSpecId = 1, BuiltInPosition = 0, StorageOutput = 3, ModelVertex = 0,
};
bool is_header_op(uint32_t op) {  // before the types: capabilities .. annotations
    switch (op) {
        case 17: case 10: case 11: case 14: case 15: case 16: case 330: case 3: case 4: case 5: case 6: case 7: case 8:
        case 71: case 72: case 73: case 74: case 75: return true;
        default: return false;
    }
}
struct Inst { size_t at; uint32_t op, len; };
bool split(const std::vector<uint32_t>& in, std::vector<Inst>& insts) {
    if (in.size() < 5) return false;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    return true;
}
}  // namespace

bool patch_x_scale(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    std::vector<Inst> insts;
    if (!split(in, insts)) return false;
    uint32_t bound = in[3], entry = 0, f32 = 0, u32 = 0, v4f = 0, ptr_v4f = 0, zero = 0;
    uint32_t block = 0, member = 0, position_var = 0;
    std::map<uint32_t, uint32_t> struct_ptr;
    size_t first_function = in.size(), first_type = in.size();
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (first_type == in.size() && !is_header_op(n.op)) first_type = n.at;
        if (n.op == OpEntryPoint && n.len >= 3 && w[1] == ModelVertex && !entry) entry = w[2];
        else if (n.op == OpMemberDecorate && n.len >= 5 && w[3] == DecorationBuiltIn && w[4] == BuiltInPosition)
            block = w[1], member = w[2];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationBuiltIn && w[3] == BuiltInPosition)
            position_var = w[1];
        else if (n.op == OpTypeFloat && n.len == 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeInt && n.len == 4 && w[2] == 32 && w[3] == 0) u32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4 && w[2] == f32 && w[3] == 4) v4f = w[1];
        else if (n.op == OpTypePointer && n.len == 4 && w[2] == StorageOutput) {
            if (w[3] == v4f && v4f) ptr_v4f = w[1];
            struct_ptr[w[1]] = w[3];
        } else if (n.op == OpConstant && n.len == 4 && u32 && w[1] == u32 && w[3] == member) {
            zero = w[2];
        } else if (n.op == OpFunction && first_function == in.size()) {
            first_function = n.at;
        }
    }
    if (!entry || !f32 || !v4f || first_type == in.size()) return false;
    uint32_t var = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op != OpVariable || n.len < 4 || w[3] != StorageOutput) continue;
        auto p = struct_ptr.find(w[1]);
        if (p == struct_ptr.end()) continue;
        if (block && p->second == block) var = w[2];
        if (!block && w[2] == position_var && p->second == v4f) var = w[2];
    }
    if (!var) return false;
    uint32_t spec_x = bound++, spec_y = bound++;
    std::vector<uint32_t> globals;
    if (!ptr_v4f) ptr_v4f = bound++, globals.insert(globals.end(), {(4u << 16) | OpTypePointer, ptr_v4f, StorageOutput, v4f});
    if (block) {
        if (!u32) u32 = bound++, globals.insert(globals.end(), {(4u << 16) | OpTypeInt, u32, 32, 0});
        if (!zero) zero = bound++, globals.insert(globals.end(), {(4u << 16) | OpConstant, u32, zero, member});
    }
    globals.insert(globals.end(), {(4u << 16) | OpSpecConstant, f32, spec_x, 0x3f800000u});
    globals.insert(globals.end(), {(4u << 16) | OpSpecConstant, f32, spec_y, 0x3f800000u});

    out.assign(in.begin(), in.begin() + 5);
    uint32_t function = 0;
    int returns = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.at == first_type) {
            out.insert(out.end(), {(4u << 16) | OpDecorate, spec_x, DecorationSpecId, 0u});
            out.insert(out.end(), {(4u << 16) | OpDecorate, spec_y, DecorationSpecId, 1u});
        }
        if (n.at == first_function) out.insert(out.end(), globals.begin(), globals.end());
        if (n.op == OpFunction) function = w[2];
        if (n.op == OpReturn && function == entry) {
            uint32_t p = var;
            if (block) {
                p = bound++;
                out.insert(out.end(), {(5u << 16) | OpAccessChain, ptr_v4f, p, var, zero});
            }
            uint32_t v = bound++, x = bound++, mx = bound++, y = bound++, my = bound++, r1 = bound++, r = bound++;
            out.insert(out.end(), {(4u << 16) | OpLoad, v4f, v, p});
            out.insert(out.end(), {(5u << 16) | OpCompositeExtract, f32, x, v, 0});
            out.insert(out.end(), {(5u << 16) | OpFMul, f32, mx, x, spec_x});
            out.insert(out.end(), {(5u << 16) | OpCompositeExtract, f32, y, v, 1});
            out.insert(out.end(), {(5u << 16) | OpFMul, f32, my, y, spec_y});
            out.insert(out.end(), {(6u << 16) | OpCompositeInsert, v4f, r1, mx, v, 0});
            out.insert(out.end(), {(6u << 16) | OpCompositeInsert, v4f, r, my, r1, 1});
            out.insert(out.end(), {(3u << 16) | OpStore, p, r});
            returns++;
        }
        if (n.op == OpFunctionEnd) function = 0;
        out.insert(out.end(), w, w + n.len);
    }
    if (!returns) return false;
    out[3] = bound;
    return true;
}

}  // namespace nx::nvn
