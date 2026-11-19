// The guest can leave SetPolygonOffsetClamp unchanged while binding different polygon states.
#include <cstdio>

#include "nvn_polygon.h"

int main() {
    using namespace nx::nvn;
    const PolygonOffset stored{1.0f, -2.0f, 3.0f};
    PolygonInfo polygon;
    for (uint32_t mode = 0; mode < 3; ++mode) {
        polygon.mode = mode;
        for (uint32_t mask = 0; mask < 8; ++mask) {
            polygon.offset_enables = mask;
            PolygonOffset actual = polygon.effective_offset(stored);
            PolygonOffset expected = (mask & (1u << mode)) ? stored : PolygonOffset{};
            if (actual.factor != expected.factor || actual.units != expected.units || actual.clamp != expected.clamp) {
                std::fprintf(stderr, "mode %u mask %u: got factor %g units %g clamp %g\n",
                             mode, mask, actual.factor, actual.units, actual.clamp);
                return 1;
            }
        }
    }
    return 0;
}
