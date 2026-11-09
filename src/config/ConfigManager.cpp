// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * GeekMagic Open Firmware
 * Copyright (C) 2026 Times-Z
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <ArduinoJson.h>
#include <array>
#include <LittleFS.h>

#include <Logger.h>
#include "config/ConfigManager.h"
#include "ui/night_core.h"
#include "config/SecureStorage.h"

namespace {
/** @brief Night settings; a bad value keeps the default. */
void loadNight(JsonVariantConst cfg, UiSettings& settings) {
    NightCore::Mode mode = NightCore::Mode::Disabled;
    if (NightCore::modeFrom(cfg[F("night_mode")] | "", mode)) {
        settings.nightMode = NightCore::modeName(mode);
    }
    NightCore::Style style = NightCore::Style::Face;
    if (NightCore::styleFrom(cfg[F("night_style")] | "", style)) {
        settings.nightStyle = NightCore::styleName(style);
    }
    int minutes = 0;
    if (NightCore::parseHm(cfg[F("night_start")] | "", minutes)) {
        settings.nightStart = static_cast<uint16_t>(minutes);
    }
    if (NightCore::parseHm(cfg[F("night_end")] | "", minutes)) {
        settings.nightEnd = static_cast<uint16_t>(minutes);
    }
    const int pct = cfg[F("night_brightness")] | static_cast<int>(settings.nightBrightness);
    if (pct >= NightCore::BRIGHTNESS_MIN && pct <= NightCore::BRIGHTNESS_MAX) {
        settings.nightBrightness = static_cast<uint8_t>(pct);
    }
    settings.nightManual = cfg[F("night_manual")] | settings.nightManual;
}

// NOLINTBEGIN(readability-misplaced-array-index)
void saveNight(JsonObject cfg, const UiSettings& settings) {
    std::array<char, NightCore::HM_SIZE> text{};
    cfg[F("night_mode")] = settings.nightMode.c_str();
    cfg[F("night_style")] = settings.nightStyle.c_str();
    NightCore::formatHm(settings.nightStart, text.data(), text.size());
    cfg[F("night_start")] = text.data();
    NightCore::formatHm(settings.nightEnd, text.data(), text.size());
    cfg[F("night_end")] = text.data();
    cfg[F("night_brightness")] = settings.nightBrightness;
    cfg[F("night_manual")] = settings.nightManual;
}
// NOLINTEND(readability-misplaced-array-index)

auto readBool(JsonVariantConst value, bool fallback) -> bool { return value.isNull() ? fallback : value.as<bool>(); }

auto readUint16(JsonVariantConst value, uint16_t fallback) -> uint16_t {
    return value.isNull() ? fallback : value.as<uint16_t>();
}

auto readString(JsonVariantConst value, const std::string& fallback) -> std::string {
    return value.is<const char*>() ? std::string(value.as<const char*>()) : fallback;
}
}  // namespace

ConfigManager::ConfigManager(const char* filename) : filename(filename), secure() {}

/**
 * @brief Loads the configuration from a file stored in SPIFFS
 *
 * @return true if the configuration was successfully loaded and parsed false otherwise
 */
