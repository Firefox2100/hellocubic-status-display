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

#include "ntp/NTPClient.h"
#include <ctime>
#include <array>
#include <lwip/apps/sntp.h>
#include <coredecls.h>
#include <cstdlib>
#include <Logger.h>
#include <wireless/WiFiManager.h>
#include "config/ConfigManager.h"

extern ConfigManager configManager;

static constexpr const char* TAG = "NTPClient";

/**
 * @brief Default NTP server
 */
static constexpr const char* DEFAULT_NTP_SERVER1 = "pool.ntp.org";

/**
 * @brief miliseconds per second
 */
static constexpr unsigned long MILLIS_PER_SECOND = 1000UL;

/**
 * @brief Default NTP timeout in milliseconds
 */
static constexpr unsigned long DEFAULT_NTP_TIMEOUT_MS = 10000UL;

/**
 * @brief Poll delay in milliseconds
 */
static constexpr unsigned long POLL_DELAY_MS = 200UL;

/**
 * @brief Reasonable epoch time to 2020/09/13
 */
static constexpr time_t REASONABLE_EPOCH = 1600000000UL;

/**
 * @brief Status buffer size
 */
static constexpr size_t STATUS_BUFFER_SIZE = 64U;

/**
 * @brief Year base for struct tm
 */
static constexpr int TM_YEAR_BASE = 1900;

/**
 * @brief Delay before retrying after all attempts failed (e.g. hotspot without internet)
 */
static constexpr unsigned long RETRY_AFTER_FAILURE_MS = 60000UL;

/**
 * @brief Set by the SNTP callback when the system time was really updated from a server.
 *
 * Checking only "time looks valid" is wrong after the first sync: the clock is already valid,
 * so a resync would be reported as done before any server answered.
 */
static volatile bool s_sntpUpdated = false;

NTPClient::NTPClient() = default;

/**
 * @brief Apply a POSIX TZ string so localtime() returns local time
 *
 * @param posixTz TZ string, falls back to UTC when empty
 */
void NTPClient::applyTimezone(const char* posixTz) {
    setenv("TZ", (posixTz != nullptr && posixTz[0] != '\0') ? posixTz : "UTC0", 1);
    tzset();
}

/**
 * @brief Initialize the NTP client
 * @param syncIntervalSeconds Sync interval in seconds (default: 6 hours)
 * @param maxRetries Maximum number of retries on failure (default: 3)
 *
 * @return void
 */
void NTPClient::begin(uint32_t syncIntervalSeconds, uint8_t maxRetries) {
    _syncIntervalSeconds = syncIntervalSeconds;
    _maxRetries = maxRetries;
    _lastStatus = "not started";
    _nextSyncAttemptMs = millis();

    applyTimezone(configManager.timezone.c_str());
    settimeofday_cb([]() { s_sntpUpdated = true; });

    Logger::info("NTP client initialized", TAG);
}

/**
 * @brief Begin one SNTP attempt (does not wait for the answer)
 */
void NTPClient::startAttempt() {
    const char* srv = configManager.getNtpServer();
    const bool custom = (srv != nullptr && srv[0] != '\0');

    s_sntpUpdated = false;
    _attempt++;
    _attemptStartMs = millis();
    _syncing = true;
    _lastStatus = "syncing";

    // The tz overload sets TZ and tzset(), keeping local time correct
    if (custom) {
        configTime(configManager.timezone.c_str(), srv, DEFAULT_NTP_SERVER1);
    } else {
        configTime(configManager.timezone.c_str(), DEFAULT_NTP_SERVER1);
    }
}

/**
 * @brief Record a successful sync
 */
void NTPClient::recordSuccess() {
    const time_t now = time(nullptr);
    _lastSync = now;
    _lastOk = true;
    _syncing = false;

    std::array<char, STATUS_BUFFER_SIZE> buf;
    struct tm tmInfo {};
    localtime_r(&now, &tmInfo);
    snprintf(buf.data(), buf.size(), "Synced: %04d-%02d-%02d %02d:%02d:%02d", tmInfo.tm_year + TM_YEAR_BASE,
             tmInfo.tm_mon + 1, tmInfo.tm_mday, tmInfo.tm_hour, tmInfo.tm_min, tmInfo.tm_sec);
    _lastStatus = buf.data();

    Logger::info(_lastStatus.c_str(), TAG);

    sntp_stop();
}

