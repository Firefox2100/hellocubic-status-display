// SPDX-License-Identifier: MIT
// Adapted from glimmer (https://github.com/Avinava/glimmer), Copyright (c) 2026 glimmer contributors.
// Used under the MIT license; see THIRD_PARTY.md.
// Clock — design-true from ClockScreen, with partial-redraw tick().
//
//   y=8..18   "TUE · MAY 17" DMMono-11 left  ·  "17°C" right
//   y=42..130 huge VT323-86 HH:MM, ":" CORAL between
//   y=148     "good morning, friend" PixelifySans-14
//   y=178     24-dot day-progress strip
//   y=210..   footer row Pip happy + week info

#include "channel.h"
#include "display.h"
#include "theme.h"
#include "layout.h"
#include "clockfmt.h"
#include "weather.h"
#include <time.h>

static int  s_lastHH = -1, s_lastMM = -1;
static char s_lastDate[20]  = "";
static char s_lastGreet[40] = "";
static int  s_lastDayHour = -1;
static bool s_colonOn = true;   // the colon blinks 1 s on / 1 s off, so a frozen screen is obvious

static constexpr int CLOCK_Y  = 42;
static constexpr int CLOCK_H  = 86;
static const char*   CLOCK_FN = "VT323-86";

static char s_greetBuf[40];
static const char* greetingFor(int hour, const char* name) {
    const char* who = (name && name[0]) ? name : "friend";
    const char* prefix;
    if (hour < 5)       prefix = "rest well";
    else if (hour < 12) prefix = "good morning";
    else if (hour < 17) prefix = "good afternoon";
    else if (hour < 21) prefix = "good evening";
    else                prefix = "winding down";
    snprintf(s_greetBuf, sizeof(s_greetBuf), "%s, %s", prefix, who);
    return s_greetBuf;
}

static void clockGeom(int& hhX, int& colonX, int& mmX, int& digitW, int& colonW) {
    Display::useFont(CLOCK_FN);
    // textWidth() of a single glyph is its right edge, not its advance, so a cell sized from "0" is ~5 px too
    // narrow: the right of the last minute digit was never cleared (ghosting). The advance is the width of
    // two glyphs minus the width of one.
    digitW = tft.textWidth("00") - tft.textWidth("0");
    colonW = tft.textWidth("::") - tft.textWidth(":");
    int total = digitW * 4 + colonW;
    hhX = (SCREEN_W - total) / 2;
    colonX = hhX + digitW * 2;
    mmX = colonX + colonW;
}

static void paintHH(int h, bool h24) {
    char buf[4]; ClockFmt::hourField(h, h24, buf, sizeof(buf));
    int hhX, colonX, mmX, dw, cw;
    clockGeom(hhX, colonX, mmX, dw, cw);
    tft.fillRect(hhX, CLOCK_Y, dw * 2, CLOCK_H, Theme::BG);
    Display::useFont(CLOCK_FN);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(buf, hhX, CLOCK_Y);
}

static void paintMM(int m) {
    char buf[4]; snprintf(buf, sizeof(buf), "%02d", m);
    int hhX, colonX, mmX, dw, cw;
    clockGeom(hhX, colonX, mmX, dw, cw);
    tft.fillRect(mmX, CLOCK_Y, dw * 2, CLOCK_H, Theme::BG);
    Display::useFont(CLOCK_FN);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(buf, mmX, CLOCK_Y);
}

static void paintColon(bool on) {
    int hhX, colonX, mmX, dw, cw;
    clockGeom(hhX, colonX, mmX, dw, cw);
    Display::useFont(CLOCK_FN);
    tft.setTextDatum(TL_DATUM);
    // Opaque text repaints the glyph cell, so "off" is the same glyph in the background colour.
    tft.setTextColor(on ? Theme::CORAL : Theme::BG, Theme::BG);
    tft.drawString(":", colonX, CLOCK_Y);
    s_colonOn = on;
}

// Date left; AM/PM right in 12-hour mode.
static void paintDateRow(const char* date, int hour, bool h24) {
    tft.fillRect(0, 6, SCREEN_W, 16, Theme::BG);
    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(date, 10, 8);
    if (!h24) {
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(Theme::AMBER, Theme::BG);
        tft.drawString(hour < 12 ? "AM" : "PM", SCREEN_W - 10, 8);
    }
}

