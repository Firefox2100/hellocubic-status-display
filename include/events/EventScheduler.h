// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ArduinoJson.h>

/**
 * @brief Turns stored events into notification cards at their time.
 *
 * Needs the clock: until NTP has set it nothing fires. When it first does, an occurrence that is at most 5 minutes
 * old still fires (it was missed while the device was off or offline); older ones are skipped, and one-off events
 * whose time has passed are deleted. A fired one-off is deleted; a recurring event records when it last fired, so a
 * reboot or a re-sent schedule never fires it twice.
 *
 * update() is cheap: it wakes once a second and only does real work when something is due or the schedule changed.
 */
namespace EventScheduler {

constexpr long GRACE_S = 300;  // how late an event may still fire

void begin();
void update();

/** @brief Seconds until the next event fires, or -1 if none / the clock is not set. */
auto secondsToNext() -> long;

/** @brief Fire an event right now (testing), regardless of its schedule. @return false if there is no such id. */
auto fireNow(const char* eventId) -> bool;

/** @brief Counts, clock state and what fires next. */
void fillStatus(JsonDocument& doc);

}  // namespace EventScheduler
