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
// Changes of the offset: the ticker's, the player's new date and a reload of the setting, one at a time (else a tick
// could write an old offset over the date just chosen). g_dirty: time gained that config.ini doesn't have yet.
std::mutex g_mu;
bool g_dirty = false;

// Under g_mu: the offset into config.ini (written by config_save, outside the lock).
bool note_offset() {
    if (!g_dirty) return false;
    config_set("game.time_travel_offset", std::to_string(g_offset.load(std::memory_order_relaxed)));
    g_dirty = false;
    return true;
}

// A faster clock (Settings > Game, "Clock speed"): every second the time-travel offset gains speed - 1 seconds and the
// clocks in time:u's shared memory are rewritten (the game reads them as an offset plus the steady clock, so only a
// moved offset moves its time). The offset goes to config.ini every 10 s and when the game is closed or sent to the
// background (game_clock_save), so the time gained stays after a restart: going back to the computer's clock is a time
// travel backwards, which the player chooses (real-time sync). Paused while Nextendo's services are on: the servers
// expect an island in real time.
void speed_ticker() {
    for (int tick = 1;; tick++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const int k = g_speed.load(std::memory_order_relaxed);
        bool save = false;
        {
            std::lock_guard lk(g_mu);
            if (k > 1 && !nextendo::online_services_active()) {
                g_offset.fetch_add(k - 1, std::memory_order_relaxed);
                g_dirty = true;
                nx_time_refresh_shared_clocks();
            }
            if (tick % 10 == 0 || k <= 1) save = note_offset();
        }
        if (save) config_save();
    }
}
}  // namespace

void game_clock_save() {
    bool save;
    {
        std::lock_guard lk(g_mu);
        save = note_offset();
    }
    if (save) config_save();
}

void game_clock_set_offset(int64_t offset) {
    {
        std::lock_guard lk(g_mu);
        g_offset.store(offset, std::memory_order_relaxed);
        g_dirty = false;
        config_set("game.time_travel_offset", std::to_string(offset));
        nx_time_refresh_shared_clocks();
    }
    config_save();
}

int64_t game_clock_real_unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t game_clock_unix_seconds() { return game_clock_real_unix_seconds() + g_offset.load(std::memory_order_relaxed); }

void game_clock_apply_config() {
    // NX_TIME_OFFSET set before the game (a script, a benchmark) wins over the setting.
    static const char* const s_override = config_env_override("NX_TIME_OFFSET");
    std::lock_guard lk(g_mu);
    note_offset();  // time gained before this reload: into config.ini, so it is read back below
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
