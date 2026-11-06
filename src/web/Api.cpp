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

#include <Arduino.h>
#include <Logger.h>
#include <ArduinoJson.h>
#include <Updater.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>

#include "web/Webserver.h"
#include "web/Api.h"
#include "display/DisplayManager.h"
#include "display.h"
#include "net/TlsFetch.h"
#include "net/UsageManager.h"
#include "ui/BootScreen.h"
#include "ui/ChannelManager.h"
#include "config/CredentialStore.h"
#include "system/Diagnostics.h"

#include "config/ConfigManager.h"
#include "wireless/WiFiManager.h"
#include "ntp/NTPClient.h"

extern ConfigManager configManager;
extern WiFiManager* wifiManager;
extern NTPClient* ntpClient;

static bool otaError = false;
static size_t otaSize = 0;
static String otaStatus;
static volatile bool otaInProgress = false;
static volatile bool otaCancelRequested = false;
static size_t otaTotal = 0;

static constexpr int OTA_TEXT_X_OFFSET = 50;
static constexpr int OTA_TEXT_Y_OFFSET = 80;
static constexpr int OTA_LOADING_Y_OFFSET = 110;

static void otaHandleStart(HTTPUpload& upload, int mode);
static void otaHandleWrite(HTTPUpload& upload);
static void otaHandleEnd(HTTPUpload& upload, int mode);
static void otaHandleAborted(HTTPUpload& upload);
void handleDeleteGif(Webserver* webserver);

extern const char* AP_SSID;
extern const char* AP_PASSWORD;

static constexpr int WIFI_CONNECT_TIMEOUT_MS = 15000;
static constexpr size_t NTP_CONFIG_DOC_SIZE = 512;
static constexpr int BEARER_LEN = 7;

