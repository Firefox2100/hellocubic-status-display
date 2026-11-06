// SPDX-License-Identifier: GPL-3.0-or-later
// Endpoints, headers and fetch policy adapted from glimmer's src/data/api.cpp
// (https://github.com/Avinava/glimmer, MIT, see THIRD_PARTY.md).
#include "net/UsageManager.h"

#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <Logger.h>

#include <array>
#include <cstring>
#include <ctime>

#include "config/ConfigManager.h"
#include "config/CredentialStore.h"
#include "display.h"
#include "events/EventScheduler.h"
#include "display/DisplayManager.h"
#include "fetch_policy.h"
#include "history.h"
#include "net/TlsFetch.h"
#include "net/fetch_failure.h"
#include "net/usage_cache_format.h"
#include "ui/ChannelManager.h"
#include "usage_parse.h"

extern ConfigManager configManager;

namespace {
constexpr const char* LOG_TAG = "Usage";
constexpr const char* CACHE_PATH = "/usage_cache.bin";
constexpr const char* CACHE_TMP_PATH = "/usage_cache.tmp";
constexpr uint32_t CACHE_SAVE_MIN_MS = 10UL * 60UL * 1000UL;  // at most ~144 flash writes a day
constexpr uint32_t MS_PER_SECOND = 1000;
constexpr uint32_t SECONDS_PER_MINUTE = 60;
constexpr uint32_t FIRST_FETCH_DELAY_MS = 15000;   // after the first update(): let the boot screens and NTP settle
constexpr uint32_t MIN_JOB_GAP_MS = 10000;          // heap fragments after a handshake; give it time to recover
constexpr uint32_t HEAP_RETRY_MS = 15000;           // when a job was refused for lack of heap
constexpr uint32_t CRED_RECHECK_MS = 10000;
constexpr uint32_t EVENT_QUIET_S = 20;               // no fetch this close to a scheduled event
constexpr uint32_t SETTLE_DELAY_MS = 20;
constexpr uint32_t CLAUDE_ORG_PAUSE_MS = 150;
constexpr long REASONABLE_EPOCH = 1000000000L;
constexpr const char* USER_AGENT = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)";
constexpr uint32_t FNV_OFFSET = 2166136261U;
constexpr uint32_t FNV_PRIME = 16777619U;
constexpr size_t MESSAGE_SIZE = 24;       // matches ClaudeData::err / CodexData::err
constexpr size_t BEARER_PREFIX_LEN = 7;   // "Bearer "

enum Job : uint8_t { JOB_CLAUDE = 0, JOB_CODEX, JOB_CODEX_RESETS, JOB_COUNT };

struct JobState {
    uint32_t dueMs = 0;
    bool primed = false;  // dueMs set
};

ClaudeData g_claude;
CodexData g_codex;
FetchPolicy::State g_claudePolicy;
FetchPolicy::State g_codexPolicy;
FetchFailure::Latch g_claudeLatch;  // set when retrying cannot help (see fetch_failure.h): fetching stops
FetchFailure::Latch g_codexLatch;
bool g_claudeLive = false;          // false while the snapshot only came from the flash cache
bool g_codexLive = false;
bool g_cacheDirty = false;
bool g_cacheEverSaved = false;
uint32_t g_lastCacheSaveMs = 0;
std::array<JobState, JOB_COUNT> g_jobs{};
uint32_t g_revision = 1;
uint32_t g_lastJobEndMs = 0;
uint32_t g_lastCredCheckMs = 0;
uint32_t g_heapRefusals = 0;
bool g_started = false;
int g_lastClaudeHttp = 0;
int g_lastCodexHttp = 0;

String g_orgId;           // claude.ai organisation id, cached per credential
uint32_t g_orgKeyHash = 0;

auto hashOf(const String& text) -> uint32_t {
    uint32_t hash = FNV_OFFSET;
    for (const char chr : text) {
        hash ^= static_cast<uint8_t>(chr);
        hash *= FNV_PRIME;
    }

    return hash;
}

auto timeSynced() -> bool { return time(nullptr) > REASONABLE_EPOCH; }

auto reachable() -> bool { return WiFi.status() == WL_CONNECTED && WiFi.getMode() == WIFI_STA && timeSynced(); }

