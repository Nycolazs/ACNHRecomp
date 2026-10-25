// Temporary diagnostics for docs/NATIVE60.md (NX_NATIVE60_TRACE): what moves the player.
//   <root>/scan1: candidate Vector3f near the camera target (heap scan); <root>/scan2: those that changed since;
//   <root>/watchnow + "<address>" in <root>/watchaddr: writes "<address>" to watch.txt and raises SIGTRAP (watch.py).
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <nx/runtime.h>

namespace nx {

namespace {

std::string root_file(const char* name) {
    static const char* root = std::getenv("NX_DATA_ROOT");
    return std::string(root ? root : ".") + "/" + name;
}

bool take(const char* name) {
    std::string p = root_file(name);
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    std::remove(p.c_str());
    return true;
}

struct Cand {
    uint64_t addr;
    float x, y, z;
};
std::vector<Cand> g_cands;

void scan(float ax, float az) {
    g_cands.clear();
    FILE* m = std::fopen("/proc/self/maps", "r");
    char line[512];
    uint64_t total = 0;
    while (m && std::fgets(line, sizeof line, m)) {
        unsigned long long lo, hi;
        char perms[8];
        if (std::sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) != 3 || perms[0] != 'r' || perms[1] != 'w') continue;
        if (lo < kHeapBase || hi > kHeapBase + kHeapMax) continue;
        total += hi - lo;
        for (uint64_t a = lo; a + 12 <= hi; a += 4) {
            float x = ld<float>(a);
            if (!(std::fabs(x - ax) < 6.0f)) continue;
            float y = ld<float>(a + 4), z = ld<float>(a + 8);
            if (std::fabs(z - az) < 25.0f && y > -5.0f && y < 40.0f && g_cands.size() < 20000) g_cands.push_back({a, x, y, z});
        }
    }
    if (m) std::fclose(m);
    log("native60diag: scan around x %g z %g: %zu candidates in %llu MB", double(ax), double(az), g_cands.size(),
        (unsigned long long)(total >> 20));
}

void rescan() {
    int shown = 0;
    for (const Cand& c : g_cands) {
        float x = ld<float>(c.addr), y = ld<float>(c.addr + 4), z = ld<float>(c.addr + 8);
        float d = std::fabs(x - c.x) + std::fabs(z - c.z);
        if (d > 1.0f && d < 200.0f && std::fabs(y) < 3.0f && (c.addr < 0x502dc00000ull || c.addr > 0x502dd00000ull) && shown < 400) {
            shown++;
            log("native60diag: moved %llx (%g %g %g) -> (%g %g %g)", (unsigned long long)c.addr, double(c.x), double(c.y),
                double(c.z), double(x), double(y), double(z));
        }
    }
    log("native60diag: rescan: %d moved", shown);
}

// sead::LookAtCamera::doUpdateMatrix (main+37e4624): pos at this+0x38, at this+0x44.
void lookat_update(CPU& c) {
    if (!std::getenv("NX_NATIVE60_TRACE")) return;
    uint64_t cam = c.x[0];
    if (cam < kHeapBase || cam >= kHeapBase + kHeapMax) return;
    // The field camera: looks down at a target 10 units above the ground.
    if (std::fabs(ld<float>(cam + 0x48) - 10.0f) > 0.5f || ld<float>(cam + 0x3c) <= ld<float>(cam + 0x48)) return;
    static int calls = 0;
    if (++calls % 30) return;
    if (take("scan1")) scan(ld<float>(cam + 0x44), ld<float>(cam + 0x4c));
    if (take("scan2")) rescan();
    if (take("watchnow")) {
        unsigned long long a[4] = {};
        int n = 0;
        if (FILE* f = std::fopen(root_file("watchaddr").c_str(), "r")) {
            while (n < 4 && std::fscanf(f, "%llx", &a[n]) == 1) n++;
            std::fclose(f);
        }
        if (!n) a[n++] = cam + 0x44;
        FILE* o = std::fopen(root_file("watch.txt").c_str(), "w");
        for (int k = 0; k < n; k++) std::fprintf(o, "%llu ", a[k]), log("native60diag: watching %llx", a[k]);
        std::fprintf(o, "\n");
        std::fclose(o);
        std::raise(SIGTRAP);
    }
}

template <uint64_t F>
void entry_regs(CPU& c) {
    if (!std::getenv("NX_NATIVE60_TRACE")) return;
    static int n = 0;
    if (n++ % 600 == 5)
        log("native60diag: entry main+%llx x0 %llx x1 %llx x2 %llx x3 %llx (lr %s)", (unsigned long long)F,
            (unsigned long long)c.x[0], (unsigned long long)c.x[1], (unsigned long long)c.x[2],
            (unsigned long long)c.x[3], describe(c.x[30]).c_str());
}

}  // namespace

NX_HOOK(0x2216938, entry_regs<0x2216938>);
NX_HOOK(0x226af00, entry_regs<0x226af00>);
NX_HOOK(0x7c6430, entry_regs<0x7c6430>);
NX_HOOK_VERSIONS(0x37e4624, 0x37e4634, lookat_update);  // 3.0.2, 3.0.3

}  // namespace nx