/**
 * @brief Register API endpoints for the webserver
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void registerApiEndpoints(Webserver* webserver) {
    Logger::info("Registering API endpoints", "API");

    // @openapi {get} /wifi/scan version=v1 group=WiFi summary="Scan available WiFi networks" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/wifi/scan", HTTP_GET, handleWifiScan);

    // @openapi {post} /wifi/connect version=v1 group=WiFi summary="Connect to a WiFi network" requiresAuth=true
    // requestBody=application/json requestBodySchema=ssid:string,password:string
    // example={"ssid":"MyNetwork","password":"password123"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->route("/api/v1/wifi/connect", HTTP_POST, handleWifiConnect);

    // @openapi {get} /wifi/status version=v1 group=WiFi summary="Get WiFi connection status" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/wifi/status", HTTP_GET, handleWifiStatus);

    // @openapi {post} /ntp/sync version=v1 group=NTP summary="Trigger NTP sync" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/ntp/sync", HTTP_POST, handleNtpSync);

    // @openapi {get} /ntp/status version=v1 group=NTP summary="Get NTP status" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/ntp/status", HTTP_GET, handleNtpStatus);

    // @openapi {get} /ntp/config version=v1 group=NTP summary="Get NTP configuration" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/ntp/config", HTTP_GET, handleNtpConfigGet);

    // @openapi {post} /ntp/config version=v1 group=NTP summary="Set NTP configuration" requiresAuth=true
    // requestBody=application/json requestBodySchema=ntp_server:string example={"ntp_server":"pool.ntp.org"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->route("/api/v1/ntp/config", HTTP_POST, handleNtpConfigSet);

    // @openapi {get} /display/rotation version=v1 group=Display summary="Get display rotation" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/display/rotation", HTTP_GET, handleDisplayRotationGet);

    // @openapi {post} /display/rotation version=v1 group=Display summary="Set display rotation" requiresAuth=true
    // requestBody=application/json requestBodySchema=rotation:integer example={"rotation":4}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->route("/api/v1/display/rotation", HTTP_POST, handleDisplayRotationSet);

    // @openapi {post} /reboot version=v1 group=System summary="Reboot the device" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/reboot", HTTP_POST, handleReboot);

    // @openapi {post} /ota/fw version=v1 group=OTA summary="Upload firmware (OTA)" requiresAuth=true
    // requestBody=multipart/form-data responses=200:application/json,401:application/json
    webserver->raw().on(
        "/api/v1/ota/fw", HTTP_POST, [webserver]() { handleOtaFinished(webserver); },
        [webserver]() { handleOtaUpload(webserver, U_FLASH); });

    // @openapi {post} /ota/fs version=v1 group=OTA summary="Upload filesystem (OTA)" requiresAuth=true
    // requestBody=multipart/form-data responses=200:application/json,401:application/json
    webserver->raw().on(
        "/api/v1/ota/fs", HTTP_POST, [webserver]() { handleOtaFinished(webserver); },
        [webserver]() { handleOtaUpload(webserver, U_FS); });

    // @openapi {get} /ota/status version=v1 group=OTA summary="Get OTA status" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/ota/status", HTTP_GET, handleOtaStatus);

    // @openapi {post} /ota/cancel version=v1 group=OTA summary="Cancel OTA" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/ota/cancel", HTTP_POST, handleOtaCancel);

    // @openapi {post} /gif version=v1 group=GIF summary="Upload a GIF" requiresAuth=true
    // requestBody=multipart/form-data responses=200:application/json,401:application/json
    webserver->raw().on(
        "/api/v1/gif", HTTP_POST, [webserver]() { handleGifUpload(webserver); },
        [webserver]() { handleGifUpload(webserver); });

    // @openapi {post} /gif/play version=v1 group=GIF summary="Play a GIF by name" requiresAuth=true
    // requestBody=application/json requestBodySchema=name:string example={"name":"animation.gif"}
    // responses=200:application/json,400:application/json,401:application/json,404:application/json
    webserver->route("/api/v1/gif/play", HTTP_POST, handlePlayGif);

    // @openapi {post} /gif/stop version=v1 group=GIF summary="Stop GIF playback" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/gif/stop", HTTP_POST, handleStopGif);

    // @openapi {delete} /gif version=v1 group=GIF summary="Delete a GIF by name" requiresAuth=true
    // requestBody=application/json requestBodySchema=name:string example={"name":"animation.gif"}
    // responses=200:application/json,400:application/json,401:application/json,404:application/json
    webserver->route("/api/v1/gif", HTTP_DELETE, handleDeleteGif);

    // @openapi {get} /gif version=v1 group=GIF summary="List GIFs" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/gif", HTTP_GET, handleListGifs);

    // @openapi {get} /system version=v1 group=System summary="Firmware version, uptime, heap and last reset reason"
    // requiresAuth=true responses=200:application/json,401:application/json
    webserver->route("/api/v1/system", HTTP_GET, handleSystemGet);

    // @openapi {get} /ui version=v1 group=Display summary="Get display settings, channel and current time" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/ui", HTTP_GET, handleUiGet);

    // @openapi {post} /ui version=v1 group=Display summary="Update display settings (partial update)"
    // requiresAuth=true requestBody=application/json
    // requestBodySchema=mode:string,gif:string,interval_s:number,auto_rotate:boolean,clock_24h:boolean,user_name:string,pin_approvals:boolean,approval_ttl_min:number,timezone:string,show_claude:boolean,show_codex:boolean,refresh_min:number,claude_weekly_hero:boolean,codex_weekly_hero:boolean,tls_verify:boolean
    // example={"mode":"cycle","gif":"arch-i-use-arch-btw.gif","interval_s":15,"user_name":"Patrick","timezone":"GMT0BST,M3.5.0/1,M10.5.0"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->route("/api/v1/ui", HTTP_POST, handleUiSet);

    // Earlier name of /ui, kept so existing scripts keep working
    webserver->route("/api/v1/idle", HTTP_GET, handleUiGet);
    webserver->route("/api/v1/idle", HTTP_POST, handleUiSet);

    // @openapi {get} /credentials version=v1 group=Credentials summary="Which Claude/Codex credentials are set (never the values)"
    // requiresAuth=true responses=200:application/json,401:application/json
    webserver->route("/api/v1/credentials", HTTP_GET, handleCredentialsGet);

    // @openapi {post} /credentials version=v1 group=Credentials summary="Set or clear Claude/Codex credentials (write-only)"
    // requiresAuth=true requestBody=application/json
    // requestBodySchema=claude_key:string,codex_token:string,codex_device_id:string,codex_model_label:string
    // example={"claude_key":"sk-ant-sid02-...","codex_model_label":"GPT-5"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->route("/api/v1/credentials", HTTP_POST, handleCredentialsSet);

    // @openapi {post} /display/debug version=v1 group=Display summary="Display bring-up debugging (reinit, fill, backlight, invert)" requiresAuth=true
    // requestBody=application/json requestBodySchema=action:string example={"action":"fill","color":"red"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->route("/api/v1/display/debug", HTTP_POST, handleDisplayDebug);

    // @openapi {get} /usage version=v1 group=Usage summary="Claude/Codex usage snapshots and fetch state (no credentials)" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/usage", HTTP_GET, handleUsageGet);

    // @openapi {post} /usage/refresh version=v1 group=Usage summary="Resume and fetch the usage sources as soon as possible (clears a stopped or rejected state)" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/usage/refresh", HTTP_POST, handleUsageRefresh);

    // @openapi {post} /usage/selftest version=v1 group=Usage summary="Anonymous TLS handshake test against claude.ai or chatgpt.com (time and heap)" requiresAuth=true
    // requestBody=application/json requestBodySchema=host:string,verify:boolean example={"host":"claude.ai","verify":true}
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/usage/selftest", HTTP_POST, handleTlsSelfTest);

    registerPushEndpoints(webserver);
    registerEventEndpoints(webserver);

    // @openapi {get} /token/check version=v1 group=Authentication summary="Check bearer token validity"
    // requiresAuth=true responses=200:application/json,401:application/json
    webserver->route("/api/v1/token/check", HTTP_GET, handleTokenCheck);

    // @openapi {post} /token/save version=v1 group=Authentication summary="Save a new bearer token" requiresAuth=true
    // requestBody=application/json requestBodySchema=token:string example={"token":"your_secure_token_value"}
    // responses=200:application/json,401:application/json,400:application/json
    webserver->route("/api/v1/token/save", HTTP_POST, handleTokenSave);

    // @openapi {get} /logs version=v1 group=System summary="Get recent logs" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/logs", HTTP_GET, handleLogsGet);

    // @openapi {get} /logs/download version=v1 group=System summary="Download logs as text file" requiresAuth=true
    // responses=200:text/plain,401:application/json
    webserver->route("/api/v1/logs/download", HTTP_GET, handleLogsDownload);

    // @openapi {post} /logs/clear version=v1 group=System summary="Clear log buffer" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->route("/api/v1/logs/clear", HTTP_POST, handleLogsClear);
}

/**
 * @brief Set CORS headers for API responses
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void setCorsHeaders(Webserver* webserver) {
    webserver->raw().sendHeader("Access-Control-Allow-Origin", "*");
    webserver->raw().sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    webserver->raw().sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    webserver->raw().sendHeader("Access-Control-Max-Age", "3600");
}

/**
 * @brief Validate bearer token from Authorization header
 * @param webserver Pointer to the Webserver instance
 *
 * @return true if token is valid false otherwise
 */
static auto validateBearerToken(Webserver* webserver) -> bool {
    if (!webserver->raw().hasHeader("Authorization")) {
        return false;
    }

    String authHeader = webserver->raw().header("Authorization");

    if (!authHeader.startsWith("Bearer ")) {
        return false;
    }

    String providedToken = authHeader.substring(BEARER_LEN);
    String storedToken = configManager.getApiToken();

    if (storedToken.length() == 0) {
        return false;
    }

    return providedToken.equals(storedToken);
}

/**
 * @brief Enforce bearer token check and send 401 response if invalid
 * @param webserver Pointer to the Webserver instance
 *
 * @return true if token is valid false otherwise
 */
auto requireBearerToken(Webserver* webserver) -> bool {
    if (validateBearerToken(webserver)) {
        return true;
    }

    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = "Invalid or missing token";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

    Logger::warn(("Unauthorized request from " + webserver->raw().client().remoteIP().toString()).c_str(), "API");

    return false;
}

