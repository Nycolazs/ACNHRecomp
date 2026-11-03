// The game's code and the player are built for x86-64-v3 (AVX2, FMA, BMI1/2, F16C, LZCNT, MOVBE: /arch:AVX2,
// -march=x86-64-v3, the Importer's -mcpu=x86_64_v3). On an older CPU they stop at the first such instruction
// (0xc000001d on Windows). The launcher itself is built without them (launcher.cmake), so it can say why instead.
#include "launcher.h"

#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace launcher {

#if defined(__x86_64__) || defined(_M_X64)
namespace {
void cpuid(unsigned leaf, unsigned sub, unsigned r[4]) {
#if defined(_MSC_VER)
    int v[4];
    __cpuidex(v, int(leaf), int(sub));
    for (int i = 0; i < 4; ++i) r[i] = unsigned(v[i]);
#else
    __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
#endif
}
unsigned long long xgetbv0() {
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (unsigned long long)hi << 32 | lo;
#endif
}
}  // namespace

const char* cpu_missing_feature() {
    unsigned r[4];
    cpuid(0, 0, r);
    unsigned max = r[0];
    cpuid(1, 0, r);
    unsigned ecx1 = r[2];
    bool osxsave = ecx1 >> 27 & 1;
    if (!(ecx1 >> 28 & 1) || !osxsave || (xgetbv0() & 6) != 6) return "AVX";
    if (!(ecx1 >> 12 & 1)) return "FMA";
    if (!(ecx1 >> 29 & 1)) return "F16C";
    if (!(ecx1 >> 22 & 1)) return "MOVBE";
    if (max < 7) return "AVX2";
    cpuid(7, 0, r);
    if (!(r[1] >> 5 & 1)) return "AVX2";
    if (!(r[1] >> 3 & 1)) return "BMI1";
    if (!(r[1] >> 8 & 1)) return "BMI2";
    cpuid(0x80000001u, 0, r);
    if (!(r[2] >> 5 & 1)) return "LZCNT";
    return nullptr;
}
#else
const char* cpu_missing_feature() { return nullptr; }
#endif

}  // namespace launcher
