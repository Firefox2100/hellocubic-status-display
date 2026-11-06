// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Decides when a usage source should STOP being fetched because retrying cannot help.
// Pure (no Arduino), so it is unit-tested on the host (see test/host/).
//
// Transient problems (no network, timeouts, 429, 5xx) are handled by glimmer's backoff in fetch_policy.h and never stop
// anything. This adds the "give up until the owner acts" cases that fetch_policy.h does not cover:
//   * a client error that a retry cannot fix (404 endpoint gone, 405, 410, 400, 422, ...) seen twice in a row;
//   * a 200 whose body is not what the parser expects, five times in a row (the response format changed).
// Bad credentials (401/403) and edge blocks are already latched by fetch_policy.h, and the manager stops fetching
// for those too. All stops are cleared by setting the credential again, an explicit refresh, or a restart.

#include <stdint.h>
#include <stdio.h>

namespace FetchFailure {

constexpr uint8_t kPermanentStrikes = 2;
constexpr uint8_t kSchemaStrikes = 5;

enum class Kind : uint8_t {
    Transient,  // retry later with backoff
    Auth,       // 401/403 with a JSON body: the credential may be bad (fetch_policy.h counts the strikes)
    EdgeBlock,  // 401/403 answered with HTML: an edge challenge, not a verdict on the credential
    Permanent,  // a client error a retry cannot solve
};

inline Kind classify(int code, bool markup) {
    if (code == 401 || code == 403) return markup ? Kind::EdgeBlock : Kind::Auth;
    // 408 request timeout, 425 too early and 429 rate limit are explicitly "try again later"
    if (code >= 400 && code < 500 && code != 408 && code != 425 && code != 429) return Kind::Permanent;
    return Kind::Transient;  // transport errors (< 0), 5xx, and "0" for a parse failure
}

struct Latch {
    uint8_t permanentStrikes = 0;
    uint8_t schemaStrikes = 0;
    bool tripped = false;
    char reason[24] = "";  // shown on screen, e.g. "STOPPED: HTTP 404"
};

inline void reset(Latch& latch) { latch = Latch(); }

inline void onSuccess(Latch& latch) {
    latch.permanentStrikes = 0;
    latch.schemaStrikes = 0;
}

// code: HTTP status (negative = transport error). parsed: what the body parser returned for a 200.
inline void onFailure(Latch& latch, int code, bool parsed, bool markup) {
    if (latch.tripped) return;
    if (code == 200 && !parsed) {
        latch.permanentStrikes = 0;
        if (++latch.schemaStrikes >= kSchemaStrikes) {
            latch.tripped = true;
            snprintf(latch.reason, sizeof(latch.reason), "STOPPED: BAD RESPONSE");
        }
        return;
    }
    latch.schemaStrikes = 0;
    if (classify(code, markup) == Kind::Permanent) {
        if (++latch.permanentStrikes >= kPermanentStrikes) {
            latch.tripped = true;
            snprintf(latch.reason, sizeof(latch.reason), "STOPPED: HTTP %d", code);
        }
    } else {
        latch.permanentStrikes = 0;
    }
}

}  // namespace FetchFailure