/**
 * @brief Check if bearer token is valid
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleTokenCheck(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Token is valid";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Save a new bearer token
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleTokenSave(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    if (err) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        Logger::warn("Attempt to save API token with invalid JSON", "API");

        return;
    }

    const char* newToken = ddoc["token"] | "";

    if (strlen(newToken) == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "token field is required";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        Logger::warn("Attempt to save empty API token", "API");
        return;
    }

    configManager.setApiToken(newToken);
    configManager.save();

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Token saved successfully";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    Logger::info("API token updated", "API");
}

/**
 * @brief OTA status endpoint
 */
void handleOtaStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["inProgress"] = otaInProgress;
    doc["bytesWritten"] = otaSize;
    doc["totalBytes"] = otaTotal;
    doc["error"] = otaError;
    doc["message"] = otaStatus;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief OTA cancel endpoint
 */
void handleOtaCancel(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    otaCancelRequested = true;
    otaStatus = "Cancel requested";

    JsonDocument doc;
    doc["status"] = "cancelling";
    doc["message"] = "Cancel request received";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief List GIF files and FS info
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleListGifs(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    JsonArray files = doc["files"].to<JsonArray>();

    size_t usedBytes = 0;
    size_t totalBytes = 0;

    if (LittleFS.begin()) {
        Dir dir = LittleFS.openDir("/gif");

        while (dir.next()) {
            String name = dir.fileName();
            if (name.endsWith(".gif") || name.endsWith(".GIF")) {
                JsonObject fileObj = files.add<JsonObject>();

                fileObj["name"] = name;            // NOLINT(readability-misplaced-array-index)
                fileObj["size"] = dir.fileSize();  // NOLINT(readability-misplaced-array-index)
                usedBytes += dir.fileSize();
            }
        }

        FSInfo fs_info;

        if (LittleFS.info(fs_info)) {
            totalBytes = fs_info.totalBytes;
            usedBytes = fs_info.usedBytes;
        }
    }

    doc["usedBytes"] = usedBytes;
    doc["totalBytes"] = totalBytes;
    doc["freeBytes"] = totalBytes > usedBytes ? totalBytes - usedBytes : 0;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Handle GIF upload start
 * @param currentFilename The current filename being uploaded
 * @param gifFile Reference to the File object for the GIF
 * @param uploadError Reference to the upload error flag
 *
 * @return void
 */
void handleGifUploadStart(const String& currentFilename, File& gifFile, bool& uploadError) {
    uploadError = false;
    Logger::info((String("UPLOAD_FILE_START for: ") + currentFilename).c_str(), "API::GIF");

    if (!LittleFS.exists("/gif")) {
        Logger::info("/gif directory does not exist, creating...", "API::GIF");
        if (!LittleFS.mkdir("/gif")) {
            Logger::error("Failed to create /gif directory!", "API::GIF");
        }
    }

    gifFile = LittleFS.open(currentFilename, "w");
    if (!gifFile) {
        uploadError = true;
        Logger::error((String("Impossible to open file: ") + currentFilename).c_str(), "API::GIF");
        Logger::error("GIF UPLOAD Failed to open file", "API::GIF");
    } else {
        Logger::info("File opened successfully for writing.", "API::GIF");
    }
}

/**
 * @brief Handle GIF upload write
 * @param upload Reference to the HTTPUpload object
 * @param gifFile Reference to the File object for the GIF
 * @param uploadError Reference to the upload error flag
 *
 * @return void
 */
void handleGifUploadWrite(HTTPUpload& upload, File& gifFile, bool& uploadError) {
    if (!uploadError && gifFile) {
        size_t total = 0;

        while (total < upload.currentSize) {
            size_t remaining = upload.currentSize - total;
            int toWrite = static_cast<int>(remaining > static_cast<size_t>(INT_MAX) ? INT_MAX : remaining);
            size_t written = gifFile.write(upload.buf + total, toWrite);

            if (written == 0) {
                Logger::error("Write returned 0 bytes!", "API::GIF");
                uploadError = true;
                break;
            }

            total += written;
        }
    } else {
        Logger::error("Cannot write, file not open or previous error", "API::GIF");
    }
}

/**
 * @brief Handle GIF upload end
 * @param currentFilename The current filename being uploaded
 * @param gifFile Reference to the File object for the GIF
 *
 * @return void
 */
void handleGifUploadEnd(const String& currentFilename, File& gifFile) {
    if (gifFile) {
        gifFile.close();
    }

    Logger::info((String("Gif upload end: ") + currentFilename).c_str(), "API::GIF");
}

/**
 * @brief Handle GIF upload aborted
 * @param currentFilename The current filename being uploaded
 * @param gifFile Reference to the File object for the GIF
 * @param uploadError Reference to the upload error flag
 *
 * @return void
 */
void handleGifUploadAborted(const String& currentFilename, File& gifFile, bool& uploadError) {
    Logger::warn("UPLOAD_FILE_ABORTED", "API::GIF");

    if (gifFile) {
        gifFile.close();

        Logger::warn("File closed after abort", "API::GIF");
    }

    if (!currentFilename.isEmpty()) {
        if (LittleFS.remove(currentFilename)) {
            Logger::warn((String("Removed incomplete file: ") + currentFilename).c_str(), "API::GIF");
        } else {
            Logger::error((String("Failed to remove incomplete file: ") + currentFilename).c_str(), "API::GIF");
        }
    }

    uploadError = true;
}

/**
 * @brief Send GIF upload result
 * @param webserver Pointer to the Webserver instance
 * @param currentFilename The current filename being uploaded
 * @param uploadError The upload error flag
 *
 * @return void
 */
void sendGifUploadResult(Webserver* webserver, const String& currentFilename, bool uploadError) {
    JsonDocument doc;

    if (uploadError) {
        doc["status"] = "error";
        doc["message"] = "Error during GIF upload";

        Logger::error("GIF UPLOAD Error during upload", "API::GIF");
    } else {
        doc["status"] = "success";
        doc["message"] = "GIF uploaded successfully";
        doc["filename"] = currentFilename;

        Logger::info((String("Gif upload success, filename: ") + currentFilename).c_str(), "API::GIF");
    }

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Handle GIF upload
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleGifUpload(Webserver* webserver) {
    HTTPUpload& upload = webserver->raw().upload();
    static File gifFile;
    static bool uploadError = false;

    if (upload.status == UPLOAD_FILE_START && !validateBearerToken(webserver)) {
        uploadError = true;
        JsonDocument doc;

        doc["status"] = "error";
        doc["message"] = "Invalid or missing token";

        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

        return;
    }

    String filename = upload.filename;
    filename.replace("\\", "/");
    filename = filename.substring(filename.lastIndexOf('/') + 1);
    String currentFilename = "/gif/" + filename;

    switch (upload.status) {
        case UPLOAD_FILE_START:
            handleGifUploadStart(currentFilename, gifFile, uploadError);
            break;
        case UPLOAD_FILE_WRITE:
            handleGifUploadWrite(upload, gifFile, uploadError);
            break;
        case UPLOAD_FILE_END:
            handleGifUploadEnd(currentFilename, gifFile);
            break;
        case UPLOAD_FILE_ABORTED:
            handleGifUploadAborted(currentFilename, gifFile, uploadError);
            break;
        default:
            Logger::warn("Unknown upload status.", "API::GIF");
            break;
    }

    if (upload.status == UPLOAD_FILE_END || upload.status == UPLOAD_FILE_ABORTED) {
        sendGifUploadResult(webserver, currentFilename, uploadError);
    }
}

/**
 * @brief Reboot endpoint
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleReboot(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    int constexpr rebootDelayMs = 1000;

    doc["status"] = "rebooting";
    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    delay(rebootDelayMs);
    ESP.restart();  // NOLINT(readability-static-accessed-through-instance)
}

/**
 * @brief Manual NTP sync trigger endpoint
 */
void handleNtpSync(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;

    if (ntpClient == nullptr) {
        doc["status"] = "error";
        doc["message"] = "NTP client not initialized";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);

        return;
    }

    bool syncOk = ntpClient->syncNow();
    doc["status"] = syncOk ? "ok" : "error";
    doc["lastStatus"] = ntpClient->lastStatus();
    doc["lastSyncTime"] = ntpClient->lastSyncTime();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Return NTP status
 */
void handleNtpStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;

    if (ntpClient == nullptr) {
        doc["status"] = "error";
        doc["message"] = "NTP client not initialized";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);
        return;
    }

    doc["lastOk"] = ntpClient->lastSyncOk();
    doc["lastStatus"] = ntpClient->lastStatus();
    doc["lastSyncTime"] = ntpClient->lastSyncTime();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Get NTP configuration
 */
void handleNtpConfigGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["ntp_server"] = configManager.getNtpServer();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Set NTP configuration
 */
void handleNtpConfigSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";

        String json;

        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    if (err) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    const char* server = ddoc["ntp_server"] | "";

    if (strlen(server) == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "ntp_server missing";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    configManager.setNtpServer(server);

    if (!configManager.save()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Failed to save config";

        String json;

        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);

        return;
    }

    // optionally trigger a sync
    if (ntpClient != nullptr) {
        ntpClient->syncNow();
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["ntp_server"] = server;
    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Get display rotation configuration
 */
void handleDisplayRotationGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["rotation"] = configManager.getLCDRotationSafe();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Set display rotation configuration
 */
void handleDisplayRotationSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    if (err || !ddoc["rotation"].is<int>()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON or missing rotation";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    int rotation = ddoc["rotation"].as<int>();
    const int rotation_range_min = 0;
    const int rotation_range_max = 7;

    if (rotation < rotation_range_min || rotation > rotation_range_max) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] =
            "rotation must be between " + String(rotation_range_min) + " and " + String(rotation_range_max);

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    auto newRotation = static_cast<uint8_t>(rotation);
    configManager.setLCDRotation(newRotation);
    DisplayManager::setRotation(newRotation);
    ChannelManager::invalidate();

    if (!configManager.save()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Failed to save config";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);

        return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["rotation"] = newRotation;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    Logger::info(("Display rotation updated to " + String(newRotation)).c_str(), "API");
}

/**
 * @brief Handle OTA upload
 * @param webserver Pointer to the Webserver instance
 * @param mode Update mode U_FLASH U_FS
 *
 * @return void
 */
void handleOtaUpload(Webserver* webserver, int mode) {
    HTTPUpload& upload = webserver->raw().upload();

    if (upload.status == UPLOAD_FILE_START && !validateBearerToken(webserver)) {
        otaError = true;
        otaStatus = "Unauthorized";

        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid or missing token";

        String json;

        serializeJson(doc, json);
        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

        return;
    }

    switch (upload.status) {
        case UPLOAD_FILE_START:
            otaHandleStart(upload, mode);
            break;
        case UPLOAD_FILE_WRITE:
            otaHandleWrite(upload);
            break;
        case UPLOAD_FILE_END:
            otaHandleEnd(upload, mode);
            break;
        case UPLOAD_FILE_ABORTED:
            otaHandleAborted(upload);
            break;
        default:
            break;
    }
}

/**
 * @brief Handle OTA finished
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleOtaFinished(Webserver* webserver) {
    if (!validateBearerToken(webserver)) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid or missing token";

        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

        return;
    }

    JsonDocument doc;
    int constexpr rebootDelayMs = 5000;

    doc["status"] = "Upload successful";
    doc["message"] = otaStatus;

    if (otaError) {
        doc["status"] = "Error";
    }

    otaInProgress = false;
    otaCancelRequested = false;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    if (!otaError) {
        delay(rebootDelayMs);
        ESP.restart();  // NOLINT(readability-static-accessed-through-instance)
    }
}

/**
 * @brief Play a GIF from LittleFS full screen
 *
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handlePlayGif(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "invalid json";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    const char* name = doc["name"];
    if (name == nullptr || strlen(name) == 0) {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "missing name";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    String filename(name);
    filename.replace("\\", "/");
    filename = filename.substring(filename.lastIndexOf('/') + 1);

    String path1 = String("/gifs/") + filename;
    String path2 = String("/gif/") + filename;
    String foundPath;

    if (LittleFS.exists(path1)) {
        foundPath = path1;
    } else if (LittleFS.exists(path2)) {
        foundPath = path2;
    } else {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "file not found";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_NOT_FOUND, "application/json", jsonOut);

        return;
    }

    bool playOk = ChannelManager::playIdleGif(foundPath);

    JsonDocument resp;

    resp["status"] = playOk ? "playing" : "error";
    resp["file"] = foundPath;

    String jsonOut;

    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);
}

/**
 * @brief Send a JSON document with CORS headers
 */
void sendJsonDoc(Webserver* webserver, int code, JsonDocument& doc) {
    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(code, "application/json", json);
}

static void sendIdleError(Webserver* webserver, const char* message) {
    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = message;
    sendJsonDoc(webserver, HTTP_CODE_BAD_REQUEST, doc);
}

/**
 * @brief Accept POSIX TZ strings such as "UTC0" or "GMT0BST,M3.5.0/1,M10.5.0"
 */
static auto validTimezone(const char* timezone) -> bool {
    constexpr size_t MAX_TZ_LEN = 63;
    const size_t len = strlen(timezone);
    if (len == 0 || len > MAX_TZ_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const char chr = timezone[i];
        const bool allowed = isalnum(static_cast<unsigned char>(chr)) != 0 || chr == '+' || chr == '-' || chr == ',' ||
                             chr == '.' || chr == '/' || chr == ':' || chr == '<' || chr == '>';
        if (!allowed) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Firmware version, uptime, heap and previous reset reason (remote debugging)
 */
void handleSystemGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    Diagnostics::fill(doc);
    sendJsonDoc(webserver, HTTP_CODE_OK, doc);
}

/**
 * @brief Run one display debugging action. @return false for an unknown action
 */
static auto runDisplayAction(const String& action, JsonDocument& body) -> bool {
    if (action == "reinit") {
        DisplayManager::reinit(body["vendor"].as<bool>(), body["invert"].isNull() ? true : body["invert"].as<bool>());
        ChannelManager::invalidate();

        return true;
    }
    if (action == "fill") {
        const String color = body["color"].as<String>();
        constexpr std::array<std::pair<const char*, uint16_t>, 4> COLORS = {
            {{"red", LCD_RED}, {"green", LCD_GREEN}, {"blue", LCD_BLUE}, {"white", LCD_WHITE}}};
        uint16_t value = LCD_BLACK;
        for (const auto& entry : COLORS) {
            if (color == entry.first) {
                value = entry.second;
            }
        }
        DisplayManager::fillScreenColor(value);

        return true;
    }
    if (action == "backlight") {
        DisplayManager::setBacklight(body["pct"].as<uint8_t>());

        return true;
    }
    if (action == "invert") {
        DisplayManager::setInversion(body["on"].as<bool>());

        return true;
    }
    if (action == "screen") {
        // Preview a boot screen: connecting | online | setup. {"name": "restore"} hands the screen back.
        const String name = body["name"].as<String>();
        if (name == "connecting") {
            BootScreen::showConnecting(WiFi.SSID().c_str());
        } else if (name == "online") {
            BootScreen::showOnline("starting up");
        } else if (name == "setup") {
            BootScreen::showSetup(AP_SSID, AP_PASSWORD, WiFi.SSID().c_str());
        } else {
            ChannelManager::invalidate();
        }

        return true;
    }

    return false;
}

/**
 * @brief Usage snapshots and fetch state (never any credentials)
 */
void handleUsageGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    UsageManager::fillStatus(doc);
    sendJsonDoc(webserver, HTTP_CODE_OK, doc);
}

/**
 * @brief Make the usage sources due now (the fetch itself runs from the main loop)
 */
void handleUsageRefresh(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    UsageManager::resume();  // also clears a stopped / rejected state: an explicit refresh is the owner asking again
    JsonDocument doc;
    doc["status"] = "scheduled";
    sendJsonDoc(webserver, HTTP_CODE_OK, doc);
}

/**
 * @brief Anonymous TLS handshake test (no credentials): {"host": "claude.ai", "verify": true|false}.
 *        Reports the HTTP code, how long the blocking handshake took and the heap around it.
 */
void handleTlsSelfTest(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument body;
    deserializeJson(body, webserver->raw().arg("plain"));
    const String host = body["host"].is<const char*>() ? body["host"].as<String>() : String("claude.ai");
    const bool verify = body["verify"].isNull() ? true : body["verify"].as<bool>();
    // Only the hosts the firmware really talks to: pointing the BearSSL client at an arbitrary server (for example one
    // with an RSA or untrusted chain) is how the TLS stack's fixed 6 KB stack can be overrun and the device reset.
    if (host != "claude.ai" && host != "chatgpt.com") {
        JsonDocument refusal;
        refusal["status"] = "error";
        refusal["message"] = "host must be claude.ai or chatgpt.com";
        sendJsonDoc(webserver, HTTP_CODE_BAD_REQUEST, refusal);

        return;
    }

    ChannelManager::releaseMemory();
    Display::releaseFont();
    JsonDocument doc;
    doc["host"] = host;
    doc["verify"] = verify;
    doc["heap_before"] = ESP.getFreeHeap();            // NOLINT(readability-static-accessed-through-instance)
    doc["block_before"] = ESP.getMaxFreeBlockSize();   // NOLINT(readability-static-accessed-through-instance)
    if (!TlsFetch::heapOk()) {
        doc["status"] = "error";
        doc["message"] = "not enough contiguous heap for a TLS handshake";
        sendJsonDoc(webserver, HTTP_CODE_OK, doc);

        return;
    }

    const TlsFetch::Result result = TlsFetch::selfTest(host.c_str(), verify);
    doc["status"] = "ok";
    doc["http_code"] = result.code;
    doc["elapsed_ms"] = result.elapsedMs;
    doc["heap_after"] = ESP.getFreeHeap();           // NOLINT(readability-static-accessed-through-instance)
    doc["block_after"] = ESP.getMaxFreeBlockSize();  // NOLINT(readability-static-accessed-through-instance)
    sendJsonDoc(webserver, HTTP_CODE_OK, doc);
}

/**
 * @brief Display bring-up debugging. Body {"action": "reinit"|"fill"|"backlight"|"invert", ...}:
 *   reinit    {"vendor": bool, "invert": bool}   re-run the panel init
 *   fill      {"color": "red|green|blue|white|black"}
 *   backlight {"pct": 0..100}
 *   invert    {"on": bool}
 *   screen    {"name": "connecting"|"online"|"setup"|"restore"}  preview a boot screen
 */
void handleDisplayDebug(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument body;
    if (deserializeJson(body, webserver->raw().arg("plain")) != DeserializationError::Ok) {
        sendIdleError(webserver, "invalid json");

        return;
    }

    if (!runDisplayAction(body["action"].as<String>(), body)) {
        sendIdleError(webserver, "action must be reinit, fill, backlight, invert or screen");

        return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    sendJsonDoc(webserver, HTTP_CODE_OK, doc);
}

/**
 * @brief GIF names are plain file names: 1..32 of [A-Za-z0-9._ -], no leading dot, no "..".
 */
static auto validGifName(const std::string& name) -> bool {
    constexpr size_t MAX_GIF_NAME = 32;
    if (name.empty() || name.size() > MAX_GIF_NAME || name[0] == '.' || name.find("..") != std::string::npos) {
        return false;
    }

    return std::all_of(name.begin(), name.end(), [](char chr) {
        return isalnum(static_cast<unsigned char>(chr)) != 0 || chr == '.' || chr == '_' || chr == '-' || chr == ' ';
    });
}

/**
 * @brief The fonts only cover printable ASCII, so reject anything else in the user name.
 */
static auto validUserName(const std::string& name) -> bool {
    constexpr size_t MAX_USER_NAME = 24;
    if (name.size() > MAX_USER_NAME) {
        return false;
    }

    return std::all_of(name.begin(), name.end(), [](char chr) { return chr >= ' ' && chr <= '~'; });
}

/**
 * @brief Current display settings plus the device's idea of the time (handy to debug timezone issues)
 */
void handleUiGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    const UiSettings& settings = configManager.ui;
    JsonDocument doc;
    doc["mode"] = settings.idleMode.c_str();
    doc["gif"] = settings.idleGif.c_str();
    doc["interval_s"] = settings.channelSec;
    doc["auto_rotate"] = settings.autoRotate;
    doc["clock_24h"] = settings.clock24h;
    doc["user_name"] = settings.userName.c_str();
    doc["pin_approvals"] = settings.pinApprovals;
    doc["approval_ttl_min"] = settings.approvalTtlMin;
    doc["agent_done_cards"] = settings.agentDoneCards;
    doc["show_claude"] = settings.showClaude;
    doc["show_codex"] = settings.showCodex;
    doc["refresh_min"] = settings.refreshMin;
    doc["claude_weekly_hero"] = settings.claudeWeeklyHero;
    doc["codex_weekly_hero"] = settings.codexWeeklyHero;
    doc["tls_verify"] = settings.tlsVerify;
    doc["timezone"] = configManager.timezone.c_str();
    doc["active_channel"] = ChannelManager::activeName();

    constexpr time_t REASONABLE_EPOCH = 1600000000;
    constexpr size_t TIME_TEXT_SIZE = 24;
    const time_t now = time(nullptr);
    const bool valid = now > REASONABLE_EPOCH;
    doc["time_valid"] = valid;
    if (valid) {
        struct tm local {};
        localtime_r(&now, &local);
        std::array<char, TIME_TEXT_SIZE> buf{};
        strftime(buf.data(), buf.size(), "%Y-%m-%d %H:%M:%S", &local);
        doc["local_time"] = buf.data();
    }

    sendJsonDoc(webserver, HTTP_CODE_OK, doc);
}

/**
 * @brief Validate a settings body and apply it onto `next` / `nextTz`.
 * @return nullptr when everything is valid, otherwise the message to send back with a 400
 */
static auto applyUiSchedule(JsonDocument& body, UiSettings& next) -> const char* {
    // "mode"/"gif"/"interval_s" are the original names, "idle_mode"/"idle_gif"/"channel_sec" the config names
    JsonVariantConst modeVal = body["mode"].isNull() ? body["idle_mode"] : body["mode"];
    if (!modeVal.isNull()) {
        const std::string mode = modeVal | "";
        if (mode != "clock" && mode != "gif" && mode != "cycle") {
            return "mode must be clock, gif or cycle";
        }
        next.idleMode = mode;
    }
    JsonVariantConst gifVal = body["gif"].isNull() ? body["idle_gif"] : body["gif"];
    if (!gifVal.isNull()) {
        next.idleGif = gifVal | "";
    }
    JsonVariantConst intervalVal = body["interval_s"].isNull() ? body["channel_sec"] : body["interval_s"];
    if (!intervalVal.isNull()) {
        const int interval = intervalVal | 0;
        constexpr int MIN_INTERVAL_S = 3;
        constexpr int MAX_INTERVAL_S = 3600;
        if (interval < MIN_INTERVAL_S || interval > MAX_INTERVAL_S) {
            return "interval_s must be between 3 and 3600";
        }
        next.channelSec = static_cast<uint16_t>(interval);
    }

    return nullptr;
}

static auto applyUiFlags(JsonDocument& body, UiSettings& next, std::string& nextTz) -> const char* {
    if (!body["auto_rotate"].isNull()) {
        next.autoRotate = body["auto_rotate"] | next.autoRotate;
    }
    if (!body["clock_24h"].isNull()) {
        next.clock24h = body["clock_24h"] | next.clock24h;
    }
    if (!body["pin_approvals"].isNull()) {
        next.pinApprovals = body["pin_approvals"] | next.pinApprovals;
    }
    if (!body["agent_done_cards"].isNull()) {
        next.agentDoneCards = body["agent_done_cards"] | next.agentDoneCards;
    }
    if (!body["approval_ttl_min"].isNull()) {
        const int ttl = body["approval_ttl_min"] | 0;
        constexpr int MAX_APPROVAL_TTL_MIN = 1440;
        if (ttl < 1 || ttl > MAX_APPROVAL_TTL_MIN) {
            return "approval_ttl_min must be between 1 and 1440";
        }
        next.approvalTtlMin = static_cast<uint16_t>(ttl);
    }
    if (!body["user_name"].isNull()) {
        next.userName = body["user_name"] | "";
        if (!validUserName(next.userName)) {
            return "user_name must be printable ASCII, at most 24 characters";
        }
    }
    if (!body["timezone"].isNull()) {
        nextTz = body["timezone"] | "";
        if (!validTimezone(nextTz.c_str())) {
            return "timezone must be a POSIX TZ string, e.g. GMT0BST,M3.5.0/1,M10.5.0";
        }
    }

    // A GIF the mode needs (or that is configured at all) must exist now. It can still be deleted later, and the
    // display then skips it.
    const bool needsGif = next.idleMode == "gif" || next.idleMode == "cycle";
    if ((needsGif || !next.idleGif.empty()) &&
        (!validGifName(next.idleGif) || !LittleFS.exists(String("/gif/") + next.idleGif.c_str()))) {
        return "gif must be the name of an existing file in /gif (required for modes gif and cycle)";
    }

    return nullptr;
}

/**
 * @brief Validate a settings body and apply it onto `next` / `nextTz`.
 * @return nullptr when everything is valid, otherwise the message to send back with a 400
 */
static auto applyUiBody(JsonDocument& body, UiSettings& next, std::string& nextTz) -> const char* {
    if (const char* error = applyUiSchedule(body, next); error != nullptr) {
        return error;
    }
    if (const char* error = applyUiFlags(body, next, nextTz); error != nullptr) {
        return error;
    }

    return nullptr;
}

/**
 * @brief Partial update of the display settings. Everything is validated before anything is applied.
 */
void handleUiSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument body;
    if (deserializeJson(body, webserver->raw().arg("plain")) != DeserializationError::Ok || !body.is<JsonObject>()) {
        sendIdleError(webserver, "invalid json");

        return;
    }

    UiSettings next = configManager.ui;
    std::string nextTz = configManager.timezone;
    if (const char* error = applyUiBody(body, next, nextTz); error != nullptr) {
        sendIdleError(webserver, error);

        return;
    }

    configManager.ui = next;
    configManager.timezone = nextTz;
    NTPClient::applyTimezone(nextTz.c_str());
    ChannelManager::applySettings(next);

    JsonDocument doc;
    doc["status"] = configManager.save() ? "ok" : "error";
    sendJsonDoc(webserver, HTTP_CODE_OK, doc);
}

/**
 * @brief Which credentials are set (never their values)
 */
void handleCredentialsGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    const CredentialStore::Status status = CredentialStore::status();
    JsonDocument doc;
    doc["claude_key_set"] = status.claudeKeySet;
    doc["codex_token_set"] = status.codexTokenSet;
    doc["codex_device_id_set"] = status.codexDeviceIdSet;
    doc["codex_model_label"] = status.codexModelLabel;
    sendJsonDoc(webserver, HTTP_CODE_OK, doc);
}

/**
 * @brief Set or clear credentials. Write-only: values are never returned or logged. An empty string clears a field.
 */
void handleCredentialsSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    // A Codex token is around 1 KB and this request parses it twice: make sure a running GIF is not holding the heap
    ChannelManager::releaseMemory();

    JsonDocument body;
    if (deserializeJson(body, webserver->raw().arg("plain")) != DeserializationError::Ok || !body.is<JsonObject>()) {
        sendIdleError(webserver, "invalid json");

        return;
    }

    int changed = 0;
    for (JsonPair pair : body.as<JsonObject>()) {
        const char* field = pair.key().c_str();
        if (!CredentialStore::knownField(field)) {
            sendIdleError(webserver, "unknown field (claude_key, codex_token, codex_device_id, codex_model_label)");

            return;
        }
        if (!pair.value().is<const char*>()) {
            sendIdleError(webserver, "values must be strings (empty string clears)");

            return;
        }
        if (!CredentialStore::put(field, String(pair.value().as<const char*>()))) {
            sendIdleError(webserver, "value too long or storage error");

            return;
        }
        changed++;
    }
    Logger::info((String("Credentials updated: ") + changed + " field(s)").c_str(), "API");
    UsageManager::credentialsChanged();

    JsonDocument doc;
    doc["status"] = "ok";
    doc["changed"] = changed;
    sendJsonDoc(webserver, HTTP_CODE_OK, doc);
}

