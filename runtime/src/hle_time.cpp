// nn::time at the API level: the clocks read the host's clock, and calendar conversions use the host's time zone.
// First needed by Animal Crossing: New Horizons, whose whole world runs on the local date and time. SDK 20 converts
// between POSIX and calendar time in process, with the TZif rule nn::time::LoadTimeZoneRule loads from the time
// service; time:u has no such rule to give (ITimeZoneService 4 was a stub), so the conversion produced a date the game
// rejected ("The system is set to a date outside the supported range."). Here the rule is ignored and the host's
// localtime/mktime do the work: the island follows the computer's clock, as it follows the console's.
#include <cstring>
#include <ctime>

#include <nx/runtime.h>

#include "game_clock.h"

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
NX_HLE("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", current_time);
NX_HLE("_ZN2nn4time25AdjustableUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", current_time);
NX_HLE("_ZN2nn4time25AdjustableUserSystemClock6AdjustEv", ok);
NX_HLE("_ZN2nn4time26StandardNetworkSystemClock20IsAccuracySufficientEv", no);
NX_HLE("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", GetDeviceLocationName);
NX_HLE("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", LoadTimeZoneRule);
NX_HLE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE",
       ToCalendarTime);
NX_HLE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE", ToCalendarTime);
NX_HLE("_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE", ToPosixTime);

}  // namespace nx
