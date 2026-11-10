// nn::time at the API level: the clocks read the host's clock, and calendar conversions use the host's time zone.
// First needed by Animal Crossing: New Horizons, whose whole world runs on the local date and time. SDK 20 converts
// between POSIX and calendar time in process, with the TZif rule nn::time::LoadTimeZoneRule loads from the time
// service; time:u has no such rule to give (ITimeZoneService 4 was a stub), so the conversion produced a date the game
// rejected ("The system is set to a date outside the supported range."). Here the rule is ignored and the host's
// localtime/mktime do the work: the island follows the computer's clock, as it follows the console's.
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#include <nx/runtime.h>

#include "game_clock.h"
#include "platform.h"
#include "nextendo.h"

namespace nx {

namespace {

int64_t unix_now() { return game_clock_unix_seconds(); }

std::tm local_tm(int64_t t) {
    time_t tt = time_t(t);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    return tm;
}

long gmt_offset(const std::tm& tm, int64_t t) {
#if defined(_WIN32)
    // The local calendar time read as UTC, minus the time: the offset in force at t, daylight saving included.
    std::tm local = tm;
    return long(int64_t(_mkgmtime(&local)) - t);
#else
    (void)t;
    return tm.tm_gmtoff;
#endif
}

// CalendarTime {s16 year; s8 month 1-12; s8 day; s8 hour; s8 minute; s8 second; u8 pad} (8 bytes).
void write_calendar(uint64_t p, const std::tm& tm) {
    st<int16_t>(p, int16_t(tm.tm_year + 1900));
    st<int8_t>(p + 2, int8_t(tm.tm_mon + 1));
    st<int8_t>(p + 3, int8_t(tm.tm_mday));
    st<int8_t>(p + 4, int8_t(tm.tm_hour));
    st<int8_t>(p + 5, int8_t(tm.tm_min));
    st<int8_t>(p + 6, int8_t(tm.tm_sec));
    st<int8_t>(p + 7, 0);
}

// CalendarAdditionalInfo {s32 day of week 0-6; s32 day of year 0-365; char time zone[8]; s32 is DST; s32 UTC offset
// in seconds} (0x18 bytes).
void write_additional(uint64_t p, const std::tm& tm, int64_t t) {
    if (!p) return;
    st<int32_t>(p, tm.tm_wday);
    st<int32_t>(p + 4, tm.tm_yday);
    char zone[8] = {};
#if !defined(_WIN32)
    if (tm.tm_zone) std::strncpy(zone, tm.tm_zone, sizeof zone - 1);
#endif
    std::memcpy(reinterpret_cast<void*>(p + 8), zone, sizeof zone);
    st<int32_t>(p + 0x10, tm.tm_isdst > 0 ? 1 : 0);
    st<int32_t>(p + 0x14, int32_t(gmt_offset(tm, t)));
}

void current_time(CPU& c) {  // Result GetCurrentTime(PosixTime*)
    st<int64_t>(c.x[0], unix_now());
    c.x[0] = 0;
}

void ok(CPU& c) { c.x[0] = 0; }
void no(CPU& c) { c.x[0] = 0; }  // bool false

// The network clock: online, a console's is synced to real UTC and "accurate"; the island services wait for that
// (an inaccurate clock ended Orville's connection with a timeout, ens 2219-1028). Offline it stays the game clock,
// as before, so time travel is untouched.
void network_time(CPU& c) {
    st<int64_t>(c.x[0], nextendo::online_services_active() ? game_clock_real_unix_seconds() : unix_now());
    c.x[0] = 0;
}
// libc time for the game's own libraries (libcurl's clock): gettimeofday(timeval* {s64 sec, s64 usec}, tz), time(t*),
// clock(). Left to the SDK's libc they read the time service the runtime only partly emulates; online, curl then saw
// its request already past its deadline and gave up before resolving (CURLE_OPERATION_TIMEDOUT, ens 2219-1028). Only
// online: offline the SDK's own functions answer, as they always did.
bool sdk_libc(CPU& c, const char* name) {
    if (nextendo::online_services_active()) return false;
    const uint64_t fn = find_symbol(name);
    if (!fn) return false;
    call(c, fn);
    return true;
}

void gettimeofday_(CPU& c) {
    if (sdk_libc(c, "gettimeofday")) return;
    // Steady between calls (curl subtracts them): the game clock, then the host's monotonic clock from there; anchored
    // again when the game clock moves away (time travel changed), so it never drifts from time().
    static std::mutex mu;
    static int64_t base_s = 0;
    static uint64_t base_ns = 0;
    int64_t sec, usec;
    {
        std::lock_guard lk(mu);
        const uint64_t now_ns = plat::monotonic_ns();
        const int64_t wall = unix_now();
        int64_t us = int64_t((now_ns - base_ns) / 1000);
        if (!base_ns || std::llabs(base_s + us / 1000000 - wall) > 2) {
            base_s = wall, base_ns = now_ns, us = 0;
        }
        sec = base_s + us / 1000000, usec = us % 1000000;
    }
    if (c.x[0]) {
        st<int64_t>(c.x[0], sec);
        st<int64_t>(c.x[0] + 8, usec);
    }
    c.x[0] = 0;
}
void time_(CPU& c) {
    if (sdk_libc(c, "time")) return;
    const int64_t t = unix_now();
    if (c.x[0]) st<int64_t>(c.x[0], t);
    c.x[0] = uint64_t(t);
}
void clock_(CPU& c) {  // CLOCKS_PER_SEC is 1000000 on the Switch (musl)
    if (sdk_libc(c, "clock")) return;
    static const uint64_t start = plat::monotonic_ns();
    c.x[0] = (plat::monotonic_ns() - start) / 1000;
}

void network_accurate(CPU& c) { c.x[0] = nextendo::online_services_active() ? 1 : 0; }

// void GetDeviceLocationName(LocationName*): 0x24 chars. The rule behind it is the host's, whatever the name.
void GetDeviceLocationName(CPU& c) {
    char name[0x24] = "UTC";
    if (const char* tz = std::getenv("TZ"); tz && *tz) std::strncpy(name, tz[0] == ':' ? tz + 1 : tz, sizeof name - 1);
    std::memcpy(reinterpret_cast<void*>(c.x[0]), name, sizeof name);
}

// Result LoadTimeZoneRule(TimeZoneRule*, const LocationName&): the rule is not used by the conversions below.
void LoadTimeZoneRule(CPU& c) { c.x[0] = 0; }

// Result ToCalendarTime(CalendarTime*, CalendarAdditionalInfo*, const PosixTime&[, const TimeZoneRule&])
void ToCalendarTime(CPU& c) {
    int64_t t = ld<int64_t>(c.x[2]);
    std::tm tm = local_tm(t);
    write_calendar(c.x[0], tm);
    write_additional(c.x[1], tm, t);
    c.x[0] = 0;
}

// Result ToPosixTime(int* count, PosixTime* out, int max, const CalendarTime&, const TimeZoneRule&): local calendar
// time to POSIX time (one answer; DST gaps and overlaps resolved by mktime).
void ToPosixTime(CPU& c) {
    uint64_t count = c.x[0], out = c.x[1], cal = c.x[3];
    int max = int(c.x[2]);
    std::tm tm{};
    tm.tm_year = ld<int16_t>(cal) - 1900;
    tm.tm_mon = ld<int8_t>(cal + 2) - 1;
    tm.tm_mday = ld<int8_t>(cal + 3);
    tm.tm_hour = ld<int8_t>(cal + 4);
    tm.tm_min = ld<int8_t>(cal + 5);
    tm.tm_sec = ld<int8_t>(cal + 6);
    tm.tm_isdst = -1;
    int64_t t = int64_t(std::mktime(&tm));
    if (max >= 1 && out) st<int64_t>(out, t);
    if (count) st<int32_t>(count, max >= 1 ? 1 : 0);
    c.x[0] = 0;
}

}  // namespace

NX_HLE("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", current_time);
NX_HLE("gettimeofday", gettimeofday_);
NX_HLE("time", time_);
NX_HLE("clock", clock_);
NX_HLE("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", network_time);
NX_HLE("_ZN2nn4time25AdjustableUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", current_time);
NX_HLE("_ZN2nn4time25AdjustableUserSystemClock6AdjustEv", ok);
NX_HLE("_ZN2nn4time26StandardNetworkSystemClock20IsAccuracySufficientEv", network_accurate);
NX_HLE("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", GetDeviceLocationName);
NX_HLE("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", LoadTimeZoneRule);
NX_HLE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE",
       ToCalendarTime);
NX_HLE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE", ToCalendarTime);
NX_HLE("_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE", ToPosixTime);

}  // namespace nx
