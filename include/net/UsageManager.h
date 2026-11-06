// SPDX-License-Identifier: GPL-3.0-or-later
// Fetch policy and endpoints adapted from glimmer (https://github.com/Avinava/glimmer, MIT, see THIRD_PARTY.md).
#pragma once

#include <ArduinoJson.h>

#include "usage_types.h"

/**
 * @brief Keeps the Claude and Codex usage snapshots fresh by polling the same web endpoints the claude.ai and
 *        chatgpt.com usage pages use, with the credentials the owner stored (CredentialStore).
 *
 * These endpoints are undocumented and are not a public API: see THIRD_PARTY.md and AGENTS.md. Nothing is fetched
 * unless a credential is set. The last good snapshots are cached in flash (/usage_cache.bin) so a restart shows
 * real numbers instead of "Loading". A failed fetch never affects anything else: it only ever updates this state.
 *
 * update() is meant to be called every loop() iteration. When a fetch is due and conditions allow it, update()
 * BLOCKS for the length of the TLS handshake and transfer (typically 3-8 s): the web server and the clock stop
 * meanwhile. Fetches are spaced out and skipped when there is not enough heap.
 */
namespace UsageManager {

/** @brief Load the history ring and the credential state. Call once after the filesystem is mounted. */
void begin();

/** @brief Run at most one due fetch. Cheap when nothing is due. */
void update();

/** @brief Re-read which credentials are set (call after they were changed) and fetch soon. */
void credentialsChanged();

/** @brief Make every source due now (still subject to spacing and the heap check). */
void refreshNow();

/**
 * @brief Forget every verdict (rejected credential, edge block, stopped endpoint) and fetch again soon.
 *
 * Fetching stops by itself when retrying cannot help (see net/fetch_failure.h and fetch_policy.h). It stays stopped
 * until resume(), setting a credential (which calls this), or a restart.
 */
void resume();

auto claude() -> const ClaudeData&;
auto codex() -> const CodexData&;

/** @brief Changes whenever a snapshot or a credential state changed (screens recompute their channel list). */
auto revision() -> uint32_t;

/** @brief Snapshot summary for debugging. Never contains credentials. */
void fillStatus(JsonDocument& doc);

}  // namespace UsageManager
