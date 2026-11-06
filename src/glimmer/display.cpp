// SPDX-License-Identifier: MIT
// Adapted from glimmer (https://github.com/Avinava/glimmer), Copyright (c) 2026 glimmer contributors.
// Used under the MIT license; see THIRD_PARTY.md.
#include "display.h"

#include <LittleFS.h>

#include <cstring>

static const char* s_loadedFont = nullptr;

void Display::useFont(const char* name) {
    if (s_loadedFont && strcmp(s_loadedFont, name) == 0) return;
    if (s_loadedFont) tft.unloadFont();
    // TFT_eSPI prepends '/' and appends '.vlw', and defaults to SPIFFS, so pass the FS explicitly.
    String path = String("fonts/") + name;
    tft.loadFont(path, LittleFS);
    s_loadedFont = name;
}

void Display::releaseFont() {
    if (s_loadedFont) {
        tft.unloadFont();
        s_loadedFont = nullptr;
    }
}

void Display::clear() { tft.fillScreen(Theme::BG); }

void Display::drawGlyph16(const uint16_t rows[16], int x, int y, uint16_t color, int scale) {
    for (int row = 0; row < 16; row++) {
        uint16_t bits = rows[row];
        if (!bits) continue;
        for (int col = 0; col < 16; col++) {
            if (!(bits & (1 << (15 - col)))) continue;
            if (scale <= 1)
                tft.drawPixel(x + col, y + row, color);
            else
                tft.fillRect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

// key: ring on the left, shaft + two teeth to the right
static const uint16_t KEY_ROWS[16] = {
    0x0000, 0x0000, 0x0000, 0x3C00,
    0x4200, 0x8100, 0x81FE, 0x81FE,
    0x811A, 0x421A, 0x3C00, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000,
};

void Display::drawKeyGlyph(int x, int y, uint16_t color, int scale) {
    drawGlyph16(KEY_ROWS, x, y, color, scale);
}

// Circle with a diagonal slash, ~30 px across, 2 px stroke.
void Display::drawBlockedGlyph(int cx, int cy, uint16_t color) {
    tft.drawCircle(cx, cy, 14, color);
    tft.drawCircle(cx, cy, 13, color);
    for (int d = -1; d <= 1; d++)
        tft.drawLine(cx - 9 + d, cy + 9, cx + 9 + d, cy - 9, color);
}

uint16_t Display::usageColor(float pct) {
    if (pct < 0) return Theme::MUTED;
    if (pct <= 20.0f) return Theme::CORAL;
    if (pct <= 50.0f) return Theme::AMBER;
    return Theme::MINT;
}

void Display::statusBar(const char* title, const char* rightMeta, uint16_t accent, uint16_t metaColor) {
    using namespace Layout;
    tft.fillRect(0, STATUS_TOP, SCREEN_W, STATUS_BOTTOM, Theme::BG);

    Display::useFont("Silkscreen-12");
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(title, SCREEN_W / 2, STATUS_BOTTOM / 2);

    if (rightMeta && *rightMeta) {
        Display::useFont("DMMono-11");
        tft.setTextDatum(MR_DATUM);
        tft.setTextColor(metaColor, Theme::BG);
        tft.drawString(rightMeta, SCREEN_W - 4, STATUS_BOTTOM / 2);
    }

    tft.drawFastHLine(0, STATUS_BOTTOM, SCREEN_W, accent);
}

// Right-meta slot = the 76 px right of the centred title.
void Display::statusMeta(const char* rightMeta, uint16_t accent, uint16_t metaColor) {
    using namespace Layout;
    const int x = SCREEN_W - 76;
    tft.fillRect(x, STATUS_TOP, 76, STATUS_BOTTOM, Theme::BG);
    if (rightMeta && *rightMeta) {
        Display::useFont("DMMono-11");
        tft.setTextDatum(MR_DATUM);
        tft.setTextColor(metaColor, Theme::BG);
        tft.drawString(rightMeta, SCREEN_W - 4, STATUS_BOTTOM / 2);
    }
    tft.drawFastHLine(x, STATUS_BOTTOM, 76, accent);
}

void Display::pixelBar(int x, int y, int w, int h, float pct, uint16_t color) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    // Discrete 9-segment meter: each cell is fully on or fully off, so any value >= 89% lights every cell.
    constexpr int SEGS = 9;
    constexpr int GAP = 1;
    tft.drawRect(x, y, w, h, Theme::LINE);
    int innerX = x + 1, innerY = y + 1;
    int innerW = w - 2, innerH = h - 2;
    int totalGaps = (SEGS - 1) * GAP;
    int segW = (innerW - totalGaps) / SEGS;
    int leftover = innerW - (segW * SEGS + totalGaps);
    const float segPct = 100.0f / (float)SEGS;
    int cx = innerX;
    for (int i = 0; i < SEGS; i++) {
        int sw = segW + (i < leftover ? 1 : 0);
        bool lit = (pct > (float)i * segPct);
        tft.fillRect(cx, innerY, sw, innerH, lit ? color : Theme::PANEL);
        cx += sw + GAP;
    }
}

void Display::dotsDivider(int x, int y, int w) {
    for (int i = 0; i < w; i += 4) tft.drawPixel(x + i, y, Theme::LINE);
}

void Display::loadingDots(int x, int y, int litIndex, uint16_t accent, int count) {
    int lit = ((litIndex % count) + count) % count;
    for (int i = 0; i < count; i++) tft.fillRect(x + i * 9, y, 6, 6, i == lit ? accent : Theme::LINE);
}

// Channel theme colour (indicator strip, status bar accents).
uint16_t Theme::channelColor(const char* name) {
    if (!name) return MUTED;
    if (!strcmp(name, "Claude")) return CORAL;
    if (!strcmp(name, "Codex")) return LILAC;
    if (!strcmp(name, "Clock")) return AMBER;
    if (!strcmp(name, "Home")) return AMBER;
    if (!strcmp(name, "Gif")) return SKY;
    if (!strcmp(name, "Agents")) return LILAC;
    return MUTED;
}
