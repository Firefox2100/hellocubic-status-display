// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>

#include "ui/presence_mode.h"

/**
 * @brief The persisted presence state ("busy", "away", "relaxing" or back to "normal").
 *
 * Kept in a tiny file on flash so it survives a reboot or a power cut: a cube that says "busy" keeps saying it.
 * Cards (notifications) still interrupt every state; when they expire the presence screen returns.
 */
namespace Presence {

/** @brief Load the saved state (Normal when there is none or it is damaged). */
void begin();

auto mode() -> Mode;

/** @brief Optional line shown under the big word (empty = a default per mode). */
auto text() -> const String&;

/** @brief GIF used while relaxing (empty = the configured idle GIF, else the first one in /gif). */
auto gif() -> const String&;

/** @brief Counts changes, so the screen scheduler knows when to repaint. */
auto revision() -> uint32_t;

/**
 * @brief Change the state and save it.
 * @param text line under the big word (truncated to TEXT_MAX), may be empty
 * @param gif  GIF file name for Relaxing, may be empty
 * @return false if saving to flash failed (the state is still changed in RAM)
 */
auto set(Mode mode, const char* text, const char* gif) -> bool;

}  // namespace Presence
