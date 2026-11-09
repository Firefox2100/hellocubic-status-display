// SPDX-License-Identifier: GPL-3.0-or-later
//
//   GET    /api/v1/events?offset=&limit=   list (streamed), or ?id=X to pull one event
//   POST   /api/v1/events                  insert or replace: one event object, or {"events":[...]} (up to 16)
//   DELETE /api/v1/events?id=X             delete one, or ?all=true to clear the schedule
//   POST   /api/v1/events/replace          replace the whole schedule, in parts if it is big (begin / commit)
//   POST   /api/v1/events/fire?id=X        fire an event right now (testing)
//   GET    /api/v1/events/status           counts, clock state, what fires next
#include <ArduinoJson.h>
#include <Logger.h>

#include <ctime>

#include "events/EventScheduler.h"
#include "events/EventStore.h"
#include "events/event_record.h"
#include "web/Api.h"
#include "web/Webserver.h"

namespace {
constexpr size_t MAX_BODY = 6144;
constexpr size_t MAX_BATCH = 16;
constexpr size_t MAX_LIST_LIMIT = Events::MAX_EVENTS;
constexpr size_t DEFAULT_LIST_LIMIT = 20;
constexpr int HTTP_CODE_TOO_LARGE = 413;
constexpr int HTTP_CODE_CONFLICT = 409;
constexpr long REASONABLE_EPOCH = 1000000000L;

void sendError(Webserver* webserver, int code, const __FlashStringHelper* message, int index = -1) {
    JsonDocument doc;
    doc[F("status")] = "error";
    doc[F("message")] = message;
    if (index >= 0) {
        doc[F("index")] = index;
    }
    sendJsonDoc(webserver, code, doc);
}

void sendText(Webserver* webserver, int code, const String& json) {
    setCorsHeaders(webserver);
    webserver->raw().send(code, "application/json", json);
}

/** @brief Read the body into `doc`, answering 413 / 400 itself. @return false if the caller should stop. */
auto parseBody(Webserver* webserver, JsonDocument& doc) -> bool {
    const String& body = webserver->raw().arg("plain");
    if (body.length() > MAX_BODY) {
        sendError(webserver, HTTP_CODE_TOO_LARGE, F("body too large (6 KB max): send fewer events per request"));

        return false;
    }
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        sendError(webserver, HTTP_CODE_BAD_REQUEST, F("bad json"));

        return false;
    }

    return true;
}

auto nextInSeconds(const Events::Record& record) -> long {
    const time_t now = time(nullptr);
    if (now < REASONABLE_EPOCH || !Events::enabled(record)) {
        return -1;
    }
    const time_t next = Schedule::nextAfter(Events::specOf(record), now);

    return next == 0 ? -1 : static_cast<long>(next - now);
}

/** @brief One event as a JSON object, with the computed `next_in_s`. */
auto eventJson(const Events::Record& record) -> String {
    JsonDocument doc;
    Events::toJson(record, doc, 0);
    doc[0][F("next_in_s")] = nextInSeconds(record);
    String text;
    serializeJson(doc[0], text);

    return text;
}

void handleList(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    ESP8266WebServer& server = webserver->raw();

    if (server.hasArg("id")) {
        Events::Record record;
        if (EventStore::find(server.arg("id").c_str(), &record) < 0) {
            sendError(webserver, HTTP_CODE_NOT_FOUND, F("no such event"));

            return;
        }
        sendText(webserver, HTTP_CODE_OK, eventJson(record));

        return;
    }

    const size_t total = EventStore::count();
    const size_t offset = server.hasArg("offset") ? static_cast<size_t>(server.arg("offset").toInt()) : 0;
    size_t limit = server.hasArg("limit") ? static_cast<size_t>(server.arg("limit").toInt()) : DEFAULT_LIST_LIMIT;
    if (limit == 0 || limit > MAX_LIST_LIMIT) {
        limit = MAX_LIST_LIMIT;
    }

    // Streamed one event at a time: the whole list would not fit in RAM next to everything else
    setCorsHeaders(webserver);
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(HTTP_CODE_OK, "application/json", "");
    JsonDocument status;
    EventScheduler::fillStatus(status);
    status[F("offset")] = offset;
    String head;
    serializeJson(status, head);
    head.remove(head.length() - 1);  // drop the closing brace: the events array follows
    server.sendContent(head + ",\"events\":[");
    Events::Record record;
    size_t sent = 0;
    for (size_t slot = offset; slot < total && sent < limit && EventStore::read(slot, record); slot++, sent++) {
        server.sendContent((sent > 0 ? "," : "") + eventJson(record));
    }
    server.sendContent("]}");
    server.sendContent("");
}

