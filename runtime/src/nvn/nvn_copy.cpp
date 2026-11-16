// NVN copies between textures (docs/NVN.md, N6). nvnCommandBufferCopyTextureToTexture(cmd, const NVNtexture* src,
// const NVNtextureView* srcView, const NVNcopyRegion* srcRegion, const NVNtexture* dst, const NVNtextureView* dstView,
// const NVNcopyRegion* dstRegion, int copyFlags) (Wonder's call sites). NVNcopyRegion {int x, y, z, width, height,
// depth}; for array textures z/depth are layers. Flags: 1 linear filter, 4/8/16 mirror x/y/z (2 selects the 2D
// engine: no difference here). Replayed as a vkCmdCopyImage when formats and sizes match, else a vkCmdBlitImage.
// Wonder copies its frame into a texture for screen transitions (the opening's cloud wipe).
// nvnCommandBufferCopyBufferToTexture(cmd, NVNbufferAddress src, const NVNtexture* dst, const NVNtextureView* dstView,
// const NVNcopyRegion* dstRegion, int flags): texels packed row after row at the source (no copy row stride set;
// ACNH's opening movie copies its decoded NV12 planes into an R8 and an RG8 texture every frame). Replayed from the
// guest bytes as they are when the replay reaches it, through the upload ring, as a vkCmdCopyBufferToImage.
// Copies within one image (another level or layer, or another rectangle of the same one: blur chains, mips, atlases;
// textures over one storage share an image since #36) are replayed with the image in GENERAL, as Vulkan allows when
// the regions do not overlap; overlapping ones are dropped (G10). NX_COPY_NO_SAME_IMAGE=1: all dropped, the old way.
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "nvn_pipeline.h"
#include "nvn_ultrawide.h"
#include "nvn_draw_state.h"
#include "nvn_vk.h"

namespace nx::nvn {

using namespace vk;

namespace {

struct CopyInfo {
    ViewInfo src_view, dst_view;
    int32_t src[6] = {}, dst[6] = {};
    uint32_t flags = 0;
};

void CopyTextureToTexture(CPU& c) {
    CopyInfo info;
    info.src_view = read_view(c, c.x[2]);
    info.dst_view = read_view(c, c.x[5]);
    for (int k = 0; k < 6; k++) {
        info.src[k] = c.x[3] ? ld<int32_t>(c.x[3] + 4ull * k) : 0;
        info.dst[k] = c.x[6] ? ld<int32_t>(c.x[6] + 4ull * k) : 0;
    }
    info.flags = uint32_t(c.x[7]);
    Cmd cmd{Cmd::CopyTextureToTexture};
    cmd.a[0] = c.x[1], cmd.a[1] = c.x[4];
    record(c.x[0], cmd, &info, sizeof info);
}

bool compressed(VkFormat f) {
    return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
}

// A copy within one image keeps it GENERAL, where transition() records nothing: the writes before it (a pass,
// another copy) and the reads after it need a barrier of their own.
void general_barrier(VkCommandBuffer cb, const GpuImage& img) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
}

// Half-open ranges [a0, a0 + an) and [b0, b0 + bn) intersect.
bool ranges_meet(int64_t a0, int64_t an, int64_t b0, int64_t bn) { return a0 < b0 + bn && b0 < a0 + an; }

uint64_t g_same_image = 0, g_same_overlap = 0;

struct BufferCopyInfo {
    ViewInfo view;
    int32_t region[6] = {};
    uint32_t flags = 0;
};

void CopyBufferToTexture(CPU& c) {
    BufferCopyInfo info;
    info.view = read_view(c, c.x[3]);
    for (int k = 0; k < 6; k++) info.region[k] = c.x[4] ? ld<int32_t>(c.x[4] + 4ull * k) : 0;
    info.flags = uint32_t(c.x[5]);
    Cmd cmd{Cmd::CopyBufferToTexture};
    cmd.a[0] = c.x[1], cmd.a[1] = c.x[2];
    record(c.x[0], cmd, &info, sizeof info);
}

// nvnCommandBufferCopyTextureToBuffer(cmd, const NVNtexture* src, const NVNtextureView* srcView,
// const NVNcopyRegion* srcRegion, NVNbufferAddress dst, int flags): the reverse, texels packed row after row at the
// destination. Replayed as a vkCmdCopyImageToBuffer into the upload ring, copied to guest memory once the submit
// completed (a write-back, as compute results are). ACNH renders parts of its characters' textures and reads them
// back this way (main+37fb298).
void CopyTextureToBuffer(CPU& c) {
    BufferCopyInfo info;
    info.view = read_view(c, c.x[2]);
    for (int k = 0; k < 6; k++) info.region[k] = c.x[3] ? ld<int32_t>(c.x[3] + 4ull * k) : 0;
    info.flags = uint32_t(c.x[5]);
    Cmd cmd{Cmd::CopyTextureToBuffer};
    cmd.a[0] = c.x[1], cmd.a[1] = c.x[4];
    record(c.x[0], cmd, &info, sizeof info);
}

}  // namespace

