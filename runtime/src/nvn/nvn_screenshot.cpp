// F12: the presented frame (before the settings screen is drawn over it) to <data root>/screenshots/acnh_<time>.png.
// The phone camera (hle_album.cpp): the texture the game drew the photo into, read back the same way, to <stem>.png.
// With an HDR swapchain the PNG is HDR: 16 bits a channel, BT.2020 primaries, PQ, and the cICP chunk saying so (9, 16,
// 0, 1), which Chrome and Discord show in HDR. scRGB (RGBA16F, linear BT.709, 1 = 80 nits) is converted; HDR10
// (A2B10G10R10, already BT.2020 PQ) is widened. An SDR swapchain gives an 8-bit sRGB PNG. Compressed with zlib when
// the build has it (NX_HAVE_ZLIB), else stored (~22 MB at 2560x1440 HDR).
//
// The copy is recorded into the present's command buffer; the image is written on a thread once the frames in flight
// are past it (the readback is 16 presents old: more than the slots nvn_frames.cpp keeps).
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(NX_HAVE_ZLIB)
#include <zlib.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>  // PlaySound (winmm): the shutter
#endif

#include "nvn_vk.h"
#include "platform.h"
#include "ui/ui.h"

namespace nx::nvn {

using namespace vk;

int hdr_ui_params(float& ui_nits, bool& gamma22);  // nvn_present.cpp: 0 SDR, 1 scRGB, 2 HDR10
int hdr_tonemap_mode();                            // 0 vanilla, 1 HDR, 2 RenoDRT

namespace {

std::atomic<bool> g_wanted{false};
struct Pending {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint64_t at = 0;  // the present it was recorded in
    // With HDR: the tonemap when it was taken, and the UI's white and gamma. Vanilla's picture never passes the game's
    // white, so it is saved as the SDR picture it is (an 8-bit PNG to compare with the HDR ones).
    int tonemap = 1;
    float ui_nits = 203;
    bool gamma22 = true;
    std::string stem;  // the file's name without .png; empty: acnh_<time>_<look>
} g_pending;
std::mutex g_stem_lock;
std::string g_wanted_stem;  // the next capture's name (screenshot_request)
Pending g_album;                          // the camera's photo being read back
std::atomic<uint64_t> g_album_texture{0};  // the NVNtexture to read back at the next present (album_capture)
std::string g_album_stem;                 // its name, under g_stem_lock
uint64_t g_presents = 0;

#include "nvn_pq_icc.inc"

uint32_t crc32_of(const uint8_t* p, size_t n, uint32_t c = 0xffffffffu) {
    static uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t k = 0; k < 256; k++) {
            uint32_t v = k;
            for (int b = 0; b < 8; b++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            table[k] = v;
        }
        made = true;
    }
    for (size_t k = 0; k < n; k++) c = table[(c ^ p[k]) & 0xff] ^ (c >> 8);
    return c;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.insert(v.end(), {uint8_t(x >> 24), uint8_t(x >> 16), uint8_t(x >> 8), uint8_t(x)});
}

void chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
    put32(png, uint32_t(data.size()));
    const size_t at = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    put32(png, crc32_of(&png[at], png.size() - at) ^ 0xffffffffu);
}

// The zlib stream of the filtered rows: deflate when the build has zlib, else stored blocks.
std::vector<uint8_t> zlib_of(const std::vector<uint8_t>& raw) {
#if defined(NX_HAVE_ZLIB)
    uLongf size = compressBound(uLong(raw.size()));
    std::vector<uint8_t> deflated(size);
    if (compress2(deflated.data(), &size, raw.data(), uLong(raw.size()), 9) == Z_OK) return deflated.resize(size), deflated;
#endif
    std::vector<uint8_t> out = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t x : raw) a = (a + x) % 65521, b = (b + a) % 65521;
    for (size_t at = 0; at < raw.size() || at == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        const bool last = at + n >= raw.size();
        out.insert(out.end(), {uint8_t(last ? 1 : 0), uint8_t(n), uint8_t(n >> 8), uint8_t(~n), uint8_t(~n >> 8)});
        out.insert(out.end(), raw.begin() + at, raw.begin() + at + n);
        at += n;
        if (last) break;
    }
    put32(out, (b << 16) | a);
    return out;
}

