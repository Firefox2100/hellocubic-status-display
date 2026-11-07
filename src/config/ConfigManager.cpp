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
#include <LittleFS.h>

#include <Logger.h>
#include "config/ConfigManager.h"
#include "config/SecureStorage.h"

namespace {
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
        Logger::error("Failed to mount LittleFS", "ConfigManager");
        return false;
    }

    File file = LittleFS.open(filename.c_str(), "r");
    if (!file) {
        Logger::error("Failed to open config file", "ConfigManager");
        return false;
    }

    size_t size = file.size();
    if (size == 0) {
        Logger::warn("Config file is empty", "ConfigManager");
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

    String ssid = doc["wifi_ssid"] | "";
    String password = doc["wifi_password"] | "";
    String api_token = doc["api_token"] | "";
    String ntp_server_cfg = doc["ntp_server"] | "";

    this->lcd_rotation = doc["lcd_rotation"] | lcd_rotation;

    // Always honor the saved NTP server (it used to be applied only during credential migration)
    if (ntp_server_cfg.length() != 0) {
        this->ntp_server = ntp_server_cfg.c_str();
    }

    this->timezone = readString(doc["timezone"], this->timezone);

    if (!doc["ui"].isNull()) {
        JsonVariantConst uiCfg = doc["ui"];
        this->ui.userName = readString(uiCfg["user_name"], ui.userName);
        this->ui.clock24h = readBool(uiCfg["clock_24h"], ui.clock24h);
        this->ui.idleMode = readString(uiCfg["idle_mode"], ui.idleMode);
        this->ui.idleGif = readString(uiCfg["idle_gif"], ui.idleGif);
        this->ui.channelSec = readUint16(uiCfg["channel_sec"], ui.channelSec);
        this->ui.autoRotate = readBool(uiCfg["auto_rotate"], ui.autoRotate);
        this->ui.pinApprovals = readBool(uiCfg["pin_approvals"], ui.pinApprovals);
        this->ui.approvalTtlMin = readUint16(uiCfg["approval_ttl_min"], ui.approvalTtlMin);
        this->ui.agentNightShow = readBool(uiCfg["agent_night_show"], ui.agentNightShow);
        this->ui.agentDoneCards = readBool(uiCfg["agent_done_cards"], ui.agentDoneCards);
        this->ui.showClaude = readBool(uiCfg["show_claude"], ui.showClaude);
        this->ui.showCodex = readBool(uiCfg["show_codex"], ui.showCodex);
        this->ui.showTrend = readBool(uiCfg["show_trend"], ui.showTrend);
        this->ui.showAiDash = readBool(uiCfg["show_ai"], ui.showAiDash);
        this->ui.showInfo = readBool(uiCfg["show_info"], ui.showInfo);
        this->ui.refreshMin = readUint16(uiCfg["refresh_min"], ui.refreshMin);
        this->ui.claudeWeeklyHero = readBool(uiCfg["claude_weekly_hero"], ui.claudeWeeklyHero);
        this->ui.codexWeeklyHero = readBool(uiCfg["codex_weekly_hero"], ui.codexWeeklyHero);
        this->ui.tlsVerify = readBool(uiCfg["tls_verify"], ui.tlsVerify);
        this->ui.showWeather = readBool(uiCfg["show_weather"], ui.showWeather);
        this->ui.useFahrenheit = readBool(uiCfg["fahrenheit"], ui.useFahrenheit);
        this->ui.weatherLat = uiCfg["weather_lat"] | ui.weatherLat;
        this->ui.weatherLon = uiCfg["weather_lon"] | ui.weatherLon;
        this->ui.weatherName = readString(uiCfg["weather_name"], ui.weatherName);
    } else if (!doc["idle"].isNull()) {
        // Phase 2 layout: carry the old settings over (the cycle list is gone, mode "cycle" now alternates)
        JsonVariantConst oldCfg = doc["idle"];
        this->ui.idleMode = readString(oldCfg["mode"], ui.idleMode);
        this->ui.idleGif = readString(oldCfg["gif"], ui.idleGif);
        this->ui.channelSec = readUint16(oldCfg["interval_s"], ui.channelSec);
        this->ui.clock24h = readBool(oldCfg["clock_24h"], ui.clock24h);
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
        Logger::error("Failed to mount LittleFS", "ConfigManager");

        return false;
    }

    File file = LittleFS.open(filename.c_str(), "w");

    if (!file) {
        Logger::error("Failed to open config file for writing", "ConfigManager");

        return false;
    }

    JsonDocument doc;

    secure.put("wifi_ssid", this->getSSID());
    secure.put("wifi_password", this->getPassword());

    doc["lcd_rotation"] = lcd_rotation;
    if (!this->ntp_server.empty()) {
        doc["ntp_server"] = this->ntp_server.c_str();
    }
    doc["timezone"] = this->timezone.c_str();

    doc["ui"]["user_name"] = ui.userName.c_str();
    doc["ui"]["clock_24h"] = ui.clock24h;
    doc["ui"]["idle_mode"] = ui.idleMode.c_str();
    doc["ui"]["idle_gif"] = ui.idleGif.c_str();
    doc["ui"]["channel_sec"] = ui.channelSec;
    doc["ui"]["auto_rotate"] = ui.autoRotate;
    doc["ui"]["pin_approvals"] = ui.pinApprovals;
    doc["ui"]["approval_ttl_min"] = ui.approvalTtlMin;
    doc["ui"]["agent_night_show"] = ui.agentNightShow;
    doc["ui"]["agent_done_cards"] = ui.agentDoneCards;
    doc["ui"]["show_claude"] = ui.showClaude;
    doc["ui"]["show_trend"] = ui.showTrend;
    doc["ui"]["show_ai"] = ui.showAiDash;
    doc["ui"]["show_info"] = ui.showInfo;
    doc["ui"]["show_codex"] = ui.showCodex;
    doc["ui"]["refresh_min"] = ui.refreshMin;
    doc["ui"]["claude_weekly_hero"] = ui.claudeWeeklyHero;
    doc["ui"]["codex_weekly_hero"] = ui.codexWeeklyHero;
    doc["ui"]["tls_verify"] = ui.tlsVerify;
    doc["ui"]["show_weather"] = ui.showWeather;
    doc["ui"]["fahrenheit"] = ui.useFahrenheit;
    doc["ui"]["weather_lat"] = ui.weatherLat;
    doc["ui"]["weather_lon"] = ui.weatherLon;
    doc["ui"]["weather_name"] = ui.weatherName;

    if (serializeJson(doc, file) == 0) {
        Logger::error("Failed to write config file", "ConfigManager");
        file.close();

        return false;
    }

    file.close();
    Logger::info("Configuration saved", "ConfigManager");

    return true;
}
