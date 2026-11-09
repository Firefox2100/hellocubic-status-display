// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/Night.h"

#include <array>
#include <ctime>

#include "config/ConfigManager.h"
#include "ui/ChannelManager.h"
#include "weather.h"

extern ConfigManager configManager;

namespace {
constexpr long REASONABLE_EPOCH = 1000000000L;
constexpr uint32_t CHECK_INTERVAL_MS = 1000;

bool g_night = false;
bool g_dark = false;
uint32_t g_revision = 0;
uint32_t g_lastCheckMs = 0;
const char* g_reason = "disabled";
NightCore::Override g_override;

/** @brief The automatic state and why. `reason` is set to the source. */
auto automaticNight(const UiSettings& settings, bool& known) -> bool {
    known = true;
    NightCore::Mode mode = NightCore::Mode::Disabled;
    NightCore::modeFrom(settings.nightMode.c_str(), mode);
    const time_t now = time(nullptr);
    if (mode == NightCore::Mode::Disabled) {
        g_reason = "disabled";

        return false;
    }
    if (mode == NightCore::Mode::Manual) {
        g_reason = "manual";

        return settings.nightManual;
    }
    if (now < REASONABLE_EPOCH) {
        known = false;
        g_reason = "no-time";

        return false;
    }
    if (mode == NightCore::Mode::Sunset) {
        const WeatherData& data = Weather::snapshot();
        if (data.valid) {
            std::array<time_t, NightCore::SUN_DAYS> rise{};
            std::array<time_t, NightCore::SUN_DAYS> set{};
            for (size_t day = 0; day < rise.size(); day++) {
                rise.at(day) = data.forecast[day].sunrise;
                set.at(day) = data.forecast[day].sunset;
            }
            const int bySun = NightCore::bySun(now, rise.data(), set.data());
            if (bySun >= 0) {
                g_reason = "sun";

                return bySun == 1;
            }
        }
        g_reason = "schedule-fallback";
    } else {
        g_reason = "schedule";
    }
    struct tm local {};
    localtime_r(&now, &local);
    const int minuteOfDay = (local.tm_hour * NightCore::MINUTES_PER_HOUR) + local.tm_min;

    return NightCore::inWindow(minuteOfDay, settings.nightStart, settings.nightEnd);
}

auto styleIsOff(const UiSettings& settings) -> bool { return settings.nightStyle == NightCore::styleName(NightCore::Style::Off); }
}  // namespace

void Night::update() {
    const uint32_t now = millis();
    if (now - g_lastCheckMs < CHECK_INTERVAL_MS) {
        return;
    }
    g_lastCheckMs = now;
    const UiSettings& settings = configManager.ui;
    bool known = true;
    const bool automatic = automaticNight(settings, known);
    const bool night = NightCore::resolve(automatic, g_override);
    if (g_override.want >= 0) {
        g_reason = "override";
    }
    const bool dark = night && styleIsOff(settings);
    if (night != g_night || dark != g_dark) {
        g_night = night;
        g_dark = dark;
        g_revision++;
    }
}

auto Night::active() -> bool { return g_night; }

auto Night::dark() -> bool { return g_dark; }

auto Night::revision() -> uint32_t { return g_revision; }

auto Night::backlightPct() -> int {
    if (!g_night) {
        return static_cast<int>(configManager.ui.brightness);
    }

    return g_dark ? 0 : static_cast<int>(configManager.ui.nightBrightness);
}

auto Night::reason() -> const char* { return g_reason; }

auto Night::overrideName() -> const char* {
    if (g_override.want < 0) {
        return "none";
    }

    return g_override.want == 1 ? "night" : "day";
}

auto Night::switchTo(bool night) -> bool {
    UiSettings& settings = configManager.ui;
    NightCore::Mode mode = NightCore::Mode::Disabled;
    NightCore::modeFrom(settings.nightMode.c_str(), mode);
    bool saved = true;
    if (mode == NightCore::Mode::Manual) {
        settings.nightManual = night;
        saved = configManager.save();
    } else {
        bool known = true;
        const bool automatic = automaticNight(settings, known);
        NightCore::setOverride(g_override, night, automatic);
    }
    g_lastCheckMs = millis() - CHECK_INTERVAL_MS;  // evaluate on the next call
    update();

    return saved;
}

void Night::clearOverride() {
    g_override.want = -1;
    g_lastCheckMs = millis() - CHECK_INTERVAL_MS;
    update();
}
