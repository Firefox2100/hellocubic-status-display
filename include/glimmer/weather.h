// SPDX-License-Identifier: MIT
// Adapted from glimmer (https://github.com/Avinava/glimmer), Copyright (c) 2026 glimmer contributors.
// Used under the MIT license; see THIRD_PARTY.md.
#pragma once
#include <Arduino.h>
#include "storage.h"
#include "fetch_policy.h"
#include "weather_core.h"

namespace Weather {
    // The shared snapshot (the Home page and GET /api/v1/weather read it).
    WeatherData& snapshot();
    const FetchPolicy::State& policy();

    // Restore the last good snapshot from /weather.json (shown until it goes stale).
    void begin();

    // Call from loop(): fetches when due (plain HTTP, ~1 s, blocks the loop meanwhile). Handles a changed location.
    void update();

    // Fetch again as soon as possible (POST /weather/refresh).
    void refreshSoon();

    // Minutes between successful fetches: the user's refresh interval, but never more often than every 10 min.
    uint32_t intervalMin(const Settings& s);
    bool     isStale(const Settings& s);
    bool     configured(const Settings& s);

    // True when the Home page should replace the plain clock: weather is on, a location is set and the data is fresh.
    bool     homeActive(const Settings& s);

    // Inspection helpers for the API.
    bool     liveThisBoot();                 // true once a fetch succeeded since power-up (else the data is cached)
    long     nextFetchInS();                 // -1 when nothing is scheduled
    uint32_t attempts();                     // fetches tried since power-up

    void rainHint(const Settings& s, char* buf, size_t n);
    const char* describe(uint8_t code);
    float       toDisplay(float c, bool fahrenheit);
}
