// SPDX-License-Identifier: GPL-3.0-or-later
//
//   GET  /api/v1/weather          what the cube has fetched and what the Home page shows from it
//   POST /api/v1/weather/refresh  fetch again as soon as possible
//
// The location, units and on/off switch are settings: see /api/v1/ui (show_weather, weather_lat, weather_lon,
// weather_name, fahrenheit).
#include <ArduinoJson.h>

#include "config/ConfigManager.h"
#include "ui/ChannelManager.h"
#include "weather.h"
#include "web/Api.h"
#include "web/Webserver.h"

extern ConfigManager configManager;

// Subscripting a JsonObject with a string literal reads as a "misplaced array index" to clang-tidy.
// NOLINTBEGIN(readability-misplaced-array-index)
namespace {
constexpr size_t TEXT_SIZE = 24;
constexpr uint8_t UNKNOWN_PCT = 255;
constexpr float NO_VALUE = -900.0F;
constexpr long REASONABLE_EPOCH = 1000000000L;

auto ageSeconds(time_t lastOk) -> long {
    const time_t now = time(nullptr);

    return (lastOk > 0 && now > REASONABLE_EPOCH) ? static_cast<long>(now - lastOk) : -1;
}

/** @brief "ok", "failing" (the last try failed), "never" (no data yet) or "off". */
auto fetchState(const UiSettings& settings, const WeatherData& data) -> const char* {
    if (!settings.showWeather || !Weather::configured(settings)) {
        return "off";
    }
    if (Weather::policy().fails > 0) {
        return "failing";
    }

    return data.valid ? "ok" : "never";
}

void addTemp(JsonObject target, const char* key, float celsius) {
    if (celsius > NO_VALUE) {
        target[key] = celsius;
    }
}

void describeFetch(JsonObject fetch, const UiSettings& settings, const WeatherData& data) {
    fetch["state"] = fetchState(settings, data);
    fetch["source"] = !data.valid ? "none" : (Weather::liveThisBoot() ? "live" : "cache");
    fetch["age_s"] = ageSeconds(data.lastOk);
    fetch["stale"] = Weather::isStale(settings);
    fetch["fails"] = Weather::policy().fails;
    fetch["last_http"] = Weather::policy().lastCode;
    fetch["error"] = data.err;
    fetch["attempts_since_boot"] = Weather::attempts();
    fetch["interval_min"] = Weather::intervalMin(settings);
    fetch["next_fetch_in_s"] = Weather::nextFetchInS();
}

void describeCurrent(JsonObject current, const WeatherData& data) {
    addTemp(current, "temp_c", data.tempC);
    addTemp(current, "feels_c", data.feelsC);
    current["code"] = data.code;
    current["condition"] = Weather::describe(data.code);
    current["is_day"] = data.isDay;
    if (data.humidity >= 0) {
        current["humidity_pct"] = data.humidity;
    }
    if (data.windKmh >= 0) {
        current["wind_kmh"] = data.windKmh;
    }
}

/** @brief Exactly the figures the Home page draws (rounded, in the chosen unit). */
void describeShown(JsonObject shown, const UiSettings& settings, const WeatherData& data) {
    std::array<char, TEXT_SIZE> text{};
    snprintf(text.data(), text.size(), "%.0f", Weather::toDisplay(data.tempC, settings.useFahrenheit));
    shown["temp"] = text.data();
    const float feels = data.feelsC > NO_VALUE ? data.feelsC : data.tempC;
    snprintf(text.data(), text.size(), "%.0f", Weather::toDisplay(feels, settings.useFahrenheit));
    shown["feels"] = text.data();
    shown["unit"] = settings.useFahrenheit ? "F" : "C";
    shown["condition"] = Weather::describe(data.code);
    Weather::rainHint(settings, text.data(), text.size());
    shown["rain_hint"] = text.data();
}

void describeForecast(JsonArray forecast, const WeatherData& data) {
    for (const WeatherDay& day : data.forecast) {
        JsonObject entry = forecast.add<JsonObject>();
        addTemp(entry, "min_c", day.tmin);
        addTemp(entry, "max_c", day.tmax);
        entry["code"] = day.code;
        entry["condition"] = Weather::describe(day.code);
        if (day.uvMax >= 0) {
            entry["uv_max"] = day.uvMax;
        }
        entry["sunrise"] = static_cast<uint32_t>(day.sunrise);
        entry["sunset"] = static_cast<uint32_t>(day.sunset);
    }
}

void handleGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    const UiSettings& settings = configManager.ui;
    const WeatherData& data = Weather::snapshot();

    JsonDocument doc;
    doc["enabled"] = settings.showWeather;
    doc["configured"] = Weather::configured(settings);
    JsonObject place = doc["location"].to<JsonObject>();
    place["name"] = settings.weatherName.c_str();
    place["lat"] = settings.weatherLat;
    place["lon"] = settings.weatherLon;
    doc["units"] = settings.useFahrenheit ? "F" : "C";
    doc["home_active"] = Weather::homeActive(settings);
    doc["active_channel"] = ChannelManager::activeName();
    describeFetch(doc["fetch"].to<JsonObject>(), settings, data);
    if (data.valid) {
        describeCurrent(doc["current"].to<JsonObject>(), data);
        describeShown(doc["shown"].to<JsonObject>(), settings, data);
        describeForecast(doc["forecast"].to<JsonArray>(), data);
        doc["rain_start"] = static_cast<uint32_t>(data.rainStart);
        JsonArray rain = doc["rain_pct_next_12h"].to<JsonArray>();
        for (const uint8_t pct : data.rainPct) {
            if (pct == UNKNOWN_PCT) {
                rain.add(nullptr);
            } else {
                rain.add(pct);
            }
        }
    }
    sendJsonDoc(webserver, HTTP_CODE_OK, doc);
}

void handleRefresh(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    Weather::refreshSoon();
    JsonDocument doc;
    doc["status"] = "scheduled";
    sendJsonDoc(webserver, HTTP_CODE_OK, doc);
}
}  // namespace
// NOLINTEND(readability-misplaced-array-index)

void registerWeatherEndpoints(Webserver* webserver) {
    // @openapi {get} /weather version=v1 group=Weather summary="Fetched weather, fetch status and what the Home page shows" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/weather", HTTP_GET, handleGet);

    // @openapi {post} /weather/refresh version=v1 group=Weather summary="Fetch the weather again soon" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/weather/refresh", HTTP_POST, handleRefresh);
}