auto intervalMs() -> uint32_t {
    return static_cast<uint32_t>(configManager.ui.refreshMin) * SECONDS_PER_MINUTE * MS_PER_SECOND;
}

auto due(const JobState& job, uint32_t now) -> bool {
    return job.primed && static_cast<int32_t>(now - job.dueMs) >= 0;
}

void schedule(Job job, uint32_t inMs) {
    g_jobs.at(job).dueMs = millis() + inMs;
    g_jobs.at(job).primed = true;
}

auto jobEnabled(Job job) -> bool {
    const UiSettings& settings = configManager.ui;
    switch (job) {
        case JOB_CLAUDE:
            return settings.claudeConfigured && !g_claudeLatch.tripped && !CredState::bad(g_claude.cred) && settings.showClaude;
        case JOB_CODEX:
            return settings.codexConfigured && !g_codexLatch.tripped && !CredState::bad(g_codex.cred) && settings.showCodex;
        case JOB_CODEX_RESETS:  // only once the token is known to work
            return settings.showCodex && !g_codexLatch.tripped && (g_codex.cred == Cred::OK || g_codex.cred == Cred::EXPIRING);
        default:
            return false;
    }
}

/**
 * @brief Record a failed fetch against a source and decide whether to show it.
 * @param parseFailure the server answered 200 but the body was not usable
 */
template <class Data>
auto failSource(FetchPolicy::State& policy, FetchFailure::Latch& latch, Data& out, const TlsFetch::Result& result,
                const char* message, bool parseFailure) -> bool {
    // fetch_policy counts a parse failure as code 0; the latch needs to know it was a 200 with an unusable body
    FetchPolicy::onFailure(policy, parseFailure ? 0 : result.code, result.retryAfter, result.markup);
    FetchFailure::onFailure(latch, parseFailure ? HTTP_CODE_OK : result.code, !parseFailure, result.markup);

    const bool credentialIssue = policy.authLatched || FetchPolicy::blocked(policy);
    if (latch.tripped) {
        strncpy(out.err, latch.reason, sizeof(out.err) - 1);
        out.err[sizeof(out.err) - 1] = '\0';
        Logger::warn((String("fetching stopped: ") + latch.reason).c_str(), LOG_TAG);
    } else if (credentialIssue || !FetchPolicy::shouldSurface(policy)) {
        out.err[0] = '\0';
    } else {
        strncpy(out.err, message, sizeof(out.err) - 1);
        out.err[sizeof(out.err) - 1] = '\0';
    }
    // Status codes only: never log headers, bodies or credentials
    Logger::warn((String("fetch failed: code=") + result.code + (parseFailure ? " (unusable body)" : "") +
                  " fails=" + policy.fails + " wait=" + policy.waitS + "s")
                     .c_str(),
                 LOG_TAG);

    return false;
}

template <class Data>
void okSource(FetchPolicy::State& policy, FetchFailure::Latch& latch, Data& out) {
    FetchPolicy::onSuccess(policy, time(nullptr));
    FetchFailure::onSuccess(latch);
    out.err[0] = '\0';
    out.valid = true;
    out.lastOk = policy.lastOk;
    g_cacheDirty = true;
}

void describeCode(int code, char* out, size_t size) {
    if (code < 0) {
        snprintf(out, size, "Offline");
    } else {
        snprintf(out, size, "HTTP %d", code);
    }
}

/** @brief Re-derive the credential state of both sources (cheap; the JWT expiry tracks the clock). */
void refreshCredStates() {
    const time_t now = time(nullptr);
    const UiSettings& settings = configManager.ui;
    const Cred claude = CredState::derive(settings.claudeConfigured, g_claudePolicy, 0, now, g_claude.valid);
    const Cred codex = CredState::derive(settings.codexConfigured, g_codexPolicy, g_codex.jwtExp, now, g_codex.valid);
    if (claude != g_claude.cred || codex != g_codex.cred) {
        g_revision++;
    }
    g_claude.cred = claude;
    g_codex.cred = codex;
}

void saveCacheIfDue();  // defined below, with the other cache functions

// ── Claude ──────────────────────────────────────────────────────────────────

void claudeHeaders(HTTPClient& http, const String& key) {
    http.addHeader("Cookie", "sessionKey=" + key);
    http.addHeader("Accept", "application/json");
    http.addHeader("User-Agent", USER_AGENT);
    http.addHeader("Referer", "https://claude.ai");
    http.addHeader("Origin", "https://claude.ai");
}

