// cellRtc: the calendar and the clock.
//
// A tick is a count of microseconds from the start of the year 1 (in the
// calendar we use now, run backwards). The time is the host's; the time
// zone and summer time are the console's settings (sysutil.cpp: UTC and
// none, until the settings menu holds them).
#include <chrono>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

constexpr uint64_t kInvalidPointer = 0x80010601u, kInvalidValue = 0x80010602u;  // the SDK's CELL_RTC_ERROR_...
constexpr int64_t kMicro = 1000000, kDay = 86400 * kMicro;
constexpr int64_t kDaysToUnix = 719162;  // from 0001-01-01 to 1970-01-01

int64_t now_tick() {
    const auto since = std::chrono::system_clock::now().time_since_epoch();
    return kDaysToUnix * kDay + std::chrono::duration_cast<std::chrono::microseconds>(since).count();
}
// The console's offset from UTC, in ticks.
int64_t local_offset() { return (int64_t{console_time_zone_minutes()} + console_summer_time_minutes()) * 60 * kMicro; }

// A tick as a date and time: CellRtcDateTime is year, month, day, hour, minute, second (16 bits each) and microsecond (32).
bool store_date(uint32_t out, int64_t tick) {
    if (tick < 0) return false;
    const int64_t days = tick / kDay, rest = tick % kDay;
    // Days from 0001-01-01 to a date, by 400-year cycles (146,097 days), with the year starting in March.
    const int64_t z = days + 306;  // days from 0000-03-01
    const int64_t era = z / 146097, day_of_era = z % 146097;
    const int64_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const int64_t m = (5 * day_of_year + 2) / 153;  // March is 0
    const int64_t day = day_of_year - (153 * m + 2) / 5 + 1, month = m < 10 ? m + 3 : m - 9;
    const int64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
    if (year > 9999) return false;
    const auto put16 = [&](uint32_t at, int64_t v) {
        guest_memory()[out + at] = static_cast<uint8_t>(v >> 8);
        guest_memory()[out + at + 1] = static_cast<uint8_t>(v);
    };
    put16(0, year);
    put16(2, month);
    put16(4, day);
    put16(6, rest / (3600 * kMicro));
    put16(8, rest / (60 * kMicro) % 60);
    put16(10, rest / kMicro % 60);
    store32(out + 12, static_cast<uint32_t>(rest % kMicro));
    return true;
}

// cellRtcGetCurrentTick(CellRtcTick* tick): now, in UTC.
void cellRtcGetCurrentTick(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]);
    if (!out) {
        c.r[3] = kInvalidPointer;
        return;
    }
    store64(out, static_cast<uint64_t>(now_tick()));
    c.r[3] = kOk;
}

// cellRtcGetCurrentClockLocalTime(CellRtcDateTime* time): now, by the console's time zone.
void cellRtcGetCurrentClockLocalTime(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]);
    if (!out) {
        c.r[3] = kInvalidPointer;
        return;
    }
    c.r[3] = store_date(out, now_tick() + local_offset()) ? kOk : kInvalidValue;
}

// cellRtcSetTick(CellRtcDateTime* time, const CellRtcTick* tick): the tick as a date and time.
void cellRtcSetTick(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]), tick = static_cast<uint32_t>(c.r[4]);
    if (!out || !tick) {
        c.r[3] = kInvalidPointer;
        return;
    }
    c.r[3] = store_date(out, static_cast<int64_t>(load64(tick))) ? kOk : kInvalidValue;
}

// cellRtcTickAddMinutes(CellRtcTick* result, const CellRtcTick* tick, minutes)
void cellRtcTickAddMinutes(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]), tick = static_cast<uint32_t>(c.r[4]);
    if (!out || !tick) {
        c.r[3] = kInvalidPointer;
        return;
    }
    store64(out, load64(tick) + static_cast<uint64_t>(static_cast<int64_t>(c.r[5]) * 60 * kMicro));
    c.r[3] = kOk;
}

// cellRtcConvertUtcToLocalTime(const CellRtcTick* utc, CellRtcTick* local)
void cellRtcConvertUtcToLocalTime(Cpu& c) {
    const uint32_t utc = static_cast<uint32_t>(c.r[3]), out = static_cast<uint32_t>(c.r[4]);
    if (!utc || !out) {
        c.r[3] = kInvalidPointer;
        return;
    }
    store64(out, load64(utc) + static_cast<uint64_t>(local_offset()));
    c.r[3] = kOk;
}

constexpr NativeImport kImports[] = {
    {"cellRtc", "cellRtcGetCurrentTick", cellRtcGetCurrentTick},
    {"cellRtc", "cellRtcGetCurrentClockLocalTime", cellRtcGetCurrentClockLocalTime},
    {"cellRtc", "cellRtcSetTick", cellRtcSetTick},
    {"cellRtc", "cellRtcTickAddMinutes", cellRtcTickAddMinutes},
    {"cellRtc", "cellRtcConvertUtcToLocalTime", cellRtcConvertUtcToLocalTime},
};

}  // namespace

std::span<const NativeImport> rtc_imports() { return kImports; }

}  // namespace rr7
