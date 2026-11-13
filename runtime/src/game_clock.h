// In-game clock: the host's time plus the time-travel offset (game.time_travel_* in config.ini, or NX_TIME_OFFSET
// set before the game, which wins). With a custom time, game.clock_speed (1-60) makes it run that many times faster.
#pragma once

#include <cstdint>

namespace nx {

int64_t game_clock_real_unix_seconds();
int64_t game_clock_unix_seconds();

// Reads the offset from config.ini. Call after changing game.time_travel_*.
void game_clock_apply_config();
// A new custom date: the offset set and saved, in step with the faster clock's ticks (game.clock_speed).
void game_clock_set_offset(int64_t offset);
// The time a faster clock gained, into config.ini: when the game is closed or sent to the background.
void game_clock_save();

// svc_time.cpp: rewrites the system clocks in time:u's shared memory (once the game has mapped it).
void nx_time_refresh_shared_clocks();

}  // namespace nx