/** @brief The organisation id is needed in the usage URL. The org list is big, so only its first uuid is kept. */
auto fetchClaudeOrg(const String& key, bool verify) -> TlsFetch::Result {
    TlsFetch::Result cached;
    if (!g_orgId.isEmpty() && g_orgKeyHash == hashOf(key)) {
        cached.code = HTTP_CODE_OK;
        cached.parsed = true;

        return cached;
    }

    String orgId;
    const TlsFetch::Result result = TlsFetch::get(
        "https://claude.ai/api/organizations", [&key](HTTPClient& http) { claudeHeaders(http, key); },
        [&orgId](Stream& body) {
            JsonDocument filter;
            filter[0]["uuid"] = true;
            JsonDocument doc;
            if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
                return false;
            }
            if (!doc[0]["uuid"].is<const char*>()) {
                return false;
            }
            orgId = doc[0]["uuid"].as<String>();

            return !orgId.isEmpty();
        },
        verify);
    if (result.code == HTTP_CODE_OK && result.parsed) {
        g_orgId = orgId;
        g_orgKeyHash = hashOf(key);
    }

    return result;
}

auto fetchClaude() -> bool {
    String key = CredentialStore::get(CredentialStore::CLAUDE_KEY);
    if (key.isEmpty()) {
        return false;
    }
    const bool verify = configManager.ui.tlsVerify;

    const bool orgWasCached = !g_orgId.isEmpty() && g_orgKeyHash == hashOf(key);
    TlsFetch::Result result = fetchClaudeOrg(key, verify);
    g_lastClaudeHttp = result.code;
    if (result.code != HTTP_CODE_OK) {
        std::array<char, MESSAGE_SIZE> message{};
        describeCode(result.code, message.data(), message.size());

        return failSource(g_claudePolicy, g_claudeLatch, g_claude, result, message.data(), false);
    }
    if (!result.parsed) {
        TlsFetch::Result parseFailure;
        parseFailure.code = 0;

        return failSource(g_claudePolicy, g_claudeLatch, g_claude, parseFailure, "No org", true);
    }
    if (!orgWasCached) {
        yield();
        delay(CLAUDE_ORG_PAUSE_MS);
    }

    // cedar_ember=1 is the query the claude.ai usage page itself sends; without it the limit-reset grants are omitted
    const String url = "https://claude.ai/api/organizations/" + g_orgId + "/usage?cedar_ember=1";
    ClaudeData next = g_claude;
    result = TlsFetch::get(
        url.c_str(), [&key](HTTPClient& http) { claudeHeaders(http, key); },
        [&next](Stream& body) {
            JsonDocument filter;
            UsageParse::claudeFilter(filter);
            JsonDocument doc;
            if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
                return false;
            }

            return UsageParse::claude(doc.as<JsonVariantConst>(), next);
        },
        verify);
    g_lastClaudeHttp = result.code;
    if ((result.code == HTTP_CODE_UNAUTHORIZED || result.code == HTTP_CODE_FORBIDDEN) && !result.markup) {
        g_orgId = String();  // a dead session can also invalidate the cached org: rediscover next time
    }
    if (result.code >= HTTP_CODE_BAD_REQUEST && result.code < HTTP_CODE_INTERNAL_SERVER_ERROR) {
        g_orgId = String();  // a 4xx can mean the cached organisation is stale: rediscover it next time
    }
    if (result.code != HTTP_CODE_OK) {
        std::array<char, MESSAGE_SIZE> message{};
        describeCode(result.code, message.data(), message.size());

        return failSource(g_claudePolicy, g_claudeLatch, g_claude, result, message.data(), false);
    }
    if (!result.parsed) {
        TlsFetch::Result parseFailure;
        parseFailure.code = 0;

        return failSource(g_claudePolicy, g_claudeLatch, g_claude, parseFailure, "No data", true);
    }
    g_claude = next;
    g_claudeLive = true;
    okSource(g_claudePolicy, g_claudeLatch, g_claude);

    return true;
}

// ── Codex ───────────────────────────────────────────────────────────────────