/** @brief Validate every event of a batch before anything is stored, so a bad one changes nothing. */
auto validateBatch(JsonArrayConst events, int& badIndex) -> Events::Msg {
    int index = 0;
    for (JsonVariantConst item : events) {
        Events::Record scratch;
        if (const Events::Msg error = Events::fromJson(item, scratch); error != nullptr) {
            badIndex = index;

            return error;
        }
        index++;
    }

    return nullptr;
}

/** @brief The events of a request: {"events":[...]} or a single event object. Empty/oversized batches are refused. */
auto batchOf(Webserver* webserver, JsonDocument& doc, JsonArrayConst& events, JsonDocument& single) -> bool {
    if (doc[F("events")].is<JsonArrayConst>()) {
        events = doc[F("events")].as<JsonArrayConst>();
    } else {
        single.add(doc.as<JsonVariant>());
        events = single.as<JsonArrayConst>();
    }
    if (events.size() == 0 || events.size() > MAX_BATCH) {
        sendError(webserver, HTTP_CODE_BAD_REQUEST, F("send 1-16 events per request"));

        return false;
    }

    return true;
}

void handleUpsert(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    JsonDocument doc;
    if (!parseBody(webserver, doc)) {
        return;
    }
    JsonDocument single;
    JsonArrayConst events;
    if (!batchOf(webserver, doc, events, single)) {
        return;
    }
    int badIndex = -1;
    if (const Events::Msg error = validateBatch(events, badIndex); error != nullptr) {
        sendError(webserver, HTTP_CODE_BAD_REQUEST, error, badIndex);

        return;
    }

    size_t saved = 0;
    for (JsonVariantConst item : events) {
        Events::Record record;
        Events::fromJson(item, record);
        const EventStore::Result result = EventStore::upsert(record);
        if (result == EventStore::Result::Full) {
            sendError(webserver, HTTP_CODE_CONFLICT, F("schedule is full (64 events): delete some or replace it"));

            return;
        }
        if (result != EventStore::Result::Ok) {
            sendError(webserver, HTTP_CODE_INTERNAL_ERROR, F("could not write the schedule to flash"));

            return;
        }
        saved++;
    }
    JsonDocument reply;
    reply[F("status")] = "ok";
    reply[F("saved")] = saved;
    reply[F("count")] = EventStore::count();
    sendJsonDoc(webserver, HTTP_CODE_OK, reply);
}

void handleDelete(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    ESP8266WebServer& server = webserver->raw();
    JsonDocument reply;
    if (server.hasArg("all") && server.arg("all") == "true") {
        EventStore::clear();
        reply[F("status")] = "ok";
        reply[F("count")] = 0;
        sendJsonDoc(webserver, HTTP_CODE_OK, reply);

        return;
    }
    if (!server.hasArg("id")) {
        sendError(webserver, HTTP_CODE_BAD_REQUEST, F("give ?id=<event id>, or ?all=true to clear the schedule"));

        return;
    }
    const EventStore::Result result = EventStore::remove(server.arg("id").c_str());
    if (result == EventStore::Result::NotFound) {
        sendError(webserver, HTTP_CODE_NOT_FOUND, F("no such event"));

        return;
    }
    if (result != EventStore::Result::Ok) {
        sendError(webserver, HTTP_CODE_INTERNAL_ERROR, F("could not write the schedule to flash"));

        return;
    }
    reply[F("status")] = "ok";
    reply[F("count")] = EventStore::count();
    sendJsonDoc(webserver, HTTP_CODE_OK, reply);
}

