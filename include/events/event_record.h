// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A scheduled event as stored on flash, and its JSON form. Pure (ArduinoJson only), unit-tested on the host.
//
// The firing time is always explicit: the cube does not compute "15 minutes before". A meeting at 09:00 that should
// warn at 08:45 is sent as two events (08:45 "starts in 15 min", 09:00 "starting now"): the sending script owns that.

#include <ArduinoJson.h>
#include <stdint.h>
#include <string.h>

#include <ctime>

#include "attention_core.h"
#include "events/schedule.h"
#include "timeutil.h"

namespace Events {

constexpr size_t MAX_EVENTS = 64;
constexpr size_t ID_LEN = 16;
constexpr size_t TITLE_LEN = 27;  // same limits as a push card (include/glimmer/attention_core.h)
constexpr size_t BODY_LEN = 39;
constexpr size_t VALUE_LEN = 15;
constexpr uint16_t TTL_DEFAULT = 0xFFFF;  // "use the default lifetime of the card's kind"
constexpr uint32_t MAX_TTL_S = 86400;
constexpr uint8_t FLAG_ENABLED = 0x01;
constexpr uint16_t MAX_WEEKDAY = 6;
constexpr int MAX_DAY_OF_MONTH = 31;
constexpr size_t ISO_MIN_LEN = 20;  // "2026-11-07T09:00:00Z"

// Fixed-size so the file can be read and updated record by record without holding it all in RAM.
struct Record {
    char id[ID_LEN + 1];
    char title[TITLE_LEN + 1];
    char body[BODY_LEN + 1];
    char value[VALUE_LEN + 1];
    uint8_t icon;         // Attention::Icon
    uint8_t kind;         // Attention::Kind
    uint8_t display;      // Attention::Display
    uint8_t repeat;       // Schedule::Repeat
    uint8_t weekdayMask;  // bit i = tm_wday i (0 = Sunday)
    uint8_t dayOfMonth;
    uint16_t minuteOfDay;  // recurring: local time of day
    uint16_t ttlS;         // TTL_DEFAULT or seconds
    uint8_t flags;         // FLAG_ENABLED
    uint8_t reserved[3];
    uint32_t at;         // one-off: epoch seconds
    uint32_t from;       // recurring window, epoch seconds, 0 = none
    uint32_t until;
    uint32_t lastFired;  // epoch seconds of the occurrence that last fired, 0 = never
};
static_assert(sizeof(Record) % 4 == 0, "Record must have no trailing padding surprises");

inline auto blank() -> Record {
    Record record;
    memset(&record, 0, sizeof(record));
    record.ttlS = TTL_DEFAULT;
    record.dayOfMonth = 1;
    record.weekdayMask = Schedule::ALL_WEEKDAYS;
    record.flags = FLAG_ENABLED;

    return record;
}

inline auto specOf(const Record& record) -> Schedule::Spec {
    Schedule::Spec spec;
    spec.repeat = static_cast<Schedule::Repeat>(record.repeat);
    spec.at = record.at;
    spec.minuteOfDay = record.minuteOfDay;
    spec.weekdayMask = record.weekdayMask;
    spec.dayOfMonth = record.dayOfMonth;
    spec.from = record.from;
    spec.until = record.until;

    return spec;
}

inline auto enabled(const Record& record) -> bool { return (record.flags & FLAG_ENABLED) != 0; }

inline auto validId(const char* text) -> bool {
    const size_t length = text != nullptr ? strlen(text) : 0;
    if (length == 0 || length > ID_LEN) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        const char chr = text[i];
        const bool allowed = (chr >= 'a' && chr <= 'z') || (chr >= 'A' && chr <= 'Z') || (chr >= '0' && chr <= '9') ||
                             chr == '.' || chr == '_' || chr == '-';
        if (!allowed) {
            return false;
        }
    }

    return true;
}

