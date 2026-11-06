// SPDX-License-Identifier: GPL-3.0-or-later
//
//   GET  /api/v1/presence   {"mode":"busy","text":"","gif":""}
//   POST /api/v1/presence   {"mode":"normal|busy|away|relaxing","text":"back at 14:00","gif":"name.gif"}
//
// The state is saved on flash. Notification cards still interrupt every state.
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Logger.h>

#include "ui/Presence.h"
#include "web/Api.h"
#include "web/Webserver.h"

namespace {
constexpr size_t MAX_BODY = 512;
constexpr int HTTP_CODE_NOT_FOUND_ERR = 404;

void sendError(Webserver* webserver, int code, const char* message) {
    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = message;
    sendJsonDoc(webserver, code, doc);
}

void sendState(Webserver* webserver) {
    JsonDocument doc;
    doc["mode"] = Presence::modeName(Presence::mode());
    doc["text"] = Presence::text();
    doc["gif"] = Presence::gif();
    sendJsonDoc(webserver, HTTP_CODE_OK, doc);
}

void handleGet(Webserver* webserver) {
    if (requireBearerToken(webserver)) {
        sendState(webserver);
    }
}

void handleSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    const String& body = webserver->raw().arg("plain");
    JsonDocument doc;
    if (body.length() > MAX_BODY || deserializeJson(doc, body) != DeserializationError::Ok) {
        sendError(webserver, HTTP_CODE_BAD_REQUEST, "bad json");

        return;
    }

    Presence::Mode mode = Presence::Mode::Normal;
    if (!Presence::modeFrom(doc["mode"] | "", mode)) {
        sendError(webserver, HTTP_CODE_BAD_REQUEST, "mode must be normal, busy, away or relaxing");

        return;
    }
    const char* text = doc["text"] | "";
    if (strlen(text) > Presence::TEXT_MAX) {
        sendError(webserver, HTTP_CODE_BAD_REQUEST, "text is at most 27 characters");

        return;
    }
    const char* gif = doc["gif"] | "";
    if (gif[0] != '\0') {
        if (!Presence::validGifName(gif)) {
            sendError(webserver, HTTP_CODE_BAD_REQUEST, "gif must be a plain file name");

            return;
        }
        if (!LittleFS.exists(String("/gif/") + gif)) {
            sendError(webserver, HTTP_CODE_NOT_FOUND_ERR, "no such gif");

            return;
        }
    }

    const bool saved = Presence::set(mode, text, gif);
    Logger::info((String("presence ") + Presence::modeName(mode)).c_str(), "Presence");
    if (!saved) {
        sendError(webserver, HTTP_CODE_INTERNAL_ERROR, "changed, but saving to flash failed");

        return;
    }
    sendState(webserver);
}
}  // namespace

void registerPresenceEndpoints(Webserver* webserver) {
    // @openapi {get} /presence version=v1 group=Presence summary="Get the presence state (normal, busy, away, relaxing)" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/presence", HTTP_GET, handleGet);

    // @openapi {post} /presence version=v1 group=Presence summary="Set the presence state; it is saved and survives a reboot" requiresAuth=true
    // requestBody=application/json requestBodySchema=mode:string,text:string,gif:string example={"mode":"away","text":"back at 14:00"}
    // responses=200:application/json,400:application/json,401:application/json,404:application/json
    webserver->route("/api/v1/presence", HTTP_POST, handleSet);
}