void codexHeaders(HTTPClient& http, String& authorization, const String& deviceId) {
    http.addHeader("Authorization", authorization);
    authorization = String();  // HTTPClient holds its own copy now: free ours (a token is about 1 KB)
    http.addHeader("Accept", "application/json");
    http.addHeader("User-Agent", USER_AGENT);
    http.addHeader("Origin", "https://chatgpt.com");
    http.addHeader("Referer", "https://chatgpt.com/");
    if (!deviceId.isEmpty()) {
        http.addHeader("oai-device-id", deviceId);
    }
}

/** @brief The Authorization header value. Built so that only one extra copy of the token exists. */
auto bearerValue() -> String {
    String token = CredentialStore::get(CredentialStore::CODEX_TOKEN);
    if (token.isEmpty()) {
        return {};
    }
    String value;
    value.reserve(token.length() + BEARER_PREFIX_LEN);
    value = "Bearer ";
    value += token;

    return value;
}

auto codexJwtExpired() -> bool {
    const time_t now = time(nullptr);

    return g_codex.jwtExp != 0 && now > REASONABLE_EPOCH && now >= g_codex.jwtExp;
}

auto fetchCodex() -> bool {
    if (codexJwtExpired()) {
        return false;
    }
    String authorization = bearerValue();
    if (authorization.isEmpty()) {
        return false;
    }
    const String deviceId = CredentialStore::get(CredentialStore::CODEX_DEVICE_ID);
    CodexData next = g_codex;
    const TlsFetch::Result result = TlsFetch::get(
        "https://chatgpt.com/backend-api/wham/usage",
        [&authorization, &deviceId](HTTPClient& http) { codexHeaders(http, authorization, deviceId); },
        [&next](Stream& body) {
            JsonDocument filter;
            UsageParse::codexFilter(filter);
            JsonDocument doc;
            if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
                return false;
            }

            return UsageParse::codex(doc.as<JsonVariantConst>(), next);
        },
        configManager.ui.tlsVerify);
    g_lastCodexHttp = result.code;
    if (result.code != HTTP_CODE_OK) {
        std::array<char, MESSAGE_SIZE> message{};
        describeCode(result.code, message.data(), message.size());

        return failSource(g_codexPolicy, g_codexLatch, g_codex, result, message.data(), false);
    }
    if (!result.parsed) {
        TlsFetch::Result parseFailure;
        parseFailure.code = 0;

        return failSource(g_codexPolicy, g_codexLatch, g_codex, parseFailure, "No data", true);
    }
    next.resets = g_codex.resets;  // owned by fetchCodexResets
    g_codex = next;
    g_codexLive = true;
    okSource(g_codexPolicy, g_codexLatch, g_codex);

    return true;
}

/** @brief Hourly and best effort: a failure here never touches the usage source's backoff or credential verdict. */
auto fetchCodexResets() -> bool {
    if (codexJwtExpired()) {
        return false;
    }
    String authorization = bearerValue();
    if (authorization.isEmpty()) {
        return false;
    }
    const String deviceId = CredentialStore::get(CredentialStore::CODEX_DEVICE_ID);
    ResetGrant grant;
    const TlsFetch::Result result = TlsFetch::get(
        "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits",
        [&authorization, &deviceId](HTTPClient& http) { codexHeaders(http, authorization, deviceId); },
        [&grant](Stream& body) {
            JsonDocument filter;
            UsageParse::codexResetsFilter(filter);
            JsonDocument doc;
            if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
                return false;
            }

            return UsageParse::codexResets(doc.as<JsonVariantConst>(), grant);
        },
        configManager.ui.tlsVerify);
    if (result.code != HTTP_CODE_OK || !result.parsed) {
        return false;
    }
    g_codex.resets = grant;

    return true;
}

// ── Scheduling ──────────────────────────────────────────────────────────────

