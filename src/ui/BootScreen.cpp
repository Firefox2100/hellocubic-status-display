// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/BootScreen.h"

#include <ESP8266WiFi.h>

#include "display.h"

namespace {
enum class Mode : uint8_t { None, Connecting, Online, Setup };

constexpr int CENTER_X = SCREEN_W / 2;
constexpr int MAX_TEXT_WIDTH = 216;
constexpr size_t MAX_SSID_CHARS = 26;
constexpr int DOT_COUNT = 5;
constexpr int DOT_PITCH = 9;
constexpr int DOT_SIZE = 6;
constexpr int DOTS_X = (SCREEN_W - ((DOT_COUNT * DOT_PITCH) - (DOT_PITCH - DOT_SIZE))) / 2;
constexpr uint32_t DOT_STEP_MS = 150;

// Connecting
constexpr int CONNECTING_TITLE_Y = 76;
constexpr int CONNECTING_SSID_Y = 108;
constexpr int CONNECTING_DOTS_Y = 146;

// Online
constexpr int ONLINE_TITLE_Y = 42;
constexpr int ONLINE_SSID_Y = 74;
constexpr int ONLINE_IP_Y = 112;
constexpr int ONLINE_SIGNAL_Y = 150;
constexpr int ONLINE_NOTE_Y = 190;
constexpr int ONLINE_DOTS_Y = 206;

// Setup
constexpr int SETUP_TITLE_Y = 30;
constexpr int SETUP_JOIN_Y = 62;
constexpr int SETUP_SSID_Y = 86;
constexpr int SETUP_PASS_LABEL_Y = 120;
constexpr int SETUP_PASS_Y = 142;
constexpr int SETUP_OPEN_LABEL_Y = 176;
constexpr int SETUP_IP_Y = 198;
constexpr int SETUP_NOTE_Y = 226;

Mode g_mode = Mode::None;
int g_lastDot = -1;
uint32_t g_onlineSince = 0;
String g_connectingSsid;
String g_onlineNote;
String g_apSsid;
String g_apPassword;
String g_staSsid;

void centerText(const char* font, const String& text, int yPos, uint16_t color) {
    Display::useFont(font);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(color, Theme::BG);
    tft.drawString(text, CENTER_X, yPos);
}

/** @brief Draw `text` in the largest of two fonts that fits the screen width. */
void centerFitted(const char* big, const char* small, const String& text, int yPos, uint16_t color) {
    Display::useFont(big);
    centerText(tft.textWidth(text) <= MAX_TEXT_WIDTH ? big : small, text, yPos, color);
}

auto shortened(const String& text) -> String {
    if (text.length() <= MAX_SSID_CHARS) {
        return text;
    }

    return text.substring(0, MAX_SSID_CHARS - 3) + "...";
}

void paintDots(int yPos, uint16_t accent, uint32_t nowMs) {
    const int lit = static_cast<int>((nowMs / DOT_STEP_MS) % DOT_COUNT);
    if (lit == g_lastDot) {
        return;
    }
    g_lastDot = lit;
    Display::loadingDots(DOTS_X, yPos, lit, accent, DOT_COUNT);
}

void paintConnecting() {
    Display::clear();
    centerText("Silkscreen-16", "CONNECTING", CONNECTING_TITLE_Y, Theme::INK);
    centerText("DMMono-11", shortened(g_connectingSsid), CONNECTING_SSID_Y, Theme::MUTED);
    g_lastDot = -1;
}

void paintOnline() {
    Display::clear();
    centerText("Silkscreen-16", "ONLINE", ONLINE_TITLE_Y, Theme::MINT);
    centerText("DMMono-11", shortened(WiFi.SSID()), ONLINE_SSID_Y, Theme::MUTED);
    centerFitted("VT323-44", "VT323-32", WiFi.localIP().toString(), ONLINE_IP_Y, Theme::INK);
    centerText("DMMono-11", String("signal ") + WiFi.RSSI() + " dBm", ONLINE_SIGNAL_Y, Theme::MUTED);
    if (!g_onlineNote.isEmpty()) {
        centerText("DMMono-11", g_onlineNote, ONLINE_NOTE_Y, Theme::INK_DIM);
    }
    g_lastDot = -1;
}

void paintSetup() {
    Display::clear();
    centerText("Silkscreen-16", "SETUP MODE", SETUP_TITLE_Y, Theme::AMBER);
    centerText("DMMono-11", "join this Wi-Fi", SETUP_JOIN_Y, Theme::MUTED);
    centerFitted("VT323-32", "DMMono-11", g_apSsid, SETUP_SSID_Y, Theme::INK);
    centerText("DMMono-11", "password", SETUP_PASS_LABEL_Y, Theme::MUTED);
    centerFitted("VT323-32", "DMMono-11", g_apPassword, SETUP_PASS_Y, Theme::INK);
    centerText("DMMono-11", "then open", SETUP_OPEN_LABEL_Y, Theme::MUTED);
    centerFitted("VT323-32", "DMMono-11", WiFi.softAPIP().toString(), SETUP_IP_Y, Theme::SKY);
    if (!g_staSsid.isEmpty()) {
        centerText("DMMono-11", String("could not join ") + shortened(g_staSsid), SETUP_NOTE_Y, Theme::CORAL);
    }
}
}  // namespace

void BootScreen::showConnecting(const char* ssid) {
    const String name = ssid != nullptr ? ssid : "";
    if (g_mode != Mode::Connecting || name != g_connectingSsid) {
        g_connectingSsid = name;
        g_mode = Mode::Connecting;
        paintConnecting();
    }
    paintDots(CONNECTING_DOTS_Y, Theme::AMBER, millis());
}

void BootScreen::showOnline(const char* note) {
    const String text = note != nullptr ? note : "";
    if (g_mode != Mode::Online) {
        g_onlineSince = millis();
    }
    if (g_mode != Mode::Online || text != g_onlineNote) {
        g_onlineNote = text;
        g_mode = Mode::Online;
        paintOnline();
    }
    paintDots(ONLINE_DOTS_Y, Theme::SKY, millis());
}

void BootScreen::showSetup(const char* apSsid, const char* apPassword, const char* staSsid) {
    g_apSsid = apSsid != nullptr ? apSsid : "";
    g_apPassword = apPassword != nullptr ? apPassword : "";
    g_staSsid = staSsid != nullptr ? staSsid : "";
    redrawSetup();
}

void BootScreen::redrawSetup() {
    g_mode = Mode::Setup;
    paintSetup();
}

void BootScreen::tick(uint32_t nowMs) {
    if (g_mode == Mode::Connecting) {
        paintDots(CONNECTING_DOTS_Y, Theme::AMBER, nowMs);
    } else if (g_mode == Mode::Online) {
        paintDots(ONLINE_DOTS_Y, Theme::SKY, nowMs);
    }
}

auto BootScreen::onlineSinceMs() -> uint32_t { return g_onlineSince; }
