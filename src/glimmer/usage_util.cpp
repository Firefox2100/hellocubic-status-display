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

static const char* provName(bool claude, bool token) {
    return claude ? (token ? "CLAUDE KEY" : "CLAUDE") : (token ? "CODEX TOKEN" : "CODEX");
}

void Api::adviceText(const ClaudeData& cl, const CodexData& cx, char* buf, size_t n) {
    if (n) buf[0] = '\0';
    // 1. A credential problem outranks any advice: the numbers aren't live.
    if (CredState::bad(cl.cred) || CredState::bad(cx.cred)) {
        bool claude = CredState::bad(cl.cred);
        Cred c = claude ? cl.cred : cx.cred;
        if (c == Cred::BLOCKED) snprintf_P(buf, n, PSTR("%s BLOCKED \xC2\xB7 RETRYING"), provName(claude, false));
        else snprintf_P(buf, n, PSTR("%s %s"), provName(claude, true), c == Cred::EXPIRED ? "EXPIRED" : "REJECTED");
        return;
    }
    if (!cl.valid || !cx.valid || cl.weeklyPct < 0 || cx.primaryPct < 0) return;
    time_t now = time(nullptr);
    if (now < 1000000000L) return;
    struct Cand { const char* name; float pct; time_t reset; const ResetGrant* g; } c[2] = {
        {"CLAUDE", cl.weeklyPct, cl.weeklyReset, &cl.resets},
        {"CODEX",  cx.primaryPct, cx.primaryReset, &cx.resets},
    };
    // 2. Nearly out, but a limit-reset credit can be spent right now.
    for (auto& k : c) {
        if (k.pct < 15.0f && k.g->left && k.g->usable) {
            snprintf_P(buf, n, PSTR("%s: %u RESET%s AVAILABLE"), k.name, k.g->left, k.g->left > 1 ? "S" : "");
            return;
        }
    }
    // 3. Use-it-or-lose-it: an allowance that resets soon with plenty unused.
    int pick = -1;
    for (int i = 0; i < 2; i++) {
        long left = (long)(c[i].reset - now);
        if (c[i].reset <= now || left > 48L * 3600L || c[i].pct < 25.0f) continue;
        if (pick < 0 || c[i].reset < c[pick].reset) pick = i;
    }
    if (pick >= 0) {
        char d[8]; TimeUtil::shortDuration((long)(c[pick].reset - now), d, sizeof(d));
        snprintf_P(buf, n, PSTR("USE %s \xC2\xB7 RESETS %s"), c[pick].name, d);
        return;
    }
    int most = c[0].pct >= c[1].pct ? 0 : 1;
    if (c[most].pct < 15.0f) { snprintf_P(buf, n, PSTR("BOTH LOW")); return; }
    snprintf_P(buf, n, PSTR("MOST ROOM: %s %.0f%%"), c[most].name, c[most].pct);
}
