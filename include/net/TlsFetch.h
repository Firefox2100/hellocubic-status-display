// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>

#include <functional>

class HTTPClient;  // keep ESP8266HTTPClient.h out of this header: its HTTP_CODE_* enums clash with web/Webserver.h

/**
 * @brief HTTPS GET for the usage fetchers. One fresh client per request, body streamed to the caller.
 *
 * BearSSL on the ESP8266 is memory-hungry (a handshake needs ~20-25 KB of contiguous heap), so callers must check
 * `TlsFetch::heapOk()` first and release the font cache. The call BLOCKS for the whole handshake and transfer
 * (seconds): the main loop, the web server and the clock do not run meanwhile.
 */
namespace TlsFetch {

constexpr uint32_t MIN_BLOCK_BYTES = 20000;  // largest free block a handshake needs

struct Result {
    int code = 0;          ///< HTTP status; negative = HTTPClient/transport error; -2 = begin() failed
    bool parsed = false;   ///< what the body callback returned (only called for HTTP 200)
    long retryAfter = -1;  ///< Retry-After seconds, -1 when absent
    bool markup = false;   ///< a 401/403 answered with HTML (edge challenge, not a bad credential)
    uint32_t elapsedMs = 0;
};

/** @brief True when the largest free heap block is big enough to start a handshake. */
auto heapOk() -> bool;

/**
 * @param url        https URL
 * @param addHeaders called once to add request headers
 * @param onBody     called with the response stream for HTTP 200; return false if the body was unusable
 * @param verify     validate the server certificate against the pinned roots (needs synced time)
 */
auto get(const char* url, const std::function<void(HTTPClient&)>& addHeaders,
         const std::function<bool(Stream&)>& onBody, bool verify) -> Result;

/**
 * @brief Anonymous GET of https://<host>/ with no credentials: reports the HTTP code and how long the blocking
 *        handshake took. Used to measure what TLS costs on this device.
 */
auto selfTest(const char* host, bool verify) -> Result;

}  // namespace TlsFetch
