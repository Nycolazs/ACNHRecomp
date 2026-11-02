// HID service commands used by the game's vibration mixer.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "config.h"
#include "ipc.h"
#include "platform.h"

namespace nx {

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kDurationMs = 60;
constexpr float kStrength = 3.0f;
constexpr uint32_t kLeft = 0, kRight = 1, kCombined = 2, kDeviceCount = 3;

struct Vibration {
    float low = 0, high = 0;
    Clock::time_point until;
};

float amplitude(uint64_t address) {
    float value = ld<float>(address);
    return std::isfinite(value) ? std::max(0.0f, value) : 0.0f;
}

std::pair<float, float> mix_vibrations(const Vibration (&devices)[kDeviceCount], Clock::time_point now) {
    // Motor mixing follows Ryubing's standard rumble conversion.
    float low = 0, high = 0;
    if (now < devices[kRight].until) low = devices[kRight].low * 0.85f + devices[kRight].high * 0.15f;
    if (now < devices[kLeft].until) high = devices[kLeft].low * 0.15f + devices[kLeft].high * 0.85f;
    // A combined device has no separate sides: use its low and high amplitudes.
    if (now < devices[kCombined].until) {
        low = std::max(low, devices[kCombined].low);
        high = std::max(high, devices[kCombined].high);
    }
    return {std::min(1.0f, low * kStrength), std::min(1.0f, high * kStrength)};
}

uint32_t send_vibration_values(IpcCtx& c) {
    const Buffer handles = c.in_buf(0), values = c.in_buf(1);
    constexpr uint64_t handle_size = 4, value_size = 16;
    if (!handles.addr || !values.addr || handles.size % handle_size != 0 ||
        values.size / value_size != handles.size / handle_size || values.size % value_size != 0)
        return 0;

    if (!config_get_bool("input.rumble", true)) return 0;
    // Keep each device's latest value; the game may update them separately.
    static Vibration devices[kDeviceCount];
    const auto now = Clock::now();
    bool player_one = false;
    const uint64_t count = handles.size / handle_size;
    for (uint64_t i = 0; i < count; ++i) {
        const uint32_t handle = ld<uint32_t>(handles.addr + i * handle_size);
        if (((handle >> 8) & 0xff) != 0) continue;
        const uint32_t device = (handle >> 16) & 0xff;
        if (device >= kDeviceCount) continue;
        player_one = true;
        const uint64_t value = values.addr + i * value_size;
        devices[device] = {amplitude(value), amplitude(value + 8), now + std::chrono::milliseconds(kDurationMs)};
    }
    if (player_one) {
        auto [low, high] = mix_vibrations(devices, now);
        plat::rumble(low, high, low > 0.001f || high > 0.001f ? kDurationMs : 0);
    }
    return 0;
}

ServiceRegistrar r_hid("hid", [] {
    auto s = std::make_shared<Service>("hid");
    s->cmds[206] = send_vibration_values;
    return s;
});

}  // namespace

}  // namespace nx
