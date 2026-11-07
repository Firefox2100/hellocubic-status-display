// SPDX-License-Identifier: GPL-3.0-or-later
// Weather parsing and labels against a real Open-Meteo answer (and garbage).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "weather_core.h"

static const char* kSample =
    "{\"latitude\":52.64,\"longitude\":-1.14,\"current\":{\"time\":1762468200,\"interval\":900,"
    "\"temperature_2m\":9.6,\"apparent_temperature\":7.4,\"weather_code\":61,\"relative_humidity_2m\":88,"
    "\"wind_speed_10m\":14.2,\"is_day\":0},"
    "\"hourly\":{\"time\":[1762468200,1762471800],\"precipitation_probability\":[10,70,null]},"
    "\"daily\":{\"time\":[1,2,3],\"temperature_2m_min\":[8.7,7.7,7.0],\"temperature_2m_max\":[11.6,11.8,12.1],"
    "\"weather_code\":[55,3,0],\"sunrise\":[1762500000,1762586400,1762672800],"
    "\"sunset\":[1762530000,1762616400,1762702800],\"uv_index_max\":[0.9,1.1,1.0]}}";

static void testParse() {
    JsonDocument doc;
    assert(deserializeJson(doc, kSample) == DeserializationError::Ok);
    WeatherData w;
    assert(WeatherCore::parse(doc, w));
    assert(w.valid && w.tempC > 9.5f && w.tempC < 9.7f && w.feelsC < 7.5f);
    assert(w.code == 61 && !w.isDay && w.humidity == 88 && w.windKmh > 14.0f);
    assert(w.forecast[0].code == 55 && w.forecast[2].tmax > 12.0f && w.forecast[1].sunrise == 1762586400);
    assert(w.rainStart == 1762468200 && w.rainPct[0] == 10 && w.rainPct[1] == 70 && w.rainPct[2] == 255);
    assert(w.rainPct[11] == 255);
}

static void testGarbage() {
    const char* inputs[] = {"{}", "{\"current\":{}}", "{\"current\":5}", "[]", "{\"current\":{\"temperature_2m\":\"x\"}}"};
    for (const char* text : inputs) {
        JsonDocument doc;
        if (deserializeJson(doc, text) != DeserializationError::Ok) continue;
        WeatherData w;
        const bool ok = WeatherCore::parse(doc, w);
        assert(!ok || w.valid);
    }
    JsonDocument doc;
    deserializeJson(doc, "{\"current\":{\"temperature_2m\":3.0}}");   // sparse but usable
    WeatherData w;
    assert(WeatherCore::parse(doc, w) && w.feelsC == w.tempC && w.isDay && w.humidity == -1);
    assert(w.forecast[0].tmin < -900.0f && w.rainStart == 0);
}

static void testLabels() {
    assert(std::string(WeatherCore::describe(0)) == "Clear");
    assert(std::string(WeatherCore::describe(61)) == "Rain");
    assert(std::string(WeatherCore::describe(95)) == "Thunder");
    assert(std::string(WeatherCore::describe(200)) == "-");
    assert(WeatherCore::toDisplay(10.0f, true) == 50.0f);
    assert(WeatherCore::toDisplay(10.0f, false) == 10.0f);
    assert(WeatherCore::toDisplay(-999.0f, true) == -999.0f);
}

static void testRainHint() {
    setenv("TZ", "UTC", 1);
    tzset();
    JsonDocument doc;
    deserializeJson(doc, kSample);
    WeatherData w;
    WeatherCore::parse(doc, w);
    char buf[32];
    WeatherCore::rainHint(w, 1762468300, true, buf, sizeof(buf));   // 70% in the second hour slot
    assert(std::string(buf).rfind("RAIN 70% ", 0) == 0 && std::string(buf) != "RAIN 70% NOW");
    WeatherCore::rainHint(w, 1762471900, false, buf, sizeof(buf));  // inside that slot: now
    assert(std::string(buf) == "RAIN 70% NOW");
    WeatherCore::rainHint(w, 1762468300 + 3 * 3600, true, buf, sizeof(buf));   // all slots past
    assert(buf[0] == '\0');
    WeatherData none;
    WeatherCore::rainHint(none, 1762468300, true, buf, sizeof(buf));
    assert(buf[0] == '\0');
}

int main() {
    testParse();
    testGarbage();
    testLabels();
    testRainHint();
    std::puts("weather_test: ok");
    return 0;
}
