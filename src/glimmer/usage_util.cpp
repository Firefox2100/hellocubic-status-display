// SPDX-License-Identifier: MIT
// Adapted from glimmer (https://github.com/Avinava/glimmer), Copyright (c) 2026 glimmer contributors.
// Used under the MIT license; see THIRD_PARTY.md.
#include "api.h"
#include "timeutil.h"

String Api::formatCountdown(time_t t) {
    time_t now = time(nullptr);
    if (now < 1000000000L) return "--";                 // clock not synced yet
    long diff = (long)(t - now);
    if (diff <= 0) return "now";
    long h = diff / 3600;
    long m = (diff % 3600) / 60;
    if (h >= 24) { char b[8]; snprintf_P(b, sizeof(b), PSTR("%ldd"), (h + 12) / 24); return b; }
    if (h >= 1)  { char b[10]; snprintf_P(b, sizeof(b), PSTR("%ldh %ldm"), h, m);   return b; }
    char b[8]; snprintf_P(b, sizeof(b), PSTR("%ldm"), m); return b;
}

bool Api::isStale(time_t lastOk, const Settings& s) {
    time_t now = time(nullptr);
    if (lastOk <= 0 || now < 1000000000L) return false;
    long limit = (long)s.refreshMin * 60L * 3L;
    if (limit < 15L * 60L) limit = 15L * 60L;
    return (long)(now - lastOk) > limit;
}

void Api::staleText(time_t lastOk, const Settings& s, char* buf, size_t n) {
    if (!isStale(lastOk, s)) { if (n) buf[0] = '\0'; return; }
    char d[8]; TimeUtil::shortDuration((long)(time(nullptr) - lastOk), d, sizeof(d));
    snprintf_P(buf, n, PSTR("STALE %s"), d);
}