/** @brief Run one job and schedule its next run. */
void runJob(Job job) {
    Display::releaseFont();  // frees a few KB; a screen reloads its font on its next paint
    yield();
    delay(SETTLE_DELAY_MS);

    bool succeeded = false;
    switch (job) {
        case JOB_CLAUDE:
            succeeded = fetchClaude();
            schedule(JOB_CLAUDE, succeeded || g_claudePolicy.waitS == 0 ? intervalMs() : g_claudePolicy.waitS * MS_PER_SECOND);
            break;
        case JOB_CODEX:
            succeeded = fetchCodex();
            schedule(JOB_CODEX, succeeded || g_codexPolicy.waitS == 0 ? intervalMs() : g_codexPolicy.waitS * MS_PER_SECOND);
            break;
        default:
            succeeded = fetchCodexResets();
            schedule(JOB_CODEX_RESETS, SECONDS_PER_MINUTE * SECONDS_PER_MINUTE * MS_PER_SECOND);
            break;
    }
    if (succeeded) {
        UsageHistory::record(g_claude, g_codex);
    }
    refreshCredStates();
    saveCacheIfDue();
    g_revision++;
    g_lastJobEndMs = millis();
}

void loadCredentialFlags() {
    const CredentialStore::Status status = CredentialStore::status();
    UiSettings& settings = configManager.ui;
    settings.claudeConfigured = status.claudeKeySet;
    settings.codexConfigured = status.codexTokenSet;
    settings.codexModelLabel = status.codexModelLabel.c_str();
    g_codex.jwtExp = 0;
    if (status.codexTokenSet) {
        const String token = CredentialStore::get(CredentialStore::CODEX_TOKEN);
        g_codex.jwtExp = CredState::jwtExp(token.c_str());
    }
}
/** @brief Restore the last snapshots from flash (shown dimmed until they are refreshed). */
void loadCache() {
    File file = LittleFS.open(CACHE_PATH, "r");
    if (!file) {
        return;
    }
    std::array<uint8_t, UsageCacheFormat::kEncodedSize> buffer{};
    const bool complete = file.size() == buffer.size() && file.read(buffer.data(), buffer.size()) == buffer.size();
    file.close();
    if (!complete || !UsageCacheFormat::decode(buffer.data(), buffer.size(), g_claude, g_codex)) {
        Logger::warn("Usage cache ignored (damaged or from another build)", LOG_TAG);
    }
}

/** @brief Write the snapshots to flash: temp file + rename, so a power cut never leaves a half-written cache. */
void saveCache() {
    std::array<uint8_t, UsageCacheFormat::kEncodedSize> buffer{};
    if (UsageCacheFormat::encode(g_claude, g_codex, buffer.data(), buffer.size()) != buffer.size()) {
        return;
    }
    File file = LittleFS.open(CACHE_TMP_PATH, "w");
    if (!file) {
        return;
    }
    const bool written = file.write(buffer.data(), buffer.size()) == buffer.size();
    file.close();
    if (!written) {
        LittleFS.remove(CACHE_TMP_PATH);

        return;
    }
    LittleFS.remove(CACHE_PATH);
    LittleFS.rename(CACHE_TMP_PATH, CACHE_PATH);
    g_cacheDirty = false;
    g_cacheEverSaved = true;
    g_lastCacheSaveMs = millis();
}

/** @brief Save when there is something new, at most every CACHE_SAVE_MIN_MS (the very first snapshot saves at once). */
void saveCacheIfDue() {
    if (!g_cacheDirty) {
        return;
    }
    if (g_cacheEverSaved && millis() - g_lastCacheSaveMs < CACHE_SAVE_MIN_MS) {
        return;
    }
    saveCache();
}
}  // namespace

void UsageManager::begin() {
    UsageHistory::begin();
    loadCache();
    loadCredentialFlags();
    // Cached numbers are only worth showing for a provider that is still configured
    if (!configManager.ui.claudeConfigured) {
        g_claude = ClaudeData();
    }
    if (!configManager.ui.codexConfigured) {
        g_codex = CodexData();
    }
    refreshCredStates();
    g_started = true;
}

void UsageManager::resume() {
    // Forget every verdict (bad credential, edge block, stopped endpoint) and try again soon
    g_claudePolicy = FetchPolicy::State();
    g_codexPolicy = FetchPolicy::State();
    FetchFailure::reset(g_claudeLatch);
    FetchFailure::reset(g_codexLatch);
    g_claude.err[0] = '\0';
    g_codex.err[0] = '\0';
    g_orgId = String();
    refreshCredStates();
    refreshNow();
    g_revision++;
}