static void paintGreeting(const char* msg) {
    tft.fillRect(0, 138, SCREEN_W, 22, Theme::BG);
    Display::useFont("PixelifySans-14");
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(Theme::INK_DIM, Theme::BG);
    tft.drawString(msg, SCREEN_W/2, 148);
}

static void paintDayStrip(int hour) {
    int dotW = 4, gap = 3;
    int stripW = 24 * dotW + 23 * gap;
    int sx = (SCREEN_W - stripW) / 2;
    tft.fillRect(sx, 178, stripW, 4, Theme::BG);
    for (int i = 0; i < 24; i++) {
        uint16_t c = (i < hour)         ? Theme::AMBER
                    : (i == hour)        ? Theme::INK
                                         : Theme::LINE;
        tft.fillRect(sx + i * (dotW + gap), 178, dotW, 4, c);
    }
}

bool chClockEnabled(const ChannelCtx& ctx) {
    return ctx.settings && (ctx.settings->idleMode == "clock" || ctx.settings->idleMode == "cycle")
        && time(nullptr) > 1000000000L && !Weather::homeActive(*ctx.settings);   // Home replaces it while weather works
}

void chClockDraw(const ChannelCtx& ctx) {
    Display::clear();
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);

    char date[20];
    strftime(date, sizeof(date), "%a · %b %d", &tmv);
    for (char* p = date; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    const bool h24 = ctx.settings->clock24h;
    paintDateRow(date, tmv.tm_hour, h24);
    strncpy(s_lastDate, date, sizeof(s_lastDate) - 1);

    paintHH(tmv.tm_hour, h24);
    paintColon((tmv.tm_sec % 2) == 0);
    paintMM(tmv.tm_min);
    s_lastHH = tmv.tm_hour;
    s_lastMM = tmv.tm_min;
    s_lastDayHour = tmv.tm_hour;

    const char* uname = ctx.settings ? ctx.settings->userName.c_str() : "";
    const char* g = greetingFor(tmv.tm_hour, uname);
    paintGreeting(g);
    strncpy(s_lastGreet, g, sizeof(s_lastGreet) - 1);

    paintDayStrip(tmv.tm_hour);

    // Footer (mascot removed in v0.13)
    Display::useFont("DMMono-11");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    char weekBuf[16];
    char isoBuf[8]; strftime(isoBuf, sizeof(isoBuf), "%V", &tmv);
    snprintf(weekBuf, sizeof(weekBuf), "w/ %s", isoBuf);
    tft.drawString(weekBuf, SCREEN_W - 10, 208);

    // End with VT323-86 loaded so tick() doesn't need to switch
    Display::useFont(CLOCK_FN);
}

void chClockTick(const ChannelCtx& ctx) {
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);

    const bool colonOn = (tmv.tm_sec % 2) == 0;
    if (colonOn != s_colonOn) paintColon(colonOn);

    if (tmv.tm_min != s_lastMM) {
        paintMM(tmv.tm_min);
        s_lastMM = tmv.tm_min;
    }
    if (tmv.tm_hour != s_lastHH) {
        const bool h24 = ctx.settings->clock24h;
        paintHH(tmv.tm_hour, h24);
        s_lastHH = tmv.tm_hour;

        char date[20];
        strftime(date, sizeof(date), "%a · %b %d", &tmv);
        for (char* p = date; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        // AM/PM flips at noon/midnight even when the date doesn't.
        if (strcmp(date, s_lastDate) != 0 || (!h24 && (tmv.tm_hour == 0 || tmv.tm_hour == 12))) {
            paintDateRow(date, tmv.tm_hour, h24);
            strncpy(s_lastDate, date, sizeof(s_lastDate) - 1);
        }
        const char* uname = ctx.settings ? ctx.settings->userName.c_str() : "";
        const char* g = greetingFor(tmv.tm_hour, uname);
        if (strcmp(g, s_lastGreet) != 0) {
            paintGreeting(g);
            strncpy(s_lastGreet, g, sizeof(s_lastGreet) - 1);
        }
        paintDayStrip(tmv.tm_hour);
        s_lastDayHour = tmv.tm_hour;
    }
}
