// SPDX-License-Identifier: MIT
// Adapted from glimmer (https://github.com/Avinava/glimmer), Copyright (c) 2026 glimmer contributors.
// Used under the MIT license; see THIRD_PARTY.md.
// Night face — shown alone (no rotation) during the night window when the
// night mode is "clock" or "dark". Low-contrast time + date, nothing else, so
// a desk display at night reads the time without lighting the room.
//
//   y=64..150  VT323-86 HH:MM, digits INK_DIM, colon MUTED
//   y=172      date (+ AM/PM in 12-hour mode), MUTED

#include "channel.h"
#include "display.h"
#include "theme.h"
#include "config.h"
#include "clockfmt.h"
#include <time.h>

#include "ui/Night.h"

static constexpr int kClockY = 64;
static int s_hh = -1, s_mm = -1, s_day = -1;

bool chNightEnabled(const ChannelCtx&) {
    return Night::active();
}

static void geom(int& hhX, int& colonX, int& mmX, int& digitW) {
    Display::useFont("VT323-86");
    digitW = tft.textWidth("00") - tft.textWidth("0");   // advance, not the right edge of one glyph
    int colonW = tft.textWidth("::") - tft.textWidth(":");
    hhX = (SCREEN_W - (digitW * 4 + colonW)) / 2;
    colonX = hhX + digitW * 2;
    mmX = colonX + colonW;
}

static bool s_colonOn = true;

static void paintColonOnly(bool on) {
    int hhX, colonX, mmX, dw;
    geom(hhX, colonX, mmX, dw);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(on ? Theme::MUTED : Theme::BG, Theme::BG);
    tft.drawString(":", colonX, kClockY);
    s_colonOn = on;
}

static void paintTime(int hh, int mm, bool h24, bool hoursToo) {
    int hhX, colonX, mmX, dw;
    geom(hhX, colonX, mmX, dw);
    tft.setTextDatum(TL_DATUM);
    char b[4];
    if (hoursToo) {
        tft.fillRect(hhX, kClockY, dw * 2, 86, Theme::BG);
        ClockFmt::hourField(hh, h24, b, sizeof(b));
        tft.setTextColor(Theme::INK_DIM, Theme::BG);
        tft.drawString(b, hhX, kClockY);
        paintColonOnly(s_colonOn);
    }
    tft.fillRect(mmX, kClockY, dw * 2, 86, Theme::BG);
    snprintf(b, sizeof(b), "%02d", mm);
    tft.setTextColor(Theme::INK_DIM, Theme::BG);
    tft.drawString(b, mmX, kClockY);
}

static void paintDate(const struct tm& tmv, bool h24) {
    char date[24];
    strftime(date, sizeof(date), "%a %b %d", &tmv);
    for (char* p = date; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    if (!h24) strncat(date, tmv.tm_hour < 12 ? "  AM" : "  PM", sizeof(date) - strlen(date) - 1);
    tft.fillRect(0, 164, SCREEN_W, 18, Theme::BG);
    Display::useFont("DMMono-11");
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(date, SCREEN_W / 2, 168);
}

void chNightDraw(const ChannelCtx& ctx) {
    Display::clear();
    time_t now = time(nullptr);
    struct tm tmv; localtime_r(&now, &tmv);
    const bool h24 = ctx.settings->clock24h;
    s_colonOn = (tmv.tm_sec % 2) == 0;
    paintTime(tmv.tm_hour, tmv.tm_min, h24, true);
    paintDate(tmv, h24);
    s_hh = tmv.tm_hour; s_mm = tmv.tm_min; s_day = tmv.tm_yday;
}

void chNightTick(const ChannelCtx& ctx) {
    time_t now = time(nullptr);
    struct tm tmv; localtime_r(&now, &tmv);
    const bool colonOn = (tmv.tm_sec % 2) == 0;
    if (colonOn != s_colonOn) paintColonOnly(colonOn);
    if (tmv.tm_min == s_mm) return;
    const bool h24 = ctx.settings->clock24h;
    paintTime(tmv.tm_hour, tmv.tm_min, h24, tmv.tm_hour != s_hh);
    if (tmv.tm_yday != s_day || tmv.tm_hour != s_hh) paintDate(tmv, h24);
    s_hh = tmv.tm_hour; s_mm = tmv.tm_min; s_day = tmv.tm_yday;
}