namespace detail {

template <size_t N>
inline void copyText(char (&dst)[N], const char* src) {  // NOLINT(modernize-avoid-c-arrays)
    memset(dst, 0, N);
    if (src != nullptr) {
        strncpy(dst, src, N - 1);
    }
}

constexpr const char* WEEKDAY_NAMES[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};  // NOLINT(modernize-avoid-c-arrays)

inline auto weekdayFrom(JsonVariantConst value) -> int {
    if (value.is<int>()) {
        const int number = value.as<int>();

        return (number >= 0 && number <= static_cast<int>(MAX_WEEKDAY)) ? number : -1;
    }
    if (value.is<const char*>()) {
        const char* name = value.as<const char*>();
        for (int day = 0; day <= static_cast<int>(MAX_WEEKDAY); day++) {
            if (strncasecmp(name, WEEKDAY_NAMES[day], 3) == 0) {  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
                return day;
            }
        }
    }

    return -1;
}

// An instant: epoch seconds, or ISO 8601 that carries an offset ("...Z" or "...+01:00"). Returns false if unusable.
inline auto instantFrom(JsonVariantConst value, time_t& out) -> bool {
    if (value.is<const char*>()) {
        const char* text = value.as<const char*>();
        const size_t length = strlen(text);
        if (length < ISO_MIN_LEN) {
            return false;
        }
        const char tail = text[length - 1];
        const bool hasOffset = tail == 'Z' || (length >= ISO_MIN_LEN + 4 && (text[length - 6] == '+' || text[length - 6] == '-'));
        if (!hasOffset) {
            return false;
        }
        const time_t parsed = TimeUtil::parseIso8601(text);
        out = parsed;

        return parsed > 0;
    }
    if (value.is<double>() || value.is<long>()) {
        const double seconds = value.as<double>();
        if (seconds <= 0 || seconds > 4294967295.0) {
            return false;
        }
        out = static_cast<time_t>(seconds);

        return true;
    }

    return false;
}

// "HH:MM" -> minutes since midnight
inline auto minuteOfDayFrom(const char* text, uint16_t& out) -> bool {
    int hours = 0;
    int minutes = 0;
    char extra = 0;
    if (text == nullptr || sscanf(text, "%d:%d%c", &hours, &minutes, &extra) != 2) {
        return false;
    }
    if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) {
        return false;
    }
    out = static_cast<uint16_t>(hours * Schedule::MINUTES_PER_HOUR + minutes);

    return true;
}

inline auto kindFromName(const char* name, uint8_t& out) -> bool {
    for (int kind = 0; kind < Attention::KIND_COUNT; kind++) {
        if (strcmp(name, Attention::kindName(static_cast<Attention::Kind>(kind))) == 0) {
            out = static_cast<uint8_t>(kind);

            return true;
        }
    }

    return false;
}

}  // namespace detail

/**
 * @brief Build a Record from one event object. `lastFired` is left 0: the store keeps the old value for a known id.
 * @return nullptr on success, otherwise a message for the 400 reply.
 */
inline auto fromJson(JsonVariantConst doc, Record& out) -> const char* {
    if (!doc.is<JsonObjectConst>()) {
        return "an event must be a JSON object";
    }
    Record record = blank();

    const char* id = doc["id"].is<const char*>() ? doc["id"].as<const char*>() : "";
    if (!validId(id)) {
        return "id is required: 1-16 characters of A-Z a-z 0-9 . _ -";
    }
    detail::copyText(record.id, id);

    const char* title = doc["title"].is<const char*>() ? doc["title"].as<const char*>() : "";
    if (*title == '\0') {
        return "title is required";
    }
    detail::copyText(record.title, title);
    detail::copyText(record.body, doc["body"].is<const char*>() ? doc["body"].as<const char*>() : "");
    detail::copyText(record.value, doc["value"].is<const char*>() ? doc["value"].as<const char*>() : "");

    uint8_t kind = static_cast<uint8_t>(Attention::K_INFO);
    if (doc["kind"].is<const char*>() && !detail::kindFromName(doc["kind"].as<const char*>(), kind)) {
        return "unknown kind";
    }
    record.kind = kind;

    const char* icon = doc["icon"].is<const char*>() ? doc["icon"].as<const char*>() : "";
    if (*icon == '\0' && doc["type"].is<const char*>()) {
        icon = doc["type"].as<const char*>();  // "type" is an alias of "icon", as for push cards
    }
    record.icon = static_cast<uint8_t>(Attention::iconFrom(icon));
    if (*icon != '\0' && record.icon == Attention::ICON_NONE) {
        return "unknown icon";
    }

    record.display = static_cast<uint8_t>(kind == Attention::K_PROGRESS ? Attention::SHOW_QUEUE : Attention::SHOW_INTERRUPT);
    if (doc["display"].is<const char*>()) {
        const char* display = doc["display"].as<const char*>();
        if (strcmp(display, "queue") == 0) {
            record.display = static_cast<uint8_t>(Attention::SHOW_QUEUE);
        } else if (strcmp(display, "interrupt") == 0) {
            record.display = static_cast<uint8_t>(Attention::SHOW_INTERRUPT);
        } else {
            return "display must be interrupt or queue";
        }
    }

    if (!doc["ttl_s"].isNull()) {
        const double ttl = doc["ttl_s"].as<double>();
        if (!doc["ttl_s"].is<double>() || ttl < 0 || ttl > static_cast<double>(MAX_TTL_S)) {
            return "ttl_s must be 0-86400";
        }
        record.ttlS = static_cast<uint16_t>(ttl);
    }
    if (!doc["enabled"].isNull()) {
        if (!doc["enabled"].is<bool>()) {
            return "enabled must be true or false";
        }
        record.flags = doc["enabled"].as<bool>() ? FLAG_ENABLED : 0;
    }

    // When: either one absolute instant, or a repeat rule with a local time of day
    const bool hasAt = !doc["at"].isNull();
    const bool hasRepeat = !doc["repeat"].isNull();
    if (hasAt == hasRepeat) {
        return "give either \"at\" (one-off) or \"repeat\" with \"time\" (recurring)";
    }
    if (hasAt) {
        time_t instant = 0;
        if (!detail::instantFrom(doc["at"], instant)) {
            return "at must be epoch seconds or ISO 8601 with an offset (Z or +hh:mm)";
        }
        record.at = static_cast<uint32_t>(instant);
    } else {
        const char* repeat = doc["repeat"].is<const char*>() ? doc["repeat"].as<const char*>() : "";
        if (strcmp(repeat, "daily") == 0) {
            record.repeat = static_cast<uint8_t>(Schedule::Repeat::Daily);
        } else if (strcmp(repeat, "weekly") == 0) {
            record.repeat = static_cast<uint8_t>(Schedule::Repeat::Weekly);
        } else if (strcmp(repeat, "monthly") == 0) {
            record.repeat = static_cast<uint8_t>(Schedule::Repeat::Monthly);
        } else {
            return "repeat must be daily, weekly or monthly";
        }
        if (!detail::minuteOfDayFrom(doc["time"].is<const char*>() ? doc["time"].as<const char*>() : nullptr, record.minuteOfDay)) {
            return "time must be local HH:MM";
        }
        if (record.repeat == static_cast<uint8_t>(Schedule::Repeat::Weekly)) {
            JsonArrayConst days = doc["days"].as<JsonArrayConst>();
            if (days.isNull() || days.size() == 0) {
                return "weekly needs days, for example [\"mon\",\"wed\"]";
            }
            record.weekdayMask = 0;
            for (JsonVariantConst day : days) {
                const int number = detail::weekdayFrom(day);
                if (number < 0) {
                    return "days must be sun..sat or 0-6";
                }
                record.weekdayMask = static_cast<uint8_t>(record.weekdayMask | (1U << static_cast<unsigned>(number)));
            }
        }
        if (record.repeat == static_cast<uint8_t>(Schedule::Repeat::Monthly)) {
            const int day = doc["day"].is<int>() ? doc["day"].as<int>() : 0;
            if (day < 1 || day > MAX_DAY_OF_MONTH) {
                return "monthly needs day 1-31";
            }
            record.dayOfMonth = static_cast<uint8_t>(day);
        }
        time_t bound = 0;
        if (!doc["from"].isNull()) {
            if (!detail::instantFrom(doc["from"], bound)) {
                return "from must be epoch seconds or ISO 8601 with an offset";
            }
            record.from = static_cast<uint32_t>(bound);
        }
        if (!doc["until"].isNull()) {
            if (!detail::instantFrom(doc["until"], bound)) {
                return "until must be epoch seconds or ISO 8601 with an offset";
            }
            record.until = static_cast<uint32_t>(bound);
        }
    }

    out = record;

    return nullptr;
}

