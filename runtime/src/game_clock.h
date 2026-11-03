// In-game clock: the host's time plus the time-travel offset (game.time_travel_* in config.ini, or NX_TIME_OFFSET
// set before the game, which wins).
#pragma once

#include <cstdint>

namespace nx {

int64_t game_clock_real_unix_seconds();
int64_t game_clock_unix_seconds();

// Reads the offset from config.ini. Call after changing game.time_travel_*.
void game_clock_apply_config();

// svc_time.cpp: rewrites the system clocks in time:u's shared memory (once the game has mapped it).
void nx_time_refresh_shared_clocks();

}  // namespace nx
