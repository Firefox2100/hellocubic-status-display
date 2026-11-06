// SPDX-License-Identifier: MIT
// Adapted from glimmer (https://github.com/Avinava/glimmer), Copyright (c) 2026 glimmer contributors.
// Used under the MIT license; see THIRD_PARTY.md.
// The pure helpers glimmer's screens call on its Api namespace. The network side lives in net/UsageFetch.
#pragma once
#include <Arduino.h>
#include <time.h>
#include "storage.h"
#include "usage_types.h"

namespace Api {
    inline float codexHeroPct(const Settings& s, const CodexData& d) {
        bool realSecondary = d.secondaryPct >= 0 && d.secondaryTag[0] == '\0';
        return (realSecondary && s.codexWeeklyHero) ? d.secondaryPct : d.primaryPct;
    }
    inline float claudeHeroPct(const Settings& s, const ClaudeData& d) {
        return s.claudeWeeklyHero ? d.weeklyPct : d.sessionPct;
    }
    // Data counts as stale once it is older than 3 refresh intervals (15-minute floor).
    bool isStale(time_t lastOk, const Settings& s);
    // Writes "STALE 14M" into buf when stale, "" otherwise.
    void staleText(time_t lastOk, const Settings& s, char* buf, size_t n);
    String formatCountdown(time_t t);
    // One line of advice on which allowance to spend next (or a credential problem), "" when there is none.
    void adviceText(const ClaudeData& cl, const CodexData& cx, char* buf, size_t n);
}
