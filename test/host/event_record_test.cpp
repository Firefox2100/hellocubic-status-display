// Host tests for the event record <-> JSON mapping. Build and run with scripts/host-tests.sh.
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "events/event_record.h"

using namespace Events;

static const char* parse(const char* json, Record& out) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);
    assert(!error);
    return fromJson(doc.as<JsonVariantConst>(), out);
}

static bool rejects(const char* json, const char* mustMention = nullptr) {
    Record record;
    const char* error = parse(json, record);
    if (error == nullptr) return false;
    return mustMention == nullptr || strstr(error, mustMention) != nullptr;
}

static void testModeChange() {
    Record r;
    assert(!parse(R"({"id":"busy-9","mode":"busy","text":"Design review","at":1793953200})", r));
    assert(isModeChange(r) && enabled(r) && r.kind == static_cast<uint8_t>(Presence::Mode::Busy));
    assert(!strcmp(r.title, "Design review") && r.body[0] == '\0' && r.at == 1793953200u);

    // round trip: comes back as a mode event, not as a card
    JsonDocument out;
    toJson(r, out, 0);
    assert(!strcmp(out[0]["mode"] | "", "busy") && !strcmp(out[0]["text"] | "", "Design review"));
    assert(out[0]["title"].isNull() && out[0]["kind"].isNull() && out[0]["at"].as<uint32_t>() == 1793953200u);

    // recurring, relaxing with a gif, and "normal" with no text at all; no title is needed
    assert(!parse(R"({"id":"fri-chill","mode":"relaxing","gif":"arch.cgf","repeat":"weekly","days":["fri"],"time":"16:30"})", r));
    assert(isModeChange(r) && !strcmp(r.body, "arch.cgf") && r.repeat != 0);
    assert(!parse(R"({"id":"free","mode":"normal","at":1793956800,"enabled":false})", r));
    assert(isModeChange(r) && !enabled(r));  // "enabled" must not clear the mode flag

    // validation
    assert(rejects(R"({"id":"x","mode":"dnd","at":1793953200})", "mode"));
    assert(rejects(R"({"id":"x","mode":7,"at":1793953200})", "mode"));
    assert(rejects(R"({"id":"x","mode":"away","text":"this text is much longer than twenty-seven","at":1793953200})", "text"));
    assert(rejects(R"({"id":"x","mode":"relaxing","gif":"../etc","at":1793953200})", "gif"));
    assert(rejects(R"({"id":"x","mode":"busy"})", "at"));  // still needs a time

    // a card without a title is still refused
    assert(rejects(R"({"id":"x","at":1793953200})", "title"));
}

static void testOneOff() {
    Record r;
    assert(!parse(R"({"id":"mtg-1","title":"Meeting in 15 min","body":"Design review","value":"09:00","type":"meeting","kind":"warning","ttl_s":300,"at":1793953200})", r));
    assert(!strcmp(r.id, "mtg-1") && !strcmp(r.title, "Meeting in 15 min") && !strcmp(r.value, "09:00"));
    assert(r.icon == Attention::ICON_MEETING && r.kind == Attention::K_WARNING && r.ttlS == 300);
    assert(r.repeat == 0 && r.at == 1793953200u && enabled(r) && r.lastFired == 0);
    assert(r.display == Attention::SHOW_INTERRUPT);

    // ISO 8601 with an offset is converted to epoch seconds
    assert(!parse(R"({"id":"a","title":"t","at":"2026-11-07T09:00:00Z"})", r));
    time_t utc = TimeUtil::parseIso8601("2026-11-07T09:00:00Z");
    assert(r.at == (uint32_t)utc);
    assert(!parse(R"({"id":"a","title":"t","at":"2026-11-07T10:00:00+01:00"})", r));
    assert(r.at == (uint32_t)utc);                                     // same instant
    assert(rejects(R"({"id":"a","title":"t","at":"2026-11-07T09:00:00"})", "offset"));   // no offset = ambiguous: refused
    assert(rejects(R"({"id":"a","title":"t","at":"tomorrow"})", "at must"));
    assert(rejects(R"({"id":"a","title":"t","at":-5})", "at must"));

    // defaults: default ttl, kind info, no icon; progress cards default to the queue list
    assert(!parse(R"({"id":"a","title":"t","at":1793953200})", r));
    assert(r.ttlS == TTL_DEFAULT && r.kind == Attention::K_INFO && r.icon == Attention::ICON_NONE);
    assert(!parse(R"({"id":"a","title":"t","kind":"progress","at":1793953200})", r));
    assert(r.display == Attention::SHOW_QUEUE);
}