/**
 * @brief Stop currently playing GIF
 */
void handleStopGif(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument resp;

    const bool stopped = ChannelManager::stopIdleGif();

    resp["status"] = stopped ? "stopped" : "error";

    String jsonOut;
    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);
}

/**
 * @brief Delete a GIF file from storage
 */
void handleDeleteGif(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "invalid json";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    const char* name = doc["name"];
    if (name == nullptr || strlen(name) == 0) {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "missing name";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    String filename(name);
    filename.replace("\\", "/");
    filename = filename.substring(filename.lastIndexOf('/') + 1);
    String path = String("/gif/") + filename;

    if (!LittleFS.exists(path)) {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "file not found";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_NOT_FOUND, "application/json", jsonOut);

        return;
    }

    if (LittleFS.remove(path)) {
        JsonDocument resp;
        resp["status"] = "success";
        resp["message"] = "file removed";
        resp["file"] = path;

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);

        Logger::info((String("Removed file: ") + path).c_str(), "API::GIF");
    } else {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "failed to remove file";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        Logger::error((String("Failed to remove file: ") + path).c_str(), "API::GIF");
    }
}

/**
 * @brief Handle WiFi scan
 */
void handleWifiScan(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();

    if (wifiManager != nullptr) {
        WiFiManager::scanNetworks(networks);
    }

    String out;
    serializeJson(doc["networks"], out);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", out);
}

