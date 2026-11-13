// nn::image::JpegEncoder::SetPixelData: pictures the game draws, copies to memory on the GPU and encodes as JPEG.
// ACNH does this for the Custom Designs Portal: when a design is posted it draws a 240x240 and a 614x614 picture
// (net_image, preview_image), copies each into memory with nvnCommandBufferCopyTextureToBuffer, waits on a sync and
// encodes. Pipelined, the sync does not wait for that copy: it may not even be replayed yet, and it reaches guest
// memory only when its submit is retired (a late write-back, nvn_frames.cpp). The encoder read the memory before the
// copy landed: a black, garbled band at the top of each picture. Before the SDK's SetPixelData, the submits made so
// far are replayed (as nvnQueueFinish does) and the ones whose write-backs cover the pixels retired.
#include <cstdint>

#include <nx/runtime.h>

#include "nvn/nvn.h"

namespace nx {

namespace {

// Result SetPixelData(JpegEncoder*, const void* pixels, PixelFormat, const Dimension& {width, height}, int lineAlignment)
void SetPixelData(CPU& c) {
    static const uint64_t real = find_symbol("_ZN2nn5image11JpegEncoder12SetPixelDataEPKvNS0_11PixelFormatERKNS0_9DimensionEi");
    const uint64_t pixels = c.x[1], dim = c.x[3];
    const int32_t align = int32_t(c.x[4]);
    const int32_t width = dim ? ld<int32_t>(dim) : 0, height = dim ? ld<int32_t>(dim + 4) : 0;
    if (pixels && width > 0 && height > 0 && width <= 8192 && height <= 8192) {
        // Rows of `width` rounded up to lineAlignment pixels, 4 bytes each (the formats the game uses).
        const uint64_t a = align > 0 ? uint64_t(align) : 1;
        const uint64_t size = (uint64_t(width) + a - 1) / a * a * 4 * uint64_t(height);
        nvn::wait_submits_so_far(nvn::kWaitFinish);
        nvn::worker_finish();
        const bool waited = nvn::cpu_read_after_write_backs(reinterpret_cast<const uint8_t*>(pixels), size);
        log("jpeg: SetPixelData %dx%d at %llx%s", width, height, (unsigned long long)pixels,
            waited ? ": waited for the GPU's copy to land" : "");
    }
    call(c, real);
}

}  // namespace

NX_HLE("_ZN2nn5image11JpegEncoder12SetPixelDataEPKvNS0_11PixelFormatERKNS0_9DimensionEi", SetPixelData);

}  // namespace nx
