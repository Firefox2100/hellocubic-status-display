// Host tests for the pure usage logic. Build and run with scripts/host-tests.sh.
#include <ArduinoJson.h>

#include <cassert>
#include <cstdio>

#include "fetch_policy.h"
#include "net/fetch_failure.h"
#include "net/usage_cache_format.h"
#include "usage_parse.h"

static void testClassify() {
    using namespace FetchFailure;
    assert(classify(401, false) == Kind::Auth);
    assert(classify(403, false) == Kind::Auth);
    assert(classify(403, true) == Kind::EdgeBlock);
    assert(classify(404, false) == Kind::Permanent);
    assert(classify(410, false) == Kind::Permanent);
    assert(classify(405, false) == Kind::Permanent);
    assert(classify(400, false) == Kind::Permanent);
    assert(classify(429, false) == Kind::Transient);   // rate limit: back off, never stop
    assert(classify(408, false) == Kind::Transient);
    assert(classify(500, false) == Kind::Transient);
    assert(classify(503, false) == Kind::Transient);
    assert(classify(-1, false) == Kind::Transient);    // transport error
    assert(classify(-3, false) == Kind::Transient);    // clock not set yet
    assert(classify(0, false) == Kind::Transient);
}

static void testLatch() {
    using namespace FetchFailure;
    Latch latch;
    onFailure(latch, 404, false, false);               // one 404 could be a stale org id: not enough
    assert(!latch.tripped);
    onFailure(latch, 404, false, false);               // two in a row: the endpoint is gone
    assert(latch.tripped && strstr(latch.reason, "404"));
    onFailure(latch, 200, true, false);                // a latched source stays latched
    assert(latch.tripped);
    reset(latch);
    assert(!latch.tripped && latch.permanentStrikes == 0);

    // transient failures in between reset the permanent streak
    onFailure(latch, 404, false, false);
    onFailure(latch, 503, false, false);
    onFailure(latch, 404, false, false);
    assert(!latch.tripped);
    // success clears the streaks too
    reset(latch);
    onFailure(latch, 404, false, false);
    onSuccess(latch);
    onFailure(latch, 404, false, false);
    assert(!latch.tripped);
    // network trouble and rate limits never stop anything, however long they last
    reset(latch);
    for (int i = 0; i < 200; i++) onFailure(latch, i % 2 ? -1 : 429, false, false);
    assert(!latch.tripped);
    // 401/403 are the credential latch's business (fetch_policy.h), not this one
    for (int i = 0; i < 20; i++) onFailure(latch, 401, false, false), onFailure(latch, 403, false, true);
    assert(!latch.tripped);
    // a 200 the parser cannot read, five times in a row: the format changed
    reset(latch);
    for (int i = 0; i < kSchemaStrikes - 1; i++) onFailure(latch, 200, false, false);
    assert(!latch.tripped);
    onFailure(latch, 200, false, false);
    assert(latch.tripped && strstr(latch.reason, "BAD RESPONSE"));
    // ...but a good response in between restarts the count
    reset(latch);
    for (int i = 0; i < kSchemaStrikes - 1; i++) onFailure(latch, 200, false, false);
    onSuccess(latch);
    onFailure(latch, 200, false, false);
    assert(!latch.tripped);
}

static void testPolicyStillBacksOff() {
    FetchPolicy::State st;
    FetchPolicy::onFailure(st, 503, -1);
    assert(st.waitS == 60 && !st.authLatched);
    FetchPolicy::onFailure(st, 403, -1, false);
    FetchPolicy::onFailure(st, 403, -1, false);
    assert(st.authLatched);                             // two JSON 401/403 -> credential judged bad
    FetchPolicy::State edge;
    FetchPolicy::onFailure(edge, 403, -1, true);
    FetchPolicy::onFailure(edge, 403, -1, true);
    assert(FetchPolicy::blocked(edge) && !edge.authLatched);
}