/**
 * @brief Handle WiFi connect request
 */
void handleWifiConnect(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "invalid json";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    const char* ssid = doc["ssid"] | "";
    const char* password = doc["password"] | "";

    if (strlen(ssid) == 0) {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "missing ssid";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    bool connectOk = false;
    if (wifiManager != nullptr) {
        connectOk = wifiManager->connectToNetwork(ssid, password, WIFI_CONNECT_TIMEOUT_MS);
        ChannelManager::invalidate();  // the connect screens are still up: repaint the channels (or setup screen)
    }

    JsonDocument resp;

    resp["status"] = connectOk ? "connected" : "error";
    resp["ssid"] = ssid;

    if (connectOk) {
        resp["ip"] = wifiManager->getIP().toString();
        configManager.setWiFi(ssid, password);
        configManager.save();
    }

    if (!connectOk) {
        resp["message"] = "failed to connect";
    }

    String jsonOut;
    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);
}

/**
 * @brief WiFi status
 */
void handleWifiStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument resp;

    bool connected = (wifiManager != nullptr) && WiFiManager::isConnected();

    resp["connected"] = connected;
    resp["ssid"] = connected ? WiFiManager::getConnectedSSID() : "";
    resp["ip"] = connected ? wifiManager->getIP().toString() : "";

    String jsonOut;
    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);
}

