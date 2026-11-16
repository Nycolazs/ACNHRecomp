/* __builtin_cpu_supports (nx/cpu.h: the AES-NI path) reads __cpu_model, which a program gets from libgcc or
 * compiler-rt linked statically. The game's library links no runtime of its own (gamelib.h), so it carries this:
 * the feature bit it asks for (AES, compiler-rt's FEATURE_AES = 18 of __cpu_features[0]), set at load. Weak: a
 * runtime's own definition wins. */
#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>

struct __processor_model {
    unsigned int __cpu_vendor;
    unsigned int __cpu_type;
    unsigned int __cpu_subtype;
    unsigned int __cpu_features[1];
};
__attribute__((weak, visibility("hidden"))) struct __processor_model __cpu_model;
__attribute__((weak, visibility("hidden"))) unsigned int __cpu_features2[3];

__attribute__((constructor(101))) static void nx_cpu_model_init(void) {
    unsigned int a, b, c, d;
    if (__get_cpuid(1, &a, &b, &c, &d) && (c & (1u << 25))) __cpu_model.__cpu_features[0] |= 1u << 18;
}
#endif
