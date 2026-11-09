// SPDX-License-Identifier: GPL-3.0-or-later
#include "config/CredentialStore.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Logger.h>

#include <cstring>

#include "config/SecureStorage.h"

namespace {
constexpr const char* FILE_PATH = "/credentials.bin";
constexpr const char* LOG_TAG = "CredentialStore";
constexpr size_t MAX_FILE_BYTES = 6144;

/** @brief Read and decode the file into `doc`. A missing file is an empty store. */
auto load(JsonDocument& doc) -> bool {
    File file = LittleFS.open(FILE_PATH, "r");
    if (!file) {
        return true;
    }

    const size_t size = file.size();
    if (size == 0 || size > MAX_FILE_BYTES) {
        file.close();

        return size == 0;
    }

    auto* buf = new uint8_t[size + 1];
    file.read(buf, size);
    file.close();
    SecureStorage::xorWithDeviceKey(buf, size);
    buf[size] = '\0';
    const DeserializationError err = deserializeJson(doc, reinterpret_cast<const char*>(buf));
    delete[] buf;
    if (err) {
        Logger::warn(F("Credential file unreadable (device key changed?), ignoring it"), LOG_TAG);
        doc.clear();
    }

    return true;
}

auto save(const JsonDocument& doc) -> bool {
    String out;
    serializeJson(doc, out);
    if (out.length() > MAX_FILE_BYTES) {
        return false;
    }

    auto* buf = new uint8_t[out.length()];
    memcpy(buf, out.c_str(), out.length());
    SecureStorage::xorWithDeviceKey(buf, out.length());

    File file = LittleFS.open(FILE_PATH, "w");
    const bool written = static_cast<bool>(file) && file.write(buf, out.length()) == out.length();
    if (file) {
        file.close();
    }
    delete[] buf;

    return written;
}
}  // namespace

auto CredentialStore::knownField(const char* field) -> bool {
    return field != nullptr && (strcmp(field, CLAUDE_KEY) == 0 || strcmp(field, CODEX_TOKEN) == 0 ||
                                strcmp(field, CODEX_DEVICE_ID) == 0 || strcmp(field, CODEX_MODEL_LABEL) == 0);
}

auto CredentialStore::put(const char* field, const String& value) -> bool {
    if (!knownField(field) || value.length() > MAX_VALUE_LEN) {
        return false;
    }

    JsonDocument doc;
    load(doc);
    if (value.isEmpty()) {
        doc.remove(field);
    } else {
        doc[field] = value;
    }

    return save(doc);
}

auto CredentialStore::get(const char* field) -> String {
    if (!knownField(field)) {
        return {};
    }

    JsonDocument doc;
    load(doc);

    if (!doc[field].is<const char*>()) {
        return {};
    }

    return doc[field].as<String>();
}

auto CredentialStore::status() -> Status {
    JsonDocument doc;
    load(doc);

    const auto present = [&doc](const char* field) { return doc[field].is<const char*>() && doc[field].as<String>().length() > 0; };
    Status result;
    result.claudeKeySet = present(CLAUDE_KEY);
    result.codexTokenSet = present(CODEX_TOKEN);
    result.codexDeviceIdSet = present(CODEX_DEVICE_ID);
    result.codexModelLabel = doc[CODEX_MODEL_LABEL].is<const char*>() ? doc[CODEX_MODEL_LABEL].as<String>() : String();

    return result;
}

void CredentialStore::clear() { LittleFS.remove(FILE_PATH); }