/**
 * @brief Handle OTA start
 *
 * @param upload Reference to the HTTPUpload object
 * @param mode Update mode U_FLASH or U_FS
 *
 * @return void
 */
static void otaHandleStart(HTTPUpload& upload, int mode) {
    Logger::info((String("OTA start: ") + upload.filename).c_str(), "API::OTA");

    otaError = false;
    otaSize = 0;
    otaStatus = "";
    otaInProgress = true;
    otaCancelRequested = false;
    otaTotal = static_cast<size_t>(upload.contentLength);

    DisplayManager::clearScreen();
    DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Uploading...", 2, LCD_WHITE, LCD_BLACK,
                                    true);
    DisplayManager::drawLoadingBar(0.0F, OTA_LOADING_Y_OFFSET);

    int constexpr security_space = 0x1000;
    u_int constexpr bin_mask = 0xFFFFF000;

    FSInfo fs_info;
    LittleFS.info(fs_info);
    size_t fsSize = fs_info.totalBytes;
    size_t maxSketchSpace =
        (ESP.getFreeSketchSpace() - security_space) &  // NOLINT(readability-static-accessed-through-instance)
        bin_mask;
    size_t place = (mode == U_FS) ? fsSize : maxSketchSpace;

    if (!Update.begin(place, mode)) {
        otaError = true;
        otaStatus = Update.getErrorString();
        Logger::error((String("Update.begin failed: ") + otaStatus).c_str(), "API::OTA");
    }
}

