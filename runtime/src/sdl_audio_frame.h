#pragma once

#include <algorithm>

namespace nx::plat {

inline int sdl_audio_frame_chunk(int additional, int capacity, int frame_bytes) {
    int n = std::min(additional, capacity);
    return std::max(frame_bytes, n - n % frame_bytes);
}

}  // namespace nx::plat