static void testRecurring() {
    Record r;
    assert(!parse(R"({"id":"standup","title":"Standup","repeat":"weekly","time":"09:30","days":["mon","Wed",5]})", r));
    assert(r.repeat == (uint8_t)Schedule::Repeat::Weekly && r.minuteOfDay == 9 * 60 + 30);
    assert(r.weekdayMask == ((1 << 1) | (1 << 3) | (1 << 5)));
    assert(!parse(R"({"id":"daily","title":"d","repeat":"daily","time":"00:05","from":1790000000,"until":"2027-01-01T00:00:00Z"})", r));
    assert(r.minuteOfDay == 5 && r.from == 1790000000u && r.until != 0);
    assert(!parse(R"({"id":"rent","title":"Rent","repeat":"monthly","time":"08:00","day":31})", r));
    assert(r.dayOfMonth == 31);

    assert(rejects(R"({"id":"x","title":"t","repeat":"weekly","time":"09:00"})", "days"));
    assert(rejects(R"({"id":"x","title":"t","repeat":"weekly","time":"09:00","days":[]})", "days"));
    assert(rejects(R"({"id":"x","title":"t","repeat":"weekly","time":"09:00","days":["funday"]})", "days"));
    assert(rejects(R"({"id":"x","title":"t","repeat":"monthly","time":"09:00"})", "day"));
    assert(rejects(R"({"id":"x","title":"t","repeat":"monthly","time":"09:00","day":32})", "day"));
    assert(rejects(R"({"id":"x","title":"t","repeat":"daily"})", "time"));
    assert(rejects(R"({"id":"x","title":"t","repeat":"daily","time":"25:00"})", "time"));
    assert(rejects(R"({"id":"x","title":"t","repeat":"daily","time":"9:5x"})", "time"));
    assert(rejects(R"({"id":"x","title":"t","repeat":"yearly","time":"09:00"})", "repeat"));
    // exactly one way to say when
    assert(rejects(R"({"id":"x","title":"t","repeat":"daily","time":"09:00","at":1793953200})", "either"));
    assert(rejects(R"({"id":"x","title":"t"})", "either"));
}

static void testValidation() {
    assert(rejects(R"({"title":"t","at":1793953200})", "id"));
    assert(rejects(R"({"id":"","title":"t","at":1793953200})", "id"));
    assert(rejects(R"({"id":"way-too-long-identifier","title":"t","at":1793953200})", "id"));
    assert(rejects(R"({"id":"bad id","title":"t","at":1793953200})", "id"));
    assert(rejects(R"({"id":"a/b","title":"t","at":1793953200})", "id"));
    assert(rejects(R"({"id":"a","at":1793953200})", "title"));
    assert(rejects(R"({"id":"a","title":"","at":1793953200})", "title"));
    assert(rejects(R"({"id":"a","title":"t","kind":"urgent!","at":1793953200})", "kind"));
    assert(rejects(R"({"id":"a","title":"t","icon":"rocket","at":1793953200})", "icon"));
    assert(rejects(R"({"id":"a","title":"t","display":"popup","at":1793953200})", "display"));
    assert(rejects(R"({"id":"a","title":"t","ttl_s":86401,"at":1793953200})", "ttl_s"));
    assert(rejects(R"({"id":"a","title":"t","ttl_s":"long","at":1793953200})", "ttl_s"));
    assert(rejects(R"({"id":"a","title":"t","enabled":"yes","at":1793953200})", "enabled"));
    assert(rejects("[]") && rejects("42") && rejects("null") && rejects("\"x\""));

    // long text is cut to the card limits, not refused
    Record r;
    assert(!parse(R"({"id":"a","title":"0123456789012345678901234567890123456789","body":"0123456789012345678901234567890123456789012345","at":1793953200})", r));
    assert(strlen(r.title) == TITLE_LEN && strlen(r.body) == BODY_LEN);
    // an explicit disable survives
    assert(!parse(R"({"id":"a","title":"t","enabled":false,"at":1793953200})", r));
    assert(!enabled(r));
}

static void testRoundTrip() {
    const char* inputs[] = {
        R"({"id":"m1","title":"Meeting","body":"Room 4","value":"09:00","icon":"meeting","kind":"warning","display":"interrupt","ttl_s":600,"enabled":true,"at":1793953200})",
        R"({"id":"w1","title":"Standup","icon":"chat","kind":"info","display":"interrupt","enabled":true,"repeat":"weekly","time":"09:30","days":["mon","wed"],"from":1790000000,"until":1800000000})",
        R"({"id":"mo","title":"Rent","kind":"success","display":"interrupt","enabled":false,"repeat":"monthly","time":"08:00","day":1})",
    };
    for (const char* json : inputs) {
        Record a;
        assert(!parse(json, a));
        JsonDocument out;
        toJson(a, out, 0);
        std::string text;
        serializeJson(out[0], text);
        Record b;
        assert(!parse(text.c_str(), b));
        b.lastFired = a.lastFired;
        assert(memcmp(&a, &b, sizeof(Record)) == 0);                  // what comes out can be sent back in unchanged
    }
}

int main() {
    testOneOff();
    testRecurring();
    testValidation();
    testRoundTrip();
    testModeChange();
    printf("event record ok\n");
    return 0;
}
