// SPDX-License-Identifier: GPL-3.0-or-later
// Push-card contract adapted from glimmer (https://github.com/Avinava/glimmer, MIT, see THIRD_PARTY.md).
//
//   POST /api/v1/push        create or update a card (see attention_json.h for the fields)
//   GET  /api/v1/push        the active queue, most important first
//   POST /api/v1/push/clear  {"id": "..."} or {"all": true}
//
// The same three are also served at /push, /push/clear so glimmer's agent tooling works unchanged.
#include <ArduinoJson.h>
#include <Logger.h>

#include "attention.h"
#include "attention_json.h"
#include "ui/AgentTracker.h"
#include "web/Api.h"
#include "web/Webserver.h"

extern auto mainApprovalTtlS() -> uint32_t;
extern auto mainAgentDoneCards() -> bool;

namespace {
constexpr size_t MAX_PUSH_BODY = 2048;  // a card is tiny; anything bigger is a mistake (or a raw hook event)
constexpr int HTTP_CODE_TOO_LARGE = 413;
constexpr int HTTP_CODE_CONFLICT = 409;

void sendError(Webserver* webserver, int code, const char* message) {
    JsonDocument doc;
    doc["ok"] = false;
    doc[F("error")] = message;
    sendJsonDoc(webserver, code, doc);
}

/** @brief Parse the body into `doc`, answering 413/400 itself. @return false if the caller should stop. */
auto parseBody(Webserver* webserver, JsonDocument& doc) -> bool {
    const String& body = webserver->raw().arg("plain");
    if (body.length() > MAX_PUSH_BODY) {
        sendError(webserver, HTTP_CODE_TOO_LARGE, "body too large");

        return false;
    }
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        sendError(webserver, HTTP_CODE_BAD_REQUEST, "bad json");

        return false;
    }

    return true;
}

void handlePush(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    if (!parseBody(webserver, doc)) {
        return;
    }

    static uint32_t sequence = 0;
    Attention::Item item;
    Attention::fromJson(doc.as<JsonVariantConst>(), AttentionQueue::now(), mainApprovalTtlS(), ++sequence, item);
    const int slot = AttentionQueue::put(item);

    // Ids and kinds only: never log card text, it can carry sensitive detail
    Logger::info((String("push ") + item.id + " kind=" + Attention::kindName(item.kind) +
                  (slot >= 0 ? " ok" : " rejected"))
                     .c_str(),
                 "Push");

    JsonDocument reply;
    reply["ok"] = slot >= 0;
    reply["id"] = item.id;
    if (slot < 0) {
        reply[F("error")] = "queue full of more important cards";
    }
    sendJsonDoc(webserver, slot >= 0 ? HTTP_CODE_OK : HTTP_CODE_CONFLICT, reply);
}

void handlePushList(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument reply;
    JsonArray cards = reply[F("cards")].to<JsonArray>();
    const Attention::Queue& queue = AttentionQueue::get();
    std::array<int, Attention::kMax> order{};
    const int count = Attention::ordered(queue, order.data());
    const uint32_t now = AttentionQueue::now();
    for (int i = 0; i < count; i++) {
        Attention::toJson(queue.items[order[static_cast<size_t>(i)]], now, cards.add<JsonObject>());
    }
    sendJsonDoc(webserver, HTTP_CODE_OK, reply);
}

void handlePushClear(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    if (!parseBody(webserver, doc)) {
        return;
    }

    JsonDocument reply;
    if (doc[F("all")].as<bool>()) {
        reply[F("cleared")] = AttentionQueue::clearAll();
    } else {
        const char* cardId = doc["id"].is<const char*>() ? doc["id"].as<const char*>() : "";
        reply[F("cleared")] = AttentionQueue::clear(cardId) ? 1 : 0;
    }
    sendJsonDoc(webserver, HTTP_CODE_OK, reply);
}

/**
 * @brief Agent hook events from tools/agents/hook.sh (Claude Code, Codex). Always answers 200 with the action taken,
 *        and never fails the agent: a bad or oversized event is simply ignored.
 */
