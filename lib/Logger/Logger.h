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

#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>

enum LogLevel { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR };

static constexpr LogLevel LOG_MIN_LEVEL = LOG_WARN;
static constexpr size_t LOG_BUFFER_MAX_ENTRIES = 20;
static constexpr size_t LOG_ENTRY_MAX_LEN = 96;

class Logger {
   public:
    static void log(LogLevel level, const char* message, const char* className = nullptr);
    // Below LOG_MIN_LEVEL these are empty inline functions, so the compiler drops the call and the message string
    // literal never reaches RAM (on this chip literals live in RAM). Arguments that build a String are still evaluated.
    static void debug([[maybe_unused]] const char* message, [[maybe_unused]] const char* className = nullptr) {
        if constexpr (LOG_MIN_LEVEL <= LOG_DEBUG) {
            log(LOG_DEBUG, message, className);
        }
    }
    static void info([[maybe_unused]] const char* message, [[maybe_unused]] const char* className = nullptr) {
        if constexpr (LOG_MIN_LEVEL <= LOG_INFO) {
            log(LOG_INFO, message, className);
        }
    }
    static void warn(const char* message, const char* className = nullptr);
    static void error(const char* message, const char* className = nullptr);

    static String getLogsAsString();
    static size_t getLogCount();
    static const char* getLogEntry(size_t index);
    static void clearLogs();

   private:
    static void printTime();
    static const char* levelToString(LogLevel level);
    static void addToBuffer(const char* entry);
    static void ensureBufferAllocated();

    static char* _logBuffer;
    static size_t _head;
    static size_t _count;
};

#endif  // LOGGER_H
