// SPDX-License-Identifier: MIT
// Adapted from glimmer (https://github.com/Avinava/glimmer), Copyright (c) 2026 glimmer contributors.
// Used under the MIT license; see THIRD_PARTY.md.
//
// UI helpers on top of the shared TFT_eSPI instance (`tft`, see display/DisplayManager.h).
#pragma once
#include <Arduino.h>

#include "display/DisplayManager.h"
#include "layout.h"
#include "theme.h"

namespace Display {
void clear();

// VLW bitmap fonts live on LittleFS under /fonts. One font is cached at a time; switching costs ~50 ms.
//   "VT323-86"  clock digits        "VT323-64/44/32"  big numerics, headlines
//   "Silkscreen-16/12"  titles      "DMMono-11"  tabular data / body
//   "PixelifySans-22/14"  friendly copy
void useFont(const char* name);
// Drop the loaded font (frees a few KB of heap, for example before starting the GIF decoder).
void releaseFont();

// 16-row bitmap glyph (row bitmasks, MSB = leftmost pixel), each pixel drawn as a scale x scale block.
void drawGlyph16(const uint16_t rows[16], int x, int y, uint16_t color, int scale = 1);

// 16x16 key glyph (credential problems) and 32-px circle-slash (blocked).
void drawKeyGlyph(int x, int y, uint16_t color, int scale = 1);
void drawBlockedGlyph(int cx, int cy, uint16_t color);

// Universal 22-px status bar with an accent under-line.
void statusBar(const char* title, const char* rightMeta, uint16_t accent, uint16_t metaColor = Theme::MUTED);
void statusMeta(const char* rightMeta, uint16_t accent, uint16_t metaColor = Theme::MUTED);

// Flat 9-segment fill bar with a 1-px frame.
void pixelBar(int x, int y, int w, int h, float pct, uint16_t color);

// 1-px dotted divider line (4 px on, 1 px off).
void dotsDivider(int x, int y, int w);

// Animated "loading" chase dots.
void loadingDots(int x, int y, int litIndex, uint16_t accent, int count = 5);

uint16_t usageColor(float pct);
}  // namespace Display
