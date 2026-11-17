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
#include "platform.h"

namespace nx {

namespace {
std::atomic<int64_t> g_offset{0};
std::atomic<int> g_speed{1};  // game.clock_speed, with a custom time only: that many times faster; 0: stopped
// Changes of the offset: the ticker's, the player's new date and a reload of the setting, one at a time (else a tick
// could write an old offset over the date just chosen). g_dirty: time gained that config.ini doesn't have yet.
std::mutex g_mu;
bool g_dirty = false;
// Stopped (speed 0): the island's moment (unix seconds), kept in config.ini as game.clock_frozen_at so that it stays
// put through a suspend, a closed game or a restart; the offset is worked out from it at each tick.
std::atomic<int64_t> g_frozen{0};

// Under g_mu: the offset (and a stopped clock's moment) into config.ini (written by config_save, outside the lock).
bool note_offset() {
    if (!g_dirty) return false;
    config_set("game.time_travel_offset", std::to_string(g_offset.load(std::memory_order_relaxed)));
    if (g_speed.load(std::memory_order_relaxed) == 0) config_set("game.clock_frozen_at", std::to_string(g_frozen.load()));
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
        // Just after the steady clock's next second (svc_time.cpp: steady_seconds is monotonic_ns / 1e9): the game's
        // time, offset + steady clock, then moves when the clocks below are rewritten, not a second before (which
        // would make a stopped clock read X, X+1, X).
        const uint64_t now = plat::monotonic_ns();
        std::this_thread::sleep_for(std::chrono::nanoseconds((now / 1000000000ull + 1) * 1000000000ull + 100000ull - now));
        bool save = false;
        {
            std::lock_guard lk(g_mu);
            const int k = g_speed.load(std::memory_order_acquire);  // under the lock: a reload can't slip in between
            const bool online = nextendo::online_services_active();
            if (k == 0) {
                const int64_t real = game_clock_real_unix_seconds();
                if (online) {  // the servers expect real time: the island goes on from where it stood, then stops again
                    g_frozen.store(real + g_offset.load(std::memory_order_relaxed));
                } else {
                    g_offset.store(g_frozen.load() - real, std::memory_order_relaxed);
                    nx_time_refresh_shared_clocks();
                }
                g_dirty = true;
            } else if (k != 1 && !online) {
                g_offset.fetch_add(k - 1, std::memory_order_relaxed);
                g_dirty = true;
                nx_time_refresh_shared_clocks();
            }
            if (tick % 10 == 0 || k == 1) save = note_offset();
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
        if (g_speed.load(std::memory_order_relaxed) == 0) {  // stopped: at the new date
            g_frozen.store(game_clock_real_unix_seconds() + offset);
            config_set("game.clock_frozen_at", std::to_string(g_frozen.load()));
        }
        nx_time_refresh_shared_clocks();
    }
    config_save();
}

int64_t game_clock_real_unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t game_clock_unix_seconds() {
    // stopped: the moment itself, not real time + an offset last worked out up to a second ago
    if (g_speed.load(std::memory_order_acquire) == 0) return g_frozen.load(std::memory_order_relaxed);
    return game_clock_real_unix_seconds() + g_offset.load(std::memory_order_relaxed);
}

void game_clock_apply_config() {
    // NX_TIME_OFFSET set before the game (a script, a benchmark) wins over the setting.
    static const char* const s_override = config_env_override("NX_TIME_OFFSET");
    std::lock_guard lk(g_mu);
    if (g_speed.load(std::memory_order_relaxed) == 0) {  // leaving (or reloading) a stop: from the stopped moment exactly
        g_offset.store(g_frozen.load() - game_clock_real_unix_seconds(), std::memory_order_relaxed);
        g_dirty = true;
    }
    note_offset();  // time gained before this reload: into config.ini, so it is read back below
    int64_t offset = 0;
    int speed = 1;
    if (s_override) {
        offset = std::strtoll(s_override, nullptr, 10);
    } else if (!config_get_bool("game.time_travel_enabled", true)) {
        offset = config_get_int64("game.time_travel_offset", 0);
        speed = std::clamp(config_get_int("game.clock_speed", 1), 0, 60);
    }
    if (speed == 0) {  // stopped: at the moment kept from before (a restart), else where the island is now
        const std::string at = config_get("game.clock_frozen_at");
        const int64_t real = game_clock_real_unix_seconds();
        g_frozen.store(at.empty() ? real + offset : std::strtoll(at.c_str(), nullptr, 10));
        offset = g_frozen.load() - real;
        config_set("game.clock_frozen_at", std::to_string(g_frozen.load()));
    } else if (!s_override && !config_get("game.clock_frozen_at").empty()) {
        config_set("game.clock_frozen_at", "");  // a moment from an earlier stop must not come back
    }
    g_offset.store(offset, std::memory_order_relaxed);
    g_speed.store(speed, std::memory_order_release);  // after g_frozen: a reader that sees 0 sees the moment
    if (speed != 1) {
        static std::once_flag started;
        std::call_once(started, [] { std::thread(speed_ticker).detach(); });
    }
    nx_time_refresh_shared_clocks();
}

}  // namespace nx
