// SPDX-License-Identifier: GPL-3.0-or-later
#include "events/EventScheduler.h"

#include <Arduino.h>
#include <Logger.h>

#include <ctime>

#include "attention.h"
#include "attention_json.h"
#include "events/EventStore.h"
#include "events/event_record.h"

extern auto mainApprovalTtlS() -> uint32_t;

namespace {
constexpr const char* LOG_TAG = "Events";
constexpr uint32_t TICK_MS = 1000;
constexpr uint32_t RESCAN_MS = 10UL * 60UL * 1000UL;  // clock steps and DST changes: look again now and then
constexpr long REASONABLE_EPOCH = 1000000000L;
constexpr size_t CARD_ID_PREFIX_LEN = 4;

uint32_t g_lastTickMs = 0;
uint32_t g_lastRescanMs = 0;
uint32_t g_seenRevision = 0;
bool g_dirty = true;
bool g_wasSynced = false;
time_t g_nextDue = 0;
char g_nextId[Events::ID_LEN + 1] = "";  // NOLINT(modernize-avoid-c-arrays)
uint32_t g_firedCount = 0;
uint32_t g_sequence = 0;

auto timeSynced() -> bool { return time(nullptr) > REASONABLE_EPOCH; }

/** @brief Put the event's card in the notification queue (same path as a push, so limits and priorities match). */
void fire(const Events::Record& record) {
    JsonDocument card;
    card["id"] = String("evt:") + record.id;  // distinct from push ids; fits the 23-character card id
    card["title"] = record.title;
    if (record.body[0] != '\0') {
        card["body"] = record.body;
    }
    if (record.value[0] != '\0') {
        card["value"] = record.value;
    }
    card["kind"] = Attention::kindName(static_cast<Attention::Kind>(record.kind));
    if (record.icon != Attention::ICON_NONE) {
        card["icon"] = Attention::iconName(static_cast<Attention::Icon>(record.icon));
    }
    card["display"] = record.display == Attention::SHOW_QUEUE ? "queue" : "interrupt";
    if (record.ttlS != Events::TTL_DEFAULT) {
        card["ttl_s"] = record.ttlS;
    }

    Attention::Item item;
    Attention::fromJson(card.as<JsonVariantConst>(), AttentionQueue::now(), mainApprovalTtlS(), ++g_sequence, item);
    const int slot = AttentionQueue::put(item);
    g_firedCount++;
    if (slot < 0) {
        Logger::warn((String("Event card rejected (queue full of more important cards): ") + record.id).c_str(), LOG_TAG);
    }
}

/** @brief One pass over the schedule: fire what is due, drop what can never fire, find the next time to wake. */
void pass(time_t now) {
    uint64_t toDelete = 0;
    time_t soonest = 0;
    g_nextId[0] = '\0';

    Events::Record record;
    const size_t total = EventStore::count();
    for (size_t slot = 0; slot < total && EventStore::read(slot, record); slot++) {
        if (!Events::enabled(record)) {
            continue;
        }
        const Schedule::Spec spec = Events::specOf(record);
        const Schedule::Verdict verdict = Schedule::evaluate(spec, now, record.lastFired, EventScheduler::GRACE_S);
        if (verdict.fireAt != 0) {
            fire(record);
            if (!verdict.remove) {
                EventStore::markFired(slot, static_cast<uint32_t>(verdict.fireAt));
                record.lastFired = static_cast<uint32_t>(verdict.fireAt);
            }
        }
        if (verdict.remove) {
            toDelete |= 1ULL << slot;  // fired one-off, a one-off that can no longer fire, or a recurring event past its end

            continue;
        }
        if (verdict.next != 0 && (soonest == 0 || verdict.next < soonest)) {
            soonest = verdict.next;
            strncpy(g_nextId, record.id, sizeof(g_nextId) - 1);
        }
    }
    if (toDelete != 0) {
        EventStore::removeSlots(toDelete);
    }
    g_nextDue = soonest;
    g_dirty = false;
    g_seenRevision = EventStore::revision();
}
}  // namespace

void EventScheduler::begin() {
    EventStore::begin();
    g_dirty = true;
}

void EventScheduler::update() {
    const uint32_t nowMs = millis();
    if (nowMs - g_lastTickMs < TICK_MS) {
        return;
    }
    g_lastTickMs = nowMs;

    if (!timeSynced()) {
        g_wasSynced = false;

        return;
    }
    if (!g_wasSynced) {
        g_wasSynced = true;  // the clock just jumped from 1970: take a full look, with the grace rules
        g_dirty = true;
    }
    if (nowMs - g_lastRescanMs >= RESCAN_MS) {
        g_lastRescanMs = nowMs;
        g_dirty = true;
    }
    if (EventStore::revision() != g_seenRevision) {
        g_dirty = true;
    }
    const time_t now = time(nullptr);
    if (!g_dirty && (g_nextDue == 0 || now < g_nextDue)) {
        return;
    }
    pass(now);
}

auto EventScheduler::secondsToNext() -> long {
    if (!timeSynced() || g_nextDue == 0) {
        return -1;
    }

    return static_cast<long>(g_nextDue - time(nullptr));
}

auto EventScheduler::fireNow(const char* eventId) -> bool {
    Events::Record record;
    if (EventStore::find(eventId, &record) < 0) {
        return false;
    }
    fire(record);

    return true;
}

void EventScheduler::fillStatus(JsonDocument& doc) {
    doc["count"] = EventStore::count();
    doc["capacity"] = Events::MAX_EVENTS;
    doc["time_synced"] = timeSynced();
    doc["next_in_s"] = secondsToNext();
    doc["next_id"] = g_nextId;
    doc["fired_since_boot"] = g_firedCount;
    doc["grace_s"] = GRACE_S;
}
