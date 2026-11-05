#include <cstdlib>
// Android host (NativeActivity): memory (mmap at fixed addresses), pthreads, the app's window (ANativeWindow),
// game controllers + touch, AAudio, the Vulkan loader and android surface, a signal crash handler and
// android_main. Game data lives in the app's external files dir (see docs/ANDROID.md):
//   <files>/analysis/{main,sdk}.bin   module images (tools/nso2bin.py)
//   <files>/extract/romfs/...         the RomFS
//   <files>/user/...                  saves
//   <files>/config.txt                optional: resolution=WxH
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"
#include "platform_android.h"

// Android audio out: AAudio (split from platform_android.cpp).
namespace nx::plat {

// ---- audio: AAudio (callback pulls from a queue of submitted buffers) ------------------------------------------

namespace {
struct Pending {
    const uint8_t* data;
    size_t size, pos;
    void* ctx;
};
std::mutex g_audio;
std::deque<Pending> g_pending;
AudioDone g_done = nullptr;
AAudioStream* g_stream = nullptr;

aaudio_data_callback_result_t audio_cb(AAudioStream*, void*, void* out, int32_t frames) {
    auto* dst = static_cast<uint8_t*>(out);
    size_t want = size_t(frames) * 4;
    std::vector<void*> finished;
    {
        std::lock_guard lk(g_audio);
        while (want && !g_pending.empty()) {
            Pending& p = g_pending.front();
            size_t n = std::min(want, p.size - p.pos);
            std::memcpy(dst, p.data + p.pos, n);
            dst += n;
            want -= n;
            p.pos += n;
            if (p.pos == p.size) {
                finished.push_back(p.ctx);
                g_pending.pop_front();
            }
        }
    }
    if (want) std::memset(dst, 0, want);  // underrun: silence
    for (void* c : finished) g_done(c);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
}  // namespace

bool audio_open(AudioDone done, int) {
    g_done = done;
    // NX_MUTE=1 (and headless test runs): no device, the callers pace themselves with a timer (as on Windows and macOS).
    if (headless() || std::getenv("NX_MUTE")) return false;
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, audio_cb, nullptr);
    bool ok = AAudioStreamBuilder_openStream(b, &g_stream) == AAUDIO_OK && AAudioStream_requestStart(g_stream) == AAUDIO_OK;
    AAudioStreamBuilder_delete(b);
    if (!ok && g_stream) AAudioStream_requestStop(g_stream), AAudioStream_close(g_stream), g_stream = nullptr;
    return ok;
}

void audio_pause(bool paused) {
    if (!g_stream) return;
    if (paused)
        AAudioStream_requestPause(g_stream);
    else
        AAudioStream_requestStart(g_stream);
}

bool audio_submit(const void* data, size_t bytes, void* ctx) {
    if (!g_stream) return false;
    std::lock_guard lk(g_audio);
    g_pending.push_back({static_cast<const uint8_t*>(data), bytes, 0, ctx});
    return true;
}

uint32_t audio_queued() {
    std::lock_guard lk(g_audio);
    return uint32_t(g_pending.size());
}

}  // namespace nx::plat
