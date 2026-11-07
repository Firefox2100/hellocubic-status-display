// SPDX-License-Identifier: MIT
// Adapted from glimmer (https://github.com/Avinava/glimmer), Copyright (c) 2026 glimmer contributors.
// Used under the MIT license; see THIRD_PARTY.md.
#pragma once
// Weather data + the pure logic around it (parsing Open-Meteo's answer, labels, the rain hint). No Arduino, so it is
// host-tested (test/host/weather_test.cpp).
#include <ArduinoJson.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct WeatherDay {
    float    tmin    = -999.0f;
    float    tmax    = -999.0f;
    uint8_t  code    = 0;
    float    uvMax   = -1.0f;
    time_t   sunrise = 0;
    time_t   sunset  = 0;
};

struct WeatherData {
    float       tempC      = -999.0f;
    float       feelsC     = -999.0f;
    uint8_t     code       = 0;       // WMO weather code
    bool        isDay      = true;
    int         humidity   = -1;
    float       windKmh    = -1.0f;
    WeatherDay  forecast[3];
    // Next 12 hours of precipitation probability (%, 255 = unknown); rainStart = epoch of hourly slot 0.
    uint8_t     rainPct[12];
    time_t      rainStart  = 0;
    bool        valid      = false;
    time_t      lastOk     = 0;
    char        err[24]    = "";
    WeatherData() { memset(rainPct, 255, sizeof(rainPct)); }
};

namespace WeatherCore {

// An epoch from the JSON. Read through a double: the 32-bit integer paths of the JSON library returned 0 on the cube.
inline time_t asEpoch(JsonVariantConst v) {
    return v.isNull() ? 0 : (time_t)v.as<double>();
}

// Fills `w` from an Open-Meteo answer (timeformat=unixtime). False when there is no "current" block.
inline bool parse(JsonVariantConst doc, WeatherData& w) {
    JsonObjectConst cur = doc["current"].as<JsonObjectConst>();
    if (cur.isNull() || cur["temperature_2m"].isNull()) return false;
    w.tempC    = cur["temperature_2m"]       | -999.0f;
    w.feelsC   = cur["apparent_temperature"] | w.tempC;
    w.code     = cur["weather_code"]         | 0;
    // Open-Meteo sends is_day as 0/1: read it as an int, not a JSON bool.
    w.isDay    = cur["is_day"].isNull() ? true : (cur["is_day"].as<int>() != 0);
    w.humidity = cur["relative_humidity_2m"] | -1;
    w.windKmh  = cur["wind_speed_10m"]       | -1.0f;

    JsonObjectConst daily = doc["daily"].as<JsonObjectConst>();
    for (int i = 0; i < 3; i++) {
        WeatherDay& d = w.forecast[i];
        d.tmin    = daily["temperature_2m_min"][i] | -999.0f;
        d.tmax    = daily["temperature_2m_max"][i] | -999.0f;
        d.code    = daily["weather_code"][i]       | 0;
        d.uvMax   = daily["uv_index_max"][i]       | -1.0f;
        d.sunrise = asEpoch(daily["sunrise"][i]);
        d.sunset  = asEpoch(daily["sunset"][i]);
    }

    JsonObjectConst hourly = doc["hourly"].as<JsonObjectConst>();
    w.rainStart = asEpoch(hourly["time"][0]);
    for (int i = 0; i < 12; i++) {
        JsonVariantConst p = hourly["precipitation_probability"][i];
        w.rainPct[i] = p.isNull() ? 255 : (uint8_t)p.as<int>();
    }
    w.valid = true;
    return true;
}

// WMO weather interpretation codes, compressed to short labels.
inline const char* describe(uint8_t code) {
    if (code == 0) return "Clear";
    if (code == 1 || code == 2) return "Mostly clear";
    if (code == 3) return "Cloudy";
    if (code == 45 || code == 48) return "Fog";
    if (code >= 51 && code <= 57) return "Drizzle";
    if (code >= 61 && code <= 67) return "Rain";
    if (code >= 71 && code <= 77) return "Snow";
    if (code == 80 || code == 81 || code == 82) return "Showers";
    if (code == 85 || code == 86) return "Snow showers";
    if (code >= 95 && code <= 99) return "Thunder";
    return "-";
}

inline float toDisplay(float c, bool fahrenheit) {
    if (c <= -900.0f) return c;
    return fahrenheit ? (c * 9.0f / 5.0f + 32.0f) : c;
}

// "RAIN 70% 15:00" when there is a >= 40% chance in the next 12 hours, else "".
inline void rainHint(const WeatherData& w, time_t now, bool clock24h, char* buf, size_t n) {
    if (n) buf[0] = '\0';
    if (!w.valid || !w.rainStart) return;
    int best = -1, bestPct = 0;
    for (int i = 0; i < 12; i++) {
        time_t slot = w.rainStart + i * 3600L;
        if (slot + 3600 <= now || w.rainPct[i] == 255) continue;
        if (w.rainPct[i] > bestPct) { bestPct = w.rainPct[i]; best = i; }
    }
    if (best < 0 || bestPct < 40) return;
    time_t at = w.rainStart + best * 3600L;
    struct tm tmv;
    localtime_r(&at, &tmv);
    if (at <= now)       snprintf(buf, n, "RAIN %d%% NOW", bestPct);
    else if (clock24h)   snprintf(buf, n, "RAIN %d%% %02d:00", bestPct, tmv.tm_hour);
    else {
        int h12 = tmv.tm_hour % 12;
        if (!h12) h12 = 12;
        snprintf(buf, n, "RAIN %d%% %d%s", bestPct, h12, tmv.tm_hour < 12 ? "AM" : "PM");
    }
}

}  // namespace WeatherCore
