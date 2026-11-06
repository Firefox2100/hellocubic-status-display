// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>

/**
 * @brief Procedural notification icons: 16x16 one-bit glyphs with a few animation frames, drawn straight to the
 *        panel as filled blocks. The pixel data lives in flash (PROGMEM), so the icons cost no RAM, and there is
 *        no image decoder involved.
 *
 * Icon ids are Attention::Icon (include/glimmer/attention_core.h); 0 means "no icon".
 */
namespace Icons {

constexpr int GLYPH_SIZE = 16;
constexpr uint32_t FRAME_MS = 450;  // time each animation frame stays up

/** @brief Number of animation frames of an icon (0 for "none" or an unknown id). */
auto frameCount(uint8_t icon) -> uint8_t;

/** @brief The frame to show at `nowMs` for an animated icon. */
auto frameAt(uint8_t icon, uint32_t nowMs) -> uint8_t;

/**
 * @brief Draw one frame. The whole `GLYPH_SIZE * scale` square is cleared to `background` first, so a frame can be
 *        replaced by drawing the next one on top.
 */
void draw(uint8_t icon, uint8_t frame, int xPos, int yPos, int scale, uint16_t color, uint16_t background);

}  // namespace Icons