/**
 * @brief Handle OTA write
 *
 * @param upload Reference to the HTTPUpload object
 *
 * @return void
 */
static void otaHandleWrite(HTTPUpload& upload) {
    if (!otaError) {
        if (otaCancelRequested) {
            Update.end();
            otaError = true;
            otaStatus = "Update canceled";
            otaInProgress = false;
            Logger::warn("OTA canceled by user", "API::OTA");

            DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Canceled", 2, LCD_WHITE, LCD_BLACK,
                                            true);
            DisplayManager::drawLoadingBar(0.0F, OTA_LOADING_Y_OFFSET);

            return;
        }

        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            otaError = true;
            otaStatus = Update.getErrorString();
            Logger::error((String("Write failed: ") + otaStatus).c_str(), "API::OTA");
        }

        otaSize += upload.currentSize;

        float progress = 0.0F;
        if (otaTotal > 0) {
            progress = static_cast<float>(otaSize) / static_cast<float>(otaTotal);
        }

        DisplayManager::drawLoadingBar(progress, OTA_LOADING_Y_OFFSET);
    }
}

/**
 * @brief Handle OTA end
 *
 * @param upload Reference to the HTTPUpload object
 * @param mode Update mode U_FLASH or U_FS
 *
 * @return void
 */
static void otaHandleEnd(HTTPUpload& /*upload*/, int mode) {
    if (!otaError) {
        if (Update.end(true)) {
            if (mode == U_FS) {
                Logger::info("OTA FS update complete, mounting file system...", "API::OTA");
                LittleFS.begin();
            }

            otaStatus = String("Update OK (") + String(otaSize) + " bytes)";
            Logger::info(otaStatus.c_str(), "API::OTA");

            DisplayManager::drawLoadingBar(1.0F, OTA_LOADING_Y_OFFSET);
            DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Success!", 2, LCD_WHITE, LCD_BLACK,
                                            true);
        } else {
            otaError = true;
            otaStatus = Update.getErrorString();
        }
    }
}

