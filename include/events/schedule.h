// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// When does an event fire? Pure date arithmetic (no Arduino), unit-tested on the host with real timezone rules.
//
// One-off events carry an absolute instant. Recurring events (daily / weekly / monthly) carry a LOCAL wall-clock time
// of day, so "09:00" stays 09:00 across daylight-saving changes. Local time comes from the C library's TZ rules
// (a POSIX TZ string on the device), via localtime_r / mktime, so it behaves the same on the host and the cube.
//
// Missed events: an occurrence fires only if it is at most `grace` seconds old and newer than the last time the
// event fired (lastFired). Anything older is skipped, never replayed.

#include <stdint.h>

#include <ctime>

namespace Schedule {

enum class Repeat : uint8_t { None = 0, Daily = 1, Weekly = 2, Monthly = 3 };

constexpr uint16_t MINUTES_PER_HOUR = 60;
constexpr uint16_t MINUTES_PER_DAY = 24 * 60;
constexpr uint8_t ALL_WEEKDAYS = 0x7F;  // bit i = tm_wday i (0 = Sunday)
constexpr int SEARCH_DAYS = 400;        // longest gap searched for the next occurrence
constexpr int NOON_HOUR = 12;           // dates are walked at noon, far from any DST transition
constexpr long SECONDS_PER_DAY = 86400;
constexpr long DEFAULT_GRACE_S = 300;

struct Spec {
    Repeat repeat = Repeat::None;
    time_t at = 0;                      // one-off: the instant
    uint16_t minuteOfDay = 0;           // recurring: local time of day, minutes since midnight
    uint8_t weekdayMask = ALL_WEEKDAYS; // weekly: which days
    uint8_t dayOfMonth = 1;             // monthly: 1..31 (clamped to the last day of shorter months)
    time_t from = 0;                    // recurring: no occurrence before this instant (0 = no limit)
    time_t until = 0;                   // recurring: no occurrence after this instant (0 = no limit)
};

namespace detail {

constexpr int DAYS_IN_MONTH[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};  // NOLINT(modernize-avoid-c-arrays)
constexpr int FEB = 1;
constexpr int YEAR_BASE = 1900;
constexpr int LEAP_CYCLE = 4;
constexpr int CENTURY = 100;
constexpr int LEAP_CENTURY = 400;

inline auto isLeap(int year) -> bool {
    return (year % LEAP_CYCLE == 0 && year % CENTURY != 0) || year % LEAP_CENTURY == 0;
}

inline auto daysInMonth(int year, int month) -> int {
    return DAYS_IN_MONTH[month] + ((month == FEB && isLeap(year)) ? 1 : 0);  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
}

// The local calendar date `daysAhead` days after the date of `anchor`, as a normalised tm at noon.
inline auto dateAfter(time_t anchor, int daysAhead) -> tm {
    tm date{};
    localtime_r(&anchor, &date);
    date.tm_hour = NOON_HOUR;
    date.tm_min = 0;
    date.tm_sec = 0;
    date.tm_mday += daysAhead;
    date.tm_isdst = -1;
    mktime(&date);  // normalises tm_mon / tm_year / tm_wday / tm_yday

    return date;
}

// Does the recurrence rule select this calendar date?
inline auto selects(const Spec& spec, const tm& date) -> bool {
    switch (spec.repeat) {
        case Repeat::Daily:
            return true;
        case Repeat::Weekly:
            return (spec.weekdayMask & (1U << static_cast<unsigned>(date.tm_wday))) != 0;
        case Repeat::Monthly: {
            const int last = daysInMonth(date.tm_year + YEAR_BASE, date.tm_mon);
            const int wanted = spec.dayOfMonth > last ? last : spec.dayOfMonth;  // "the 31st" means the last day in short months

            return date.tm_mday == wanted;
        }
        case Repeat::None:
            return false;
    }

    return false;
}

// The instant of the event on the given local date, or 0 if the rule does not select that date. A wall-clock time
// that does not exist (spring forward) is normalised by mktime; one that occurs twice (fall back) fires once.
inline auto onDate(const Spec& spec, const tm& date) -> time_t {
    if (!selects(spec, date)) {
        return 0;
    }
    tm when = date;
    when.tm_hour = spec.minuteOfDay / MINUTES_PER_HOUR;
    when.tm_min = spec.minuteOfDay % MINUTES_PER_HOUR;
    when.tm_sec = 0;
    when.tm_isdst = -1;

    return mktime(&when);
}

inline auto inWindow(const Spec& spec, time_t when) -> bool {
    return (spec.from == 0 || when >= spec.from) && (spec.until == 0 || when <= spec.until);
}

}  // namespace detail

/** @brief The first occurrence strictly after `after`, or 0 if there is none (one-off in the past, `until` passed). */
inline auto nextAfter(const Spec& spec, time_t after) -> time_t {
    if (spec.repeat == Repeat::None) {
        return spec.at > after ? spec.at : 0;
    }
    // Start one day early: an occurrence late "yesterday" in local time can still be after `after` in UTC terms
    for (int day = -1; day < SEARCH_DAYS; day++) {
        const tm date = detail::dateAfter(after, day);
        const time_t when = detail::onDate(spec, date);
        if (when > after && when != 0 && detail::inWindow(spec, when)) {
            return when;
        }
        tm probe = date;
        if (spec.until != 0 && mktime(&probe) > spec.until + SECONDS_PER_DAY) {
            return 0;  // walked past the end of the window
        }
    }

    return 0;
}

/**
 * @brief The occurrence that should fire now, or 0.
 *
 * It must be at or before `now`, at most `graceS` old, and newer than `lastFired`. If several qualify the latest wins.
 */
inline auto dueNow(const Spec& spec, time_t now, time_t lastFired, long graceS) -> time_t {
    const auto fires = [&](time_t when) { return when != 0 && when <= now && when > lastFired && now - when <= graceS; };
    if (spec.repeat == Repeat::None) {
        return fires(spec.at) ? spec.at : 0;
    }
    // Today and yesterday cover a grace window that crosses local midnight
    time_t best = 0;
    for (int day = -1; day <= 0; day++) {
        const tm date = detail::dateAfter(now, day);
        const time_t when = detail::onDate(spec, date);
        if (fires(when) && detail::inWindow(spec, when) && when > best) {
            best = when;
        }
    }

    return best;
}

/** @brief A one-off whose moment has passed beyond the grace window: it can never fire, so it can be deleted. */
inline auto expired(const Spec& spec, time_t now, long graceS) -> bool {
    if (spec.repeat == Repeat::None) {
        return spec.at != 0 && spec.at + graceS < now;
    }

    return spec.until != 0 && spec.until + graceS < now;
}

/** @brief What a scheduler pass should do with one event right now. */
struct Verdict {
    time_t fireAt = 0;    // non-zero: fire the occurrence at this instant
    bool remove = false;  // delete the event (a fired or expired one-off, or a recurring event past its end)
    time_t next = 0;      // the next occurrence after `now`, 0 if none
};

inline auto evaluate(const Spec& spec, time_t now, time_t lastFired, long graceS) -> Verdict {
    Verdict verdict;
    if (expired(spec, now, graceS)) {
        verdict.remove = true;

        return verdict;
    }
    verdict.fireAt = dueNow(spec, now, lastFired, graceS);
    if (verdict.fireAt != 0 && spec.repeat == Repeat::None) {
        verdict.remove = true;  // a one-off is done once it has fired

        return verdict;
    }
    verdict.next = nextAfter(spec, now);

    return verdict;
}

}  // namespace Schedule