// A camera shutter, made here (no sample shipped): two clicks 70 ms apart, each a burst of decaying noise over a short
// 2.6 kHz ring, as a 16-bit mono WAV in memory, played once without waiting. Windows only (PlaySound).
void play_shutter() {
#if defined(_WIN32)
    static std::vector<uint8_t> wav = [] {
        const uint32_t rate = 44100, n = rate * 180 / 1000;
        std::vector<int16_t> s(n);
        uint32_t seed = 12345;
        for (uint32_t i = 0; i < n; i++) {
            float v = 0;
            for (float at : {0.0f, 0.07f}) {
                const float t = float(i) / float(rate) - at;
                if (t < 0) continue;
                seed = seed * 1664525u + 1013904223u;
                const float noise = float(int32_t(seed >> 8) - (1 << 23)) / float(1 << 23);
                const float ring = std::sin(2.0f * 3.14159265f * 2600.0f * t);
                v += (0.55f * noise + 0.45f * ring) * std::exp(-t / 0.012f) * (at == 0 ? 1.0f : 0.8f);
            }
            s[i] = int16_t(std::lround(std::fmax(-1.0f, std::fmin(1.0f, v * 0.5f)) * 32767.0f));
        }
        std::vector<uint8_t> w;
        auto u32 = [&](uint32_t x) { w.insert(w.end(), {uint8_t(x), uint8_t(x >> 8), uint8_t(x >> 16), uint8_t(x >> 24)}); };
        auto u16 = [&](uint16_t x) { w.insert(w.end(), {uint8_t(x), uint8_t(x >> 8)}); };
        const uint32_t data = n * 2;
        w.insert(w.end(), {'R', 'I', 'F', 'F'}), u32(36 + data), w.insert(w.end(), {'W', 'A', 'V', 'E'});
        w.insert(w.end(), {'f', 'm', 't', ' '}), u32(16), u16(1), u16(1), u32(rate), u32(rate * 2), u16(2), u16(16);
        w.insert(w.end(), {'d', 'a', 't', 'a'}), u32(data);
        const uint8_t* b = reinterpret_cast<const uint8_t*>(s.data());
        w.insert(w.end(), b, b + data);
        return w;
    }();
    PlaySoundA(reinterpret_cast<LPCSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
#endif
}

float half_to_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? 0.0f : 65504.0f) : std::ldexp(float(m | 0x400), int(e) - 25);
    return s ? -v : v;
}

