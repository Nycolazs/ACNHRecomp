#pragma once

#include <cstdint>

namespace nx::nvn {

struct PolygonOffset {
    float factor = 0, units = 0, clamp = 0;
};

struct PolygonInfo {
    uint32_t cull = 0, front = 0, mode = 0, offset_enables = 0;

    // NVN has a separate offset enable for points, lines and filled polygons.
    constexpr bool offset_enabled() const {
        uint32_t bit = mode <= 2 ? 1u << mode : 4u;
        return (offset_enables & bit) != 0;
    }

    constexpr PolygonOffset effective_offset(PolygonOffset stored) const {
        return offset_enabled() ? stored : PolygonOffset{};
    }
};

}  // namespace nx::nvn
