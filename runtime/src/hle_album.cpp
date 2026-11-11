// nn::album (the console's Album) at the API level: the pictures the game saves itself, such as the phone's camera
// app, go to <data root>/screenshots/<island>_<date>_<time>.png beside the F12 screenshots.
//
// On the console SaveScreenshot sends the picture to capsrv (caps:su), which has no implementation here: the call
// went to a stub, succeeded and the picture was lost. The SDK accepts only ImageSize 0, 1280x720
// (nn::album::GetImageWidth/GetImageHeight abort on anything else), as RGBA8 sRGB, at least width * height * 4 bytes.
// ACNH draws the photo into a linear RGBA8 texture and passes that texture's memory (main+1457884), which only the
// GPU image holds here: the image is read back at the next present (nvn_screenshot.cpp). Other memory is saved as is.
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#include <nx/runtime.h>

#include "platform.h"
#include "save_storage.h"

namespace nx {
namespace nvn {
void album_capture(uint64_t texture, const std::string& stem);                                              // nvn_screenshot.cpp
void screenshot_save_rgba(const uint8_t* rgba, uint32_t width, uint32_t height, const std::string& stem);  // nvn_screenshot.cpp
}
uint64_t texture_at_memory(uint64_t addr);  // nvn_objects.cpp

namespace {

constexpr uint32_t kWidth = 1280, kHeight = 720;

// The island's name as a file name: the characters Windows refuses (and control characters) dropped, no trailing dots
// or spaces; "Island" before the first save writes landname.dat.
std::string file_island_name() {
    std::string out;
    for (char c : save_storage::island_display_name(save_storage::device_dir(plat::data_root()))) {
        if (static_cast<unsigned char>(c) < 0x20 || std::string("<>:\"/\\|?*").find(c) != std::string::npos) continue;
        out += c;
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    return out.empty() ? "Island" : out;
}

// Result SaveScreenshot(const void* data, size_t size, ImageSize, AlbumReportOption)
void SaveScreenshot(CPU& c) {
    const uint64_t data = c.x[0], size = c.x[1];
    const uint32_t image_size = uint32_t(c.x[2]);
    if (!data || image_size != 0 || size < uint64_t(kWidth) * kHeight * 4) {
        // Success all the same, as the stub gave before: the game goes on, only this picture is not kept.
        log("album: SaveScreenshot(size %llu, image size %u) not saved", (unsigned long long)size, image_size);
        c.x[0] = 0;
        return;
    }
    const std::time_t t = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", std::localtime(&t));
    // Several in one second (the camera can be quick): _2, _3... after the first.
    static std::string last;
    static int same = 0;
    std::string stem = file_island_name() + "_" + stamp;
    same = stem == last ? same + 1 : 1;
    last = stem;
    if (same > 1) stem += "_" + std::to_string(same);
    if (const uint64_t texture = texture_at_memory(data))
        nvn::album_capture(texture, stem);
    else
        nvn::screenshot_save_rgba(reinterpret_cast<const uint8_t*>(data), kWidth, kHeight, stem);
    c.x[0] = 0;
}

}  // namespace

NX_HLE("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionE", SaveScreenshot);

}  // namespace nx
