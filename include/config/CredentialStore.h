// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>

/**
 * @brief Storage for the API credentials the usage channels will need (Claude session key, Codex/OpenAI token).
 *
 * These can be large (a Codex token is around 1 KB), and the EEPROM image is a fixed 2 KB that stays resident in
 * RAM, so they live in an obfuscated file on LittleFS instead and are read on demand, never kept in RAM. The
 * obfuscation uses the same device-derived key as SecureStorage (obfuscation, not encryption). Reflashing the
 * filesystem image erases them.
 *
 * Nothing consumes these yet: this only prepares the configuration.
 */
namespace CredentialStore {

constexpr const char* CLAUDE_KEY = "claude_key";
constexpr const char* CODEX_TOKEN = "codex_token";
constexpr const char* CODEX_DEVICE_ID = "codex_device_id";
constexpr const char* CODEX_MODEL_LABEL = "codex_model_label";

constexpr size_t MAX_VALUE_LEN = 2048;

struct Status {
    bool claudeKeySet = false;
    bool codexTokenSet = false;
    bool codexDeviceIdSet = false;
    String codexModelLabel;  // not a secret: shown on the status bar
};

/** @brief True if `field` is one of the four known field names. */
auto knownField(const char* field) -> bool;

/** @brief Set a field. An empty value removes it. @return false on an unknown field, too-long value or I/O error. */
auto put(const char* field, const String& value) -> bool;

/** @brief Read one field on demand (empty if unset). Callers must not hold on to it. */
auto get(const char* field) -> String;

/** @brief Which fields are set, without exposing the values. */
auto status() -> Status;

/** @brief Remove everything. */
void clear();

}  // namespace CredentialStore
