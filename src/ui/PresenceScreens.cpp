// SPDX-License-Identifier: GPL-3.0-or-later
// Full-screen "busy" and "away" faces. Static pictures: nothing is redrawn after the first paint.
#include "channel.h"
#include "display.h"
#include "ui/Presence.h"

namespace {
constexpr int CENTER_X = SCREEN_W / 2;

// RGB565
constexpr uint16_t BUSY_RED = 0xE104;     // #E02020
constexpr uint16_t BUSY_WHITE = 0xFFFF;
constexpr uint16_t AWAY_BG = 0x10A2;      // dark slate
constexpr uint16_t AWAY_ACCENT = 0xFD20;  // orange
constexpr uint16_t AWAY_SUB = 0xC618;     // light grey

void centered(const char* font, const String& text, int yPos, uint16_t foreground, uint16_t background) {
    Display::useFont(font);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(foreground, background);
    tft.drawString(text, CENTER_X, yPos);
}

/** @brief Text that fits the screen: the long font when it fits, else the small one. */
void fitted(const String& text, int yPos, uint16_t foreground, uint16_t background) {
    constexpr int MAX_WIDTH = 224;
    Display::useFont("PixelifySans-22");
    centered(tft.textWidth(text) <= MAX_WIDTH ? "PixelifySans-22" : "PixelifySans-14", text, yPos, foreground, background);
}
}  // namespace

void presenceBusyDraw(const ChannelCtx& /*ctx*/) {
    constexpr int SIGN_Y = 66;
    constexpr int SIGN_RADIUS = 46;
    constexpr int SIGN_RIM = 8;
    constexpr int BAR_HALF_W = 30;
    constexpr int BAR_HALF_H = 8;
    constexpr int WORD_Y = 152;
    constexpr int NOTE_Y = 202;

    tft.fillScreen(BUSY_RED);
    tft.fillCircle(CENTER_X, SIGN_Y, SIGN_RADIUS, BUSY_WHITE);
    tft.fillCircle(CENTER_X, SIGN_Y, SIGN_RADIUS - SIGN_RIM, BUSY_RED);
    tft.fillRect(CENTER_X - BAR_HALF_W, SIGN_Y - BAR_HALF_H, 2 * BAR_HALF_W, 2 * BAR_HALF_H, BUSY_WHITE);
    centered("VT323-64", "BUSY", WORD_Y, BUSY_WHITE, BUSY_RED);
    fitted(Presence::text().isEmpty() ? String("do not disturb") : Presence::text(), NOTE_Y, BUSY_WHITE, BUSY_RED);
}

void presenceAwayDraw(const ChannelCtx& /*ctx*/) {
    constexpr int DOOR_X = 86;
    constexpr int DOOR_Y = 22;
    constexpr int DOOR_W = 48;
    constexpr int DOOR_H = 84;
    constexpr int DOOR_LINE = 5;
    constexpr int ARROW_Y = DOOR_Y + (DOOR_H / 2);
    constexpr int ARROW_TAIL_X = 112;
    constexpr int ARROW_TIP_X = 168;
    constexpr int ARROW_HEAD = 14;
    constexpr int ARROW_THICK = 6;
    constexpr int ARROW_EXTRA = 6;
    constexpr int WORD_Y = 152;
    constexpr int NOTE_Y = 202;

    tft.fillScreen(AWAY_BG);
    // a doorway with an arrow leaving through it
    tft.fillRect(DOOR_X, DOOR_Y, DOOR_W, DOOR_H, AWAY_ACCENT);
    tft.fillRect(DOOR_X + DOOR_LINE, DOOR_Y + DOOR_LINE, DOOR_W - (2 * DOOR_LINE), DOOR_H - DOOR_LINE, AWAY_BG);
    tft.fillRect(ARROW_TAIL_X, ARROW_Y - (ARROW_THICK / 2), ARROW_TIP_X - ARROW_TAIL_X, ARROW_THICK, AWAY_ACCENT);
    tft.fillTriangle(ARROW_TIP_X, ARROW_Y - ARROW_HEAD, ARROW_TIP_X, ARROW_Y + ARROW_HEAD, ARROW_TIP_X + ARROW_HEAD + ARROW_EXTRA,
                     ARROW_Y, AWAY_ACCENT);
    centered("VT323-64", "AWAY", WORD_Y, AWAY_ACCENT, AWAY_BG);
    fitted(Presence::text().isEmpty() ? String("not back soon") : Presence::text(), NOTE_Y, AWAY_SUB, AWAY_BG);
}

auto presenceEnabledBusy(const ChannelCtx& /*ctx*/) -> bool { return Presence::mode() == Presence::Mode::Busy; }
auto presenceEnabledAway(const ChannelCtx& /*ctx*/) -> bool { return Presence::mode() == Presence::Mode::Away; }