void UsageManager::credentialsChanged() {
    loadCredentialFlags();
    if (!configManager.ui.claudeConfigured) {
        g_claude = ClaudeData();
        g_claudeLive = false;
        g_cacheDirty = true;
    }
    if (!configManager.ui.codexConfigured) {
        g_codex = CodexData();
        g_codexLive = false;
        g_cacheDirty = true;
    }
    resume();  // a newly set credential starts with a clean slate
    ChannelManager::applySettings(configManager.ui);
}

void UsageManager::refreshNow() {
    const uint32_t soon = millis() + MS_PER_SECOND;
    for (JobState& job : g_jobs) {
        job.dueMs = soon;
        job.primed = true;
    }
}

void UsageManager::update() {
    if (!g_started) {
        return;
    }
    const uint32_t now = millis();

    if (now - g_lastCredCheckMs >= CRED_RECHECK_MS) {
        g_lastCredCheckMs = now;
        refreshCredStates();
        saveCacheIfDue();  // also picks up a cleared credential
    }
    if (!reachable()) {
        return;
    }
    // First run: stagger the sources so each blocking fetch is short and the heap recovers in between
    for (size_t i = 0; i < g_jobs.size(); i++) {
        if (!g_jobs.at(i).primed) {
            g_jobs.at(i).dueMs = now + FIRST_FETCH_DELAY_MS + static_cast<uint32_t>(i) * MIN_JOB_GAP_MS;
            g_jobs.at(i).primed = true;
        }
    }
    if (now - g_lastJobEndMs < MIN_JOB_GAP_MS) {
        return;
    }
    // A fetch blocks for seconds: do not start one right before a scheduled event is due
    const long untilEvent = EventScheduler::secondsToNext();
    if (untilEvent >= 0 && untilEvent <= static_cast<long>(EVENT_QUIET_S)) {
        return;
    }

    for (size_t i = 0; i < g_jobs.size(); i++) {
        const auto job = static_cast<Job>(i);
        if (!due(g_jobs.at(i), now) || !jobEnabled(job)) {
            if (due(g_jobs.at(i), now)) {
                schedule(job, intervalMs());  // not enabled (yet): look again later
            }
            continue;
        }
        if (DisplayManager::isGifPlaying()) {
            ChannelManager::releaseMemory();  // an idle GIF holds 24 KB: drop it, the TLS handshake needs the space
        }
        Display::releaseFont();
        if (!TlsFetch::heapOk()) {
            g_heapRefusals++;
            schedule(job, HEAP_RETRY_MS);
            continue;
        }
        runJob(job);

        return;  // one blocking job per call
    }
}

auto UsageManager::claude() -> const ClaudeData& { return g_claude; }

auto UsageManager::codex() -> const CodexData& { return g_codex; }

auto UsageManager::revision() -> uint32_t { return g_revision; }

namespace {
auto credName(Cred cred) -> const char* {
    switch (cred) {
        case Cred::NOT_SET:
            return "not_set";
        case Cred::CHECKING:
            return "checking";
        case Cred::OK:
            return "ok";
        case Cred::EXPIRING:
            return "expiring";
        case Cred::EXPIRED:
            return "expired";
        case Cred::REJECTED:
            return "rejected";
        case Cred::BLOCKED:
            return "blocked";
    }

    return "unknown";
}

auto ageSeconds(time_t lastOk) -> long {
    const time_t now = time(nullptr);

    return (lastOk > 0 && now > REASONABLE_EPOCH) ? static_cast<long>(now - lastOk) : -1;
}
}  // namespace

namespace {
/** @brief "active", "stopped" (retrying cannot help), "rejected"/"blocked" (credential or edge), or "off". */
auto fetchState(bool configured, bool enabledBySetting, Cred cred, const FetchFailure::Latch& latch) -> const char* {
    if (!configured || !enabledBySetting) {
        return "off";
    }
    if (latch.tripped) {
        return "stopped";
    }
    if (cred == Cred::REJECTED) {
        return "rejected";
    }
    if (cred == Cred::BLOCKED) {
        return "blocked";
    }
    if (cred == Cred::EXPIRED) {
        return "expired";
    }

    return "active";
}

auto secondsUntil(time_t when) -> long {
    const time_t now = time(nullptr);

    return (when > 0 && now > REASONABLE_EPOCH) ? static_cast<long>(when - now) : -1;
}

void fillGrant(JsonDocument& doc, const char* source, const ResetGrant& grant) {
    doc[source]["reset_credits"]["left"] = grant.left;
    doc[source]["reset_credits"]["usable"] = grant.usable;
    doc[source]["reset_credits"]["title"] = grant.title;
    doc[source]["reset_credits"]["ends_in_s"] = secondsUntil(grant.endsAt);
}
}  // namespace