void handleReplace(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    JsonDocument doc;
    if (!parseBody(webserver, doc)) {
        return;
    }
    const bool begin = doc[F("begin")].isNull() ? true : doc[F("begin")].as<bool>();
    const bool commit = doc[F("commit")].isNull() ? true : doc[F("commit")].as<bool>();

    JsonArrayConst events = doc[F("events")].as<JsonArrayConst>();
    if (events.isNull() && !(doc[F("events")].isNull() && commit)) {
        sendError(webserver, HTTP_CODE_BAD_REQUEST, F("events must be an array (it may be empty to clear the schedule)"));

        return;
    }
    if (!events.isNull() && events.size() > MAX_BATCH) {
        sendError(webserver, HTTP_CODE_BAD_REQUEST, F("send at most 16 events per request: use begin / commit to send more"));

        return;
    }
    int badIndex = -1;
    if (!events.isNull()) {
        if (const Events::Msg error = validateBatch(events, badIndex); error != nullptr) {
            sendError(webserver, HTTP_CODE_BAD_REQUEST, error, badIndex);

            return;
        }
    }

    if (begin && EventStore::beginReplace() != EventStore::Result::Ok) {
        sendError(webserver, HTTP_CODE_INTERNAL_ERROR, F("could not start the replacement"));

        return;
    }
    if (!events.isNull()) {
        for (JsonVariantConst item : events) {
            Events::Record record;
            Events::fromJson(item, record);
            const EventStore::Result result = EventStore::appendReplace(record);
            if (result == EventStore::Result::Full) {
                EventStore::abortReplace();
                sendError(webserver, HTTP_CODE_CONFLICT, F("more than 64 events: the old schedule was left unchanged"));

                return;
            }
            if (result != EventStore::Result::Ok) {
                EventStore::abortReplace();
                sendError(webserver, HTTP_CODE_BAD_REQUEST, F("no replacement in progress: start with begin=true"));

                return;
            }
        }
    }
    JsonDocument reply;
    reply[F("staged")] = EventStore::replaceCount();
    if (commit) {
        if (EventStore::commitReplace() != EventStore::Result::Ok) {
            sendError(webserver, HTTP_CODE_BAD_REQUEST, F("nothing to commit: start with begin=true"));

            return;
        }
        reply[F("committed")] = true;
        reply[F("count")] = EventStore::count();
    } else {
        reply[F("committed")] = false;
    }
    reply[F("status")] = "ok";
    sendJsonDoc(webserver, HTTP_CODE_OK, reply);
}

void handleFire(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    ESP8266WebServer& server = webserver->raw();
    if (!server.hasArg("id") || !EventScheduler::fireNow(server.arg("id").c_str())) {
        sendError(webserver, HTTP_CODE_NOT_FOUND, F("no such event"));

        return;
    }
    JsonDocument reply;
    reply[F("status")] = "fired";
    sendJsonDoc(webserver, HTTP_CODE_OK, reply);
}

void handleStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    JsonDocument doc;
    EventScheduler::fillStatus(doc);
    sendJsonDoc(webserver, HTTP_CODE_OK, doc);
}
}  // namespace

void registerEventEndpoints(Webserver* webserver) {
    // @openapi {get} /events version=v1 group=Events summary="List scheduled events (?offset=&limit=), or pull one with ?id=" requiresAuth=true
    // responses=200:application/json,401:application/json,404:application/json
    webserver->route("/api/v1/events", HTTP_GET, handleList);

    // @openapi {post} /events version=v1 group=Events summary="Insert or replace events (one object, or {events:[...]} up to 16)" requiresAuth=true
    // requestBody=application/json requestBodySchema=id:string,title:string,body:string,value:string,icon:string,kind:string,ttl_s:number,at:string,repeat:string,time:string,days:array,day:number
    // example={"id":"mtg-0845","title":"Meeting in 15 min","value":"09:00","icon":"meeting","kind":"warning","ttl_s":600,"at":"2026-11-09T08:45:00Z"}
    // responses=200:application/json,400:application/json,401:application/json,409:application/json,413:application/json
    webserver->route("/api/v1/events", HTTP_POST, handleUpsert);

    // @openapi {delete} /events version=v1 group=Events summary="Delete one event (?id=) or the whole schedule (?all=true)" requiresAuth=true
    // responses=200:application/json,400:application/json,401:application/json,404:application/json
    webserver->route("/api/v1/events", HTTP_DELETE, handleDelete);

    // @openapi {post} /events/replace version=v1 group=Events summary="Replace the whole schedule, in parts if needed (begin/commit flags)" requiresAuth=true
    // requestBody=application/json requestBodySchema=events:array,begin:boolean,commit:boolean
    // example={"begin":true,"commit":true,"events":[]}
    // responses=200:application/json,400:application/json,401:application/json,409:application/json,413:application/json
    webserver->route("/api/v1/events/replace", HTTP_POST, handleReplace);

    // @openapi {post} /events/fire version=v1 group=Events summary="Fire an event now (?id=), for testing" requiresAuth=true
    // responses=200:application/json,401:application/json,404:application/json
    webserver->route("/api/v1/events/fire", HTTP_POST, handleFire);

    // @openapi {get} /events/status version=v1 group=Events summary="Event counts, clock state and what fires next" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/events/status", HTTP_GET, handleStatus);
}
