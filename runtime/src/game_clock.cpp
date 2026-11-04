#include "game_clock.h"

#include <atomic>
#include <chrono>
#include <cstdlib>

#include "config.h"

namespace nx {

namespace {
std::atomic<int64_t> g_offset{0};
}

int64_t game_clock_real_unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t game_clock_unix_seconds() { return game_clock_real_unix_seconds() + g_offset.load(std::memory_order_relaxed); }

void game_clock_apply_config() {
    // NX_TIME_OFFSET set before the game (a script, a benchmark) wins over the setting.
    static const char* const s_override = config_env_override("NX_TIME_OFFSET");
    int64_t offset = 0;
    if (s_override)
        offset = std::strtoll(s_override, nullptr, 10);
    else if (!config_get_bool("game.time_travel_enabled", true))
        offset = config_get_int64("game.time_travel_offset", 0);
    g_offset.store(offset, std::memory_order_relaxed);
    nx_time_refresh_shared_clocks();
}

}  // namespace nx
