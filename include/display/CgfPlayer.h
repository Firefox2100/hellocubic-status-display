// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>

/**
 * @brief Plays a CGF1 animation (see include/display/cgf.h) straight to the panel, non-blocking.
 *
 * Needs ~1.5 KB of heap while playing and none otherwise, unlike the GIF decoder (24 KB). start() takes the screen
 * (clears it); update() draws at most one frame per call when its time has come.
 */
namespace CgfPlayer {

/** @brief True if the file name is a CGF1 animation (".cgf"). */
auto isCgf(const String& path) -> bool;

/** @brief Heap the player needs; callers can check before starting. */
constexpr uint32_t HEAP_NEEDED = 3072;

auto start(const String& path) -> bool;
void stop();
auto playing() -> bool;

/** @brief Draw the next frame if it is due. Call from loop(). */
void update();

}  // namespace CgfPlayer