void handleHook(Webserver* webserver) {
    JsonDocument reply;
    reply[F("action")] = "none";
    if (!requireBearerToken(webserver)) {
        return;
    }
    JsonDocument doc;
    const String& body = webserver->raw().arg("plain");
    if (body.length() > MAX_PUSH_BODY || deserializeJson(doc, body) != DeserializationError::Ok) {
        sendJsonDoc(webserver, HTTP_CODE_OK, reply);

        return;
    }

    auto text = [&doc](const __FlashStringHelper* key) -> const char* {
        return doc[key].is<const char*>() ? doc[key].as<const char*>() : "";
    };
    Attention::HookEvent event;
    event.event = text(F("event"));
    event.type = text(F("type"));
    event.session = text(F("session"));
    event.cwd = text(F("cwd"));
    event.tool = text(F("tool"));
    event.detail = text(F("detail"));
    event.message = text(F("message"));

    const Attention::Agent agent = Attention::agentFrom(webserver->raw().arg("agent").c_str());
    const uint32_t now = AttentionQueue::now();
    Attention::Item item;
    Attention::Item extra;
    const Attention::HookAction action =
        Attention::fromHook(agent, event, mainAgentDoneCards(), mainApprovalTtlS(), now, item, &extra);
    AgentTracker::onHook(agent, event);
    if (action == Attention::HOOK_UPSERT) {
        AttentionQueue::put(item);
        if (extra.used) {
            AttentionQueue::put(extra);
        }
        reply[F("action")] = "show";
    } else if (action == Attention::HOOK_CLEAR) {
        const bool cleared = AttentionQueue::clear(item.id);
        String doneId = String(item.id) + ":done";  // a Codex DONE card lives next to the session's own card
        AttentionQueue::clear(doneId.c_str());
        reply[F("action")] = cleared ? "cleared" : "none";
    }
    sendJsonDoc(webserver, HTTP_CODE_OK, reply);
}
/** @brief Which agent sessions are working, waiting for you or done. Ids are hashes of the agents' session ids. */
void handleAgents(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    JsonDocument reply;
    const AgentSessions::Table& table = AgentTracker::table();
    int working = 0;
    int waiting = 0;
    int done = 0;
    AgentSessions::counts(table, working, waiting, done);
    reply[F("working")] = working;
    reply[F("waiting")] = waiting;
    reply[F("done")] = done;
    JsonArray list = reply[F("sessions")].to<JsonArray>();
    const uint32_t now = AttentionQueue::now();
    for (const AgentSessions::Session& session : table.s) {
        if (session.state == AgentSessions::S_FREE) {
            continue;
        }
        JsonObject row = list.add<JsonObject>();
        row[F("id")] = session.id;
        row[F("agent")] = Attention::agentName(session.agent);
        row[F("project")] = session.project;
        row[F("state")] = AgentSessions::stateName(session.state);
        row[F("run_s")] = static_cast<int32_t>(now - session.startedS);
        row[F("since_s")] = static_cast<int32_t>(now - session.changedS);
        row[F("last_event_s")] = static_cast<int32_t>(now - session.lastS);
    }
    sendJsonDoc(webserver, HTTP_CODE_OK, reply);
}
}  // namespace

void registerPushEndpoints(Webserver* webserver) {
    // @openapi {post} /push version=v1 group=Push summary="Show a notification card (upsert by id)" requiresAuth=true
    // requestBody=application/json requestBodySchema=title:string,kind:string,body:string,value:string,id:string,agent:string,project:string,progress:number,ttl_s:number,display:string,urgent:boolean
    // example={"id":"tests","kind":"success","title":"TESTS PASSED","value":"142/142","agent":"claude","ttl_s":120}
    // responses=200:application/json,400:application/json,401:application/json,409:application/json,413:application/json
    webserver->route("/api/v1/push", HTTP_POST, handlePush);

    // @openapi {get} /push version=v1 group=Push summary="List active cards, most important first" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/push", HTTP_GET, handlePushList);

    // @openapi {post} /push/clear version=v1 group=Push summary="Dismiss one card by id, or all of them" requiresAuth=true
    // requestBody=application/json requestBodySchema=id:string,all:boolean example={"id":"tests"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->route("/api/v1/push/clear", HTTP_POST, handlePushClear);

    // @openapi {post} /hook version=v1 group=Push summary="Agent hook event (Claude Code / Codex): shows or clears that session's card" requiresAuth=true
    // requestBody=application/json requestBodySchema=event:string,type:string,session:string,cwd:string,tool:string,detail:string,message:string
    // example={"event":"Notification","type":"permission_prompt","session":"abc12345","cwd":"/home/me/proj","tool":"Bash","detail":"npm test"}
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/hook", HTTP_POST, handleHook);

    // @openapi {get} /agents version=v1 group=Push summary="Agent sessions the cube is tracking: working, waiting for you, done" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/agents", HTTP_GET, handleAgents);

    // glimmer-compatible paths (its hook script and MCP bridge post here)
    webserver->route("/push", HTTP_POST, handlePush);
    webserver->route("/push", HTTP_GET, handlePushList);
    webserver->route("/push/clear", HTTP_POST, handlePushClear);
    webserver->route("/hook", HTTP_POST, handleHook);
}
