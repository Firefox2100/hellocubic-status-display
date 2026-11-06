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

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <array>
#include <functional>

/**
 * @brief HTTP status code 200
 */
static int constexpr HTTP_CODE_OK = 200;

/**
 * @brief HTTP status code 400
 */
static int constexpr HTTP_CODE_BAD_REQUEST = 400;

/**
 * @brief HTTP status code 404
 */
static int constexpr HTTP_CODE_NOT_FOUND = 404;

/**
 * @brief HTTP status code 401
 */
static int constexpr HTTP_CODE_UNAUTHORIZED = 401;

/**
 * @brief HTTP status code 500
 */
static int constexpr HTTP_CODE_INTERNAL_ERROR = 500;

class Webserver;

/**
 * @brief A plain API route: exact path + method -> handler. Routes live in a fixed table inside the server and are
 *        dispatched from one catch-all handler, instead of one heap-allocated handler object per route (that cost
 *        about 100 bytes of heap each, ~4.5 KB for the whole API).
 */
struct Route {
    const char* path = nullptr;
    HTTPMethod method = HTTP_GET;
    void (*handler)(Webserver*) = nullptr;
};

class Webserver {
   public:
    explicit Webserver(uint16_t port = 80);
    static auto beginFS(bool formatIfFailed = false) -> bool;
    void begin();
    void handleClient();
    void on(const String& uri, HTTPMethod method, std::function<void()> handler);
    void on(const String& uri, std::function<void()> handler);
    void serveStaticC(const char* uriC, const char* pathC, const char* contentTypeC = nullptr,
                      int cacheSeconds = 86400);
    void registerStaticDir(const String& fsDir, const String& uriPrefix, const String& contentType);
    void registerGenericStaticFallback(const String& fsBasePath = "/web", bool excludeRoot = true);
    /**
     * @brief Add a plain route. Routes that need an upload callback must still use raw().on().
     */
    void route(const char* path, HTTPMethod method, void (*handler)(Webserver*));
    ESP8266WebServer& raw();

   private:
    static constexpr size_t MAX_ROUTES = 64;

    ESP8266WebServer _server;
    std::array<Route, MAX_ROUTES> _routes{};
    size_t _routeCount = 0;
    String _staticBase;
    bool _staticEnabled = false;
    bool _staticExcludeRoot = true;

    void handleNotFound();
    void serveStaticFallback();

    static const char* guessContentTypeC(const char* path);
};

#endif  // WEB_SERVER_H
