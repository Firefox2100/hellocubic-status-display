// SPDX-License-Identifier: GPL-3.0-or-later
// Request shape adapted from glimmer's tlsGetStream (https://github.com/Avinava/glimmer, MIT, see THIRD_PARTY.md),
// with certificate validation added.
#include "net/TlsFetch.h"

#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <Logger.h>
#include <WiFiClientSecureBearSSL.h>

#include <Ticker.h>

#include <array>
#include <cstring>
#include <ctime>

#include "fetch_policy.h"
#include "net/TrustAnchors.h"

namespace {
constexpr const char* LOG_TAG = "TlsFetch";
constexpr uint16_t RX_BUFFER_BYTES = 4096;  // the certificate chain arrives in one record
constexpr uint16_t TX_BUFFER_BYTES = 512;
constexpr uint32_t HTTP_TIMEOUT_MS = 10000;  // bounds how long a silent server can freeze the main loop (one retry: ~20 s worst case)
constexpr uint32_t MARKUP_PEEK_MS = 2000;
constexpr uint32_t PEEK_POLL_MS = 5;
constexpr long REASONABLE_EPOCH = 1000000000L;
constexpr uint32_t RETRY_DELAY_MS = 200;
constexpr uint32_t SAMPLE_PERIOD_MS = 2;

// Lowest free heap seen during the last fetch. A handshake runs the heap down to almost nothing, and the Wi-Fi
// SDK crashes (null write in ieee80211_setup_ratetable) if it needs memory then, so the margin is worth watching.
uint32_t g_minFree = 0;
Ticker g_sampler;

void sampleHeap() {
    const uint32_t freeNow = ESP.getFreeHeap();  // NOLINT(readability-static-accessed-through-instance)
    if (freeNow < g_minFree) {
        g_minFree = freeNow;
    }
}

/** @brief Read the first non-blank byte of an error body: '<' means HTML (an edge challenge), not JSON. */
auto bodyIsMarkup(HTTPClient& http) -> bool {
    Stream& stream = http.getStream();
    const uint32_t until = millis() + MARKUP_PEEK_MS;
    while (millis() < until) {
        const int chr = stream.read();
        if (chr < 0) {
            if (!http.connected()) {
                break;
            }
            delay(PEEK_POLL_MS);

            continue;
        }
        if (chr == ' ' || chr == '\n' || chr == '\r' || chr == '\t') {
            continue;
        }

        return chr == '<';
    }

    return false;
}

/**
 * @brief One resumable TLS session per host. After the first (slow, fully validated) handshake the server hands
 *        back a session; reconnecting with it skips the certificate check and the key exchange, which is what costs
 *        ~5 s of CPU here. A resumed session is only ever one that was validated when it was created.
 */
auto sessionFor(const char* url) -> BearSSL::Session* {
    static BearSSL::Session claudeSession;
    static BearSSL::Session chatgptSession;
    static BearSSL::Session otherSession;
    if (strstr(url, "claude.ai") != nullptr) {
        return &claudeSession;
    }
    if (strstr(url, "chatgpt.com") != nullptr) {
        return &chatgptSession;
    }

    return &otherSession;
}

auto getOnce(const char* url, const std::function<void(HTTPClient&)>& addHeaders,
             const std::function<bool(Stream&)>& onBody, bool verify) -> TlsFetch::Result {
    TlsFetch::Result result;
    BearSSL::WiFiClientSecure client;
    BearSSL::X509List anchors;
    if (verify) {
        anchors.append(TrustAnchors::LE_YE1);
        anchors.append(TrustAnchors::LE_YE2);
        anchors.append(TrustAnchors::LE_YE3);
        client.setTrustAnchors(&anchors);
        client.setX509Time(time(nullptr));
    } else {
        client.setInsecure();
    }
    client.setBufferSizes(RX_BUFFER_BYTES, TX_BUFFER_BYTES);
    client.setSession(sessionFor(url));  // resume the previous (validated) session when the server allows it

    HTTPClient http;
    http.useHTTP10(true);  // no chunked encoding, so the body can be streamed
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, url)) {
        result.code = -2;

        return result;
    }
    static std::array<const char*, 1> COLLECTED = {"Retry-After"};  // collectHeaders wants a mutable array
    http.collectHeaders(COLLECTED.data(), COLLECTED.size());
    addHeaders(http);

    g_minFree = ESP.getFreeHeap();  // NOLINT(readability-static-accessed-through-instance)
    g_sampler.attach_ms(SAMPLE_PERIOD_MS, sampleHeap);
    result.code = http.GET();
    if (result.code < 0) {
        const uint32_t freeHeap = ESP.getFreeHeap();         // NOLINT(readability-static-accessed-through-instance)
        const uint32_t block = ESP.getMaxFreeBlockSize();    // NOLINT(readability-static-accessed-through-instance)
        const String detail = String("TLS connect failed: ssl error ") + client.getLastSSLError() + " free=" + freeHeap +
                              " block=" + block;
        Logger::warn(detail.c_str(), LOG_TAG);
    }
    result.retryAfter = FetchPolicy::retryAfterSec(http.header("Retry-After").c_str(), time(nullptr));
    if (result.code == HTTP_CODE_OK) {
        result.parsed = onBody(http.getStream());
    } else if (result.code == HTTP_CODE_UNAUTHORIZED || result.code == HTTP_CODE_FORBIDDEN) {
        result.markup = bodyIsMarkup(http);
    }
    g_sampler.detach();
    http.end();
    client.stop();  // stop() is what stores the session parameters for the next resumption

    return result;
}
}  // namespace

auto TlsFetch::minFreeHeap() -> uint32_t { return g_minFree; }

auto TlsFetch::heapOk() -> bool {
    return ESP.getMaxFreeBlockSize() >= MIN_BLOCK_BYTES;  // NOLINT(readability-static-accessed-through-instance)
}

auto TlsFetch::get(const char* url, const std::function<void(HTTPClient&)>& addHeaders,
                   const std::function<bool(Stream&)>& onBody, bool verify) -> Result {
    if (verify && time(nullptr) < REASONABLE_EPOCH) {
        Result noTime;
        noTime.code = -3;  // certificate dates cannot be checked before the clock is set

        return noTime;
    }

    yield();
    const uint32_t started = millis();
    Result result = getOnce(url, addHeaders, onBody, verify);
    if (result.code < 0 && result.code != -3) {
        // BearSSL handshakes on the ESP8266 fail a few percent of the time under heap pressure: retry once.
        Logger::warn(F("TLS request failed, retrying once"), LOG_TAG);
        yield();
        delay(RETRY_DELAY_MS);
        result = getOnce(url, addHeaders, onBody, verify);
    }
    result.elapsedMs = millis() - started;

    return result;
}

auto TlsFetch::selfTest(const char* host, bool verify) -> Result {
    const String url = String("https://") + host + "/";

    return get(
        url.c_str(), [](HTTPClient& http) { http.addHeader("Accept", "text/html"); },
        [](Stream& /*body*/) { return true; }, verify);
}