float pq(float nits) {  // ST 2084 encode
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float y = std::pow(std::fmin(std::fmax(nits / 10000.0f, 0.0f), 1.0f), m1);
    return std::pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

float pq_to_nits(float v) {  // ST 2084 EOTF
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float e = std::pow(std::fmax(v, 0.0f), 1.0f / m2);
    return 10000.0f * std::pow(std::fmax(e - c1, 0.0f) / (c2 - c3 * e), 1.0f / m1);
}

void write_png(Pending p, const uint8_t* px) {
    const bool scrgb = p.format == VK_FORMAT_R16G16B16A16_SFLOAT;
    const bool hdr10 = p.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || p.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    const bool bgr = p.format == VK_FORMAT_B8G8R8A8_UNORM || p.format == VK_FORMAT_B8G8R8A8_SRGB ||
                     p.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    // Vanilla on an HDR swapchain: the SDR picture it is (BT.709 nits over the UI's white, clipped, in the gamma the HDR
    // output corrected with), an 8-bit PNG.
    const bool vanilla = (scrgb || hdr10) && p.tonemap == 0, hdr = (scrgb || hdr10) && !vanilla;
    const uint32_t bpp = hdr ? 6 : 3, src_bpp = scrgb ? 8 : 4;
    std::vector<uint8_t> raw;
    raw.reserve(size_t(p.height) * (1 + size_t(p.width) * bpp));
    for (uint32_t y = 0; y < p.height; y++) {
        raw.push_back(0);  // filter: none
        for (uint32_t x = 0; x < p.width; x++) {
            const uint8_t* s = px + (size_t(y) * p.width + x) * src_bpp;
            if (vanilla) {
                float n[3];
                if (scrgb) {
                    uint16_t h[3];
                    std::memcpy(h, s, 6);
                    for (int k = 0; k < 3; k++) n[k] = half_to_float(h[k]) * 80.0f;
                } else {
                    uint32_t w;
                    std::memcpy(&w, s, 4);
                    const uint32_t ch[3] = {bgr ? (w >> 20) & 0x3ff : w & 0x3ff, (w >> 10) & 0x3ff, bgr ? w & 0x3ff : (w >> 20) & 0x3ff};
                    float b[3];
                    for (int k = 0; k < 3; k++) b[k] = pq_to_nits(float(ch[k]) / 1023.0f);
                    n[0] = 1.6605f * b[0] - 0.5876f * b[1] - 0.0728f * b[2];  // BT.2020 to BT.709
                    n[1] = -0.1246f * b[0] + 1.1329f * b[1] - 0.0083f * b[2];
                    n[2] = -0.0182f * b[0] - 0.1006f * b[1] + 1.1187f * b[2];
                }
                for (int k = 0; k < 3; k++) {
                    const float v = std::fmin(std::fmax(n[k] / p.ui_nits, 0.0f), 1.0f);
                    const float e = p.gamma22 ? std::pow(v, 1.0f / 2.2f)
                                              : v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
                    raw.push_back(uint8_t(std::lround(e * 255.0f)));
                }
                continue;
            }
            uint16_t out[3];
            if (scrgb) {
                uint16_t h[3];
                std::memcpy(h, s, 6);
                const float r = half_to_float(h[0]) * 80.0f, g = half_to_float(h[1]) * 80.0f, b = half_to_float(h[2]) * 80.0f;
                const float c[3] = {0.6274040f * r + 0.3292820f * g + 0.0433136f * b,  // BT.709 to BT.2020, nits
                                    0.0690970f * r + 0.9195400f * g + 0.0113612f * b,
                                    0.0163916f * r + 0.0880132f * g + 0.8955950f * b};
                // 10 bits of PQ, as HDR10 has, widened by repeating the top bits: the half floats' noise in the low
                // bits deflated poorly (16.5 MB at 2560x1440), and a display shows no more than 10 bits.
                for (int k = 0; k < 3; k++) {
                    const uint32_t v = uint32_t(std::lround(pq(c[k]) * 1023.0f));
                    out[k] = uint16_t((v << 6) | (v >> 4));
                }
            } else if (hdr10) {
                uint32_t w;
                std::memcpy(&w, s, 4);
                const uint32_t lo = w & 0x3ff, mid = (w >> 10) & 0x3ff, hi = (w >> 20) & 0x3ff;
                const uint32_t ch[3] = {bgr ? hi : lo, mid, bgr ? lo : hi};
                for (int k = 0; k < 3; k++) out[k] = uint16_t((ch[k] << 6) | (ch[k] >> 4));
            }
            if (hdr) {
                for (int k = 0; k < 3; k++) raw.push_back(uint8_t(out[k] >> 8)), raw.push_back(uint8_t(out[k]));
            } else {
                raw.push_back(s[bgr ? 2 : 0]), raw.push_back(s[1]), raw.push_back(s[bgr ? 0 : 2]);
            }
        }
    }
#if defined(NX_HAVE_ZLIB)
    // PNG's Paeth filter (each byte less its prediction from left, above and above-left, a texel apart): a smooth
    // picture deflates to a fraction. Rows from the bottom, so the ones above are still unfiltered.
    const size_t stride = 1 + size_t(p.width) * bpp;
    auto paeth = [](int a, int b, int c) {
        const int q = a + b - c, pa = std::abs(q - a), pb = std::abs(q - b), pc = std::abs(q - c);
        return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
    };
    for (size_t y = p.height; y-- > 0;) {
        uint8_t* row = &raw[y * stride];
        const uint8_t* above = y ? row - stride : nullptr;
        row[0] = 4;
        for (size_t k = stride - 1; k >= 1; k--) {  // right to left: the left neighbours are still unfiltered
            const int a = k > bpp ? row[k - bpp] : 0, b = above ? above[k] : 0, c = above && k > bpp ? above[k - bpp] : 0;
            row[k] = uint8_t(row[k] - paeth(a, b, c));
        }
    }
#endif
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}, ihdr;
    put32(ihdr, p.width), put32(ihdr, p.height);
    ihdr.insert(ihdr.end(), {uint8_t(hdr ? 16 : 8), 2, 0, 0, 0});  // depth, RGB, deflate, no filter set, no interlace
    chunk(png, "IHDR", ihdr);
    // HDR is signalled by an ICC profile with a cicp tag (BT.2020, PQ, RGB, full range; tools/shaders/hdr/make_pq_icc.py),
    // which Chrome, Edge and Windows Photos read; its curves are the SDR fallback. No cICP chunk: Discord's PNG decoder
    // mishandles one (the picture turns pale and yellow-green, the ICC ignored), as other HDR screenshot tools found
    // (ReShade, hdr-screenshot-fix). The image data in 64 KiB IDAT chunks, as those tools write it.
    if (hdr) {
#if defined(NX_HAVE_ZLIB)
        std::vector<uint8_t> iccp = {'B', 'T', '2', '1', '0', '0', ' ', 'P', 'Q', 0, 0};  // name, deflate
        const std::vector<uint8_t> icc(kPqIcc, kPqIcc + sizeof kPqIcc), z = zlib_of(icc);
        iccp.insert(iccp.end(), z.begin(), z.end());
        chunk(png, "iCCP", iccp);
#else
        chunk(png, "cICP", {9, 16, 0, 1});  // no zlib for the ICC: the PNG's own HDR signalling
#endif
    } else {
        chunk(png, "sRGB", {0});
    }
    const std::vector<uint8_t> z = zlib_of(raw);
    for (size_t at = 0; at < z.size(); at += 65536)
        chunk(png, "IDAT", std::vector<uint8_t>(z.begin() + at, z.begin() + std::min(z.size(), at + 65536)));
    chunk(png, "IEND", {});
    const std::string dir = plat::data_root() + "/screenshots";
    plat::make_dirs(dir);
    char name[64];
    const std::time_t t = std::time(nullptr);
    // The look in the name, for side-by-side comparisons: _hdr, _renodrt, _vanilla (HDR on), _sdr (HDR off).
    const char* look = vanilla ? "vanilla" : !hdr ? "sdr" : p.tonemap == 2 ? "renodrt" : "hdr";
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", std::localtime(&t));
    std::snprintf(name, sizeof name, "/acnh_%s_%s.png", stamp, look);
    const std::string file = p.stem.empty() ? std::string(name) : "/" + p.stem + ".png";
    if (FILE* f = plat::fopen_utf8(dir + file, "wb")) {
        std::fwrite(png.data(), 1, png.size(), f);
        std::fclose(f);
        log("screenshot: %s%s (%ux%u, %s, %.1f MB)", dir.c_str(), file.c_str(), p.width, p.height,
            vanilla ? "vanilla, SDR" : scrgb ? "HDR from scRGB" : hdr10 ? "HDR10" : "SDR", double(png.size()) / 1e6);
    }
}