/**
 * @brief Handle OTA aborted
 *
 * @param upload Reference to the HTTPUpload object
 *
 * @return void
 */
static void otaHandleAborted(HTTPUpload& /*upload*/) {
    Update.end();
    otaError = true;
    otaStatus = "Update aborted";
    otaInProgress = false;
    otaCancelRequested = false;

    DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Aborted", 2, LCD_WHITE, LCD_BLACK, true);
    DisplayManager::drawLoadingBar(0.0F, OTA_LOADING_Y_OFFSET);
}

/**
 * @brief Get recent logs
 * @param webserver Pointer to the Webserver instance
 */
void handleLogsGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    JsonArray logsArray = doc["logs"].to<JsonArray>();

    size_t count = Logger::getLogCount();
    for (size_t i = 0; i < count; i++) {
        const char* entry = Logger::getLogEntry(i);
        if (entry != nullptr) {
            logsArray.add(entry);
        }
    }

    doc["count"] = count;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Download logs as a text file
 * @param webserver Pointer to the Webserver instance
 */
void handleLogsDownload(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String logs = Logger::getLogsAsString();

    setCorsHeaders(webserver);
    webserver->raw().sendHeader("Content-Disposition", "attachment; filename=\"logs.log\"");
    webserver->raw().send(HTTP_CODE_OK, "text/plain", logs);
}

/**
 * @brief Clear log buffer
 * @param webserver Pointer to the Webserver instance
 */
void handleLogsClear(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    Logger::clearLogs();

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Logs cleared";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}
