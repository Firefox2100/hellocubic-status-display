// SPDX-License-Identifier: GPL-3.0-or-later
//
//   GET  /api/v1/night   settings plus the current state and why
//   POST /api/v1/night   {"mode":"disabled|sunset|schedule|manual","style":"face|off","start":"22:00","end":"07:00",
//                         "brightness":10,"switch":"on|off|auto"}  (every field optional)
//
// Night mode: the cube shows a dim clock only (style "face", cards still appear) or turns the screen off (style
// "off", cards do not appear either). "switch" is the manual switch: in mode "manual" it is the saved state, in the
// automatic modes it lasts until the automatic state next changes ("auto" drops it).
#include <ArduinoJson.h>

#include <array>

#include "config/ConfigManager.h"
#include "ui/ChannelManager.h"
#include "ui/Night.h"
#include "weather.h"
#include "web/Api.h"
#include "web/Webserver.h"

extern ConfigManager configManager;

// Subscripting a JsonObject with a string literal reads as a "misplaced array index" to clang-tidy.
// NOLINTBEGIN(readability-misplaced-array-index)
namespace {
constexpr size_t MAX_BODY = 512;

void sendError(Webserver* webserver, const char* message) {
    JsonDocument doc;
    doc[F("status")] = "error";
    doc[F("message")] = message;
    sendJsonDoc(webserver, HTTP_CODE_BAD_REQUEST, doc);
}

void sendState(Webserver* webserver) {
    const UiSettings& settings = configManager.ui;
    JsonDocument doc;
    std::array<char, NightCore::HM_SIZE> text{};
    doc[F("mode")] = settings.nightMode.c_str();
    doc[F("style")] = settings.nightStyle.c_str();
    NightCore::formatHm(settings.nightStart, text.data(), text.size());
    doc[F("start")] = text.data();
    NightCore::formatHm(settings.nightEnd, text.data(), text.size());
    doc[F("end")] = text.data();
    doc[F("brightness")] = settings.nightBrightness;
    doc[F("manual")] = settings.nightManual;
    doc[F("active")] = Night::active();
    doc[F("screen_off")] = Night::dark();
    doc[F("reason")] = Night::reason();
    doc[F("override")] = Night::overrideName();
    doc[F("backlight_pct")] = Night::backlightPct();
    doc[F("active_channel")] = ChannelManager::activeName();
    const WeatherData& data = Weather::snapshot();
    JsonObject sun = doc[F("sun")].to<JsonObject>();
    sun[F("available")] = data.valid && data.forecast[0].sunrise > 0;
    sun[F("sunrise")] = static_cast<uint32_t>(data.forecast[0].sunrise);
    sun[F("sunset")] = static_cast<uint32_t>(data.forecast[0].sunset);
    sendJsonDoc(webserver, HTTP_CODE_OK, doc);
}

void handleGet(Webserver* webserver) {
    if (requireBearerToken(webserver)) {
        sendState(webserver);
    }
}

/** @brief Validate the settings fields of a request onto `next`. @return nullptr if fine, else the 400 message. */
auto applyFields(JsonDocument& body, UiSettings& next) -> const char* {
    if (!body[F("mode")].isNull()) {
        NightCore::Mode mode = NightCore::Mode::Disabled;
        if (!NightCore::modeFrom(body[F("mode")] | "", mode)) {
            return "mode must be disabled, sunset, schedule or manual";
        }
        next.nightMode = NightCore::modeName(mode);
    }
    if (!body[F("style")].isNull()) {
        NightCore::Style style = NightCore::Style::Face;
        if (!NightCore::styleFrom(body[F("style")] | "", style)) {
            return "style must be face or off";
        }
        next.nightStyle = NightCore::styleName(style);
    }
    int minutes = 0;
    if (!body[F("start")].isNull()) {
        if (!NightCore::parseHm(body[F("start")] | "", minutes)) {
            return "start must be HH:MM (00:00 to 23:59)";
        }
        next.nightStart = static_cast<uint16_t>(minutes);
    }
    if (!body[F("end")].isNull()) {
        if (!NightCore::parseHm(body[F("end")] | "", minutes)) {
            return "end must be HH:MM (00:00 to 23:59)";
        }
        next.nightEnd = static_cast<uint16_t>(minutes);
    }
    if (!body[F("brightness")].isNull()) {
        const int pct = body[F("brightness")].is<int>() ? body[F("brightness")].as<int>() : 0;
        if (pct < NightCore::BRIGHTNESS_MIN || pct > NightCore::BRIGHTNESS_MAX) {
            return "brightness must be between 1 and 100";
        }
        next.nightBrightness = static_cast<uint8_t>(pct);
    }

    return nullptr;
}

void handleSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    const String& raw = webserver->raw().arg("plain");
    JsonDocument body;
    if (raw.length() > MAX_BODY || deserializeJson(body, raw) != DeserializationError::Ok || !body.is<JsonObject>()) {
        sendError(webserver, "bad json");

        return;
    }
    UiSettings next = configManager.ui;
    if (const char* error = applyFields(body, next); error != nullptr) {
        sendError(webserver, error);

        return;
    }
    const char* action = body[F("switch")].isNull() ? "" : (body[F("switch")] | "?");
    if (!body[F("switch")].isNull() && strcmp(action, "on") != 0 && strcmp(action, "off") != 0 && strcmp(action, "auto") != 0) {
        sendError(webserver, "switch must be on, off or auto");

        return;
    }

    const bool changedRules = next.nightMode != configManager.ui.nightMode || next.nightStart != configManager.ui.nightStart ||
                              next.nightEnd != configManager.ui.nightEnd;
    configManager.ui = next;
    if (changedRules) {
        Night::clearOverride();  // a manual switch belongs to the old rules
    }
    bool saved = true;
    if (strcmp(action, "on") == 0 || strcmp(action, "off") == 0) {
        saved = Night::switchTo(strcmp(action, "on") == 0);
    } else if (strcmp(action, "auto") == 0) {
        Night::clearOverride();
    }
    saved = configManager.save() && saved;
    ChannelManager::applySettings(configManager.ui);
    if (!saved) {
        sendError(webserver, "changed, but saving to flash failed");

        return;
    }
    sendState(webserver);
}
}  // namespace
// NOLINTEND(readability-misplaced-array-index)

void registerNightEndpoints(Webserver* webserver) {
    // @openapi {get} /night version=v1 group=Night summary="Night face settings, current state and why" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/night", HTTP_GET, handleGet);

    // @openapi {post} /night version=v1 group=Night summary="Change the night settings and/or switch night on or off by hand" requiresAuth=true
    // requestBody=application/json requestBodySchema=mode:string,style:string,start:string,end:string,brightness:number,switch:string example={"mode":"sunset","style":"face","switch":"on"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->route("/api/v1/night", HTTP_POST, handleSet);
}
