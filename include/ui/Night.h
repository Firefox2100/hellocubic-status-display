// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>

#include "ui/night_core.h"

/**
 * @brief The night face: decides when it is night, and what the backlight should be.
 *
 * Modes (settings `night_mode`): disabled, sunset (sunset to sunrise from the weather data, else the schedule
 * window), schedule (start to end every day), manual (only when switched). A manual switch on top of sunset or
 * schedule lasts until the automatic state changes. Styles: `face` (dim night clock only, no rotation, notifications
 * still shown) and `off` (the screen is off, notifications included).
 */
namespace Night {

/** @brief Re-evaluate (cheap; call every loop). Needs the clock to be set, else it is never night. */
void update();

auto active() -> bool;

/** @brief Night with the screen off (style `off`). */
auto dark() -> bool;

/** @brief Counts changes of active(), so the screen scheduler knows when to repaint. */
auto revision() -> uint32_t;

/** @brief Backlight % the screen should have right now (100 by day). */
auto backlightPct() -> int;

/** @brief Why it is (not) night: "disabled", "no-time", "manual", "override", "sun", "schedule", "schedule-fallback". */
auto reason() -> const char*;

/** @brief "none", "night" or "day": a manual switch on top of an automatic mode. */
auto overrideName() -> const char*;

/**
 * @brief Switch night on or off by hand. In `manual` mode this changes the saved state; otherwise it is a temporary
 *        override that ends when the automatic state next changes.
 * @return false if saving failed
 */
auto switchTo(bool night) -> bool;

/** @brief Drop a temporary override (back to the automatic state). */
void clearOverride();

}  // namespace Night