auto ConfigManager::load() -> bool {
    if (!LittleFS.begin()) {
        Logger::error(F("Failed to mount LittleFS"), "ConfigManager");
        return false;
    }

    File file = LittleFS.open(filename.c_str(), "r");
    if (!file) {
        Logger::error(F("Failed to open config file"), "ConfigManager");
        return false;
    }

    size_t size = file.size();
    if (size == 0) {
        Logger::warn(F("Config file is empty"), "ConfigManager");
        file.close();
        return false;
    }

    std::unique_ptr<char[]> buf(new char[size + 1]);
    file.readBytes(buf.get(), size);
    buf[size] = '\0';
    file.close();

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, buf.get());
    if (error) {
        Logger::error(("Failed to parse config file : " + String(error.c_str())).c_str(), "ConfigManager");
        return false;
    }

    String ssid = doc[F("wifi_ssid")] | "";
    String password = doc[F("wifi_password")] | "";
    String api_token = doc[F("api_token")] | "";
    String ntp_server_cfg = doc[F("ntp_server")] | "";

    this->lcd_rotation = doc[F("lcd_rotation")] | lcd_rotation;

    // Always honor the saved NTP server (it used to be applied only during credential migration)
    if (ntp_server_cfg.length() != 0) {
        this->ntp_server = ntp_server_cfg.c_str();
    }

    this->timezone = readString(doc[F("timezone")], this->timezone);

    if (!doc["ui"].isNull()) {
        JsonVariantConst uiCfg = doc["ui"];
        this->ui.userName = readString(uiCfg[F("user_name")], ui.userName);
        this->ui.clock24h = readBool(uiCfg[F("clock_24h")], ui.clock24h);
        this->ui.idleMode = readString(uiCfg[F("idle_mode")], ui.idleMode);
        this->ui.idleGif = readString(uiCfg[F("idle_gif")], ui.idleGif);
        this->ui.channelSec = readUint16(uiCfg[F("channel_sec")], ui.channelSec);
        this->ui.autoRotate = readBool(uiCfg[F("auto_rotate")], ui.autoRotate);
        this->ui.pinApprovals = readBool(uiCfg[F("pin_approvals")], ui.pinApprovals);
        this->ui.approvalTtlMin = readUint16(uiCfg[F("approval_ttl_min")], ui.approvalTtlMin);
        this->ui.agentNightShow = readBool(uiCfg[F("agent_night_show")], ui.agentNightShow);
        this->ui.agentDoneCards = readBool(uiCfg[F("agent_done_cards")], ui.agentDoneCards);
        this->ui.showClaude = readBool(uiCfg[F("show_claude")], ui.showClaude);
        this->ui.showCodex = readBool(uiCfg[F("show_codex")], ui.showCodex);
        this->ui.showTrend = readBool(uiCfg[F("show_trend")], ui.showTrend);
        this->ui.showAiDash = readBool(uiCfg[F("show_ai")], ui.showAiDash);
        this->ui.showInfo = readBool(uiCfg[F("show_info")], ui.showInfo);
        this->ui.refreshMin = readUint16(uiCfg[F("refresh_min")], ui.refreshMin);
        this->ui.claudeWeeklyHero = readBool(uiCfg[F("claude_weekly_hero")], ui.claudeWeeklyHero);
        this->ui.codexWeeklyHero = readBool(uiCfg[F("codex_weekly_hero")], ui.codexWeeklyHero);
        this->ui.tlsVerify = readBool(uiCfg[F("tls_verify")], ui.tlsVerify);
        loadNight(uiCfg, this->ui);
        this->ui.showWeather = readBool(uiCfg[F("show_weather")], ui.showWeather);
        this->ui.useFahrenheit = readBool(uiCfg[F("fahrenheit")], ui.useFahrenheit);
        this->ui.weatherLat = uiCfg[F("weather_lat")] | ui.weatherLat;
        this->ui.weatherLon = uiCfg[F("weather_lon")] | ui.weatherLon;
        this->ui.weatherName = readString(uiCfg[F("weather_name")], ui.weatherName);
    } else if (!doc[F("idle")].isNull()) {
        // Phase 2 layout: carry the old settings over (the cycle list is gone, mode "cycle" now alternates)
        JsonVariantConst oldCfg = doc[F("idle")];
        this->ui.idleMode = readString(oldCfg[F("mode")], ui.idleMode);
        this->ui.idleGif = readString(oldCfg[F("gif")], ui.idleGif);
        this->ui.channelSec = readUint16(oldCfg[F("interval_s")], ui.channelSec);
        this->ui.clock24h = readBool(oldCfg[F("clock_24h")], ui.clock24h);
    }

    String nvs_ssid = secure.get("wifi_ssid", "");
    String nvs_password = secure.get("wifi_password", "");
    String nvs_api_token = secure.get("api_token", "");

    if ((ssid.length() != 0 && nvs_ssid.length() == 0) || (password.length() != 0 && nvs_password.length() == 0)) {
        secure.put("wifi_ssid", ssid.c_str());
        secure.put("wifi_password", password.c_str());

        this->ssid = secure.get("wifi_ssid").c_str();
        this->password = secure.get("wifi_password").c_str();

        // Ensure we delete the wifi credentials from the json config after migrating
        ConfigManager::save();

        Logger::info("WiFi credentials migrated to SecureStorage", "ConfigManager");
    } else {
        this->ssid = secure.get("wifi_ssid").c_str();
        this->password = secure.get("wifi_password").c_str();
    }

    if (api_token.length() != 0 && nvs_api_token.length() == 0) {
        secure.put("api_token", api_token.c_str());
        this->api_token = secure.get("api_token").c_str();

        // Ensure we delete the api token from the json config after migrating
        ConfigManager::save();

        Logger::info("API token migrated to SecureStorage", "ConfigManager");
    } else {
        this->api_token = secure.get("api_token").c_str();
    }

    return true;
}

/**
 * @brief Retrieves the current Wi-Fi SSID
 *
 * @return The SSID as a c style string
 */
