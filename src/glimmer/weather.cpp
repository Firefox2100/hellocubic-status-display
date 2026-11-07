// SPDX-License-Identifier: MIT
// Adapted from glimmer (https://github.com/Avinava/glimmer), Copyright (c) 2026 glimmer contributors.
// Used under the MIT license; see THIRD_PARTY.md.
#include "weather.h"
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <math.h>
#include "config/ConfigManager.h"
#include "display.h"
#include "display/DisplayManager.h"
#include "events/EventScheduler.h"
#include "ui/ChannelManager.h"

extern ConfigManager configManager;

// Open-Meteo is served over plain HTTP too; HTTP avoids the 20 KB TLS block. Only the coordinates are sent.
// API doc: https://open-meteo.com/en/docs

namespace Weather {

static WeatherData        s_w;
static FetchPolicy::State s_pol;
static const char*        kCachePath = "/weather.json";

static constexpr uint32_t FIRST_FETCH_DELAY_MS = 5000;
static constexpr uint32_t BLOCKED_RETRY_MS = 30000;   // heap too low / nothing to do yet: look again later
static constexpr uint32_t MIN_FREE_HEAP = 12000;
static constexpr uint32_t MIN_FREE_BLOCK = 9000;
static constexpr long     EVENT_QUIET_S = 20;
static constexpr long     REASONABLE_EPOCH = 1000000000L;

static bool     s_primed = false;
static uint32_t s_dueMs = 0;
static bool     s_live = false;
static uint32_t s_attempts = 0;
static bool     s_locKnown = false;
static float    s_lat = 0.0f, s_lon = 0.0f;   // the location the snapshot belongs to

WeatherData& snapshot() { return s_w; }
const FetchPolicy::State& policy() { return s_pol; }

bool configured(const Settings& s) {
    return s.weatherLat != 0.0f || s.weatherLon != 0.0f;
}

uint32_t intervalMin(const Settings& s) {
    return s.refreshMin < 10 ? 10 : s.refreshMin;
}

bool isStale(const Settings& s) {
    time_t now = time(nullptr);
    if (!s_w.valid || s_w.lastOk <= 0 || now < REASONABLE_EPOCH) return false;
    return (long)(now - s_w.lastOk) > (long)intervalMin(s) * 60L * 3L;
}

bool homeActive(const Settings& s) {
    return s.showWeather && configured(s) && s_w.valid && !isStale(s);
}

bool liveThisBoot() { return s_live; }
uint32_t attempts() { return s_attempts; }

long nextFetchInS() {
    if (!s_primed) return -1;
    long ms = (int32_t)(s_dueMs - millis());
    return ms <= 0 ? 0 : ms / 1000;
}

void refreshSoon() {
    s_primed = true;
    s_dueMs = millis();
}

// ── last-good persistence ───────────────────────────────────────────────────

static void save() {
    JsonDocument d;
    d["la"] = s_lat; d["lo"] = s_lon;
    d["t"] = s_w.tempC; d["f"] = s_w.feelsC; d["c"] = s_w.code; d["day"] = s_w.isDay;
    d["h"] = s_w.humidity; d["w"] = s_w.windKmh; d["ok"] = (uint32_t)s_w.lastOk;
    d["rs"] = (uint32_t)s_w.rainStart;
    JsonArray rp = d["rp"].to<JsonArray>();
    for (uint8_t p : s_w.rainPct) rp.add(p);
    JsonArray fc = d["fc"].to<JsonArray>();
    for (const WeatherDay& wd : s_w.forecast) {
        JsonObject o = fc.add<JsonObject>();
        o["lo"] = wd.tmin; o["hi"] = wd.tmax; o["c"] = wd.code; o["uv"] = wd.uvMax;
        o["sr"] = (uint32_t)wd.sunrise; o["ss"] = (uint32_t)wd.sunset;
    }
    File f = LittleFS.open("/weather.tmp", "w");
    if (!f) return;
    size_t n = serializeJson(d, f);
    f.close();
    if (!n) { LittleFS.remove("/weather.tmp"); return; }
    LittleFS.remove(kCachePath);
    LittleFS.rename("/weather.tmp", kCachePath);
}

void begin() {
    const Settings& cfg = configManager.ui;
    s_lat = cfg.weatherLat;
    s_lon = cfg.weatherLon;
    s_locKnown = true;
    File f = LittleFS.open(kCachePath, "r");
    if (!f) return;
    JsonDocument d;
    DeserializationError e = deserializeJson(d, f);
    f.close();
    if (e || d["ok"].isNull()) return;
    // A cache from another location is not this place's weather
    if (fabsf((d["la"] | 0.0f) - s_lat) > 0.0001f || fabsf((d["lo"] | 0.0f) - s_lon) > 0.0001f) return;
    s_w.tempC = d["t"] | -999.0f; s_w.feelsC = d["f"] | s_w.tempC;
    s_w.code = d["c"] | 0; s_w.isDay = d["day"] | true;
    s_w.humidity = d["h"] | -1; s_w.windKmh = d["w"] | -1.0f;
    s_w.lastOk = (time_t)(d["ok"] | 0UL);
    s_w.rainStart = (time_t)(d["rs"] | 0UL);
    for (int i = 0; i < 12; i++) s_w.rainPct[i] = d["rp"][i] | 255;
    for (int i = 0; i < 3; i++) {
        JsonVariant o = d["fc"][i];
        s_w.forecast[i].tmin = o["lo"] | -999.0f; s_w.forecast[i].tmax = o["hi"] | -999.0f;
        s_w.forecast[i].code = o["c"] | 0;        s_w.forecast[i].uvMax = o["uv"] | -1.0f;
        s_w.forecast[i].sunrise = (time_t)(o["sr"] | 0UL);
        s_w.forecast[i].sunset  = (time_t)(o["ss"] | 0UL);
    }
    s_w.valid = s_w.tempC > -900.0f;
}

// ── fetch ───────────────────────────────────────────────────────────────────

static bool fail(int code, const char* err) {
    FetchPolicy::onFailure(s_pol, code, -1);
    // Keep showing the last good snapshot (it goes stale on its own clock); the error text is kept for the API.
    snprintf(s_w.err, sizeof(s_w.err), "%s", err);
    return false;
}

static bool fetch(const Settings& s) {
    s_attempts++;
    char url[400];
    snprintf_P(url, sizeof(url), PSTR("http://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,apparent_temperature,weather_code,relative_humidity_2m,"
        "wind_speed_10m,is_day"
        "&hourly=precipitation_probability&forecast_hours=12"
        "&daily=temperature_2m_min,temperature_2m_max,weather_code,sunrise,sunset,uv_index_max"
        "&forecast_days=3&timezone=auto&timeformat=unixtime"),
        s.weatherLat, s.weatherLon);

    WiFiClient client;
    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(5000);
    if (!http.begin(client, url)) return fail(-2, "http begin");
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        char e[24]; snprintf(e, sizeof(e), "HTTP %d", code);
        return fail(code, e);
    }