void UsageManager::fillStatus(JsonDocument& doc) {
    const UiSettings& settings = configManager.ui;
    const uint32_t now = millis();
    static const std::array<const char*, JOB_COUNT> JOB_NAMES = {"claude", "codex", "codex_resets"};

    doc["tls_verify"] = settings.tlsVerify;
    doc["refresh_min"] = settings.refreshMin;
    doc["heap_refusals"] = g_heapRefusals;
    doc["time_synced"] = timeSynced();
    for (size_t i = 0; i < g_jobs.size(); i++) {
        const JobState& job = g_jobs.at(i);
        doc["next_fetch_in_s"][JOB_NAMES.at(i)] =
            job.primed ? static_cast<int32_t>(job.dueMs - now) / static_cast<int32_t>(MS_PER_SECOND) : -1;
    }

    // Claude
    doc["claude"]["fetch"] = fetchState(settings.claudeConfigured, settings.showClaude, g_claude.cred, g_claudeLatch);
    doc["claude"]["stopped_reason"] = g_claudeLatch.reason;
    doc["claude"]["cred"] = credName(g_claude.cred);
    doc["claude"]["source"] = !g_claude.valid ? "none" : (g_claudeLive ? "live" : "cache");
    doc["claude"]["valid"] = g_claude.valid;
    doc["claude"]["age_s"] = ageSeconds(g_claude.lastOk);
    doc["claude"]["last_http"] = g_lastClaudeHttp;
    doc["claude"]["fails"] = g_claudePolicy.fails;
    doc["claude"]["error"] = g_claude.err;
    doc["claude"]["session"]["pct_left"] = g_claude.sessionPct;
    doc["claude"]["session"]["resets_in_s"] = secondsUntil(g_claude.sessionReset);
    doc["claude"]["weekly"]["pct_left"] = g_claude.weeklyPct;
    doc["claude"]["weekly"]["resets_in_s"] = secondsUntil(g_claude.weeklyReset);
    size_t slot = 0;
    for (const ModelSlot& model : g_claude.models) {
        if (model.pct >= 0) {
            doc["claude"]["models"][slot]["label"] = model.label;
            doc["claude"]["models"][slot]["pct_left"] = model.pct;
            slot++;
        }
    }
    fillGrant(doc, "claude", g_claude.resets);

    // Codex
    doc["codex"]["fetch"] = fetchState(settings.codexConfigured, settings.showCodex, g_codex.cred, g_codexLatch);
    doc["codex"]["stopped_reason"] = g_codexLatch.reason;
    doc["codex"]["cred"] = credName(g_codex.cred);
    doc["codex"]["source"] = !g_codex.valid ? "none" : (g_codexLive ? "live" : "cache");
    doc["codex"]["valid"] = g_codex.valid;
    doc["codex"]["age_s"] = ageSeconds(g_codex.lastOk);
    doc["codex"]["last_http"] = g_lastCodexHttp;
    doc["codex"]["fails"] = g_codexPolicy.fails;
    doc["codex"]["error"] = g_codex.err;
    doc["codex"]["token_expires_in_s"] = secondsUntil(g_codex.jwtExp);
    doc["codex"]["primary"]["pct_left"] = g_codex.primaryPct;
    doc["codex"]["primary"]["resets_in_s"] = secondsUntil(g_codex.primaryReset);
    doc["codex"]["primary"]["window_s"] = g_codex.primaryWinSec;
    doc["codex"]["secondary"]["pct_left"] = g_codex.secondaryPct;
    doc["codex"]["secondary"]["resets_in_s"] = secondsUntil(g_codex.secondaryReset);
    doc["codex"]["secondary"]["window_s"] = g_codex.secondaryWinSec;
    doc["codex"]["secondary"]["tag"] = g_codex.secondaryTag;
    doc["codex"]["credits_remaining"] = g_codex.creditsRemain;
    fillGrant(doc, "codex", g_codex.resets);
}