static void testCacheRoundTrip() {
    ClaudeData claude;
    claude.valid = true;
    claude.sessionPct = 63.5f;
    claude.weeklyPct = 12.0f;
    claude.sessionReset = 1800000000;
    claude.lastOk = 1790000000;
    claude.cred = Cred::OK;
    snprintf(claude.err, sizeof(claude.err), "HTTP 503");
    snprintf(claude.models[0].label, sizeof(claude.models[0].label), "OPUS");
    claude.models[0].pct = 40;
    CodexData codex;
    codex.valid = true;
    codex.primaryPct = 80;
    codex.primaryWinSec = 604800;
    codex.lastOk = 1790000100;

    uint8_t buf[UsageCacheFormat::kEncodedSize];
    assert(UsageCacheFormat::encode(claude, codex, buf, sizeof(buf)) == sizeof(buf));
    assert(UsageCacheFormat::encode(claude, codex, buf, sizeof(buf) - 1) == 0);

    ClaudeData c2;
    CodexData x2;
    assert(UsageCacheFormat::decode(buf, sizeof(buf), c2, x2));
    assert(c2.valid && c2.sessionPct == 63.5f && c2.weeklyPct == 12.0f && c2.lastOk == 1790000000);
    assert(!strcmp(c2.models[0].label, "OPUS") && c2.models[0].pct == 40);
    assert(c2.cred == Cred::NOT_SET && c2.err[0] == '\0');   // run-time state is not restored
    assert(x2.valid && x2.primaryPct == 80 && x2.primaryWinSec == 604800);

    // damaged or foreign files are rejected and leave the targets untouched
    ClaudeData keep;
    keep.sessionPct = 7;
    CodexData keepX;
    assert(!UsageCacheFormat::decode(buf, sizeof(buf) - 1, keep, keepX));
    buf[0] ^= 0xFF;                                          // bad magic
    assert(!UsageCacheFormat::decode(buf, sizeof(buf), keep, keepX));
    buf[0] ^= 0xFF;
    buf[4] ^= 0x01;                                          // other version
    assert(!UsageCacheFormat::decode(buf, sizeof(buf), keep, keepX));
    assert(keep.sessionPct == 7);

    // an empty snapshot is not restored as data
    ClaudeData empty;
    CodexData emptyX;
    assert(UsageCacheFormat::encode(empty, emptyX, buf, sizeof(buf)) == sizeof(buf));
    ClaudeData c3;
    c3.valid = true;
    CodexData x3;
    assert(UsageCacheFormat::decode(buf, sizeof(buf), c3, x3) && !c3.valid);
}

// Whatever a provider (or a captive portal, or a truncated transfer) sends back, the parsers must reject it
// without crashing or reporting data.
static void testParsersSurviveGarbage() {
    const char* bodies[] = {"", "{}", "[]", "null", "42", "\"text\"", "<html>blocked</html>", "{\"limits\":[]}",
                            "{\"limits\":[{\"kind\":\"session\"}]}", "{\"five_hour\":null}",
                            "{\"rate_limit\":{\"primary_window\":{}}}", "{\"limits\":[{\"kind\":1,\"percent\":\"x\"}]}",
                            "{\"a\":{\"b\":{\"c\":{\"d\":{}}}}}"};
    for (const char* body : bodies) {
        JsonDocument doc;
        deserializeJson(doc, body);  // may fail: that is part of the test
        ClaudeData claude;
        CodexData codex;
        ResetGrant grant;
        UsageParse::claude(doc.as<JsonVariantConst>(), claude);
        UsageParse::codex(doc.as<JsonVariantConst>(), codex);
        UsageParse::codexResets(doc.as<JsonVariantConst>(), grant);
        assert(!(claude.valid && claude.sessionPct < 0 && claude.weeklyPct < 0));
    }
}

int main() {
    testClassify();
    testLatch();
    testPolicyStillBacksOff();
    testCacheRoundTrip();
    testParsersSurviveGarbage();
    printf("net logic ok\n");
    return 0;
}
