// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/Presence.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Logger.h>

namespace {
constexpr const char* LOG_TAG = "Presence";
constexpr const char* FILE_PATH = "/presence.json";
constexpr size_t MAX_FILE_BYTES = 256;

Presence::Mode g_mode = Presence::Mode::Normal;
String g_text;
String g_gif;
uint32_t g_revision = 0;

auto save() -> bool {
    JsonDocument doc;
    doc[F("mode")] = Presence::modeName(g_mode);
    doc[F("text")] = g_text;
    doc[F("gif")] = g_gif;

    File file = LittleFS.open(FILE_PATH, "w");
    if (!file) {
        return false;
    }
    const size_t written = serializeJson(doc, file);
    file.close();

    return written > 0;
}
}  // namespace

void Presence::begin() {
    File file = LittleFS.open(FILE_PATH, "r");
    if (!file) {
        return;
    }
    JsonDocument doc;
    const bool parsed = file.size() <= MAX_FILE_BYTES && deserializeJson(doc, file) == DeserializationError::Ok;
    file.close();
    Mode mode = Mode::Normal;
    if (!parsed || !modeFrom(doc[F("mode")] | "", mode)) {
        Logger::warn(F("Presence file unreadable, starting normal"), LOG_TAG);

        return;
    }
    g_mode = mode;
    g_text = String(doc[F("text")] | "").substring(0, TEXT_MAX);
    const char* gifName = doc[F("gif")] | "";
    g_gif = validGifName(gifName) ? gifName : "";
}

auto Presence::mode() -> Mode { return g_mode; }

auto Presence::text() -> const String& { return g_text; }

auto Presence::gif() -> const String& { return g_gif; }

auto Presence::revision() -> uint32_t { return g_revision; }

auto Presence::set(Mode mode, const char* text, const char* gif) -> bool {
    g_mode = mode;
    g_text = String(text != nullptr ? text : "").substring(0, TEXT_MAX);
    g_gif = validGifName(gif) ? gif : "";
    g_revision++;

    return save();
}