auto ConfigManager::getSSID() const -> const char* { return ssid.c_str(); }

/**
 * @brief Retrieves the current Wi-Fi password
 *
 * @return The password as a c style string
 */
auto ConfigManager::getPassword() const -> const char* { return password.c_str(); }

/**
 * @brief Retrieves the current API token
 *
 * @return The API token as a c style string
 */
auto ConfigManager::getApiToken() const -> const char* { return api_token.c_str(); }

/**
 * @brief Retrieves the LCD rotation setting
 *
 * @return The rotation of the LCD
 */
auto ConfigManager::getLCDRotation() const -> uint8_t { return lcd_rotation; }

/**
 * @brief Set LCD rotation in memory
 *
 * @param newRotation Rotation value in range [0, 7]
 *
 * @return void
 */
auto ConfigManager::setLCDRotation(uint8_t newRotation) -> void { lcd_rotation = newRotation; }

/**
 * @brief Set WiFi credentials in memory
 * @param newSsid The SSID
 * @param newPassword The password
 *
 * @return void
 */
auto ConfigManager::setWiFi(const char* newSsid, const char* newPassword) -> void {
    if (newSsid != nullptr) {
        ssid = newSsid;
    }
    if (newPassword != nullptr) {
        password = newPassword;
    }
}
/**
 * @brief Set WiFi credentials in memory
 * @param newSsid The SSID
 * @param newPassword The password
 *
 * @return void
 */
auto ConfigManager::setApiToken(const char* newApiToken) -> void {
    if (newApiToken != nullptr) {
        api_token = newApiToken;
    }
}

/**
 * @brief Save the current configuration to the file
 *
 * @param clearWifiCreds If true wifi credentials will be cleared from json config
 *
 * @return true if the configuration was successfully saved false otherwise
 */
auto ConfigManager::save() -> bool {
    if (!LittleFS.begin()) {
        Logger::error(F("Failed to mount LittleFS"), "ConfigManager");

        return false;
    }

    File file = LittleFS.open(filename.c_str(), "w");

    if (!file) {
        Logger::error(F("Failed to open config file for writing"), "ConfigManager");

        return false;
    }

    JsonDocument doc;

    secure.put("wifi_ssid", this->getSSID());
    secure.put("wifi_password", this->getPassword());

    doc[F("lcd_rotation")] = lcd_rotation;
    if (!this->ntp_server.empty()) {
        doc[F("ntp_server")] = this->ntp_server.c_str();
    }
    doc[F("timezone")] = this->timezone.c_str();

    doc["ui"][F("user_name")] = ui.userName.c_str();
    doc["ui"][F("clock_24h")] = ui.clock24h;
    doc["ui"][F("idle_mode")] = ui.idleMode.c_str();
    doc["ui"][F("idle_gif")] = ui.idleGif.c_str();
    doc["ui"][F("channel_sec")] = ui.channelSec;
    doc["ui"][F("auto_rotate")] = ui.autoRotate;
    doc["ui"][F("pin_approvals")] = ui.pinApprovals;
    doc["ui"][F("approval_ttl_min")] = ui.approvalTtlMin;
    doc["ui"][F("agent_night_show")] = ui.agentNightShow;
    doc["ui"][F("agent_done_cards")] = ui.agentDoneCards;
    doc["ui"][F("show_claude")] = ui.showClaude;
    doc["ui"][F("show_trend")] = ui.showTrend;
    doc["ui"][F("show_ai")] = ui.showAiDash;
    doc["ui"][F("show_info")] = ui.showInfo;
    doc["ui"][F("show_codex")] = ui.showCodex;
    doc["ui"][F("refresh_min")] = ui.refreshMin;
    doc["ui"][F("claude_weekly_hero")] = ui.claudeWeeklyHero;
    doc["ui"][F("codex_weekly_hero")] = ui.codexWeeklyHero;
    doc["ui"][F("tls_verify")] = ui.tlsVerify;
    saveNight(doc["ui"], ui);
    doc["ui"][F("show_weather")] = ui.showWeather;
    doc["ui"][F("fahrenheit")] = ui.useFahrenheit;
    doc["ui"][F("weather_lat")] = ui.weatherLat;
    doc["ui"][F("weather_lon")] = ui.weatherLon;
    doc["ui"][F("weather_name")] = ui.weatherName;

    if (serializeJson(doc, file) == 0) {
        Logger::error(F("Failed to write config file"), "ConfigManager");
        file.close();

        return false;
    }

    file.close();
    Logger::info("Configuration saved", "ConfigManager");

    return true;
}
