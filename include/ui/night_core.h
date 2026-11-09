// SPDX-License-Identifier: GPL-3.0-or-later
// Pure logic of the night face (when it is night, and what to do about it), host-tested.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace NightCore {

/** @brief How the cube decides that it is night. */
enum class Mode : uint8_t {
    Disabled,  ///< never
    Sunset,    ///< from sunset to sunrise (from the weather data; the schedule window when there is none)
    Schedule,  ///< from a start time to an end time every day
    Manual,    ///< only when switched on
};

/** @brief What the cube does at night. */
enum class Style : uint8_t {
    Face,  ///< the dim night clock only, no rotation; notifications still appear
    Off,   ///< the screen is off, notifications included
};

constexpr int MINUTES_PER_DAY = 1440;
constexpr int MINUTES_PER_HOUR = 60;
constexpr int SUN_DAYS = 3;
constexpr int BRIGHTNESS_MIN = 1;
constexpr int BRIGHTNESS_MAX = 100;
constexpr size_t HM_SIZE = 6;  ///< "HH:MM" and the terminator

inline auto modeName(Mode mode) -> const char* {
    switch (mode) {
        case Mode::Sunset:
            return "sunset";
        case Mode::Schedule:
            return "schedule";
        case Mode::Manual:
            return "manual";
        case Mode::Disabled:
            break;
    }

    return "disabled";
}

inline auto modeFrom(const char* name, Mode& out) -> bool {
    if (name == nullptr) {
        return false;
    }
    for (const Mode mode : {Mode::Disabled, Mode::Sunset, Mode::Schedule, Mode::Manual}) {
        if (strcmp(name, modeName(mode)) == 0) {
            out = mode;

            return true;
        }
    }

    return false;
}

inline auto styleName(Style style) -> const char* { return style == Style::Off ? "off" : "face"; }

inline auto styleFrom(const char* name, Style& out) -> bool {
    if (name == nullptr) {
        return false;
    }
    for (const Style style : {Style::Face, Style::Off}) {
        if (strcmp(name, styleName(style)) == 0) {
            out = style;

            return true;
        }
    }

    return false;
}

/** @brief Parse "HH:MM" (exactly, 00:00 to 23:59) into minutes after midnight. */
inline auto parseHm(const char* text, int& minutes) -> bool {
    constexpr int MAX_HOUR = 23;
    constexpr int MAX_MINUTE = 59;
    if (text == nullptr || strlen(text) != 5 || text[2] != ':') {
        return false;
    }
    for (const int index : {0, 1, 3, 4}) {
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
    }
    const int hour = ((text[0] - '0') * 10) + (text[1] - '0');
    const int minute = ((text[3] - '0') * 10) + (text[4] - '0');
    if (hour > MAX_HOUR || minute > MAX_MINUTE) {
        return false;
    }
    minutes = (hour * MINUTES_PER_HOUR) + minute;

    return true;
}

inline void formatHm(int minutes, char* out, size_t size) {
    snprintf(out, size, "%02d:%02d", (minutes / MINUTES_PER_HOUR) % (MINUTES_PER_DAY / MINUTES_PER_HOUR),
             minutes % MINUTES_PER_HOUR);
}

/** @brief Whether a minute of the day falls in [start, end); the window wraps past midnight; start == end is empty. */
inline auto inWindow(int minuteOfDay, int startMin, int endMin) -> bool {
    if (startMin == endMin) {
        return false;
    }
    if (startMin < endMin) {
        return minuteOfDay >= startMin && minuteOfDay < endMin;
    }

    return minuteOfDay >= startMin || minuteOfDay < endMin;
}

/**
 * @brief Night by the sun, from three consecutive days of sunrise/sunset (epoch seconds, today first).
 * @return 1 night, 0 day, -1 when the data does not cover `now` (empty, or too old)
 */
inline auto bySun(time_t now, const time_t* sunrise, const time_t* sunset) -> int {
    if (sunrise[0] <= 0 || sunset[0] <= 0) {
        return -1;
    }
    for (int day = 0; day < SUN_DAYS; day++) {
        if (now < sunrise[day]) {
            return 1;
        }
        if (now < sunset[day]) {
            return 0;
        }
    }

    return -1;
}

/** @brief A manual switch on top of an automatic mode; it ends when the automatic state next changes. */
struct Override {
    int8_t want = -1;           ///< -1 none, 0 day, 1 night
    bool autoWhenSet = false;   ///< the automatic state at the time it was set
};

/** @brief The state after applying (and, when the automatic state has moved on, dropping) an override. */
inline auto resolve(bool autoNight, Override& over) -> bool {
    if (over.want >= 0 && over.autoWhenSet != autoNight) {
        over.want = -1;
    }

    return over.want >= 0 ? over.want == 1 : autoNight;
}

/** @brief Switch night on or off by hand. Switching to what the automatic state already is clears the override. */
inline void setOverride(Override& over, bool night, bool autoNight) {
    if (night == autoNight) {
        over.want = -1;

        return;
    }
    over.want = night ? 1 : 0;
    over.autoWhenSet = autoNight;
}

}  // namespace NightCore