/** @brief The stored fields (not the computed next firing time, which needs the clock). */
inline void toJson(const Record& record, JsonDocument& doc, size_t index) {
    doc[index]["id"] = record.id;
    doc[index]["title"] = record.title;
    if (record.body[0] != '\0') {
        doc[index]["body"] = record.body;
    }
    if (record.value[0] != '\0') {
        doc[index]["value"] = record.value;
    }
    doc[index]["kind"] = Attention::kindName(static_cast<Attention::Kind>(record.kind));
    if (record.icon != Attention::ICON_NONE) {
        doc[index]["icon"] = Attention::iconName(static_cast<Attention::Icon>(record.icon));
    }
    doc[index]["display"] = record.display == Attention::SHOW_QUEUE ? "queue" : "interrupt";
    if (record.ttlS != TTL_DEFAULT) {
        doc[index]["ttl_s"] = record.ttlS;
    }
    doc[index]["enabled"] = enabled(record);

    const auto repeat = static_cast<Schedule::Repeat>(record.repeat);
    if (repeat == Schedule::Repeat::None) {
        doc[index]["at"] = record.at;
    } else {
        static const char* const NAMES[] = {"", "daily", "weekly", "monthly"};  // NOLINT(modernize-avoid-c-arrays)
        doc[index]["repeat"] = NAMES[record.repeat];                              // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
        char clock[12];                                                           // NOLINT(modernize-avoid-c-arrays)
        snprintf(clock, sizeof(clock), "%02d:%02d", record.minuteOfDay / Schedule::MINUTES_PER_HOUR,
                 record.minuteOfDay % Schedule::MINUTES_PER_HOUR);
        doc[index]["time"] = clock;
        if (repeat == Schedule::Repeat::Weekly) {
            size_t slot = 0;
            for (int day = 0; day <= static_cast<int>(MAX_WEEKDAY); day++) {
                if ((record.weekdayMask & (1U << static_cast<unsigned>(day))) != 0) {
                    doc[index]["days"][slot++] = detail::WEEKDAY_NAMES[day];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
                }
            }
        }
        if (repeat == Schedule::Repeat::Monthly) {
            doc[index]["day"] = record.dayOfMonth;
        }
        if (record.from != 0) {
            doc[index]["from"] = record.from;
        }
        if (record.until != 0) {
            doc[index]["until"] = record.until;
        }
    }
    doc[index]["last_fired"] = record.lastFired;
}

}  // namespace Events
