// nn::image::JpegEncoder::SetPixelData: pictures the game draws, copies to memory on the GPU and encodes as JPEG.
// ACNH does this for the Custom Designs Portal: when a design is posted it draws a 240x240 and a 614x614 picture
// (net_image, preview_image), copies each into memory with nvnCommandBufferCopyTextureToBuffer, waits on a sync and
// encodes. Pipelined, the sync does not wait for that copy: it may not even be replayed yet, and it reaches guest
// memory only when its submit is retired (a late write-back, nvn_frames.cpp). The encoder read the memory before the
// copy landed: a black, garbled band at the top of each picture. Before the SDK's SetPixelData, the submits made so
// far are replayed (as nvnQueueFinish does) and the ones whose write-backs cover the pixels retired.
#include <algorithm>
#include <cstdint>

#include <nx/runtime.h>

#include "nvn/nvn.h"

namespace nx {

namespace {

// Bytes of a width x height picture whose rows are rounded up to lineAlignment pixels, 4 bytes each (RGBA8: the
// formats the game uses).
uint64_t picture_bytes(int32_t width, int32_t height, int32_t align) {
    const uint64_t a = align > 0 ? uint64_t(align) : 1;
    return (uint64_t(width) + a - 1) / a * a * 4 * uint64_t(height);
}

// Result SetPixelData(JpegEncoder*, const void* pixels, PixelFormat, const Dimension& {width, height}, int lineAlignment)
void SetPixelData(CPU& c) {
    static const uint64_t real = find_symbol("_ZN2nn5image11JpegEncoder12SetPixelDataEPKvNS0_11PixelFormatERKNS0_9DimensionEi");
    const uint64_t pixels = c.x[1], dim = c.x[3];
    const int32_t align = int32_t(c.x[4]);
    const int32_t width = dim ? ld<int32_t>(dim) : 0, height = dim ? ld<int32_t>(dim + 4) : 0;
    if (pixels && width > 0 && height > 0 && width <= 8192 && height <= 8192) {
        const uint64_t size = picture_bytes(width, height, align);
        nvn::wait_submits_so_far(nvn::kWaitFinish);
        nvn::worker_finish();
        const bool waited = nvn::cpu_read_after_write_backs(reinterpret_cast<const uint8_t*>(pixels), size);
        log("jpeg: SetPixelData %dx%d at %llx%s", width, height, (unsigned long long)pixels,
            waited ? ": waited for the GPU's copy to land" : "");
    }
    call(c, real);
}

// Result Decode(JpegDecoder*, void* pixels, size_t size, int lineAlignment): Happy Home Paradise decodes each photo
// straight into a texture's memory, a target it cleared on the GPU first. The CPU's pixels are uploaded over that.
void Decode(CPU& c) {
    static const uint64_t real = find_symbol("_ZN2nn5image11JpegDecoder6DecodeEPvmi");
    // Dimension GetAnalyzedDimension() const: the decoded picture's size, which may be smaller than the buffer.
    static const uint64_t dimension = find_symbol("_ZNK2nn5image11JpegDecoder20GetAnalyzedDimensionEv");
    const uint64_t pixels = c.x[1], buffer = c.x[2];
    const int32_t align = int32_t(c.x[3]);
    uint64_t size = buffer;  // without GetAnalyzedDimension: the whole buffer
    if (dimension) {
        const uint64_t d = nvn::guest_call(c, dimension, {c.x[0]});  // {int32 width, int32 height} in x0
        const int32_t w = int32_t(uint32_t(d)), h = int32_t(uint32_t(d >> 32));
        if (w > 0 && h > 0 && w <= 8192 && h <= 8192) size = std::min(size, picture_bytes(w, h, align));
    }
    call(c, real);
    static int logged = 0;
    if (logged++ < 8) log("jpeg: Decode %llu bytes (buffer %llu) at %llx: result %#x", (unsigned long long)size,
                          (unsigned long long)buffer, (unsigned long long)pixels, uint32_t(c.x[0]));
    if (pixels && size && uint32_t(c.x[0]) == 0) textures_written(pixels, size);
}

}  // namespace

NX_HLE("_ZN2nn5image11JpegEncoder12SetPixelDataEPKvNS0_11PixelFormatERKNS0_9DimensionEi", SetPixelData);
NX_HLE("_ZN2nn5image11JpegDecoder6DecodeEPvmi", Decode);

}  // namespace nx