void replay_buffer_copy(VkCommandBuffer cb, const Cmd& c, const CmdList& list) {
    if (c.blob >= list.blobs.size() || list.blobs[c.blob].size() != sizeof(BufferCopyInfo)) return;
    BufferCopyInfo info;
    std::memcpy(&info, list.blobs[c.blob].data(), sizeof info);
    const bool to_buffer = c.op == Cmd::CopyTextureToBuffer;
    const uint64_t texture = to_buffer ? c.a[0] : c.a[1], address = to_buffer ? c.a[1] : c.a[0];
    Texture* t = get<Texture>(texture);
    const uint8_t* src = replay::guest_bytes(address);  // the buffer side, either way
    if (!to_buffer && t && t->uw) {
        // A texture populated by CPU buffer upload (such as movie NV12 Luma/Chroma planes) holds native base_w x base_h
        // texels in the guest buffer; keep the texture at its native dimensions so sampling covers the full image.
        keep_game_size(*t);
    }
    GpuImage* dst = t && src ? ensure_contents(cb, texture) : nullptr;
    auto skip = [&](const char* why) {
        static int logged = 0;
        if (logged++ < 8)
            log("nvn: buffer copy %llx %s texture %llx not replayed: %s", (unsigned long long)address,
                to_buffer ? "<-" : "->", (unsigned long long)texture, why);
    };
    if (!dst) return skip(src ? "no image" : "source not in a pool");
    if (dst->uw && (dst->sx != 1.0f || dst->sy != 1.0f)) {  // a resized screen target: the guest region is the game's
        static int logged = 0;
        if (logged++ < 4)
            log("nvn: buffer copy %s resized screen target %llx (%ux%u, scale %.3f x %.3f): region not scaled",
                to_buffer ? "from" : "to", (unsigned long long)texture, dst->width, dst->height, double(dst->sx), double(dst->sy));
    }
    uint32_t bw = 1, bh = 1, bytes = 0;
    format_block(t->format, bw, bh, bytes);
    uint32_t host = vk_block_bytes(dst->format);
    if (!bytes || (host && host != bytes * bw * bh) || dst->aspect != VK_IMAGE_ASPECT_COLOR_BIT)
        return skip("format stored differently on the host");
    int32_t x = info.region[0], y = info.region[1], w = std::max(info.region[3], 1), h = std::max(info.region[4], 1);
    bool is3d = dst->view_type == VK_IMAGE_VIEW_TYPE_3D;
    int32_t depth = std::max(info.region[5], 1);
    uint64_t rows = (uint64_t(h) + bh - 1) / bh, row = (uint64_t(w) + bw - 1) / bw * bytes;
    uint64_t size = row * rows * uint64_t(depth);
    if (size > (256ull << 20)) return skip("implausible size");
    Upload u = to_buffer ? upload_alloc(size, 256) : upload(src, size, 16);
    uint32_t level = std::min(info.view.base_level, dst->levels - 1);
    // The region inside the image: on a resized screen target smaller than the game's (900 rows for its 1080) the
    // game's region went past the image's edge, which is undefined and hung the GPU on an Adreno 650.
    if (x < 0 || y < 0 || uint64_t(x) + uint64_t(w) > std::max(1u, dst->width >> level) ||
        uint64_t(y) + uint64_t(h) > std::max(1u, dst->height >> level) ||
        (is3d && uint64_t(std::max(info.region[2], 0)) + uint64_t(depth) > std::max(1u, dst->depth >> level)))
        return skip("region past the image's edge");
    uint32_t layer = is3d ? 0 : std::min<uint32_t>(uint32_t(std::max(info.region[2], 0)) + info.view.min_layer, dst->layers - 1);
    VkBufferImageCopy r{};
    r.bufferOffset = u.offset;
    r.imageSubresource = {dst->aspect, level, layer, is3d ? 1u : std::min<uint32_t>(uint32_t(depth), dst->layers - layer)};
    r.imageOffset = {x, y, is3d ? info.region[2] : 0};
    r.imageExtent = {uint32_t(w), uint32_t(h), is3d ? uint32_t(depth) : 1u};
    if (to_buffer) {
        transition(cb, *dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        p_vkCmdCopyImageToBuffer(cb, dst->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, u.buffer, 1, &r);
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = u.buffer, b.offset = u.offset, b.size = size;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &b, 0,
                               nullptr);
        replay::separate_write_backs().push_back({u.ptr, const_cast<uint8_t*>(src), size, nullptr});
        static uint64_t n = 0;
        if (++n == 1 || n % 1000 == 0)
            log("nvn: %llu texture-to-buffer copies (this one %dx%d, %llu bytes, format %#x)", (unsigned long long)n, w,
                h, (unsigned long long)size, t->format);
        return;
    }
    transition(cb, *dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    p_vkCmdCopyBufferToImage(cb, u.buffer, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    dst->rendered = true;  // GPU-written from now on: not uploaded again from the texture's own memory
    static uint64_t n = 0;
    if (++n == 1 || n % 1000 == 0)
        log("nvn: %llu buffer-to-texture copies (this one %dx%d, %llu bytes, format %#x)", (unsigned long long)n, w, h,
            (unsigned long long)size, t->format);
}

void replay_copy(VkCommandBuffer cb, const Cmd& c, const CmdList& list) {
    static const bool off = std::getenv("NX_NO_COPY") != nullptr;  // debug: skip texture copies
    if (off) return;
    if (c.blob >= list.blobs.size() || list.blobs[c.blob].size() != sizeof(CopyInfo)) return;
    CopyInfo info;
    std::memcpy(&info, list.blobs[c.blob].data(), sizeof info);
    GpuImage* src = ensure_contents(cb, c.a[0]);
    GpuImage* dst = ensure_contents(cb, c.a[1]);
    if (!src || !dst) return;
    bool same = src == dst;
    static const bool no_same = std::getenv("NX_COPY_NO_SAME_IMAGE") != nullptr;  // bisecting: the old way
    if (same && no_same) return;
    uint32_t sl = std::min(info.src_view.base_level, src->levels - 1), dl = std::min(info.dst_view.base_level, dst->levels - 1);
    bool s3d = src->view_type == VK_IMAGE_VIEW_TYPE_3D, d3d = dst->view_type == VK_IMAGE_VIEW_TYPE_3D;
    auto sub = [](const GpuImage& img, uint32_t level, int32_t z, int32_t depth, uint32_t view_layer, bool is3d) {
        VkImageAspectFlags aspect = img.aspect;
        uint32_t layer = is3d ? 0 : std::min<uint32_t>(uint32_t(std::max(z, 0)) + view_layer, img.layers - 1);
        uint32_t count = is3d ? 1 : std::clamp<uint32_t>(uint32_t(std::max(depth, 1)), 1, img.layers - layer);
        return VkImageSubresourceLayers{aspect, level, layer, count};
    };
    VkImageSubresourceLayers ss = sub(*src, sl, info.src[2], info.src[5], info.src_view.min_layer, s3d);
    VkImageSubresourceLayers ds = sub(*dst, dl, info.dst[2], info.dst[5], info.dst_view.min_layer, d3d);
    int32_t sw = std::max(info.src[3], 1), sh = std::max(info.src[4], 1);
    int32_t dw = std::max(info.dst[3], 1), dh = std::max(info.dst[4], 1);
    // Resized screen targets (nvn_ultrawide.cpp): the guest's regions, scaled to each image (a blit when the two
    // scales differ).
    auto scale = [](const GpuImage& img, uint32_t level, int32_t& x, int32_t& y, int32_t& w, int32_t& h) {
        if (!img.uw || (img.sx == 1.0f && img.sy == 1.0f)) return;
        uw_span(x, w, std::max(1u, img.width >> level), img.sx);
        uw_span(y, h, std::max(1u, img.height >> level), img.sy);
        w = std::max(w, 1), h = std::max(h, 1);
    };
    scale(*src, sl, info.src[0], info.src[1], sw, sh);
    scale(*dst, dl, info.dst[0], info.dst[1], dw, dh);
    // Both rectangles inside their images: scaled, a region at the edge of a resized target came out as one texel
    // past it (uw_span gives width 0 there, made 1 above), and a copy or blit outside an image is undefined: on an
    // Adreno 650 it can hang the GPU. Clamped; nothing left: dropped.
    auto inside = [](const GpuImage& img, uint32_t level, int32_t& x, int32_t& y, int32_t& w, int32_t& h) {
        const int32_t ew = int32_t(std::max(1u, img.width >> level)), eh = int32_t(std::max(1u, img.height >> level));
        if (x < 0) w += x, x = 0;
        if (y < 0) h += y, y = 0;
        w = std::min(w, ew - x), h = std::min(h, eh - y);
        return w > 0 && h > 0;
    };
    {
        const int32_t sw0 = sw, sh0 = sh, dw0 = dw, dh0 = dh;
        bool ok = inside(*src, sl, info.src[0], info.src[1], sw, sh) && inside(*dst, dl, info.dst[0], info.dst[1], dw, dh);
        // An unscaled copy keeps one size for both sides.
        if (ok && sw0 == dw0 && sh0 == dh0) sw = dw = std::min(sw, dw), sh = dh = std::min(sh, dh);
        if (!ok || sw != sw0 || sh != sh0 || dw != dw0 || dh != dh0) {
            static int logged = 0;
            if (logged++ < 10)
                log("nvn: copy %llx -> %llx past an image's edge: %dx%d -> %dx%d clamped to %dx%d -> %dx%d%s",
                    (unsigned long long)c.a[0], (unsigned long long)c.a[1], sw0, sh0, dw0, dh0, sw, sh, dw, dh,
                    ok ? "" : " (dropped)");
            if (!ok) return;
        }
    }
    if (same) {
        // One subresource and layer range, and the rectangles (and 3D slices) meet: Vulkan forbids it, dropped.
        bool overlap = ss.mipLevel == ds.mipLevel &&
                       ranges_meet(ss.baseArrayLayer, ss.layerCount, ds.baseArrayLayer, ds.layerCount) &&
                       ranges_meet(info.src[0], sw, info.dst[0], dw) && ranges_meet(info.src[1], sh, info.dst[1], dh) &&
                       (!s3d || ranges_meet(info.src[2], std::max(info.src[5], 1), info.dst[2],
                                            std::max(info.dst[5], 1)));
        g_same_image++, g_same_overlap += overlap;
        if (g_same_image == 1 || g_same_image % 1000 == 0 || (overlap && g_same_overlap <= 5))
            log("nvn: %llu copies within one image (%llu overlapping, dropped); this one %llx level %u -> %u, "
                "layer %u -> %u%s",
                (unsigned long long)g_same_image, (unsigned long long)g_same_overlap, (unsigned long long)c.a[0],
                ss.mipLevel, ds.mipLevel, ss.baseArrayLayer, ds.baseArrayLayer, overlap ? " (overlapping)" : "");
        if (overlap) return;
    }
    static const bool trace_cubes = std::getenv("NX_TRACE_CUBES") != nullptr;
    static int cube_logs = 0;
    if (trace_cubes && (dst->view_type == VK_IMAGE_VIEW_TYPE_CUBE || dst->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) &&
        cube_logs++ < 400)
        log("cubes: copy %llx -> %llx level %u layer %u+%u (z %d depth %d view layer %u) %dx%d", (unsigned long long)c.a[0],
            (unsigned long long)c.a[1], ds.mipLevel, ds.baseArrayLayer, ds.layerCount, info.dst[2], info.dst[5],
            info.dst_view.min_layer, info.dst[3], info.dst[4]);
    static int logged_3d = 0;
    static const bool trace_3d = std::getenv("NX_TRACE_3D") != nullptr;
    if (trace_3d && (s3d || d3d) && logged_3d++ < 24)
        log("nvn: copy %llx -> %llx with a 3D image: formats %d -> %d, %dx%d z %d depth %d layer %u+%u -> %dx%d z %d depth %d "
            "layer %u+%u", (unsigned long long)c.a[0], (unsigned long long)c.a[1], int(src->format), int(dst->format), sw,
            sh, info.src[2], info.src[5], ss.baseArrayLayer, ss.layerCount, dw, dh, info.dst[2], info.dst[5],
            ds.baseArrayLayer, ds.layerCount);
    VkImageLayout sly = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImageLayout dly = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    transition(cb, *src, sly);
    transition(cb, *dst, dly);
    if (same) general_barrier(cb, *src);
    bool mirror = info.flags & (4 | 8 | 16);
    if (src->format == dst->format && sw == dw && sh == dh && !mirror && ss.layerCount == ds.layerCount) {
        VkImageCopy r{};
        r.srcSubresource = ss, r.dstSubresource = ds;
        r.srcOffset = {info.src[0], info.src[1], s3d ? info.src[2] : 0};
        r.dstOffset = {info.dst[0], info.dst[1], d3d ? info.dst[2] : 0};
        r.extent = {uint32_t(sw), uint32_t(sh), s3d ? uint32_t(std::max(info.src[5], 1)) : 1u};
        p_vkCmdCopyImage(cb, src->image, sly, dst->image, dly, 1, &r);
    } else if (!compressed(src->format) && !compressed(dst->format) && src->aspect == dst->aspect) {
        VkImageBlit b{};
        b.srcSubresource = ss, b.dstSubresource = ds;
        ds.layerCount = ss.layerCount = std::min(ss.layerCount, ds.layerCount);
        b.srcSubresource.layerCount = b.dstSubresource.layerCount = ss.layerCount;
        b.srcOffsets[0] = {info.src[0], info.src[1], 0};
        b.srcOffsets[1] = {info.src[0] + sw, info.src[1] + sh, 1};
        b.dstOffsets[0] = {info.dst[0], info.dst[1], 0};
        b.dstOffsets[1] = {info.dst[0] + dw, info.dst[1] + dh, 1};
        if (info.flags & 4) std::swap(b.dstOffsets[0].x, b.dstOffsets[1].x);
        if (info.flags & 8) std::swap(b.dstOffsets[0].y, b.dstOffsets[1].y);
        bool linear = (info.flags & 1) && src->aspect == VK_IMAGE_ASPECT_COLOR_BIT;
        p_vkCmdBlitImage(cb, src->image, sly, dst->image, dly, 1, &b, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    } else {
        static int logged = 0;
        if (logged++ < 5)
            log("nvn: copy %llx -> %llx not replayed (formats %d -> %d, %dx%d -> %dx%d)", (unsigned long long)c.a[0],
                (unsigned long long)c.a[1], int(src->format), int(dst->format), sw, sh, dw, dh);
        return;
    }
    if (same) general_barrier(cb, *dst);
    dst->rendered = true;  // GPU-written from now on
}

NX_HLE("nvnCommandBufferCopyTextureToTexture", CopyTextureToTexture);
NX_HLE("nvnCommandBufferCopyBufferToTexture", CopyBufferToTexture);
NX_HLE("nvnCommandBufferCopyTextureToBuffer", CopyTextureToBuffer);

}  // namespace nx::nvn
