// SPDX-License-Identifier: GPL-3.0-or-later
// Channel scheduling modelled on glimmer's main loop (https://github.com/Avinava/glimmer, MIT, see THIRD_PARTY.md).
#pragma once

#include <Arduino.h>

#include "ui/settings.h"

/**
 * @brief Owns what is on the LCD. A "channel" is one full-screen view (clock, idle GIF, attention card,
 *        agents list). Exactly one is active at a time.
 *
 * API handlers only change state (push a card, play a GIF, change settings). update() renders from loop(),
 * never blocks, and repaints only what changed.
 */
namespace ChannelManager {

/**
 * @brief Enter the channel loop. The boot screen (BootScreen) is assumed to be on the LCD already.
 *
 * The "online" boot screen stays at least ~6 s after it first appeared, so the network name and IP can be read
 * while the rest of the system starts. In access-point mode the setup screen (AP name, password, IP) stays
 * permanently, since it is the only way to learn how to reach the device.
 */
void begin(bool apMode);

/** @brief Drive the active channel. Call once per loop() iteration. */
void update();

/** @brief The device left (or entered) access-point setup mode: show the right screens from now on. */
void setApMode(bool apMode);

/** @brief Apply new display settings. Clears any manual GIF override. */
void applySettings(const UiSettings& settings);

/** @brief Force a full redraw of the active channel (for example after a rotation change). */
void invalidate();

/**
 * @brief Manually show one looping GIF until stopIdleGif() or applySettings().
 * @return false if the GIF could not be started (missing file, low heap, previous crash)
 */
auto playIdleGif(const String& path) -> bool;

/** @brief Drop the manual GIF override and return to the configured channels. */
auto stopIdleGif() -> bool;

/**
 * @brief Free the GIF decoder's ~24 KB right now (a heavy request, such as setting credentials, is about to run).
 * The GIF is not retried for a minute.
 */
void releaseMemory();

/** @brief Name of the channel on screen ("Clock", "Gif", "Attention", "Agents" or "Info"). */
auto activeName() -> const char*;

/** @brief Jump to a channel by name (case-insensitive). @return false if it is not enabled. */
auto showChannel(const char* name) -> bool;

/** @brief Advance to the next enabled channel. */
void nextChannel();

}  // namespace ChannelManager