// Once the frames in flight are past the readback: written on a thread.
void finish(Pending& slot) {
    if (!slot.buffer || g_presents < slot.at + 16) return;
    Pending p = slot;
    slot = {};
    void* px = nullptr;
    if (p_vkMapMemory(vk_device(), p.memory, 0, VK_WHOLE_SIZE, 0, &px) == VK_SUCCESS) {
        std::thread([p, px] {
            write_png(p, static_cast<const uint8_t*>(px));
            p_vkUnmapMemory(vk_device(), p.memory);
            p_vkDestroyBuffer(vk_device(), p.buffer, nullptr);
            p_vkFreeMemory(vk_device(), p.memory, nullptr);
        }).detach();
    }
}

// A host-visible buffer of `size` bytes for the readback; false (and `p` cleared) when there is none.
bool alloc_readback(Pending& p, uint64_t size) {
    VkDevice d = vk_device();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (p_vkCreateBuffer(d, &bi, nullptr, &p.buffer) != VK_SUCCESS) return p = {}, false;
    VkMemoryRequirements req{};
    p_vkGetBufferMemoryRequirements(d, p.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (p_vkAllocateMemory(d, &ai, nullptr, &p.memory) != VK_SUCCESS) {
        p_vkDestroyBuffer(d, p.buffer, nullptr);
        return p = {}, false;
    }
    p_vkBindBufferMemory(d, p.buffer, p.memory, 0);
    return true;
}

// The camera's photo: the game drew it into a linear RGBA8 texture and passed that texture's memory to
// nn::album::SaveScreenshot, but the drawing is only in the texture's Vulkan image (at the screen scale: 1707x960 for
// 1280x720 at 1440p). Copied from there, at the image's size.
void record_album(VkCommandBuffer cb, uint64_t texture) {
    GpuImage* img = image_of(texture);
    const bool rgba8 = img && (img->format == VK_FORMAT_R8G8B8A8_UNORM || img->format == VK_FORMAT_R8G8B8A8_SRGB ||
                               img->format == VK_FORMAT_B8G8R8A8_UNORM || img->format == VK_FORMAT_B8G8R8A8_SRGB);
    if (!rgba8 || !img->image || img->layout == VK_IMAGE_LAYOUT_UNDEFINED || !(img->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
        log("album: texture %llx has no RGBA8 image to read (format %d)", (unsigned long long)texture,
            img ? int(img->format) : -1);
        return;
    }
    if (!alloc_readback(g_album, uint64_t(img->width) * img->height * 4)) return;
    g_album.width = img->width, g_album.height = img->height, g_album.format = img->format, g_album.at = g_presents;
    {
        std::lock_guard lk(g_stem_lock);
        g_album.stem = std::move(g_album_stem);
    }
    const VkImageLayout back = img->layout;
    transition(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {img->width, img->height, 1};
    p_vkCmdCopyImageToBuffer(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_album.buffer, 1, &r);
    transition(cb, *img, back);
    log("album: photo from texture %llx (%ux%u), written in a moment", (unsigned long long)texture, img->width, img->height);
}

}  // namespace

void album_capture(uint64_t texture, const std::string& stem) {
    {
        std::lock_guard lk(g_stem_lock);
        g_album_stem = stem;
    }
    g_album_texture = texture;
}

// Guest memory that holds the picture itself (RGBA8 sRGB): copied, then written on a thread.
void screenshot_save_rgba(const uint8_t* rgba, uint32_t width, uint32_t height, const std::string& stem) {
    std::vector<uint8_t> px(rgba, rgba + size_t(width) * height * 4);
    Pending p;
    p.width = width, p.height = height, p.format = VK_FORMAT_R8G8B8A8_UNORM, p.stem = stem;
    std::thread([p, px = std::move(px)] { write_png(p, px.data()); }).detach();
}

void screenshot_request(const std::string& stem) {
    {
        std::lock_guard lk(g_stem_lock);
        g_wanted_stem = stem;
    }
    g_wanted = true;
}

// After the frame is drawn into the swapchain image (left in PRESENT_SRC_KHR), before the settings screen.
void screenshot_record(VkCommandBuffer cb, VkImage image, VkFormat format, VkExtent2D extent) {
    g_presents++;
    finish(g_pending);
    finish(g_album);
    if (!g_album.buffer)
        if (const uint64_t t = g_album_texture.exchange(0)) record_album(cb, t);
    if (!g_wanted.exchange(false) || g_pending.buffer || !extent.width || !extent.height) return;
    const uint32_t texel = format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
    if (!alloc_readback(g_pending, uint64_t(extent.width) * extent.height * texel)) return;
    g_pending.width = extent.width, g_pending.height = extent.height, g_pending.format = format, g_pending.at = g_presents;
    {
        std::lock_guard lk(g_stem_lock);
        g_pending.stem = std::move(g_wanted_stem);
        g_wanted_stem.clear();
    }
    if (hdr_ui_params(g_pending.ui_nits, g_pending.gamma22)) g_pending.tonemap = hdr_tonemap_mode();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {extent.width, extent.height, 1};
    p_vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_pending.buffer, 1, &r);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    play_shutter();
    ui::ui_screenshot_saved("");  // at once: the PNG is converted and compressed on a thread (a few seconds)
    log("screenshot: taken (%ux%u, format %d), written in a moment", extent.width, extent.height, int(format));
}

}  // namespace nx::nvn