    JsonDocument doc;
    auto err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        char e[24]; snprintf(e, sizeof(e), "JSON %s", err.c_str());
        return fail(0, e);
    }

    WeatherData w;
    if (!WeatherCore::parse(doc, w)) return fail(0, "no current");

    FetchPolicy::onSuccess(s_pol, time(nullptr));
    w.lastOk = s_pol.lastOk;
    s_w = w;
    s_live = true;
    s_lat = s.weatherLat;
    s_lon = s.weatherLon;
    save();
    return true;
}

void update() {
    const Settings& s = configManager.ui;
    if (!s.showWeather || !configured(s)) return;
    if (WiFi.status() != WL_CONNECTED || WiFi.getMode() != WIFI_STA || time(nullptr) < REASONABLE_EPOCH) return;
    const uint32_t now = millis();

    // A changed location makes the old numbers wrong: drop them (the clock shows meanwhile) and fetch at once.
    if (s_locKnown && (fabsf(s.weatherLat - s_lat) > 0.0001f || fabsf(s.weatherLon - s_lon) > 0.0001f)) {
        s_w = WeatherData();
        s_pol = FetchPolicy::State();
        s_live = false;
        s_lat = s.weatherLat;
        s_lon = s.weatherLon;
        LittleFS.remove(kCachePath);
        s_primed = true;
        s_dueMs = now;
    }
    if (!s_primed) {
        s_primed = true;
        s_dueMs = now + FIRST_FETCH_DELAY_MS;
        // fresh cache: nothing to do until it is due again
        if (s_w.valid && !isStale(s)) s_dueMs = now + (uint32_t)intervalMin(s) * 60000UL;
    }
    if ((int32_t)(now - s_dueMs) < 0) return;

    // A fetch blocks for about a second: not right before a scheduled event
    const long untilEvent = EventScheduler::secondsToNext();
    if (untilEvent >= 0 && untilEvent <= EVENT_QUIET_S) return;

    if (DisplayManager::isGifPlaying()) ChannelManager::releaseMemory();   // an idle GIF holds 24 KB
    Display::releaseFont();
    if (ESP.getFreeHeap() < MIN_FREE_HEAP || ESP.getMaxFreeBlockSize() < MIN_FREE_BLOCK) {   // NOLINT
        s_dueMs = now + BLOCKED_RETRY_MS;
        return;
    }
    const bool ok = fetch(s);
    const uint32_t waitMs = ok ? (uint32_t)intervalMin(s) * 60000UL : s_pol.waitS * 1000UL;
    s_dueMs = millis() + waitMs;
}

void rainHint(const Settings& s, char* buf, size_t n) {
    WeatherCore::rainHint(s_w, time(nullptr), s.clock24h, buf, n);
}

const char* describe(uint8_t code) { return WeatherCore::describe(code); }

float toDisplay(float c, bool fahrenheit) { return WeatherCore::toDisplay(c, fahrenheit); }

}  // namespace Weather
