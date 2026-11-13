#include "game_clock.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "config.h"
#include "nextendo.h"

namespace nx {

namespace {
std::atomic<int64_t> g_offset{0};
std::atomic<int> g_speed{1};  // game.clock_speed, with a custom time only: the island's clock runs that many times faster

// A faster clock (Settings > Game, "Clock speed"): every second the time-travel offset gains speed - 1 seconds and the
// clocks in time:u's shared memory are rewritten (the game reads them as an offset plus the steady clock, so only a
// moved offset moves its time). The offset is written to config.ini every 30 s, so the time gained stays after a
// restart: going back to the computer's clock is a time travel backwards, which the player chooses (real-time sync).
// Paused while Nextendo's services are on: the servers expect an island in real time.
void speed_ticker() {
    int unsaved = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const int k = g_speed.load(std::memory_order_relaxed);
        if (k <= 1 || nextendo::online_services_active()) {
            if (unsaved) config_save(), unsaved = 0;
            continue;
        }
        const int64_t offset = g_offset.fetch_add(k - 1, std::memory_order_relaxed) + (k - 1);
        nx_time_refresh_shared_clocks();
        config_set("game.time_travel_offset", std::to_string(offset));
        if (++unsaved >= 30) config_save(), unsaved = 0;
    }
}
}  // namespace

int64_t game_clock_real_unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t game_clock_unix_seconds() { return game_clock_real_unix_seconds() + g_offset.load(std::memory_order_relaxed); }

void game_clock_apply_config() {
    // NX_TIME_OFFSET set before the game (a script, a benchmark) wins over the setting.
    static const char* const s_override = config_env_override("NX_TIME_OFFSET");
    int64_t offset = 0;
    int speed = 1;
    if (s_override) {
        offset = std::strtoll(s_override, nullptr, 10);
    } else if (!config_get_bool("game.time_travel_enabled", true)) {
        offset = config_get_int64("game.time_travel_offset", 0);
        speed = std::clamp(config_get_int("game.clock_speed", 1), 1, 60);
    }
    g_offset.store(offset, std::memory_order_relaxed);
    g_speed.store(speed, std::memory_order_relaxed);
    if (speed > 1) {
        static std::once_flag started;
        std::call_once(started, [] { std::thread(speed_ticker).detach(); });
    }
    nx_time_refresh_shared_clocks();
}

}  // namespace nx