/**
 * @brief Record that all attempts failed
 */
void NTPClient::recordFailure() {
    _lastOk = false;
    _syncing = false;
    _lastStatus = "sync failed";

    sntp_stop();

    Logger::error(F("NTP sync failed after retries"), TAG);
}

/**
 * @brief Check the running attempt without blocking
 */
void NTPClient::pollSync() {
    if (s_sntpUpdated && time(nullptr) > REASONABLE_EPOCH) {
        recordSuccess();
        _nextSyncAttemptMs = millis() + (_syncIntervalSeconds * MILLIS_PER_SECOND);

        return;
    }

    if ((millis() - _attemptStartMs) < DEFAULT_NTP_TIMEOUT_MS) {
        return;
    }

    std::array<char, STATUS_BUFFER_SIZE> retryBuf;
    snprintf(retryBuf.data(), retryBuf.size(), "NTP sync attempt %d failed", _attempt);
    Logger::warn(retryBuf.data(), TAG);

    if (_attempt < _maxRetries) {
        startAttempt();

        return;
    }

    recordFailure();
    _nextSyncAttemptMs = millis() + RETRY_AFTER_FAILURE_MS;
}

/**
 * @brief Main loop to handle periodic NTP sync (never blocks)
 *
 * @return void
 */
void NTPClient::loop() {
    if (!WiFiManager::isConnected()) {
        _lastStatus = "network unavailable";
        _syncing = false;

        return;
    }

    if (_syncing) {
        pollSync();

        return;
    }

    // Signed difference keeps this correct across the millis() wrap
    if (static_cast<long>(millis() - _nextSyncAttemptMs) >= 0) {
        _attempt = 0;
        Logger::info("Starting NTP sync...", TAG);
        startAttempt();
    }
}

/**
 * @brief Trigger an immediate NTP sync and wait for the result (blocking, used by the API)
 *
 * @return true if sync was successful false otherwise
 */
auto NTPClient::syncNow() -> bool {
    if (!WiFiManager::isConnected()) {
        _lastStatus = "network unavailable";
        _lastOk = false;

        Logger::warn(F("Manual NTP sync requested but network is unavailable"), TAG);

        return false;
    }

    performSync();
    _nextSyncAttemptMs = millis() + (_syncIntervalSeconds * MILLIS_PER_SECOND);

    return _lastOk;
}

/**
 * @brief Blocking sync with retries
 *
 * @return void
 */
void NTPClient::performSync() {
    Logger::info("Starting NTP sync (manual)...", TAG);

    _attempt = 0;
    while (_attempt < _maxRetries) {
        startAttempt();

        while ((millis() - _attemptStartMs) < DEFAULT_NTP_TIMEOUT_MS) {
            if (s_sntpUpdated && time(nullptr) > REASONABLE_EPOCH) {
                recordSuccess();

                return;
            }
            delay(POLL_DELAY_MS);
        }

        std::array<char, STATUS_BUFFER_SIZE> retryBuf;
        snprintf(retryBuf.data(), retryBuf.size(), "NTP sync attempt %d failed", _attempt);
        Logger::warn(retryBuf.data(), TAG);
    }

    recordFailure();
}

/**
 * @brief Check if the last sync was successful
 *
 * @return true if last sync was successful or false otherwise
 */
auto NTPClient::lastSyncOk() const -> bool { return _lastOk; }

/**
 * @brief Get the time of the last successful sync
 *
 * @return time_t of the last sync
 */
auto NTPClient::lastSyncTime() const -> time_t { return _lastSync; }

/**
 * @brief Get the last sync status message
 *
 * @return String containing the last status
 */
auto NTPClient::lastStatus() const -> String { return _lastStatus; }
